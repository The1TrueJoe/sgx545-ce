#!/usr/bin/env python3
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT

"""Generate the C enums the encoder and its callers need from an isaspec XML
(sgx.xml, ../pds/sgx_pds.xml): the opcode and operand-kind enums (one
value per encodable leaf, named as isaspec's encoder expects) and the
field-value enums a compiler uses."""

import argparse
import os
import re
import sys

sys.path.append(os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                              '..', '..', 'compiler', 'isaspec')))
from isa import ISA

# Field-value enums worth a C name. Their XML displays name the values.
VALUE_ENUMS = [
    '#pred3', '#pred2', '#movc-cond', '#movc-type', '#pck-fmt', '#rnd',
    '#test-alu', '#test-cond', '#testmask-type', '#swz0', '#smp-dim',
    '#smp-lod', '#ld-am', '#ld-size', '#emit-tgt',
]


def c_ident(s):
    s = s.strip().replace('!', 'n').replace('=', '_')
    s = re.sub(r'[^A-Za-z0-9_]', '', s)
    return s.upper()


def leaf_case_name(root, leaf):
    return root.encode.case_prefix + \
        leaf.name.upper().replace('.', '_').replace('-', '_').replace('#', '')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--xml', required=True)
    parser.add_argument('--out-h', required=True)
    parser.add_argument('--prefix', default='sgx_isa',
                        help='C prefix of the enums (sgx_isa_opc, ...)')
    parser.add_argument('--values', default=','.join(VALUE_ENUMS),
                        help='field-value enums to emit, comma-separated')
    args = parser.parse_args()
    prefix = args.prefix
    guard = re.sub(r'[^A-Za-z0-9]', '_', os.path.basename(args.out_h)).upper()

    isa = ISA(args.xml)
    out = []
    out.append('/* Generated from %s by sgx_isa_enums.py. */\n' % os.path.basename(args.xml))
    out.append('#ifndef %s' % guard)
    out.append('#define %s\n' % guard)

    for root in isa.roots.values():
        if root.encode is None or root.encode.case_prefix is None:
            continue
        cname = prefix + '_opc' if root.name == '#instruction' else \
            prefix + '_case_' + root.name.strip('#').replace('-', '_')
        names = sorted(leaf_case_name(root, l)
                       for leafs in isa.leafs.values() for l in leafs
                       if l.get_root() is root)
        out.append('enum %s {' % cname)
        cp = root.encode.case_prefix
        if root.name == '#instruction':
            out.append('   %sNONE = 0,' % cp)
        else:
            out.append('   %s%s__NONE = 0,' % (cp, root.name.strip('#').upper().replace('-', '_')))
        for n in names:
            out.append('   %s,' % n)
        if root.name == '#instruction':
            out.append('   %sCOUNT,' % cp)
        out.append('};\n')

    for name in filter(None, args.values.split(',')):
        e = isa.enums[name]
        base = prefix.upper() + '_' + c_ident(name.strip('#').replace('-', '_'))
        out.append('enum %s_%s {' % (prefix, name.strip('#').replace('-', '_')))
        seen = set()
        for val, v in e.values.items():
            label = c_ident(v.get_name()) or 'NONE'
            if label in seen:
                label = '%s_%s' % (label, val)
            seen.add(label)
            out.append('   %s_%s = %s,' % (base, label, val))
        out.append('};\n')

    out.append('#endif')
    with open(args.out_h, 'w', encoding='utf-8') as f:
        f.write('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()
