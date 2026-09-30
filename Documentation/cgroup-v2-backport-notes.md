# cgroup v2 移植笔记（4.14.357-openela / MT6893）

目标：让本内核提供 Android 17 所需的完整 cgroup v2 统一层级控制器。

## 现状总览

设备上 `/sys/fs/cgroup` 以 `cgroup2` 挂载，但 `cgroup.subtree_control` 只有
`memory` 一个控制器。`/proc/cgroups` 列出的是 cgroup v1 层级（hierarchy 0-4，
含 vendor 的 `schedtune`），`/dev/cpuctl`、`/dev/cpuset`、`/dev/blkio` 等 v1
挂载点并存，形成 v1/v2 混合布局。

## 各控制器状态

| 控制器 | 源码 | v2 (`.dfl_cftypes`) | 缺什么 |
|---|---|---|---|
| memory | `mm/memcontrol.c` | 有 | 可用 |
| pids | `kernel/cgroup/pids.c` | 有 | 仅需开 `CONFIG_CGROUP_PIDS` |
| rdma / debug | 有 | 有 | 未启用 |
| cpuset | `kernel/cgroup/cpuset.c` | 已补 | 见下 |
| cpu | **源码缺失** | — | 需移植 |
| io | **源码缺失**（`kernel/cgroup/blkio.c` 不存在；`CONFIG_BLK_CGROUP` 只构建 v1 的 `block/blk-cgroup.o`） | — | 需移植 |
| hugetlb / misc | **源码缺失** | — | 需移植 |

`include/linux/cgroup_subsys.h` 的控制器清单里已经列了 `cpu`，框架在等这个文件。

## 框架与调度器基础设施

比预期完整，MTK 已回移了不少 4.15+ 的东西：

- `cftype` / `can_attach` / `cancel_attach` / `attach` / `post_attach` /
  `css_*` 钩子齐全，`include/linux/cgroup-defs.h` 里有 `dfl_cftypes` 字段。
- `init/Kconfig:875` 有 `config CFS_BANDWIDTH`（`default n`，未启用）。
- `kernel/sched/sched.h` 里 `struct cfs_bandwidth` 完整，字段在
  `#ifdef CONFIG_CFS_BANDWIDTH` 内（`init/Kconfig` 另一处 `struct cfs_bandwidth { }`
  是 `!CONFIG_CGROUP_SCHED` 分支的空桩，本树 `CONFIG_CGROUP_SCHED=y` 不走那条）。
- `kernel/sched/fair.c` 有**完整可用的带宽节流引擎**：
  `init_cfs_bandwidth()`、`start_cfs_bandwidth()`、
  `__refill_cfs_bandwidth_runtime()`、hrtimer 周期定时器。
- **重要**：节流模型是 5.x 的 **cfs_rq 级**（`cfs_rq->tg`、
  `assign_cfs_rq_runtime()`，fair.c:4487），不是 4.14 原生的 task_group +
  smp_call_function 模型。说明 5.x 的节流机制已被回移进来，这显著降低了
  移植 v2 `cpu` 控制器胶水层的难度。

## 本分支已完成

- `e77664705717`
  - cpuset：新增 `cpuset_dfl_cftypes[]`（`cpuset.cpus` / `cpuset.mems` /
    `cpuset.effective_cpus` / `cpuset.effective_mems`）并挂到 `.dfl_cftypes`。
    此前 `cpuset_cgrp_subsys` 只带 `.legacy_cftypes`，导致 cpuset 从不出现在
    `cgroup.controllers`。全部复用现成处理函数。
  - defconfig 开启 `CONFIG_CGROUP_PIDS`。

## 已知限制

**`cpuset_detach()` 无法实现。** 上游在子 cpuset 被删除时会重算父级
effective 掩码，靠的是 `cgroup_subsys` 的 `.detach` 钩子；该钩子随 4.15+ 的
cgroup v2 重构引入，**本树的 `cgroup_subsys` 没有这个字段**（现有钩子：
`css_alloc/css_online/css_offline/css_released/css_free/css_reset/can_attach/
cancel_attach/attach/post_attach/can_fork/cancel_fork/fork/exit/release/bind`）。
后果：删除子 cpuset 后父级 effective_cpus 可能过期。不影响开机与常规使用，
但若 Android 在运行期删除 cgroup 需留意。

## 剩余工作的量级

`cpu` / `io` / `hugetlb` / `misc` 四个控制器源码均缺失，需从 5.x 移植。
`cpu` 最难（与调度器深度耦合），但因节流引擎已是 5.x 风格，胶水层可写。
`io` 约 3000 行。这不是一轮能完成的，需要编译—刷机—验证的多次迭代，
且 `cpu` 出错往往表现为全系统随机卡顿而非崩溃，验证周期长。

参考：v2 cpu 控制器由 5.2 的 "sched: Introduce cgroup bandwidth interface"
引入，位于 `kernel/sched/core.c`（不是 `kernel/cgroup/cpu.c`）。

## 真机验证结果（2026-09-30）

刷入实测，`/proc/cgroups` 中 hierarchy 0 即 v2 默认层级：

```
cpuset    3   8   1     ← 仍在 v1
memory    0 211   1     ← v2 ✅
freezer   0 211   1     ← v2 ✅
pids      0 211   1     ← v2 ✅（本移植启用）
```

`cgroup.controllers` = `memory pids`，非根 cgroup 下 `pids.max` 已生成，pids 控制器可用。

### cpuset 两次尝试均无效

1. 加 `cpuset_dfl_cftypes` + `.dfl_cftypes` —— 代码编入（vmlinux 含
   `cpuset.cpus`/`cpuset.mems`），但 `cgroup.controllers` 无 cpuset，
   `echo +cpuset > cgroup.subtree_control` 被拒。
2. 改 `.early_init = false` 走与 memory/pids 相同的
   `cgroup_init_subsys(ss, false)` 路径 —— 仍然无效，已回退
   （`6df3eb263909`）。

`kernel/cgroup/cgroup.c` 中没有对 cpuset 的特殊处理，`cgroup_init()` 循环
对它与 memory/pids 一视同仁，故差异来源尚未定位。**不要再重复这两种尝试。**

注意 MTK 的定制：`is_in_v2_mode()` 被改成无条件返回 true（结尾是 `|| true`），
`cpuset_mount()` 把 "cpuset" 文件系统重定向到 "cgroup" 并加 `noprefix`。
MTK 用自研 shim 让 cpuset 一直按 v2 语义工作，但控制器本身仍挂在 v1 层级。
继续在 cpuset 上投入前，应先判断它是否真的需要进 v2。

## cpu 控制器移植的缺口

调度器侧基础设施比预期完整：`cfs_rq->tg`(sched.h:493)、`cfs_bandwidth`(337)、
throttled 字段、完整的 hrtimer 节流引擎（`init_cfs_bandwidth()`、
`assign_cfs_rq_runtime()` 在调度路径中被调用），且是 5.x 的 cfs_rq 级模型。

但**缺少 v2 cpu 控制器所依赖的带宽设置 API**：树中没有
`tg_set_cfs_bandwidth()` / `tg_set_cfs_quotapad_size()`（v5.2 随
"sched: Introduce cgroup bandwidth interface" 引入）。带宽目前只经
`tg_cfs_bandwidth(tg)` 静态访问，运行时由 `assign_cfs_rq_runtime()` 驱动，
没有对外的设置入口。

且 `CONFIG_CFS_BANDWIDTH` 当前为 n（Kconfig 中 `default n`），
`struct cfs_bandwidth` 的字段位于 `#ifdef CONFIG_CFS_BANDWIDTH` 内，不开就无字段。

因此 cpu 控制器需要三步：开 `CONFIG_CFS_BANDWIDTH`、给调度器补带宽设置接口、
写 cgroup v2 cpu 控制器（css_alloc/can_attach + `cpu.max`/`cpu.weight`/`cpu.stat`）。
