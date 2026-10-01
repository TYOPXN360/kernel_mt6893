/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Tracing hooks - 4.14 compatible variant
 *
 * Backported from AOSP kernel_common android14-5.15 so ReKernel-X can observe
 * Binder and signal activity around frozen processes.
 *
 * Upstream routes hook invocation through static calls
 * (include/linux/static_call.h, CONFIG_HAVE_STATIC_CALL and the extra struct
 * tracepoint members that come with them). None of that exists on this 4.14
 * tree, so a hook is a plain tracepoint here and is invoked by walking
 * tp->funcs under rcu_read_lock_sched_notrace(), exactly like __DO_TRACE.
 * Dropping upstream's two-probe cap is safe for the same reason.
 *
 * The unregister helper is provided because upstream deliberately omits it
 * (its hooks live in modules that own their own lifetime) while ReKernel-X
 * calls both.
 *
 * Note: we intentionally omit include file ifdef protection. This is due to
 * the way trace events work - if a file includes two trace event headers under
 * one CREATE_TRACE_POINTS the first include would override
 * DECLARE_RESTRICTED_HOOK and break the second.
 */

#ifndef __GENKSYMS__
#include <linux/tracepoint.h>
#endif

#if defined(CONFIG_TRACEPOINTS) && defined(CONFIG_ANDROID_VENDOR_HOOKS)

#define DECLARE_HOOK DECLARE_TRACE

int android_rvh_probe_register(struct tracepoint *tp, void *probe, void *data);
int android_rvh_probe_unregister(struct tracepoint *tp, void *probe, void *data);

#define __RKX_DO_HOOK_CALL(_name, args)				\
	do {								\
		struct tracepoint_func __it_func;			\
									\
		if (!trace_##_name##_enabled())			\
			break;						\
		rcu_read_lock_sched_notrace();			\
		for (each_tracepoint_func(__it_func,		\
					  __tracepoint_##_name)) {	\
			void *__data = __it_func.data;			\
			__it_func.func(__data, args);		\
		}						\
		rcu_read_unlock_sched_notrace();			\
	} while (0)

#define __RKX_DEFINE_HOOK_FN(_name, _reg, _unreg, proto, args)	\
	static inline bool						\
	trace_##_name##_enabled(void)				\
	{								\
		return static_key_false(&__tracepoint_##_name.key);	\
	}								\
	static inline int						\
	register_trace_##_name(void (*probe)(proto), void *data)	\
	{								\
		return android_rvh_probe_register(&__tracepoint_##_name, \
						  (void *)probe, data);	\
	}								\
	static inline int						\
	unregister_trace_##_name(void (*probe)(proto), void *data)	\
	{								\
		return android_rvh_probe_unregister(&__tracepoint_##_name, \
						    (void *)probe, data); \
	}								\
	DEFINE_TRACE_FN(_name, _reg, _unreg)

#define DEFINE_HOOK_FN(_name, _reg, _unreg, proto, args)		\
	__RKX_DEFINE_HOOK_FN(_name, _reg, _unreg, proto, args)

/*
 * 4.14 has no DECLARE_TRACEPOINT/__traceiter_/static calls, so a restricted
 * hook is declared exactly like a plain DECLARE_TRACE: the extern tracepoint
 * plus the trace_##name() wrapper. Registration helpers come from
 * __RKX_DEFINE_HOOK_FN.
 */
#define __DECLARE_RESTRICTED_HOOK(name, proto, args, cond, data_proto) \
	extern struct tracepoint __tracepoint_##name;			\
	static inline bool trace_##name##_enabled(void)		\
	{								\
		return static_key_false(&__tracepoint_##name.key);	\
	}								\
	__RKX_DEFINE_HOOK_FN(name, NULL, NULL, proto, args);	\
	__attribute__((unused))					\
	static inline void trace_##name(proto)			\
	{								\
		__RKX_DO_HOOK_CALL(name, args);			\
	}

#define DECLARE_RESTRICTED_HOOK(name, proto, args, cond)		\
	__DECLARE_RESTRICTED_HOOK(name, PARAMS(proto), PARAMS(args), \
				    cond, PARAMS(void *__data, proto))

#else /* !CONFIG_TRACEPOINTS || !CONFIG_ANDROID_VENDOR_HOOKS */

/* suppress trace hooks */
#define DECLARE_HOOK DECLARE_EVENT_NOP
#define DECLARE_RESTRICTED_HOOK(name, proto, args, cond)		\
	DECLARE_EVENT_NOP(name, PARAMS(proto), PARAMS(args))
#define DEFINE_HOOK_FN(_name, _reg, _unreg, proto, args)
#define __RKX_DO_HOOK_CALL(_name, args) do { } while (0)

#endif /* CONFIG_TRACEPOINTS && CONFIG_ANDROID_VENDOR_HOOKS */
