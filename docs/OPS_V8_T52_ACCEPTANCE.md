# OPS_V8 T5.2 真机验收记录 —— 迁移执行 + 双跑对拍

> 日期：2026-10-10 晚 · 设备核 `3a6e723c`（含本轮修复）· 编辑器 v34 `108bd0e6`（未动）
> 状态：**完成**。收场 = 全方案 + live 迁移态落位（v8 元数据行 + `progs/` 产物），`.bak-v8` 回滚资产就位。

## 一、核心崩溃发现与修复（本轮头等事件）

- **现象**：脚本版 zt8（`如果 找图("名刀CD") { 等待 10 }`，即 ztp8 同构）亮屏运行 → 面板报「核心心跳停滞 3s → 自杀退出」，表现为"挂死"。
- **取证链**：核日志 `vis 抓帧 请求 → 完成 21ms` 后静默 → `/proc` 线程采样（主线程仍正常 `do_sys_poll`，随后进程整体消失）→ `logcat -b crash`：**SIGSEGV ×3**（17:44 / 17:46 / 17:47，三次"挂死"全是崩溃），backtrace：
  `vis_exec_find+1076 ← op_vis_eval+96 ← vmg_find_img+48 ← vt_vm_tick ← vt_ops_tick`；`fault addr 0x0`（空指针解引用）。
- **根因（VM↔执行器 契约缝）**：`vt_vm.c` 的 `vm_cstr` 对可选串「无」哨兵（-1）返回 **NULL**；而老执行器路径的字符串恒非 NULL（无 = 空串指针）→ `op_vis_region` 里 `name[0]` 解引用 NULL。**步骤版为何不炸**：屏熄 / 面板未就绪时抓帧先失败（`无画面`）早退，从未走到 region 段；**亮屏 + 面板健在**才会踩中。
- **修复（核侧 2 处，`src/vt_ops.c`）**：
  1. `op_vis_eval` 入口归一：`if (!ref) ref = ""; if (!region) region = "";`——两路径公共入口，一次覆盖 找图/找色/找色点 三处 VM 可选串缝（`vmg_find_dot` 的 ref=NULL 也一并消化）；
  2. `op_vis_region`：`if (!name || !name[0]) return 0;`（NULL 容错，防御性）。
- **验证**：修复后 zt8 脚本版 `vis 找图 名刀CD 未命中 (耗时 2ms)` → `op 完成 zt8 用时=29ms`（对照步骤版 zt8s：未命中 2ms / 完成 22ms——**找图本体逐毫秒一致**）。
- **部署**：devbuild → `pack_su.py` → `/sdcard/vtouch.sh` 更新 → start 安装回读 md5 = `3a6e723c…`（防载荷反向覆盖陷阱）。funcdoc `--check` 0 调整；vm 宿主单测 85 断言不受影响（宿主桩不覆盖真 vis，本雷只能真机逮）。

## 二、双跑对拍矩阵（R1 步骤版 vs R2 迁移脚本版）

驱动：`~/vtouch-shots/v8t5/t52.sh`（gate/prep/inject/restart/run/push/cleanup）；起跑全走 `ops-prog.run` 旗标 + T8 热载（零重启）；前台 = hermescontrol、亮屏、1440×3168。
证据：`t52-r1.log`（步骤版）/ `t52-r2.log`（脚本版）。

| # | 场景 | R1 步骤 | R2 脚本 | 判定 |
|---|---|---|---|---|
| zt1 | 点按序列（300/200/50） | 551ms | 550ms | ≈1ms（T3.6 金样 551=551 同族） |
| zt2 | 滑动 | 101ms | 102ms | ≈1ms |
| zt3 | 按下+弹起 | 50ms | 51ms | ≈1ms |
| zt4 | 空转×300 | 7ms | 0ms | 噪声级（纯结构） |
| zt5 | 条件块（区域判断） | 10ms | 10ms | 0 |
| zt6 | 提示 | 0ms | 0ms | 0 |
| zt7 | 框区域 | 0ms | 0ms | 0 |
| zt8 | **找图（亮屏真实抓帧）** | 55ms | 36ms | **find 本体 2ms = 2ms（门③ ✓）**；总差 = 抓帧波动（15–25ms 区间实测） |
| zt9 | 循环 · 条件 | 21ms | 20ms | ≈1ms |

- **交叉互证**：迁移产 `zt1.vtbc` 与 T3.5 手写 `ztp1.vtbc` **逐字节一致**（fnv=08fd5310 / 166B）。
- **门③（屏亮找图 ≤1%）**：过——脚本版与步骤版 find 本体耗时 2ms=2ms（T3 遗留小项一并结清）。
- 变体注：起核即起跑（面板未热身）会快速 `无画面` 中止（面板未就绪路径，行为正确）；另存为已收录「旧旗标指已删操作 → `op 丢弃 操作不存在`」正确拒收。

## 三、全方案迁移应用（R3 终态）

- 迁移器产物（T5.2 工具链：`VtMigrate`（纯逻辑）+ `VtMigrateRun`（树执行器）+ `VtMigrateCheck` 18 例离机核〔run.sh 五连核〕）→ 逐方案推送 + 逐字节审计：

| 方案 | ops.conf 迁移 md5 | 操作（程序行） |
|---|---|---|
| live / 吕布 | `a200d4b9` | op1（500B fnv=4de7db61）+ 保命装（1874B fnv=7c8de217） |
| 干将莫邪 / 默认 / 默认2 | `b12b1639` | op1（源三份本就同哈希 dd2a0fee，产物同哈希正确） |
| 橘右京 | `65cda445` | op1 |

- 本地↔设备：5 方案 ops.conf + progs 全量**逐字节 ✓**。
- 重启验净：`op 编辑 put op1 程序=500B` + `put 保命装 程序=1874B`；logcat（tag `VTouchUI`）`上表` 全绿、零 `跳过`/`被拒`；无测试残留。
- 编辑器保真：`T52Round` 快核——Ops.File 对迁移产物「解析→序列化」**逐字节 ✓**（v34 开箱可读、存盘不损）。
- 回滚资产：6× `ops.conf.bak-v8`（原始 v5 逐字节：`666a45f9` / `dd2a0fee` / `4dda0d3f`）+ 5× `regions.conf.bak-v8`；
  核二进制副本 `~/vtouch-shots/v8t5/vtouchd_ui-3a6e723c`。

## 四、T5 开口两笔（按最小风险口径处置，不动语言）

1. **「未写先读」变量**（ztF/ztf7 合成件）：**报告不译**。真实语料 8/8 无一例；不造幽灵声明（`如果 假{r3=0}` 有常量裁剪风险），该语义由语言层原生用例覆盖。
2. **找色/找色点「持续」键缺口**：真实语料零用例 → **保持报告口径**（遇此类旧数据先报告拒绝）；如未来出现再走规范修订（r8）。

## 五、设备状态速记（交接用）

- 核 `3a6e723c` 在跑；面板正常；`ops-prog.run` 已清；屏幕超时还原 `120000`；无 TRACE。
- 运行目录 = 迁移终态（v8：元数据行 + `progs/`）；live == `schemes/吕布` 逐字节。
- 证据目录 `~/vtouch-shots/v8t5/`：`t52.sh` / `t52-r1.log` / `t52-r2.log` / `core2.log` / `panel-logcat2.txt` / `crash-buffer.txt` / `tombstone_08.txt` / `out-mig/` / `out-mig-test/` / `zt-payload.txt`。
