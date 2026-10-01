// SPDX-License-Identifier: GPL-2.0-only
/*
 * Vendor trace hook definitions
 *
 * Backported from AOSP kernel_common android14-5.15 (drivers/android/
 * vendor_hooks.c) for this 4.14 tree. A hook declared with DECLARE_HOOK() is
 * a bare tracepoint - it has no trace event behind it - so the only way the
 * __tracepoint_<hook> objects get emitted is a translation unit that defines
 * CREATE_TRACE_POINTS and includes the hook headers.
 *
 * That unit also exports each hook so out-of-tree code (ReKernel-X in
 * drivers/rekernel_x) can attach probes to it.
 */

#define CREATE_TRACE_POINTS

#include <trace/hooks/vendor_hooks.h>

#include <linux/tracepoint.h>

#undef TRACE_SYSTEM
#define TRACE_SYSTEM binder
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH trace/hooks
#include <trace/hooks/binder.h>
#undef TRACE_SYSTEM
#undef TRACE_INCLUDE_PATH

#undef TRACE_SYSTEM
#define TRACE_SYSTEM signal
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH trace/hooks
#include <trace/hooks/signal.h>
#undef TRACE_SYSTEM
#undef TRACE_INCLUDE_PATH

/*
 * Export tracepoints that act as a bare tracehook (i.e. have no trace event
 * associated with them) so probes can be attached from elsewhere.
 */
EXPORT_TRACEPOINT_SYMBOL_GPL(android_vh_binder_alloc_new_buf_locked);
EXPORT_TRACEPOINT_SYMBOL_GPL(android_vh_binder_reply);
EXPORT_TRACEPOINT_SYMBOL_GPL(android_vh_binder_trans);

/*
 * The signal hook is defined here, not in kernel/signal.c: that file includes
 * both trace/events/signal.h and trace/hooks/signal.h, so it must not emit a
 * second copy of these globals.
 */

EXPORT_TRACEPOINT_SYMBOL_GPL(android_vh_do_send_sig_info);
