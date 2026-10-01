#ifndef RKX_BINDER_ALLOC_H
#define RKX_BINDER_ALLOC_H

#include <linux/types.h>
#include <linux/version.h>
/* binder_size_t / binder_uintptr_t live in the uapi header; binder_internal.h
 * uses them but does not pull it in. */
#include <uapi/linux/android/binder.h>
#include "binder_alloc.h"
#include "binder_internal.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 0, 0)
int rk_binder_alloc_copy_from_buffer(struct binder_alloc *alloc, void *dest,
	struct binder_buffer *buffer, binder_size_t buffer_offset, size_t bytes);
#endif

#endif
