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

#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/list.h>
#include <linux/spinlock.h>

#include "services_headers.h"

#include "queue.h"
#include "resman.h"
#include "pvrmmap.h"
#include "pvr_debug.h"
#include "pvrversion.h"
#include "proc.h"
#include "perproc.h"
#include "env_perproc.h"
#include "linkage.h"

#include "lists.h"

static struct proc_dir_entry * dir;

static const IMG_CHAR PVRProcDirRoot[] = "pvr";

static IMG_INT pvr_proc_open(struct inode *inode,struct file *file);
static void *pvr_proc_seq_start (struct seq_file *m, loff_t *pos);
static void pvr_proc_seq_stop (struct seq_file *m, void *v);
static void *pvr_proc_seq_next (struct seq_file *m, void *v, loff_t *pos);
static int pvr_proc_seq_show (struct seq_file *m, void *v);
static ssize_t pvr_proc_write(struct file *file, const char __user *buffer, size_t count, loff_t *ppos);

static struct proc_dir_entry* g_pProcQueue;
static struct proc_dir_entry* g_pProcVersion;
static struct proc_dir_entry* g_pProcSysNodes;

#ifdef DEBUG
static struct proc_dir_entry* g_pProcDebugLevel;
#endif

#ifdef PVR_MANUAL_POWER_CONTROL
static struct proc_dir_entry* g_pProcPowerLevel;
#endif


static void ProcSeqShowVersion(struct seq_file *sfile,void* el);

static void ProcSeqShowSysNodes(struct seq_file *sfile,void* el);
static void* ProcSeqOff2ElementSysNodes(struct seq_file * sfile, loff_t off);

off_t printAppend(IMG_CHAR * buffer, size_t size, off_t off, const IMG_CHAR * format, ...)
{
    IMG_INT n;
    size_t space = size - (size_t)off;
    va_list ap;

    va_start (ap, format);

    n = vsnprintf (buffer+off, space, format, ap);

    va_end (ap);
    
    if (n >= (IMG_INT)space || n < 0)
    {
	
        buffer[size - 1] = 0;
        return (off_t)(size - 1);
    }
    else
    {
        return (off + (off_t)n);
    }
}


void* ProcSeq1ElementOff2Element(struct seq_file *sfile, loff_t off)
{
	PVR_UNREFERENCED_PARAMETER(sfile);
	
	if(!off)
		return (void*)2;
	return NULL;
}


void* ProcSeq1ElementHeaderOff2Element(struct seq_file *sfile, loff_t off)
{
	PVR_UNREFERENCED_PARAMETER(sfile);

	if(!off)
	{
		return PVR_PROC_SEQ_START_TOKEN;
	}

	
	if(off == 1)
		return (void*)2;

	return NULL;
}


/*
 * procfs plumbing, rewritten for the post-3.10 API.
 *
 * The pre-3.10 interface let a driver hang its own read_proc/write_proc
 * function pointers and a data pointer directly off struct proc_dir_entry.
 * All three fields are private to procfs now, so the handler block travels
 * through proc_create_data()'s private pointer instead, reachable with
 * pde_data(). The DDK's own seq abstraction (PVR_PROC_SEQ_HANDLERS) is
 * untouched -- it already maps one-to-one onto struct seq_operations, so
 * every ProcSeqShow / ProcSeqOff2Element handler in the tree still works.
 *
 * The old read_proc_t entry points (CreateProcEntry, CreateProcReadEntry,
 * CreatePerProcessProcEntry, RemoveProcEntry, RemovePerProcessProcEntry) had
 * no callers left and are gone rather than ported.
 */

/*
 * Every handler block we hand to proc_create_data() is recorded here. procfs
 * keeps ->data private and proc_remove() does not give it back, so the blocks
 * cannot be freed one-by-one from the entry pointer alone; they are released
 * together once the whole /proc/pvr tree is gone.
 */
static LIST_HEAD(g_sHandlerList);
static DEFINE_SPINLOCK(g_sHandlerListLock);

static void FreeAllSeqHandlers(void)
{
	PVR_PROC_SEQ_HANDLERS *psHandlers, *psTmp;
	LIST_HEAD(sDoomed);

	spin_lock(&g_sHandlerListLock);
	list_splice_init(&g_sHandlerList, &sDoomed);
	spin_unlock(&g_sHandlerListLock);

	list_for_each_entry_safe(psHandlers, psTmp, &sDoomed, sListNode)
	{
		list_del(&psHandlers->sListNode);
		kfree(psHandlers);
	}
}

static void *pvr_proc_seq_start (struct seq_file *proc_seq_file, loff_t *pos)
{
	PVR_PROC_SEQ_HANDLERS *handlers = (PVR_PROC_SEQ_HANDLERS*)proc_seq_file->private;
	if(handlers->startstop != NULL)
		handlers->startstop(proc_seq_file, IMG_TRUE);
	return handlers->off2element(proc_seq_file, *pos);
}

static void pvr_proc_seq_stop (struct seq_file *proc_seq_file, void *v)
{
	PVR_PROC_SEQ_HANDLERS *handlers = (PVR_PROC_SEQ_HANDLERS*)proc_seq_file->private;
	PVR_UNREFERENCED_PARAMETER(v);

	if(handlers->startstop != NULL)
		handlers->startstop(proc_seq_file, IMG_FALSE);
}

static void *pvr_proc_seq_next (struct seq_file *proc_seq_file, void *v, loff_t *pos)
{
	PVR_PROC_SEQ_HANDLERS *handlers = (PVR_PROC_SEQ_HANDLERS*)proc_seq_file->private;
	(*pos)++;
	if( handlers->next != NULL)
		return handlers->next( proc_seq_file, v, *pos );
	return handlers->off2element(proc_seq_file, *pos);
}

static int pvr_proc_seq_show (struct seq_file *proc_seq_file, void *v)
{
	PVR_PROC_SEQ_HANDLERS *handlers = (PVR_PROC_SEQ_HANDLERS*)proc_seq_file->private;
	handlers->show( proc_seq_file,v );
	return 0;
}

static struct seq_operations pvr_proc_seq_operations =
{
	.start =	pvr_proc_seq_start,
	.next =		pvr_proc_seq_next,
	.stop =		pvr_proc_seq_stop,
	.show =		pvr_proc_seq_show,
};

static IMG_INT pvr_proc_open(struct inode *inode, struct file *file)
{
	IMG_INT ret = seq_open(file, &pvr_proc_seq_operations);
	struct seq_file *seq;

	if (ret != 0)
		return ret;

	seq = (struct seq_file *)file->private_data;
	seq->private = pde_data(inode);

	return 0;
}

static ssize_t pvr_proc_write(struct file *file, const char __user *buffer,
		size_t count, loff_t *ppos)
{
	PVR_PROC_SEQ_HANDLERS *handlers = pde_data(file_inode(file));

	PVR_UNREFERENCED_PARAMETER(ppos);

	if (handlers == NULL || handlers->write == NULL)
		return -EIO;

	return handlers->write(file, buffer, count, handlers->data);
}

static const struct proc_ops pvr_proc_operations =
{
	.proc_open		= pvr_proc_open,
	.proc_read		= seq_read,
	.proc_write		= pvr_proc_write,
	.proc_lseek		= seq_lseek,
	.proc_release	= seq_release,
};

static struct proc_dir_entry* CreateProcEntryInDirSeq(
									   struct proc_dir_entry *pdir,
									   const IMG_CHAR * name,
									   IMG_VOID* data,
									   pvr_next_proc_seq_t next_handler,
									   pvr_show_proc_seq_t show_handler,
									   pvr_off2element_proc_seq_t off2element_handler,
									   pvr_startstop_proc_seq_t startstop_handler,
									   pvr_write_proc_t *whandler
									   )
{
	struct proc_dir_entry *file;
	PVR_PROC_SEQ_HANDLERS *seq_handlers;
	mode_t mode = S_IFREG | S_IRUGO;

	if (!dir)
	{
		PVR_DPF((PVR_DBG_ERROR, "CreateProcEntryInDirSeq: cannot make proc entry /proc/%s/%s: no parent", PVRProcDirRoot, name));
		return NULL;
	}

	if (whandler)
		mode |= S_IWUSR;

	seq_handlers = kzalloc(sizeof(PVR_PROC_SEQ_HANDLERS), GFP_KERNEL);
	if (!seq_handlers)
	{
		PVR_DPF((PVR_DBG_ERROR, "CreateProcEntryInDirSeq: cannot allocate handlers for /proc/%s/%s", PVRProcDirRoot, name));
		return NULL;
	}

	seq_handlers->next = next_handler;
	seq_handlers->show = show_handler;
	seq_handlers->off2element = off2element_handler;
	seq_handlers->startstop = startstop_handler;
	seq_handlers->write = whandler;
	seq_handlers->data = data;

	spin_lock(&g_sHandlerListLock);
	list_add(&seq_handlers->sListNode, &g_sHandlerList);
	spin_unlock(&g_sHandlerListLock);

	file = proc_create_data(name, mode, pdir, &pvr_proc_operations, seq_handlers);
	if (!file)
	{
		spin_lock(&g_sHandlerListLock);
		list_del(&seq_handlers->sListNode);
		spin_unlock(&g_sHandlerListLock);
		kfree(seq_handlers);
		PVR_DPF((PVR_DBG_ERROR, "CreateProcEntryInDirSeq: cannot create proc entry /proc/%s/%s", PVRProcDirRoot, name));
		return NULL;
	}

	PVR_DPF((PVR_DBG_MESSAGE, "Created /proc/%s/%s", PVRProcDirRoot, name));

	return file;
}

struct proc_dir_entry* CreateProcReadEntrySeq (
								const IMG_CHAR* name,
								IMG_VOID* data,
								pvr_next_proc_seq_t next_handler,
								pvr_show_proc_seq_t show_handler,
								pvr_off2element_proc_seq_t off2element_handler,
								pvr_startstop_proc_seq_t startstop_handler
							   )
{
	return CreateProcEntrySeq(name, data, next_handler, show_handler,
							  off2element_handler, startstop_handler, NULL);
}

struct proc_dir_entry* CreateProcEntrySeq (
								const IMG_CHAR* name,
								IMG_VOID* data,
								pvr_next_proc_seq_t next_handler,
								pvr_show_proc_seq_t show_handler,
								pvr_off2element_proc_seq_t off2element_handler,
								pvr_startstop_proc_seq_t startstop_handler,
								pvr_write_proc_t *whandler
							   )
{
	return CreateProcEntryInDirSeq(dir, name, data, next_handler,
								   show_handler, off2element_handler,
								   startstop_handler, whandler);
}

struct proc_dir_entry* CreatePerProcessProcEntrySeq (
								const IMG_CHAR* name,
								IMG_VOID* data,
								pvr_next_proc_seq_t next_handler,
								pvr_show_proc_seq_t show_handler,
								pvr_off2element_proc_seq_t off2element_handler,
								pvr_startstop_proc_seq_t startstop_handler,
								pvr_write_proc_t *whandler
							   )
{
	PVRSRV_ENV_PER_PROCESS_DATA *psPerProc;
	IMG_UINT32 ui32PID;

	if (!dir)
	{
		PVR_DPF((PVR_DBG_ERROR, "CreatePerProcessProcEntrySeq: /proc/%s doesn't exist", PVRProcDirRoot));
		return NULL;
	}

	ui32PID = OSGetCurrentProcessIDKM();

	psPerProc = PVRSRVPerProcessPrivateData(ui32PID);
	if (!psPerProc)
	{
		PVR_DPF((PVR_DBG_ERROR, "CreatePerProcessProcEntrySeq: no per process data"));
		return NULL;
	}

	if (!psPerProc->psProcDir)
	{
		IMG_CHAR dirname[16];
		IMG_INT ret;

		ret = OSSNPrintf(dirname, sizeof(dirname), "%u", ui32PID);

		if (ret <=0 || ret >= (IMG_INT)sizeof(dirname))
		{
			PVR_DPF((PVR_DBG_ERROR, "CreatePerProcessProcEntries: couldn't generate per process proc directory name \"%u\"", ui32PID));
			return NULL;
		}
		else
		{
			psPerProc->psProcDir = proc_mkdir(dirname, dir);
			if (!psPerProc->psProcDir)
			{
				PVR_DPF((PVR_DBG_ERROR, "CreatePerProcessProcEntries: couldn't create per process proc directory /proc/%s/%u", PVRProcDirRoot, ui32PID));
				return NULL;
			}
		}
	}

	return CreateProcEntryInDirSeq(psPerProc->psProcDir, name, data,
								   next_handler, show_handler,
								   off2element_handler, startstop_handler,
								   whandler);
}

/*
 * proc_remove() (3.10+) takes the entry itself and tears down any subtree,
 * which is why this no longer has to walk ->subdir by name the way the
 * original did -- ->name and ->subdir are both private to procfs now.
 */
IMG_VOID RemoveProcEntrySeq( struct proc_dir_entry* proc_entry )
{
	if (!dir || !proc_entry)
		return;

	proc_remove(proc_entry);
}

IMG_VOID RemovePerProcessProcEntrySeq(struct proc_dir_entry* proc_entry)
{
	if (!proc_entry)
		return;

	proc_remove(proc_entry);
}

IMG_VOID RemovePerProcessProcDir(PVRSRV_ENV_PER_PROCESS_DATA *psPerProc)
{
	if (psPerProc->psProcDir)
	{
		proc_remove(psPerProc->psProcDir);
		psPerProc->psProcDir = NULL;
	}
}

IMG_INT CreateProcEntries(IMG_VOID)
{
	dir = proc_mkdir (PVRProcDirRoot, NULL);

	if (!dir)
	{
		PVR_DPF((PVR_DBG_ERROR, "CreateProcEntries: cannot make /proc/%s directory", PVRProcDirRoot));
		return -ENOMEM;
	}

	g_pProcQueue    = CreateProcReadEntrySeq("queue", NULL, NULL, ProcSeqShowQueue, ProcSeqOff2ElementQueue, NULL);
	g_pProcVersion  = CreateProcReadEntrySeq("version", NULL, NULL, ProcSeqShowVersion, ProcSeq1ElementHeaderOff2Element, NULL);
	g_pProcSysNodes = CreateProcReadEntrySeq("nodes", NULL, NULL, ProcSeqShowSysNodes, ProcSeqOff2ElementSysNodes, NULL);

	if(!g_pProcQueue || !g_pProcVersion || !g_pProcSysNodes)
	{
		PVR_DPF((PVR_DBG_ERROR, "CreateProcEntries: couldn't make /proc/%s files", PVRProcDirRoot));
		return -ENOMEM;
	}

#ifdef DEBUG
	g_pProcDebugLevel = CreateProcEntrySeq("debug_level", NULL, NULL,
											ProcSeqShowDebugLevel,
											ProcSeq1ElementOff2Element, NULL,
											PVRDebugProcSetLevel);
	if(!g_pProcDebugLevel)
	{
		PVR_DPF((PVR_DBG_ERROR, "CreateProcEntries: couldn't make /proc/%s/debug_level", PVRProcDirRoot));
		return -ENOMEM;
	}

#ifdef PVR_MANUAL_POWER_CONTROL
	g_pProcPowerLevel = CreateProcEntrySeq("power_control", NULL, NULL,
											ProcSeqShowPowerLevel,
											ProcSeq1ElementOff2Element, NULL,
											PVRProcSetPowerLevel);
	if(!g_pProcPowerLevel)
	{
		PVR_DPF((PVR_DBG_ERROR, "CreateProcEntries: couldn't make /proc/%s/power_control", PVRProcDirRoot));
		return -ENOMEM;
	}
#endif
#endif

	return 0;
}

IMG_VOID RemoveProcEntries(IMG_VOID)
{
	if (!dir)
		return;

	/* Removes the whole /proc/pvr subtree, per-process directories and all. */
	proc_remove(dir);
	dir = NULL;

	FreeAllSeqHandlers();
}

static void ProcSeqShowVersion(struct seq_file *sfile,void* el)
{
	SYS_DATA *psSysData;
	IMG_CHAR *pszSystemVersionString = "None";

	if(el == PVR_PROC_SEQ_START_TOKEN)
	{
		seq_printf(sfile,
				"Version %s (%s) %s\n",
				PVRVERSION_STRING,
				PVR_BUILD_TYPE, PVR_BUILD_DIR);
		return;
	}

	psSysData = SysAcquireDataNoCheck();
	if(psSysData != IMG_NULL && psSysData->pszVersionString != IMG_NULL)
	{
		pszSystemVersionString = psSysData->pszVersionString;
	}

	seq_printf( sfile, "System Version String: %s\n", pszSystemVersionString);
}

static const IMG_CHAR *deviceTypeToString(PVRSRV_DEVICE_TYPE deviceType)
{
    switch (deviceType)
    {
        default:
        {
            static IMG_CHAR text[10];

            sprintf(text, "?%x", (IMG_UINT)deviceType);

            return text;
        }
    }
}


static const IMG_CHAR *deviceClassToString(PVRSRV_DEVICE_CLASS deviceClass)
{
    switch (deviceClass)
    {
	case PVRSRV_DEVICE_CLASS_3D:
	{
	    return "3D";
	}
	case PVRSRV_DEVICE_CLASS_DISPLAY:
	{
	    return "display";
	}
	case PVRSRV_DEVICE_CLASS_BUFFER:
	{
	    return "buffer";
	}
	default:
	{
	    static IMG_CHAR text[10];

	    sprintf(text, "?%x", (IMG_UINT)deviceClass);
	    return text;
	}
    }
}

static IMG_VOID* DecOffPsDev_AnyVaCb(PVRSRV_DEVICE_NODE *psNode, va_list va)
{
	off_t *pOff = va_arg(va, off_t*);
	if (--(*pOff))
	{
		return IMG_NULL;
	}
	else
	{
		return psNode;
	}
}

static void ProcSeqShowSysNodes(struct seq_file *sfile,void* el)
{
	PVRSRV_DEVICE_NODE *psDevNode;

	if(el == PVR_PROC_SEQ_START_TOKEN)
	{
		seq_printf( sfile,
						"Registered nodes\n"
						"Addr     Type     Class    Index Ref pvDev     Size Res\n");
		return;
	}

	psDevNode = (PVRSRV_DEVICE_NODE*)el;

	seq_printf( sfile,
			  "%p %-8s %-8s %4d  %2u  %p  %3u  %p\n",
			  psDevNode,
			  deviceTypeToString(psDevNode->sDevId.eDeviceType),
			  deviceClassToString(psDevNode->sDevId.eDeviceClass),
			  psDevNode->sDevId.eDeviceClass,
			  psDevNode->ui32RefCount,
			  psDevNode->pvDevice,
			  psDevNode->ui32pvDeviceSize,
			  psDevNode->hResManContext);
}

static void* ProcSeqOff2ElementSysNodes(struct seq_file * sfile, loff_t off)
{
    SYS_DATA *psSysData;
    PVRSRV_DEVICE_NODE*psDevNode = IMG_NULL;
    
    PVR_UNREFERENCED_PARAMETER(sfile);
    
    if(!off)
    {
	return PVR_PROC_SEQ_START_TOKEN;
    }

    psSysData = SysAcquireDataNoCheck();
    if (psSysData != IMG_NULL)
    {
	
	psDevNode = (PVRSRV_DEVICE_NODE*)
			List_PVRSRV_DEVICE_NODE_Any_va(psSysData->psDeviceNodeList,
													DecOffPsDev_AnyVaCb,
													&off);
    }

    
    return (void*)psDevNode;
}

