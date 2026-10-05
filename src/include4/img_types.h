/**********************************************************************
 *
 * Copyright (C) Imagination Technologies Ltd. All rights reserved.
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 * 
 * This program is distributed in the hope it will be useful but, except 
 * as otherwise stated in writing, without any warranty; without even the 
 * implied warranty of merchantability or fitness for a particular purpose. 
 * See the GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 * 
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 * Contact Information:
 * Imagination Technologies Ltd. <gpl-support@imgtec.com>
 * Home Park Estate, Kings Langley, Herts, WD4 8LZ, UK 
 *
 ******************************************************************************/

#ifndef __IMG_TYPES_H__
#define __IMG_TYPES_H__

#if !defined(IMG_ADDRSPACE_CPUVADDR_BITS)
#define IMG_ADDRSPACE_CPUVADDR_BITS		32
#endif

#if !defined(IMG_ADDRSPACE_PHYSADDR_BITS)
#define IMG_ADDRSPACE_PHYSADDR_BITS		32
#endif

typedef unsigned int	IMG_UINT,	*IMG_PUINT;
typedef signed int		IMG_INT,	*IMG_PINT;

typedef unsigned char	IMG_UINT8,	*IMG_PUINT8;
typedef unsigned char	IMG_BYTE,	*IMG_PBYTE;
typedef signed char		IMG_INT8,	*IMG_PINT8;
typedef char			IMG_CHAR,	*IMG_PCHAR;

typedef unsigned short	IMG_UINT16,	*IMG_PUINT16;
typedef signed short	IMG_INT16,	*IMG_PINT16;
#if !defined(IMG_UINT32_IS_ULONG)
typedef unsigned int	IMG_UINT32,	*IMG_PUINT32;
typedef signed int		IMG_INT32,	*IMG_PINT32;
#else
typedef unsigned long	IMG_UINT32,	*IMG_PUINT32;
typedef signed long		IMG_INT32,	*IMG_PINT32;
#endif
#if !defined(IMG_UINT32_MAX)
	#define IMG_UINT32_MAX 0xFFFFFFFFUL
#endif

#if defined(USE_CODE)

typedef unsigned __int64	IMG_UINT64, *IMG_PUINT64;
typedef __int64				IMG_INT64,  *IMG_PINT64;

#else
	#if ((defined(LINUX) || defined(__METAG)) || defined(__QNXNTO__))
		typedef unsigned long long		IMG_UINT64,	*IMG_PUINT64;
		typedef long long 				IMG_INT64,	*IMG_PINT64;
	#else
		#error("define an OS")
	#endif
#endif

#if !(defined(LINUX) && defined (__KERNEL__))
typedef float			IMG_FLOAT,	*IMG_PFLOAT;
typedef double			IMG_DOUBLE, *IMG_PDOUBLE;
#endif

typedef	enum tag_img_bool
{
	IMG_FALSE		= 0,
	IMG_TRUE		= 1,
	IMG_FORCE_ALIGN = 0x7FFFFFFF
} IMG_BOOL, *IMG_PBOOL;

typedef void            IMG_VOID, *IMG_PVOID;

typedef IMG_INT32       IMG_RESULT;

#if defined(_WIN64)
	typedef unsigned __int64	IMG_UINTPTR_T;
	typedef signed __int64		IMG_PTRDIFF_T;
	typedef IMG_UINT64			IMG_SIZE_T;
#elif defined(__LP64__) || defined(__x86_64__) || (defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8)
	/*
	 * openHC: 64-bit Linux (x86_64). The upstream DDK only widened IMG_UINTPTR_T
	 * for _WIN64, so on a 64-bit Linux kernel it fell through to `unsigned int`
	 * and TRUNCATED every pointer stored through it. The services hash (per-process
	 * data) and the handle-table keys (HAND_KEY) hold kernel pointers this way, so
	 * a lookup returned a 32-bit-truncated pointer and the first real bridge call
	 * (CONNECT_SERVICES reading psPerProc->bInitProcess) oopsed. It MUST be
	 * pointer-sized.
	 *
	 * IMG_SIZE_T is deliberately left 32-bit: the 32-bit DDK userspace encodes the
	 * bridge structs with 32-bit sizes, and matching that ABI is what the i686
	 * compat path relies on. Only the pointer-holding type widens here.
	 */
	typedef unsigned long	IMG_UINTPTR_T;
	typedef long		IMG_PTRDIFF_T;
	typedef IMG_UINT32	IMG_SIZE_T;
#else
	typedef unsigned int	IMG_UINTPTR_T;
	typedef IMG_UINT32		IMG_SIZE_T;
#endif

typedef IMG_PVOID       IMG_HANDLE;

typedef void**          IMG_HVOID,	* IMG_PHVOID;

#define IMG_NULL        0 

typedef IMG_UINT32      IMG_SID;

typedef IMG_UINT32      IMG_EVENTSID;

#if defined(SUPPORT_SID_INTERFACE)
	typedef IMG_SID IMG_S_HANDLE;
#else
	typedef IMG_HANDLE IMG_S_HANDLE;
#endif

typedef IMG_PVOID IMG_CPU_VIRTADDR;

typedef struct _IMG_DEV_VIRTADDR
{
	
	IMG_UINT32  uiAddr;
#define IMG_CAST_TO_DEVVADDR_UINT(var)		(IMG_UINT32)(var)
	
} IMG_DEV_VIRTADDR;

typedef IMG_UINT32 IMG_DEVMEM_SIZE_T;

/* openHC/LP64: these physical-address holders MUST stay 4-byte. Upstream typed
 * uiAddr as IMG_UINTPTR_T, which was 32-bit on the 32-bit builds this DDK targets;
 * our x86_64 port widened IMG_UINTPTR_T to 8 bytes (needed for the pointer-holding
 * handle/hash types — see above). That silently grew these phys structs 4->8, which
 * changes their layout/stride inside the SGX MMU page-table + memory-mapping code
 * (mmu.c builds 32-bit PTEs/PDEs from uiAddr) and broke uKernel bring-up: the GPU
 * took a BIF (MMU) fault the instant it executed and SGXInitialise never completed.
 * IMG_ADDRSPACE_PHYSADDR_BITS==32 here, so physical addresses ARE 32-bit (this SoC
 * has <4GB RAM); pin these to IMG_UINT32 to restore the known-good 32-bit layout. */
typedef struct _IMG_CPU_PHYADDR
{

	IMG_UINT32 uiAddr;
} IMG_CPU_PHYADDR;

typedef struct _IMG_DEV_PHYADDR
{
#if IMG_ADDRSPACE_PHYSADDR_BITS == 32

	IMG_UINT32 uiAddr;
#else
	IMG_UINT32 uiAddr;
	IMG_UINT32 uiHighAddr;
#endif
} IMG_DEV_PHYADDR;

typedef struct _IMG_SYS_PHYADDR
{

	IMG_UINT32 uiAddr;
} IMG_SYS_PHYADDR;

#include "img_defs.h"

#endif	
