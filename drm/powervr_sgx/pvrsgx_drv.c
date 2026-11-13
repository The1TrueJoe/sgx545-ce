// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_prime.h>
#include <drm/drm_print.h>

#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <uapi/drm/powervr_sgx_drm.h>

#include "pvrsgx_debugfs.h"
#include "pvrsgx_device.h"
#include "pvrsgx_drv.h"
#include "pvrsgx_pm.h"
#include "pvrsgx_fw.h"
#include "pvrsgx_gem.h"
#include "pvrsgx_job.h"
#include "pvrsgx_prime.h"
#include "pvrsgx_render.h"
#include "pvrsgx_validate.h"
#include "pvrsgx_vm.h"

/* Elements are copied at min(stride, obj_size); a longer input tail must be zero. */
void *pvrsgx_obj_array_get(const struct drm_powervr_sgx_obj_array *in, u32 obj_size,
			   u32 max_count)
{
	u8 __user *src = u64_to_user_ptr(in->array);
	u8 *out;

	if (!in->count)
		return NULL;
	if (!in->stride || in->count > max_count)
		return ERR_PTR(-EINVAL);

	out = kvcalloc(in->count, obj_size, GFP_KERNEL);
	if (!out)
		return ERR_PTR(-ENOMEM);
	for (u32 i = 0; i < in->count; i++) {
		int err = copy_struct_from_user(out + (size_t)i * obj_size, obj_size,
						src + (size_t)i * in->stride, in->stride);

		if (err) {
			kvfree(out);
			/* -E2BIG: a larger struct this kernel doesn't know. */
			return ERR_PTR(err);
		}
	}
	return out;
}

int pvrsgx_obj_array_put(const struct drm_powervr_sgx_obj_array *out, const void *src,
			 u32 obj_size, u32 count)
{
	u8 __user *dst = u64_to_user_ptr(out->array);
	u32 n = min(out->count, count), len = min(out->stride, obj_size);

	if (!out->stride)
		return -EINVAL;
	for (u32 i = 0; i < n; i++) {
		if (copy_to_user(dst + (size_t)i * out->stride,
				 (const u8 *)src + (size_t)i * obj_size, len))
			return -EFAULT;
		if (out->stride > len &&
		    clear_user(dst + (size_t)i * out->stride + len, out->stride - len))
			return -EFAULT;
	}
	return 0;
}

/* Copy a fixed-size query result: min of both sizes out, the kernel's size back. */
static int query_put(void __user *ptr, u32 *size, const void *val, u32 len)
{
	if (ptr && copy_to_user(ptr, val, min(*size, len)))
		return -EFAULT;
	*size = ptr ? min(*size, len) : len;
	return 0;
}

static int pvrsgx_ioctl_dev_query(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_powervr_sgx_ioctl_dev_query_args *args = data;
	struct pvrsgx_device *sdev = to_pvrsgx_device(drm);
	void __user *ptr = u64_to_user_ptr(args->pointer);

	switch (args->type) {
	case DRM_POWERVR_SGX_DEV_QUERY_GPU_INFO_GET: {
		struct drm_powervr_sgx_dev_query_gpu_info info = {
			.core_id = sdev->core_id,
			.core_revision = sdev->core_rev,
			.fw_interface_version = sdev->fw.interface,
		};

		return query_put(ptr, &args->size, &info, sizeof(info));
	}
	case DRM_POWERVR_SGX_DEV_QUERY_LIMITS_GET: {
		/*
		 * Render-target limits are not documented: these are the sizes
		 * known to work, to be raised when larger targets are tested.
		 */
		struct drm_powervr_sgx_dev_query_limits lim = {
			.rt_max_width = 2048,
			.rt_max_height = 2048,
			.rt_max_samples = sdev->family->cmds->max_samples,
			.pb_min_pages = 0x100,
			.pb_default_pages = pvrsgx_pb_default_pages(sdev),
			.pb_max_pages = pvrsgx_heaps(sdev)->pb_max_pages,
			.max_ta_kicks = PVRSGX_MAX_TA_KICKS,
			.va_bits = sdev->family->va_bits,
			.caps = DRM_POWERVR_SGX_CAP_BACKWARD_BLITS,
			.max_layers = 1,
		};

		return query_put(ptr, &args->size, &lim, sizeof(lim));
	}
	case DRM_POWERVR_SGX_DEV_QUERY_QUIRKS_GET: {
		struct drm_powervr_sgx_dev_query_quirks q = {};
		int err;

		if (ptr) {
			err = copy_struct_from_user(&q, sizeof(q), ptr, args->size);
			if (err)
				return err;
			if (q._padding_c)
				return -EINVAL;
		}
		/* No erratum that reaches userspace is known for these cores. */
		q.count = 0;
		q.musthave_count = 0;
		return query_put(ptr, &args->size, &q, sizeof(q));
	}
	case DRM_POWERVR_SGX_DEV_QUERY_HEAP_INFO_GET:
		return pvrsgx_query_heap_info(sdev, ptr, &args->size);
	case DRM_POWERVR_SGX_DEV_QUERY_COMMANDS_GET: {
		const struct pvrsgx_cmd_def *render = pvrsgx_cmd_def(sdev, PVRSGX_CMD_RENDER);
		const struct pvrsgx_cmd_def *kick = pvrsgx_cmd_def(sdev, PVRSGX_CMD_TA_KICK);
		const struct pvrsgx_cmd_def *xfer = pvrsgx_cmd_def(sdev, PVRSGX_CMD_TRANSFER);
		struct drm_powervr_sgx_dev_query_commands c = {
			.job_types = BIT(DRM_POWERVR_SGX_JOB_TYPE_NULL),
		};

		/* A render needs its kicks too; an unsupported command is refused. */
		if (render && render->supported && kick && kick->supported)
			c.job_types |= BIT(DRM_POWERVR_SGX_JOB_TYPE_RENDER);
		if (xfer && xfer->supported)
			c.job_types |= BIT(DRM_POWERVR_SGX_JOB_TYPE_TRANSFER);
		c.render_exts = render ? pvrsgx_cmd_ext_mask(render) : 0;
		c.ta_kick_exts = kick ? pvrsgx_cmd_ext_mask(kick) : 0;
		c.transfer_exts = xfer ? pvrsgx_cmd_ext_mask(xfer) : 0;
		return query_put(ptr, &args->size, &c, sizeof(c));
	}
	default:
		return -EINVAL;
	}
}

static int pvrsgx_open(struct drm_device *drm, struct drm_file *file)
{
	struct pvrsgx_file *sfile = kzalloc_obj(*sfile);

	if (!sfile)
		return -ENOMEM;
	sfile->usage = pvrsgx_usage_new();
	if (!sfile->usage) {
		kfree(sfile);
		return -ENOMEM;
	}
	sfile->sdev = to_pvrsgx_device(drm);
	xa_init_flags(&sfile->vms, XA_FLAGS_ALLOC1);
	xa_init_flags(&sfile->contexts, XA_FLAGS_ALLOC1);
	xa_init_flags(&sfile->rtds, XA_FLAGS_ALLOC1);
	init_rwsem(&sfile->submit_lock);
	file->driver_priv = sfile;
	return 0;
}

static void pvrsgx_postclose(struct drm_device *drm, struct drm_file *file)
{
	struct pvrsgx_file *sfile = to_pvrsgx_file(file);

	pvrsgx_rtds_destroy_all(sfile);
	pvrsgx_context_destroy_all(sfile);
	pvrsgx_vm_destroy_all(sfile);
	xa_destroy(&sfile->rtds);
	xa_destroy(&sfile->contexts);
	xa_destroy(&sfile->vms);
	pvrsgx_usage_put(sfile->usage);
	kfree(sfile);
	file->driver_priv = NULL;
}

#define IOCTL(n, f) DRM_IOCTL_DEF_DRV(POWERVR_SGX_##n, f, DRM_RENDER_ALLOW)

static const struct drm_ioctl_desc pvrsgx_ioctls[] = {
	IOCTL(DEV_QUERY, pvrsgx_ioctl_dev_query),
	IOCTL(CREATE_BO, pvrsgx_ioctl_create_bo),
	IOCTL(GET_BO_MMAP_OFFSET, pvrsgx_ioctl_get_bo_mmap_offset),
	IOCTL(CREATE_VM_CONTEXT, pvrsgx_ioctl_create_vm_context),
	IOCTL(DESTROY_VM_CONTEXT, pvrsgx_ioctl_destroy_vm_context),
	IOCTL(VM_BIND, pvrsgx_ioctl_vm_bind),
	IOCTL(CREATE_CONTEXT, pvrsgx_ioctl_create_context),
	IOCTL(DESTROY_CONTEXT, pvrsgx_ioctl_destroy_context),
	IOCTL(GET_CONTEXT_STATE, pvrsgx_ioctl_get_context_state),
	IOCTL(CREATE_RT_DATASET, pvrsgx_ioctl_create_rt_dataset),
	IOCTL(DESTROY_RT_DATASET, pvrsgx_ioctl_destroy_rt_dataset),
	IOCTL(SUBMIT_JOBS, pvrsgx_ioctl_submit_jobs),
	IOCTL(UPLOAD_CODE, pvrsgx_ioctl_upload_code),
};

/* drm-usage-stats.rst: the file's render time and its objects' memory. */
static void pvrsgx_show_fdinfo(struct drm_printer *p, struct drm_file *file)
{
	struct pvrsgx_file *sfile = to_pvrsgx_file(file);

	drm_printf(p, "drm-engine-render:\t%llu ns\n",
		   (u64)atomic64_read(&sfile->usage->render_ns));
	drm_show_memory_stats(p, file);
}

static const struct file_operations pvrsgx_fops = {
	.owner = THIS_MODULE,
	DRM_GEM_FOPS,
	.show_fdinfo = drm_show_fdinfo,
};

static const struct drm_driver pvrsgx_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER | DRIVER_SYNCOBJ | DRIVER_SYNCOBJ_TIMELINE,
	.open = pvrsgx_open,
	.postclose = pvrsgx_postclose,
	.ioctls = pvrsgx_ioctls,
	.num_ioctls = ARRAY_SIZE(pvrsgx_ioctls),
	.fops = &pvrsgx_fops,
	.show_fdinfo = pvrsgx_show_fdinfo,
#if defined(CONFIG_DEBUG_FS)
	.debugfs_init = pvrsgx_debugfs_init,
#endif
	.gem_create_object = pvrsgx_gem_create_object,
	.gem_prime_import_sg_table = pvrsgx_prime_import_sg_table,

	.name = PVRSGX_DRIVER_NAME,
	.desc = PVRSGX_DRIVER_DESC,
	.major = PVRSGX_DRIVER_MAJOR,
	.minor = PVRSGX_DRIVER_MINOR,
};

struct pvrsgx_device *pvrsgx_alloc(struct device *dev)
{
	return devm_drm_dev_alloc(dev, &pvrsgx_drm_driver, struct pvrsgx_device, base);
}

/*
 * Stop the GPU as it is powered, then undo runtime PM: a suspended GPU is
 * stopped already, and undoing runtime PM first could let a parent (ti-sysc)
 * gate its clocks while the stop still writes registers.
 */
static void hw_stop(struct pvrsgx_device *sdev)
{
	struct device *dev = sdev->base.dev;
	int active = pm_runtime_get_if_active(dev);

	pvrsgx_device_unplug(sdev);
	if (active > 0)
		pm_runtime_put_noidle(dev);
	pvrsgx_power_fini(sdev);
}

/* What both buses do once the registers are mapped and the interrupt source known. */
int pvrsgx_probe_common(struct pvrsgx_device *sdev)
{
	int err;

	err = pvrsgx_device_init(sdev);
	if (err)
		return err;

	/* Runtime PM is set up before userspace can reach the device. */
	pvrsgx_power_init(sdev);
	err = drm_dev_register(&sdev->base, 0);
	if (err)
		hw_stop(sdev);
	return err;
}

void pvrsgx_remove_common(struct pvrsgx_device *sdev)
{
	/* No new ioctl, and the running ones done, before the hardware goes. */
	drm_dev_unplug(&sdev->base);
	hw_stop(sdev);
}

/*
 * Reboot or kexec: leave the GPU stopped, with no reset to start it again, for
 * whatever runs next. A suspended GPU is stopped already.
 */
void pvrsgx_shutdown_common(struct pvrsgx_device *sdev)
{
	int active;

	if (!sdev)
		return;
	active = pm_runtime_get_if_active(sdev->base.dev);
	pvrsgx_job_unplug(sdev);
	if (active > 0)
		pm_runtime_put_noidle(sdev->base.dev);
}

static int pvrsgx_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct pvrsgx_device *sdev;
	int err;

	err = pcim_enable_device(pdev);
	if (err)
		return err;
	pci_set_master(pdev);
	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (err)
		return err;
	/* The GPU's MMU maps page by page: any segment length will do. */
	dma_set_max_seg_size(&pdev->dev, UINT_MAX);

	sdev = pvrsgx_alloc(&pdev->dev);
	if (IS_ERR(sdev))
		return PTR_ERR(sdev);
	sdev->pdev = pdev;
	sdev->family = pvrsgx_core_family_get(id->driver_data);
	sdev->irq = pdev->irq;
	pci_set_drvdata(pdev, sdev);

	if (pci_resource_len(pdev, 0) < PVRSGX_REGS_MIN)
		return -ENODEV;
	sdev->regs = pcim_iomap_region(pdev, 0, PVRSGX_DRIVER_NAME);
	if (IS_ERR(sdev->regs))
		return PTR_ERR(sdev->regs);

	return pvrsgx_probe_common(sdev);
}

static void pvrsgx_remove(struct pci_dev *pdev)
{
	pvrsgx_remove_common(pci_get_drvdata(pdev));
}

static void pvrsgx_shutdown(struct pci_dev *pdev)
{
	pvrsgx_shutdown_common(pci_get_drvdata(pdev));
}

static const struct pci_device_id pvrsgx_pci_ids[] = {
	{ PCI_VDEVICE(INTEL, 0x089b), 545 },	/* CE5300 family: SGX545 */
	{ }
};
MODULE_DEVICE_TABLE(pci, pvrsgx_pci_ids);

static struct pci_driver pvrsgx_pci_driver = {
	.name = PVRSGX_DRIVER_NAME,
	.id_table = pvrsgx_pci_ids,
	.probe = pvrsgx_probe,
	.remove = pvrsgx_remove,
	.shutdown = pvrsgx_shutdown,
	.driver.pm = pm_ptr(&pvrsgx_pm_ops),
};

struct workqueue_struct *pvrsgx_put_wq;

static int __init pvrsgx_init(void)
{
	int err;

	pvrsgx_put_wq = alloc_workqueue("powervr_sgx_put", WQ_UNBOUND, 0);
	if (!pvrsgx_put_wq)
		return -ENOMEM;
	err = pci_register_driver(&pvrsgx_pci_driver);
	if (err)
		goto err_wq;
	err = pvrsgx_aux_register();
	if (err)
		goto err_pci;
	err = pvrsgx_of_register();
	if (err)
		goto err_aux;
	return 0;

err_aux:
	pvrsgx_aux_unregister();
err_pci:
	pci_unregister_driver(&pvrsgx_pci_driver);
err_wq:
	destroy_workqueue(pvrsgx_put_wq);
	return err;
}
module_init(pvrsgx_init);

static void __exit pvrsgx_exit(void)
{
	pvrsgx_of_unregister();
	pvrsgx_aux_unregister();
	pci_unregister_driver(&pvrsgx_pci_driver);
	/* After every device: drains the puts that release them. */
	destroy_workqueue(pvrsgx_put_wq);
}
module_exit(pvrsgx_exit);

MODULE_AUTHOR("Joseph Telaak <joseph@telaak.dev>");
MODULE_DESCRIPTION("PowerVR SGX5xx DRM driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("DMA_BUF");
