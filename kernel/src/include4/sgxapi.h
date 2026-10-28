/*
 * openHC shim for the Cedarview DDK 1.7 extraction.
 *
 * sgx_bridge.h pulls in "sgxapi.h" ONLY under SUPPORT_SID_INTERFACE (it uses
 * "sgxapi_km.h" otherwise). The upstream DDK's userspace sgxapi.h was not kept
 * in this kernel-source extraction, so a SID build failed to find it. sgxapi_km.h
 * IS present, carries the SGX client structs the kernel bridge references (the
 * non-SID build includes it directly and compiles), and is partly SID-aware.
 *
 * Mapping sgxapi.h -> sgxapi_km.h lets the SID build compile, which makes the
 * GENERIC services bridge (pvr_bridge.h, fully SID-guarded) present the 4-byte
 * IMG_SID handle ABI that the 32-bit DDK userspace expects on an x86_64 kernel.
 *
 * NOTE: the SGX-DEVICE bridge structs (SGX_CLIENT_INFO etc.) are not SID-guarded
 * in sgxapi_km.h, so SGX-device bridges still present 8-byte pointer handles.
 * Full SGX graphics needs the upstream SID sgxapi.h (or per-struct SID variants);
 * this shim is enough to validate the generic-bridge SID path. See
 * [[c4-ea-sgx-wpe-64bit]].
 */
#ifndef __SGXAPI_H__
#define __SGXAPI_H__
#include "sgxapi_km.h"
#endif /* __SGXAPI_H__ */
