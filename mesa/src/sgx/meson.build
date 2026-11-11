# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT

# Shared SGX545 code below the gallium driver: the USSE instruction set and
# the PDS programs (isaspec), the TA/3D state words (csbgen), and the NIR ->
# USSE compiler.
subdir('csbgen')
subdir('isa')
subdir('pds')
# NIR is a stub without a driver that uses it (-Dtools=sgx alone)
if gallium_drivers.contains('sgx')
  subdir('compiler')
endif
