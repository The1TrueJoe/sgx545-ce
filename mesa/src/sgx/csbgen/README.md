# SGX545 TA/3D state words (csbgen)

The XML files here describe the PowerVR SGX545's TA/3D state words in the
format of Imagination's csbgen. `src/imagination/csbgen/gen_pack_header.py`,
the generator the PowerVR Vulkan driver uses, turns each `<name>.xml` into
`csbgen/<name>.h`, with a struct, `_header` defaults, `_pack()` and
`_unpack()` per word. The same layout as `src/imagination/csbgen/rogue`.

| file | prefix | words |
|---|---|---|
| `vdm.xml` | `SGX_VDMCTRL_` | the VDM control stream: state program (`PDS_STATE0/1`), the six draw words (`INDEX_LIST0`-`5`), stream terminate |
| `ta.xml` | `SGX_TA_` | the TA state block: header, ISPA, ISPB, ISPC, the pixel PDS words, region clip, viewport, output select, W clamp, MTE, TSP0/TSP1 |
| `pbe.xml` | `SGX_PBE_` | the pixel back end's four words and its sideband word |
| `tex.xml` | `SGX_TEXSTATE_` | the four texture words a DOUTT sends (T0, strided T0, T1, T2, T3) |
| `sgx_csb.h` | | the address type the generated code needs, the four headers, and `sgx_pack()` |
| `tests/sgx_csb_test.c` | | packs and unpacks known words |

## Use

```c
#include "sgx_csb.h"

uint32_t w;

sgx_pack(&w, TA_STATE_ISPB, ispb) {
   ispb.line_width = 0x10;
   ispb.color_mask = 0xf;
}
```

`sgx_pack()` is the pack-in-place idiom of the Vulkan driver's
`pvr_csb_pack()`: the struct starts from the XML's defaults, the block sets
fields, and the word is packed when the block ends. Addresses are
`sgx_dev_addr_t` (`SGX_DEV_ADDR(va)`).

## Conventions

- A field named `unknown_<bits>` holds bits whose value is known but whose
  meaning isn't; its `default` (or a `<define>` per use, such as
  `SGX_TA_STATE_MTE_UNKNOWN_2_31_DRAW`) is that value, so the constant
  bits come from the description, not the caller.
- Where a field's width isn't known, it runs up to the next known field.
- Bits no field names are 0.

## Tests

`meson test --suite sgx` runs `sgx_csb` (with `-Dbuild-tests=true`).
