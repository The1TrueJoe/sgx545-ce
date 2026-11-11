# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT

# Shared SGX code below the gallium driver: the per-core descriptions
# (common), the USSE instruction set and the PDS programs (isaspec), the
# TA/3D state words (csbgen), and the NIR -> USSE compiler.
subdir('common')
subdir('csbgen')
subdir('isa')
subdir('pds')
# NIR is a stub without a driver that uses it (-Dtools=sgx alone)
if with_gallium_sgx
  subdir('compiler')
endif
