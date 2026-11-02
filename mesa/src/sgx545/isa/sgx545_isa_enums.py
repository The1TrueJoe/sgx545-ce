#!/usr/bin/env python3
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT

"""Generate the C enums the encoder and its callers need from sgx545.xml:
the opcode and operand-kind enums (one value per encodable leaf, named as
isaspec's encoder expects) and the field-value enums a compiler uses."""

import argparse
import os
import re
import sys

sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             '..', '..', 'compiler', 'isaspec'))
from isa import ISA

# Field-value enums worth a C name. Their XML displays name the values.
VALUE_ENUMS = [
    '#pred3', '#pred2', '#movc-cond', '#movc-type', '#pck-fmt', '#rnd',
    '#test-alu', '#test-cond', '#testmask-type', '#f16swz', '#smp-dim',
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
    args = parser.parse_args()

    isa = ISA(args.xml)
    out = []
    out.append('/* Generated from sgx545.xml by sgx545_isa_enums.py. */\n')
    out.append('#ifndef SGX545_ISA_ENUMS_H')
    out.append('#define SGX545_ISA_ENUMS_H\n')

    for root in isa.roots.values():
        if root.encode is None:
            continue
        cname = 'sgx_isa_opc' if root.name == '#instruction' else \
            'sgx_isa_case_' + root.name.strip('#').replace('-', '_')
        names = sorted(leaf_case_name(root, l)
                       for leafs in isa.leafs.values() for l in leafs
                       if l.get_root() is root)
        out.append('enum %s {' % cname)
        if root.name == '#instruction':
            out.append('   SGX_ISA_OPC_NONE = 0,')
        else:
            out.append('   SGX_%s__NONE = 0,' % root.name.strip('#').upper())
        for n in names:
            out.append('   %s,' % n)
        if root.name == '#instruction':
            out.append('   SGX_ISA_OPC_COUNT,')
        out.append('};\n')

    for name in VALUE_ENUMS:
        e = isa.enums[name]
        base = 'SGX_ISA_' + c_ident(name.strip('#').replace('-', '_'))
        out.append('enum sgx_isa_%s {' % name.strip('#').replace('-', '_'))
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
