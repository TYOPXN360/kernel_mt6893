/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Kernel-side dma-buf heap framework interface, backported from 5.4 for
 * this Android 17 bring-up. The uapi half lives in uapi/linux/dma-heap.h.
 */
#ifndef _LINUX_DMA_HEAP_H
#define _LINUX_DMA_HEAP_H

#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/kref.h>
#include <linux/list.h>

/**
 * struct dma_heap - represents a dmabuf heap in the system
 * @name:		used for debugging/device-node name
 * @ops:		ops struct for this heap
 * @priv:		heap exporter private data
 * @heap_devt		heap device node
 * @list:		list head connecting to list of heaps
 * @heap_cdev:		heap char device
 * @refcount:		reference count for this heap
 * @heap_dev:		heap device struct
 */
struct dma_heap {
	const char *name;
	const struct dma_heap_ops *ops;
	void *priv;
	dev_t heap_devt;
	struct list_head list;
	struct cdev heap_cdev;
	struct kref refcount;
	struct device *heap_dev;
};

struct dma_heap_ops {
	struct dma_buf *(*allocate)(struct dma_heap *heap, unsigned long len,
				    unsigned long fd_flags,
				    unsigned long heap_flags);
};

/**
 * struct dma_heap_export_info - information needed to export a new dmabuf heap
 * @name:	used for debugging/device-node name
 * @ops:	ops struct for this heap
 * @priv:	heap exporter private data
 */
struct dma_heap_export_info {
	const char *name;
	const struct dma_heap_ops *ops;
	void *priv;
};

struct dma_heap *dma_heap_find(const char *name);
void dma_heap_put(struct dma_heap *heap);
struct dma_heap *dma_heap_add(const struct dma_heap_export_info *exp_info);

static inline const char *dma_heap_get_name(struct dma_heap *heap)
{
	return heap->name;
}

static inline struct device *dma_heap_get_dev(struct dma_heap *heap)
{
	return heap->heap_dev;
}

#endif /* _LINUX_DMA_HEAP_H */
