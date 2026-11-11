# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT

"""sgx-asm and sgx-disasm over the test lists.

Each list's instruction lines go through sgx-asm in one run (hex and -b),
must give the words a line quotes, and must disassemble back to their text
through sgx-disasm (-x and binary). Also checks the error paths.

   tools_test.py <sgx-asm> <sgx-disasm> <list>...
"""

import os
import re
import struct
import subprocess
import sys
import tempfile


def run(cmd, data=None, text=True):
    return subprocess.run(cmd, input=data, capture_output=True, text=text)


def same_text(a, b):
    return re.sub(r'\s+', ' ', a.strip()) == re.sub(r'\s+', ' ', b.strip())


def check_list(asm, dis, path):
    fails = []
    lines, want = [], []
    with open(path, encoding='utf-8') as f:
        for raw in f:
            t = raw.strip()
            if (not t or t.startswith('#') or ' == ' in t or
                    t.startswith(('hazard ', 'safe ', 'reject '))):
                continue
            text, _, words = t.partition(';')
            lines.append(text.strip())
            w = words.split()
            want.append(' '.join(w[:2]) if len(w) >= 2 else None)

    src = '\n'.join(lines) + '\n'
    r = run([asm], src)
    if r.returncode:
        return [f'{path}: sgx-asm failed: {r.stderr.strip()}'], 0
    hexes = r.stdout.splitlines()
    if len(hexes) != len(lines):
        return [f'{path}: {len(lines)} lines gave {len(hexes)} words'], 0
    for text, w, h in zip(lines, want, hexes):
        if w is not None and h != w:
            fails.append(f'{path}: {text}: {h}, expected {w}')

    r = run([dis, '-x'], r.stdout)
    texts = r.stdout.splitlines()
    if r.returncode or len(texts) != len(lines):
        fails.append(f'{path}: sgx-disasm -x failed: {r.stderr.strip()}')
    else:
        for text, d in zip(lines, texts):
            if not same_text(text, d):
                fails.append(f'{path}: {text}: disassembles as {d}')

    rb = run([asm, '-b'], src.encode(), text=False)
    words = b''.join(struct.pack('<II', *(int(x, 16) for x in h.split()))
                     for h in hexes)
    if rb.returncode or rb.stdout != words:
        fails.append(f'{path}: sgx-asm -b differs from the hex output')
    rd = run([dis], rb.stdout, text=False)
    if rd.returncode or rd.stdout.decode().splitlines() != texts:
        fails.append(f'{path}: sgx-disasm on -b differs from -x')
    return fails, len(lines)


def check_errors(asm, dis):
    fails = []
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, 'bad.s')
        with open(p, 'w') as f:
            f.write('nop\nfrob r0, r1\nnop [end]\n')
        r = run([asm, p])
        if (r.returncode != 1 or f'{p}:2: frob r0, r1:' not in r.stderr or
                len(r.stdout.splitlines()) != 2):
            fails.append(f'sgx-asm on a bad line: {r.returncode} '
                         f'{r.stderr.strip()!r}')
    for tool in (asm, dis):
        for core, status in (('535', 2), ('x', 2), ('0', 2), ('545', 0),
                             ('sgx545', 0)):
            r = run([tool, '-c', core], '')
            if r.returncode != status:
                fails.append(f'{tool} -c {core}: status {r.returncode}')
    r = run([dis, '-x'], '00000000\n')
    if r.returncode != 1:
        fails.append('sgx-disasm -x on one word: status 0')
    return fails


def main():
    asm, dis, lists = sys.argv[1], sys.argv[2], sys.argv[3:]
    fails, count = [], 0
    for path in lists:
        f, n = check_list(asm, dis, path)
        fails += f
        count += n
    fails += check_errors(asm, dis)
    for f in fails:
        print(f, file=sys.stderr)
    print(f'{count} instructions through the tools, {len(fails)} failures',
          file=sys.stderr)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
