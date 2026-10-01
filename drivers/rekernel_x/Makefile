ccflags-y += -I$(src)/../android

obj-y += rkx.o rkx_genl.o rkx_netuid.o rkx_free_async.o rkx_frozen.o rkx_binder.o rkx_signal.o rkx_netfilter.o rkx_binder_kp.o rkx_binder_alloc.o


ifdef RKX_VERSION
ccflags-y += -DRKX_VERSION=\"$(RKX_VERSION)\"
endif
