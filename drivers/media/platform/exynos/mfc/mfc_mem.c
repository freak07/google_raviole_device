/*
 * drivers/media/platform/exynos/mfc/mfc_mem.c
 *
 * Copyright (c) 2016 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/property.h>
#include <linux/ion.h>
#include <linux/dma-buf.h>

#if IS_ENABLED(CONFIG_EXYNOS_CONTENT_PATH_PROTECTION)
#include <../../../../staging/android/ion/exynos/ion_exynos_prot.h>
#endif

#include "mfc_mem.h"

struct vb2_mem_ops *mfc_mem_ops(void)
{
	return (struct vb2_mem_ops *)&vb2_dma_sg_memops;
}

int mfc_mem_get_user_shared_handle(struct mfc_ctx *ctx,
	struct mfc_user_shared_handle *handle)
{
	int ret = 0;

	handle->dma_buf = dma_buf_get(handle->fd);
	if (IS_ERR(handle->dma_buf)) {
		mfc_ctx_err("Failed to import fd\n");
		ret = PTR_ERR(handle->dma_buf);
		goto import_dma_fail;
	}

	handle->vaddr = dma_buf_vmap(handle->dma_buf);
	if (handle->vaddr == NULL) {
		mfc_ctx_err("Failed to get kernel virtual address\n");
		ret = -EINVAL;
		goto map_kernel_fail;
	}

	return 0;

map_kernel_fail:
	handle->vaddr = NULL;
	dma_buf_put(handle->dma_buf);

import_dma_fail:
	handle->dma_buf = NULL;
	handle->fd = -1;
	return ret;
}

void mfc_mem_cleanup_user_shared_handle(struct mfc_ctx *ctx,
		struct mfc_user_shared_handle *handle)
{
	if (handle->vaddr)
		dma_buf_vunmap(handle->dma_buf, handle->vaddr);
	if (handle->dma_buf)
		dma_buf_put(handle->dma_buf);

	handle->dma_buf = NULL;
	handle->vaddr = NULL;
	handle->fd = -1;
}

#if IS_ENABLED(CONFIG_EXYNOS_CONTENT_PATH_PROTECTION)
static unsigned int __mfc_mem_ion_get_heapmask_by_name(struct mfc_dev *dev,
		const char *heap_name)
{
	struct ion_heap_data data[ION_NUM_MAX_HEAPS];
	int i, cnt = ion_query_heaps_kernel(NULL, 0);

	ion_query_heaps_kernel((struct ion_heap_data *)data, cnt);

	for (i = 0; i < cnt; i++) {
		if (!strncmp(data[i].name, heap_name, MAX_HEAP_NAME))
			break;
	}

	if (i == cnt) {
		mfc_dev_err("heap %s is not found\n", heap_name);
		return 0;
	}

	return 1 << data[i].heap_id;
}
#endif

int mfc_mem_ion_alloc(struct mfc_dev *dev,
		struct mfc_special_buf *special_buf)
{
	unsigned int heapmask;
	int flag = 0;
	const char *heapname;

	switch (special_buf->buftype) {
	case MFCBUF_NORMAL:
		heapname = "ion_system_heap";
		heapmask = ION_HEAP_SYSTEM;
		break;
	case MFCBUF_NORMAL_FW:
		heapname = "ion_system_heap";
		heapmask = ION_HEAP_SYSTEM;
		break;
#if IS_ENABLED(CONFIG_EXYNOS_CONTENT_PATH_PROTECTION)
	case MFCBUF_DRM:
		heapname = "vframe_heap";
		heapmask = __mfc_mem_ion_get_heapmask_by_name(dev, heapname);
		if (!heapmask)
			return -EINVAL;
		flag |= ION_EXYNOS_FLAG_PROTECTED;
		break;
	case MFCBUF_DRM_FW:
		heapname = "vfw_heap";
		heapmask = __mfc_mem_ion_get_heapmask_by_name(dev, heapname);
		if (!heapmask)
			return -EINVAL;
		flag |= ION_EXYNOS_FLAG_PROTECTED;
		break;
#endif
	default:
		heapname = "unknown";
		heapmask = ION_HEAP_SYSTEM;
		mfc_dev_err("not supported mfc mem type: %d, heapname: %s\n",
				special_buf->buftype, heapname);
		return -EINVAL;
	}
	special_buf->dma_buf = ion_alloc(special_buf->size, heapmask, flag);
	if (IS_ERR(special_buf->dma_buf)) {
		mfc_dev_err("Failed to allocate buffer (err %ld)\n",
				PTR_ERR(special_buf->dma_buf));
		goto err_ion_alloc;
	}

	special_buf->attachment = dma_buf_attach(special_buf->dma_buf, dev->device);
	if (IS_ERR(special_buf->attachment)) {
		mfc_dev_err("Failed to get dma_buf_attach (err %ld)\n",
				PTR_ERR(special_buf->attachment));
		goto err_attach;
	}

	special_buf->sgt = dma_buf_map_attachment(special_buf->attachment,
			DMA_BIDIRECTIONAL);
	if (IS_ERR(special_buf->sgt)) {
		mfc_dev_err("Failed to get sgt (err %ld)\n",
				PTR_ERR(special_buf->sgt));
		goto err_map;
	}

	special_buf->daddr = sg_dma_address(special_buf->sgt->sgl);
	if (IS_ERR_VALUE(special_buf->daddr)) {
		mfc_dev_err("Failed to get iova (err 0x%p)\n",
				&special_buf->daddr);
		goto err_daddr;
	}

	special_buf->vaddr = dma_buf_vmap(special_buf->dma_buf);
	if (IS_ERR(special_buf->vaddr)) {
		mfc_dev_err("Failed to get vaddr (err 0x%p)\n",
				&special_buf->vaddr);
		goto err_vaddr;
	}

	return 0;
err_vaddr:
	special_buf->vaddr = NULL;
err_daddr:
	special_buf->daddr = 0;
	dma_buf_unmap_attachment(special_buf->attachment, special_buf->sgt,
				 DMA_BIDIRECTIONAL);
err_map:
	special_buf->sgt = NULL;
	dma_buf_detach(special_buf->dma_buf, special_buf->attachment);
err_attach:
	special_buf->attachment = NULL;
	dma_buf_put(special_buf->dma_buf);
err_ion_alloc:
	special_buf->dma_buf = NULL;
	return -ENOMEM;
}

void mfc_mem_ion_free(struct mfc_special_buf *special_buf)
{
	if (special_buf->vaddr)
		dma_buf_vunmap(special_buf->dma_buf, special_buf->vaddr);
	if (special_buf->sgt)
		dma_buf_unmap_attachment(special_buf->attachment,
					 special_buf->sgt, DMA_BIDIRECTIONAL);
	if (special_buf->attachment)
		dma_buf_detach(special_buf->dma_buf, special_buf->attachment);
	if (special_buf->dma_buf)
		dma_buf_put(special_buf->dma_buf);

	special_buf->dma_buf = NULL;
	special_buf->attachment = NULL;
	special_buf->sgt = NULL;
	special_buf->daddr = 0;
	special_buf->vaddr = NULL;
}

void mfc_bufcon_put_daddr(struct mfc_ctx *ctx, struct mfc_buf *mfc_buf, int plane)
{
	int i;

	for (i = 0; i < mfc_buf->num_valid_bufs; i++) {
		if (mfc_buf->addr[i][plane]) {
			mfc_debug(4, "[BUFCON] put batch buf addr[%d][%d]: 0x%08llx\n",
					i, plane, mfc_buf->addr[i][plane]);
		}
		if (mfc_buf->attachments[i][plane])
			dma_buf_detach(mfc_buf->dmabufs[i][plane], mfc_buf->attachments[i][plane]);
		if (mfc_buf->dmabufs[i][plane])
			dma_buf_put(mfc_buf->dmabufs[i][plane]);

		mfc_buf->addr[i][plane] = 0;
		mfc_buf->attachments[i][plane] = NULL;
		mfc_buf->dmabufs[i][plane] = NULL;
	}
}

#if IS_ENABLED(CONFIG_MFC_USE_DMABUF_CONTAINER)
int mfc_bufcon_get_daddr(struct mfc_ctx *ctx, struct mfc_buf *mfc_buf,
					struct dma_buf *bufcon_dmabuf, int plane)
{
	struct mfc_dev *dev = ctx->dev;
	struct mfc_raw_info *raw = &ctx->raw_buf;
	int i, j = 0;
	u32 mask;

	if (dmabuf_container_get_mask(bufcon_dmabuf, &mask)) {
		mfc_ctx_err("[BUFCON] it is not buffer container\n");
		return -1;
	}

	if (mask == 0) {
		mfc_ctx_err("[BUFCON] number of valid buffers is zero\n");
		return -1;
	}

	mfc_debug(3, "[BUFCON] bufcon mask info %#x\n", mask);

	for (i = 0; i < mfc_buf->num_bufs_in_batch; i++) {
		if ((mask & (1 << i)) == 0) {
			mfc_debug(4, "[BUFCON] unmasked buf[%d]\n", i);
			continue;
		}

		mfc_buf->dmabufs[j][plane] = dmabuf_container_get_buffer(bufcon_dmabuf, i);
		if (IS_ERR(mfc_buf->dmabufs[i][plane])) {
			mfc_ctx_err("[BUFCON] Failed to get dma_buf (err %ld)",
					PTR_ERR(mfc_buf->dmabufs[i][plane]));
			call_dop(dev, dump_and_stop_debug_mode, dev);
			goto err_get_daddr;
		}

		mfc_buf->attachments[j][plane] = dma_buf_attach(mfc_buf->dmabufs[i][plane], dev->device);
		if (IS_ERR(mfc_buf->attachments[i][plane])) {
			mfc_ctx_err("[BUFCON] Failed to get dma_buf_attach (err %ld)",
					PTR_ERR(mfc_buf->attachments[i][plane]));
			call_dop(dev, dump_and_stop_debug_mode, dev);
			goto err_get_daddr;
		}

		mfc_buf->addr[j][plane] = ion_iovmm_map(mfc_buf->attachments[i][plane], 0,
				raw->plane_size[plane], DMA_BIDIRECTIONAL, 0);
		if (IS_ERR_VALUE(mfc_buf->addr[i][plane])) {
			mfc_ctx_err("[BUFCON] Failed to allocate iova (err %pa)",
					&mfc_buf->addr[i][plane]);
			call_dop(dev, dump_and_stop_debug_mode, dev);
			goto err_get_daddr;
		}

		mfc_debug(4, "[BUFCON] get batch buf addr[%d][%d]: 0x%08llx, size: %d\n",
				j, plane, mfc_buf->addr[j][plane], raw->plane_size[plane]);
		j++;
	}

	mfc_buf->num_valid_bufs = j;
	mfc_debug(3, "[BUFCON] batch buffer has %d buffers\n", mfc_buf->num_valid_bufs);

	return 0;

err_get_daddr:
	mfc_bufcon_put_daddr(ctx, mfc_buf, plane);
	return -1;
}
#endif

void mfc_put_iovmm(struct mfc_ctx *ctx, struct dpb_table *dpb,
			int num_planes, int index)
{
	struct mfc_dev *dev = ctx->dev;
	int i;

	MFC_TRACE_CTX("DPB[%d] fd: %d addr: %#llx put(%d)\n",
			index, dpb[index].fd[0], dpb[index].addr[0],
			dpb[index].mapcnt);

	for (i = 0; i < num_planes; i++) {
		if (dpb[index].addr[i])
			mfc_debug(2, "[IOVMM] index %d buf[%d] fd: %d addr: %#llx\n",
					index, i,
					dpb[index].fd[i],
					dpb[index].addr[i]);
		if (dpb[index].sgt[i])
			dma_buf_unmap_attachment(dpb[index].attach[i], dpb[index].sgt[i],
				DMA_BIDIRECTIONAL);
		if (dpb[index].attach[i])
			dma_buf_detach(dpb[index].dmabufs[i],
					dpb[index].attach[i]);
		if (dpb[index].dmabufs[i])
			dma_buf_put(dpb[index].dmabufs[i]);

		dpb[index].fd[i] = -1;
		dpb[index].addr[i] = 0;
		dpb[index].attach[i] = NULL;
		dpb[index].dmabufs[i] = NULL;
	}

	dpb[index].mapcnt--;
	mfc_debug(2, "[IOVMM] index %d mapcnt %d\n", index, dpb[index].mapcnt);

	if (dpb[index].mapcnt != 0) {
		mfc_ctx_err("[IOVMM] DPB[%d] %#llx invalid mapcnt %d\n",
				index, dpb[index].addr[0], dpb[index].mapcnt);
		call_dop(dev, dump_and_stop_debug_mode, dev);
	}
}

void mfc_get_iovmm(struct mfc_ctx *ctx, struct vb2_buffer *vb,
			struct dpb_table *dpb)
{
	struct mfc_dev *dev = ctx->dev;
	int i, mem_get_count = 0;
	struct mfc_buf *mfc_buf = vb_to_mfc_buf(vb);
	int index = mfc_buf->dpb_index;

	if (dpb[index].mapcnt != 0) {
		mfc_ctx_err("[IOVMM] DPB[%d] %#llx invalid mapcnt %d\n",
				index, dpb[index].addr[0], dpb[index].mapcnt);
		call_dop(dev, dump_and_stop_debug_mode, dev);
	}

	for (i = 0; i < ctx->dst_fmt->mem_planes; i++) {
		mem_get_count++;

		dpb[index].fd[i] = vb->planes[i].m.fd;

		dpb[index].dmabufs[i] = dma_buf_get(vb->planes[i].m.fd);
		if (IS_ERR(dpb[index].dmabufs[i])) {
			mfc_ctx_err("[IOVMM] Failed to dma_buf_get (err %ld)\n",
					PTR_ERR(dpb[index].dmabufs[i]));
			dpb[index].dmabufs[i] = NULL;
			goto err_iovmm;
		}

		dpb[index].attach[i] =
			dma_buf_attach(dpb[index].dmabufs[i], dev->device);
		if (IS_ERR(dpb[index].attach[i])) {
			mfc_ctx_err("[IOVMM] Failed dma_buf_attach (err %ld)\n",
					PTR_ERR(dpb[index].attach[i]));
			dpb[index].attach[i] = NULL;
			goto err_iovmm;
		}

		dpb[index].sgt[i] = dma_buf_map_attachment(dpb[index].attach[i],
				DMA_BIDIRECTIONAL);
		if (IS_ERR(dpb[index].sgt[i])) {
			mfc_ctx_err("[IOVMM] Failed to get sgt (err %ld)\n",
					PTR_ERR(dpb[index].sgt[i]));
			dpb[index].sgt[i] = NULL;
			goto err_iovmm;
		}

		dpb[index].addr[i] = sg_dma_address(dpb[index].sgt[i]->sgl);
		if (IS_ERR_VALUE(dpb[index].addr[i])) {
			mfc_ctx_err("[IOVMM] Failed to get iova (err 0x%p)\n",
					&dpb[index].addr[i]);
			dpb[index].addr[i] = 0;
			goto err_iovmm;
		}

		mfc_debug(2, "[IOVMM] index %d buf[%d] fd: %d addr: %#llx\n",
				index, i, dpb[index].fd[i], dpb[index].addr[i]);
	}

	dpb[index].paddr = page_to_phys(sg_page(dpb[index].sgt[0]->sgl));
	mfc_debug(2, "[DPB] dpb index [%d][%d] paddr %#llx daddr %#llx\n",
			mfc_buf->vb.vb2_buf.index,
			index, dpb[index].paddr, dpb[index].addr[0]);

	dpb[index].mapcnt++;
	mfc_debug(2, "[IOVMM] index %d mapcnt %d\n", index, dpb[index].mapcnt);
	MFC_TRACE_CTX("DPB[%d] fd: %d addr: %#llx get(%d)\n",
			index, dpb[index].fd[0], dpb[index].addr[0],
			dpb[index].mapcnt);

	return;

err_iovmm:
	dpb[index].mapcnt++;
	mfc_put_iovmm(ctx, dpb, mem_get_count, index);
}

void mfc_clear_iovmm(struct mfc_ctx *ctx, struct dpb_table *dpb,
			int num_planes, int index)
{
	int i;

	for (i = 0; i < num_planes; i++) {
		dpb[index].fd[i] = -1;
		dpb[index].addr[i] = 0;
		dpb[index].attach[i] = NULL;
		dpb[index].dmabufs[i] = NULL;
	}

	dpb[index].mapcnt--;
	dpb[index].queued = 0;
	mfc_debug(2, "[IOVMM] index %d mapcnt %d\n", index, dpb[index].mapcnt);
}

void mfc_cleanup_iovmm(struct mfc_ctx *ctx)
{
	struct mfc_dev *dev = ctx->dev;
	struct mfc_dec *dec = ctx->dec_priv;
	int i;

	mutex_lock(&dec->dpb_mutex);

	for (i = 0; i < MFC_MAX_DPBS; i++) {
		if (dec->dpb[i].mapcnt == 0) {
			continue;
		} else if (dec->dpb[i].mapcnt == 1) {
			mfc_put_iovmm(ctx, dec->dpb,
					ctx->dst_fmt->mem_planes, i);
		} else {
			mfc_ctx_err("DPB[%d] %#llx invalid mapcnt %d\n",
					i, dec->dpb[i].addr[0],
					dec->dpb[i].mapcnt);
			MFC_TRACE_CTX("DPB[%d] %#llx invalid mapcnt %d\n",
					i, dec->dpb[i].addr[0],
					dec->dpb[i].mapcnt);
			call_dop(dev, dump_and_stop_debug_mode, dev);
		}
	}

	mutex_unlock(&dec->dpb_mutex);
}

void mfc_cleanup_iovmm_except_used(struct mfc_ctx *ctx)
{
	struct mfc_dec *dec = ctx->dec_priv;
	int i;

	mutex_lock(&dec->dpb_mutex);

	for (i = 0; i < MFC_MAX_DPBS; i++) {
		if (dec->dpb[i].mapcnt == 0 || dec->dynamic_used & (1UL << i)) {
			continue;
		} else if (dec->dpb[i].mapcnt == 1) {
			dec->dpb_table_used &= ~(1UL << i);
			mfc_put_iovmm(ctx, dec->dpb,
					ctx->dst_fmt->mem_planes, i);
		} else {
			mfc_ctx_err("[IOVMM] DPB[%d] %#llx invalid mapcnt %d\n",
					i, dec->dpb[i].addr[0],
					dec->dpb[i].mapcnt);
			MFC_TRACE_CTX("DPB[%d] %#llx invalid mapcnt %d\n",
					i, dec->dpb[i].addr[0],
					dec->dpb[i].mapcnt);
		}
	}

	mutex_unlock(&dec->dpb_mutex);
}