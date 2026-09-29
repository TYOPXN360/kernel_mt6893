// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek mtk_mm DMA-BUF heap
 *
 * The userspace gralloc in this vendor is built against the Android 12+
 * dma-heap interface and asks the kernel for heaps named "mtk_mm" and
 * "mtk_mm-uncached".  The MediaTek composer, on the other hand, is an
 * Android 16 era blob that can only consume buffers whose dma_buf_ops
 * belong to ION - it calls ion_import_dma_buf(), which rejects any other
 * exporter outright:
 *
 *	ion_import_dma_buf: can not import dmabuf from another exporter
 *
 * Every rejected import leaves the composer on an error path that leaks the
 * file descriptors it duplicated for the layer, roughly 85 descriptors per
 * second while scrolling.  Once the process hits its 32768 descriptor limit
 * it can no longer duplicate buffers or obtain release fences, and the
 * display falls apart into flicker, horizontal banding and eventually a
 * black screen.
 *
 * Allocating the backing store through ION and exporting it here keeps both
 * sides consistent: gralloc finds the heap it expects, and the composer
 * gets buffers it is able to import.
 *
 * Copyright (C) 2026
 */

#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/err.h>
#include <linux/module.h>

#include "../../staging/android/ion/mtk/mtk_ion.h"
#include "../../staging/android/uapi/ion.h"

/* Declared in drivers/staging/android/ion/ion.c */
struct dma_buf *ion_alloc_export(size_t len, unsigned long flags,
				 unsigned int heap_id_mask);

struct mtk_mm_heap {
	unsigned int heap_id_mask;
	unsigned long ion_flags;
};

static struct mtk_mm_heap mtk_mm_heap_data = {
	.heap_id_mask = ION_HEAP_MULTIMEDIA_MASK,
	.ion_flags = ION_FLAG_CACHED,
};

static struct mtk_mm_heap mtk_mm_uncached_heap_data = {
	.heap_id_mask = ION_HEAP_MULTIMEDIA_MASK,
	/* ION_FLAG_CACHED is a request, so leaving it out asks for uncached. */
	.ion_flags = 0,
};

static struct dma_buf *mtk_mm_heap_allocate(struct dma_heap *heap,
					    unsigned long len,
					    unsigned long fd_flags,
					    unsigned long heap_flags)
{
	struct mtk_mm_heap *h = heap->priv;

	/* The framework already validates fd_flags/heap_flags for us. */
	return ion_alloc_export(len, h->ion_flags, h->heap_id_mask);
}

static struct dma_heap_ops mtk_mm_heap_ops = {
	.allocate = mtk_mm_heap_allocate,
};

static int __init mtk_mm_heap_create(void)
{
	struct dma_heap_export_info exp_info;
	struct dma_heap *heap;

	exp_info.ops = &mtk_mm_heap_ops;
	exp_info.priv = &mtk_mm_heap_data;
	exp_info.name = "mtk_mm";
	heap = dma_heap_add(&exp_info);
	if (IS_ERR(heap))
		return PTR_ERR(heap);

	exp_info.ops = &mtk_mm_heap_ops;
	exp_info.priv = &mtk_mm_uncached_heap_data;
	exp_info.name = "mtk_mm-uncached";
	heap = dma_heap_add(&exp_info);
	if (IS_ERR(heap))
		return PTR_ERR(heap);

	return 0;
}
module_init(mtk_mm_heap_create);
MODULE_LICENSE("GPL v2");
