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

#include <linux/version.h>

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,38))
#ifndef AUTOCONF_INCLUDED
#include <linux/config.h>
#endif
#endif

#include <linux/spinlock.h>
#include <linux/mm.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/cpufeature.h>

#include "img_defs.h"
#include "pvr_debug.h"
#include "mutils.h"

#if defined(SUPPORT_LINUX_X86_PAT)

/*
 * This used to hand-roll write-combining: read MSR_IA32_CR_PAT directly,
 * convert page-protection bits to a PAT index, and check that the entry at
 * that index really was WC before trusting it.
 *
 * None of that is needed any more. x86 has provided pgprot_writecombine()
 * for years and it already does the PAT-entry selection (and the MTRR
 * fallback) internally, so all that is left for us to decide is whether the
 * CPU has PAT at all. The old code also used _PAGE_CACHE_WC and cpu_has_pat,
 * both of which were removed -- protval constants became _PAGE_CACHE_MODE_*
 * plus cachemode2protval() in 3.19, and cpu_has_* became boot_cpu_has().
 */
static IMG_BOOL g_write_combining_available = IMG_FALSE;

static IMG_VOID
PVRLinuxX86PATProbe(IMG_VOID)
{
#if defined(SUPPORT_LINUX_X86_WRITECOMBINE)
	g_write_combining_available =
		boot_cpu_has(X86_FEATURE_PAT) ? IMG_TRUE : IMG_FALSE;

	if (g_write_combining_available)
	{
		PVR_TRACE(("%s: write combining available via PAT", __FUNCTION__));
	}
	else
	{
		PVR_TRACE(("%s: no PAT, write combining unavailable", __FUNCTION__));
	}
#else
	PVR_TRACE(("%s: write combining disabled in driver build", __FUNCTION__));
#endif
}

pgprot_t
pvr_pgprot_writecombine(pgprot_t prot)
{
	return g_write_combining_available ?
		pgprot_writecombine(prot) : pgprot_noncached(prot);
}
#endif	/* SUPPORT_LINUX_X86_PAT */

IMG_VOID
PVRLinuxMUtilsInit(IMG_VOID)
{
#if defined(SUPPORT_LINUX_X86_PAT)
	PVRLinuxX86PATProbe();
#endif
}

