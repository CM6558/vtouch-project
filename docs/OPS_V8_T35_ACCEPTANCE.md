# OPS_V8_T3.5 验收记录（契约 v17 + 邮箱程序载荷 + 并存双跑）

日期：2026-10-10 下午；设备：OnePlus PJZ110（1440×3168）；
核心：`vtouchd_ui` `2e54d501dcc7739778dd074a5f31c963`（契约 v17）；起核带 `VTOUCH_OPS_TRACE=1`。

## 载荷与通道

- **zt1（步骤版）**：`点按 300 400 300 / 等待 200 / 点按 400 300 50`（步 1/3/1，追加进双侧 ops.conf）。
- **ztp1（脚本版）**：同场景 `ztp1.vts` → 166B VTBC（`~/vtouch-shots/v8pilot/ztp1.vtbc`）。
- **坏件**：ztp1 第 20 字节（代码节首）改成 `0xEE`。
- **通道（T3.5 试点，T4 由 progs/ 正式链路取代）**：面板 `load_ops` 收尾 `pilot_prog_check` —— 读
  `ops-prog.vtbc` → `VT_EDIT_PUT_PROG`（核心 `vt_prog_put` 上表前 `vt_vm_load` 全量验证）→
  `ops-prog.run`（内容=起跑名，空=ztp1）→ `VT_EDIT_OP_RUN`。驱动：`~/vtouch-shots/v8pilot/pilot.sh gate|prep|start|run1|run2|bad|read|clean`。

## 用例与实锤

| # | 用例 | 实锤（/data/local/tmp/vt_ui_core.log / logcat VTouchUI） | 结论 |
|---|---|---|---|
| T1 | 启动导入（v17 核 + 试点载荷） | `已是最新（md5 2e54d501…）`；`put op1 步数=13`、`put 保命装 步数=68`、`put zt1 步数=3`、**`put ztp1 程序=166B`** | ✅ |
| T2 | T8 热重放 + 试点重投 | `touch ops.conf` → `op 编辑 clear` + 全量重放 + `put ztp1 程序=166B` | ✅ |
| T3 | **步骤版执行**（旧引擎） | `op 编辑 run zt1` → `op 启动 zt1 步数=3 槽=0 门控=无` → `op 步 2/3 等待 200ms` → `op 步 3/3 点按 400,300 按住50ms` → **`op 完成 zt1 用时=551ms`** | ✅ |
| T4 | **脚本版执行**（VM） | `op 编辑 run ztp1` → `op 启动 ztp1 脚本=166B 槽=0 门控=无` → **`op 完成 ztp1 用时=551ms`**（连跑两轮均为 551ms） | ✅ |
| T5 | 坏载荷拒收 | `op 被拒 ztp1: 程序非法: 未知指令`（好件还原后照跑） | ✅ |
| T6 | 收场 | 双侧 ops.conf md5 `666a45f9…`（= 备份原值，逐字节）；pilot 文件清除；无 TRACE 干净重启；用户 ops 原样（op1/保命装） | ✅ |

## 双跑对拍结论（T3 硬门①②之设备侧部分）

- 步骤版 vs 脚本版：同场景用时**逐毫秒一致（551ms = 551ms）**，双引擎同核并存（同一 v17 核内：zt1 走旧引擎、ztp1 走 VM）。
- 注入原语同源：VM 宿主钩子直调 `op_finger_ll/raw`（与旧引擎共用），动作时间表逐式对齐。
- 空转基线 / 找图端到端差 ≤1%（T3 硬门②③）与 zt 家族全量对拍 → **T3.6 影子期**继续测。

## 开发期血泪（本批修复，含一条重量级）

1. **运行期载入全灭（`首字节=00000000`、err 空）——宏碰撞致结构体跨 TU 漂移**：
   `vt_vm.h` 的字段名 `g` 在「先包含 `vt_internal.h`」的 TU（`vt_ops.c`）被 `#define g (*g_ptr)` 宏替换 →
   该 TU 眼里 `struct vt_vm` 里 `g[512]` 变成 8 字节指针，**sizeof 273392 → 256504**。
   `vt_vm.c` 侧仍是 273392：其 `memset(vm, 0, sizeof *vm)` 越界 16KB，把紧随其后的 `g_vm_prog`
   （起跑刚拷贝好的字节码）整段清零；`err` 写入/读取偏移也随之错位（所以日志空原因）。
   **修**：字段改名 `glob` + 血泪注释；宿主 `test_vt_vm.c` 与 `vt_ops.c` 同步。
   **诊断链（可复用）**：nm 符号尺寸（0x3ebf8 vs 反汇编 memset 0x42be8）→ `offsetof` 双 TU 探针对拍 →
   "no member named 'g'" 坐实宏替换。**教训：核心 TU 混编时，结构体字段名一律避开核心宏
   （`vt_internal.h` 的 `g` 等）；跨 TU 结构体只靠同头文件不保证布局一致，宏状态也是布局的一部分。**
2. `pilot.sh clean` 的 `for f` 在 su 双引号内被外层 shell 先展开（`set -u` 报 `f: parameter not set`）——
   改显式两条 `cp -f`。
3. 页首基线（启动日志 tail）曾把「旗标尚未落位」误判为读取失败——时序上 flag 文件晚于上次重放时，
   面板如实走「无旗标=只投不跑」分支，行为正确。

## 收场状态

- 设备跑 v17 `2e54d501`（影子期正常态：用户步骤数据照跑；程序表空载零影响）。
- 备份文件保留：双侧 `ops.conf.bak-v8-pilot-pre`（md5 `666a45f9`）；`/sdcard/vtouch.sh.bak-v8pilot`。
- 试点通道带 flag 文件 = 零行为；T4 起由 `progs/` 正式链路取代。
