# 操作编辑器 v2 实施计划（变量 / 按下弹起 / 条件步 / 取点提示 / 说明页）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 给 v1 操作编辑器加：触发数据变量（`tdx/tdy/tux/tuy/tms`）、按下/弹起步骤、区域判断/开关判断条件步、取点捕获标记、面板「说明」页；契约 v4→v5。

**Architecture:** 变量 = **负数编码**进现有 int 字段（零结构增长，`-1..-5`）；触发数据由区域线程在按下/抬起时捕获、经**触发槽**投递、执行器起跑快照进私有内存；按下/弹起 = 执行器新增单拍动作 + 持有态（`held`）；条件步 = 单拍判定（复用 `region_hit` + 门控同款锁纪律）；取点标记与说明页 = 纯面板。**契约 v5 在批 1 一次落全**（`step.ref[16]` + 触发槽 6 字段 + 版本号），批 2/3 只加逻辑/UI。

**Tech Stack:** C（NDK r27d 交叉编译，`-O2 -Wall -Wextra -Werror -D_GNU_SOURCE`）、C++/ImGui v1.91.8 面板（`src-ui/`）、toybox sh、Python 3 工具脚本。设备：OnePlus PJZ110（ColorOS / Android 16，KernelSU）。

**Spec:** `docs/OPS_PLAN_V2.md`（设计定稿）。执行时**两份一起读**；契约细节（字段/格式/日志行/文案）以 spec 为准，本计划写"怎么做"。

## Global Constraints

- 编译：`src/*.c` 通配一起链（`scripts/build.sh`），新函数无需改构建脚本。
- **默认（无 UI）构建必须保持可编译**：所有新增核心代码放 `#ifdef VT_UI` 分支内（同 v1 口径）。
- 告警零容忍：`-Wall -Wextra -Werror`；单文件秒查 `sh scripts/check_syntax.sh <文件>`。
- 函数文档唯一来源 `scripts/funcdoc_data.py`：**每个新/改函数的文案先写进库**，再 `python scripts/apply_funcdoc.py`；`--check` 必须 **0 处**。
- Shell：`#!/system/bin/sh`（toybox）；产物 `sh -n` 通过；LF 断言用**字节判据**（`python -c "…count(b'\x0d')"`），不用 `grep -c $'\r'`（MSYS 空转）。
- 契约：**WS 协议零改动**；不加线程、不加命令通道；坐标一律**竖屏逻辑坐标**；**`VT_SHM_VERSION` 4→5 在批 1 T1.1 一次落全部布局**（spec §6），批 2/3 只加逻辑/UI。
- **变量编码逐字**：`-1=tdx  -2=tdy  -3=tux  -4=tuy  -5=tms`（仅「允许变量的字段」放行 `[-5, max]`；字面值恒 ≥0）。
- **日志词逐字**（spec §8 + 本计划细化）：
  - 条件：`op 条件 <区域判断|开关判断> <ref> 不成立 → <中止|跳过下一步>`；
  - 收尾：`op 收尾 松开`；中止原因词：`变量无值 / 槽占用 / 未按下 / 区域不存在 / 非开关型 / 条件不成立`；
  - 步级：`op 步 k/n 按下 x,y` / `op 步 k/n 弹起` / `op 步 k/n 区域判断 <ref> x,y` / `op 步 k/n 开关判断 <ref>`；
  - TRACE：起跑一行 `op 变量 tdx=… tdy=… tux=… tuy=… tms=…`（未设打 `-`）。
- **ops.conf v2**：`#vtouch-ops v2`；step 行固定 7 字段（`ref` 空写 `-`）；读端兼容 v1（6 字段：`a3=0`、`ref=""`）。
- **md5 口径**：T1.1 起共享结构变化 ⇒ 默认核心 md5 **变更属有意**（记录新值+理由）；此后纯 VT_UI 任务默认 md5 **应不变**（守卫纪律最强信号，变了就停）。
- 提交：中文信息写到 `build/_msg_<x>.txt`（UTF-8）后 `git commit -F`；**每任务一提交、不自动 push**。
- 每批开工前：**全量备份 + 恢复点**（`build/_backup_full_<TS>/`，含 `git status` / `git diff --binary` 快照与未跟踪文件清单）。
- 测试口径（用户定）：**不前置真机探测、实现阶段不接设备**；日志按 spec §8 随实现埋好；真机测试集中到 spec §9（阶段 4 只是入口索引）。
- 本计划所有函数名/字段名/常量名**逐字使用**（后续任务按名字互认，别改拼写）。

---

## 文件结构（新增/修改地图）

| 文件 | 动作 | 职责 |
|---|---|---|
| `src/vt_internal.h` | 修改 | 步骤常量 4–7；`vt_step.ref`；`struct vt_trig_data`；变量常量/掩码位；触发槽 6 字段；原型 |
| `src/vt_shm.h` | 修改 | `VT_SHM_VERSION` 4→5 + 版本注释行 |
| `src/vt_ops.c` | 修改 | op_valid v2；变量解析；按下/弹起；条件步；收尾释放；日志 |
| `src/vt_region.c` | 修改 | 按下坐标/时间捕获；`ptrig` 携带 `td`；投递扩参 |
| `src/vtouchd.c` | 修改 | 初值表补新字段（T1.1） |
| `src/vt_shm.c` | 修改 | `VT_EDIT_OP_RUN` 调用点同步（`vt_ops_run(…, NULL)`，T2.1） |
| `src-ui/ui_glue.c` | 修改 | `vtouch_get_op_step` 扩 `ref` |
| `src-ui/ui_stubs.c` | 修改 | 同上镜像 |
| `src-ui/vtouch_ui.cpp` | 修改 | 编辑器 v2；取点标记；说明页；ops.conf v2 |
| `scripts/funcdoc_data.py` | 修改 | 新/改函数文案（先文案后代码） |
| `README.md` / `AGENTS.md`（两份） | 修改 | v2 口径（批 3 收尾） |

---

## 阶段 1 —— 契约 v5 + 触发数据（批 1）

> 开工前：全量备份到 `build/_backup_full_<TS>/`，记恢复点。

### Task 1.1: 契约 v5（步骤 ref / 触发数据 / 版本 / op_valid v2）

**Files:**
- Modify: `src/vt_internal.h`, `src/vt_shm.h`, `src/vt_ops.c`（仅 `op_valid`）, `src/vtouchd.c`（初值表）

**Interfaces:**
- Produces（逐字，后续任务全部按这些名字）：

```c
/* vt_internal.h 追加 */
#define OP_STEP_DOWN        4
#define OP_STEP_UP          5
#define OP_STEP_COND_REGION 6
#define OP_STEP_COND_TOGGLE 7
#define OP_COND_ABORT       0
#define OP_COND_SKIP        1
#define OP_VAR_TDX (-1)
#define OP_VAR_TDY (-2)
#define OP_VAR_TUX (-3)
#define OP_VAR_TUY (-4)
#define OP_VAR_TMS (-5)
#define OP_VAR_N    5
#define OP_TRIGB_TDX 1u
#define OP_TRIGB_TDY 2u
#define OP_TRIGB_TUX 4u
#define OP_TRIGB_TUY 8u
#define OP_TRIGB_TMS 16u
#define OP_TRIGB_ALL (OP_TRIGB_TDX|OP_TRIGB_TDY|OP_TRIGB_TUX|OP_TRIGB_TUY|OP_TRIGB_TMS)

struct vt_step { int type; int a1, a2, a3, a4; int ms; char ref[REGION_ID_MAX + 1]; };

struct vt_trig_data { unsigned mask; int dx, dy, ux, uy, ms; };

/* struct vt_state 触发槽追加（现 op_trig_seq / op_trig_name / op_trig_slot 之后） */
unsigned op_trig_mask;
int op_trig_dx, op_trig_dy, op_trig_ux, op_trig_uy, op_trig_ms;

/* 原型（现签名扩参） */
void vt_ops_trigger_post(const char *name, int slot, const struct vt_trig_data *td);
```

- `vt_shm.h`：`#define VT_SHM_VERSION 5u` + 注释行（`5 = v2 操作扩展：step.ref / 触发数据槽`）。
- **op_valid v2 规则**（逐条）：
  - `type ∈ 1..7`；未知类型拒。
  - 坐标字段（点按 `a1,a2`；滑动 `a1..a4`；按下 `a1,a2`；区域判断 `a1,a2`）：字面 `0..W-1` / `0..H-1`，或变量 `-5..-1`；其余拒。
  - `ms` 字段（点按/滑动/等待）：字面区间照 v1（点按 `0..60000`、滑动 `1..60000`、等待 `0..600000`），或变量 `-5..-1`。
  - 区域判断/开关判断：`a3 ∈ {0,1}`；`ref` 长度 `1..REGION_ID_MAX` 且过 `vt_id_ok`（**存在性不校验、允许悬空**，运行时报 `区域不存在`）。
  - 弹起（5）：字段忽略。
  - 名字/门控防御照 v1 不动。

- [ ] **Step 1: 改 `vt_internal.h` / `vt_shm.h`**（如上；`vt_step` 加 `ref` 后 `vt_op`/`vt_shm_edit` 随动，区 B 断言由构建期核对）。
- [ ] **Step 2: 改 `vtouchd.c` 初值表**（新字段补 0；确认"默认/引导副本"两处初始化表达式逐字一致）。
- [ ] **Step 3: 改 `vt_ops.c` `op_valid`**（逐条落实上表；`why` 人话文案同步各档）。
- [ ] **Step 4: 门**：`sh scripts/build.sh`（**默认 md5 变更属预期**——共享结构改了；记录新旧值）+ `sh scripts/build.sh ui`（新 md5 记录）；`python scripts/apply_funcdoc.py --check`（0 处）。
- [ ] **Step 5: 实测尺寸记录**（spec §6.1）：从启动日志 `共享内存就绪 … state@…(…)` 读区 A/total 新值，写进报告。
- [ ] **Step 6: 提交**：`feat: 契约 v5 —— 步骤 ref/触发数据/op_valid v2（变量编码）`。

### Task 1.2: 区域线程触发数据捕获与投递

**Files:**
- Modify: `src/vt_region.c`, `src/vt_ops.c`（仅 `vt_ops_trigger_post`）, `scripts/funcdoc_data.py`（该函数文案）

**Interfaces:**
- Consumes: `struct vt_trig_data`、`OP_TRIGB_*`（T1.1）。
- Produces: 触发槽 6 字段被正确写入（mask 按模式：**按下** = `TDX|TDY`；**完整按压** = `ALL`）。

- [ ] **Step 1: `vt_region.c` 捕获**：per-slot `r_trig_dx[MAX_PHYS]` / `r_trig_dy[MAX_PHYS]` + `uint64_t r_trig_dt[MAX_PHYS]`（`#ifdef VT_UI` 内）；DOWN 命中时记录（`ev->ts` = 按下时刻，`vt_internal.h:96`）。
- [ ] **Step 2: 投递携带数据**：`TRIGPEND` 载荷扩 `td`（形态自定，报告说明）；DOWN 触发 → `{TDX|TDY, lx, ly, 0,0,0}`；完整按压结算 → `{ALL, r_trig_dx[slot], r_trig_dy[slot], lx, ly, (int)(ev->ts - r_trig_dt[slot])}`（ms 钳 ≥0）；开关（TOGGLE）不带数据。
- [ ] **Step 3: `vt_ops_trigger_post` 扩参**：先写 `name/slot/mask/dx..ms`、最后 release `seq++`（顺序不能反）；funcdoc 文案同步（先文案后代码）。
- [ ] **Step 4: 门**：默认核心 md5 **应不变**（本任务全在 VT_UI 守卫内——变了就是泄漏，停）；UI 核心新 md5；**宿主仿真**（用 `build/` 下自建生成器 + MinGW gcc，套路同 v1 `_sim_gen*`，不入库）：DOWN 触发 mask=`TDX|TDY`、FULL mask=`ALL` 且 `ms=Δts`（断言贴报告）。
- [ ] **Step 5: 提交**：`feat: 触发数据 —— 按下坐标/时间捕获 + 触发槽投递（mask/dx/dy/ux/uy/ms）`。

### Task 1.3: 批 1 收尾

- [ ] **Step 1: 全门**：双构建 ×2（real 面板两遍 md5 一致）+ funcdoc 0/0 + `git status` 清点。
- [ ] **Step 2: 报告** `task-1-report.md`（改动/门输出/md5 集：默认新值+理由、UI、real）。
- [ ] **Step 3**: 无文件改动则无提交（报告进工作区）。

---

## 阶段 2 —— 执行器 v2 + 胶水（批 2）

> 开工前：全量备份，记恢复点。

### Task 2.1: 变量系统（快照 / 解析 / 中止 / TRACE）

**Files:**
- Modify: `src/vt_ops.c`, `src/vt_shm.c`（RUN 调用点）, `src/vt_internal.h`（`vt_ops_run` 原型）, `scripts/funcdoc_data.py`

**Interfaces:**
- Consumes: `vt_trig_data`、`OP_VAR_*`（T1.1/T1.2）。
- Produces: `int vt_ops_run(const char *name, const struct vt_trig_data *td);`（**td=NULL = 手动运行**；原型同步）；执行器私有快照 `R.trig`；解析器（static，建议名 `op_resolve`：`(int v, int *out)` → `0` 成功 / `-1` 无值）。

- [ ] **Step 1: `vt_ops_run` 扩参**：`op_consume_trigger` 传 `&td`（acquire 读全槽字段后）；`vt_shm.c` 的 `VT_EDIT_OP_RUN` 分派传 `NULL`；起跑把 `td` 快照进 `R.trig`。
- [ ] **Step 2: 解析器**：`v>=0 → 字面`；`-5..-1 → 查 mask 位`，未设 → 中止（`原因=变量无值`，走既有 abort 机制，步号=当前步）。
- [ ] **Step 3: 消费点接入**：点按 x/y/ms、滑动 4 坐标+ms、等待 ms、按下 x/y（条件点在 T2.3）。
- [ ] **Step 4: TRACE 行**：起跑时（`op_trace_on()`）`op 变量 tdx=… tdy=… tux=… tuy=… tms=…`（未设打 `-`）。
- [ ] **Step 5: 门**：默认 md5 不变；UI 新 md5；宿主仿真：字面照旧 / `-1..-5` 取值 / 未设中止（原因词）。
- [ ] **Step 6: 提交**：`feat: 执行器变量 —— 触发数据快照/负数编码解析/无值中止 + TRACE 行`。

### Task 2.2: 按下 / 弹起（状态机 + 安全 + 帧窗）

**Files:**
- Modify: `src/vt_ops.c`

- [ ] **Step 1: R 增 `held`**（起跑/中止/完成清零）。
- [ ] **Step 2: `OP_STEP_DOWN`**：解析坐标；`held` → 中止 `槽占用`；写 virt down（照点按 down 相位同款写法与帧窗纪律）；`held=1`；步日志 `op 步 k/n 按下 x,y`。
- [ ] **Step 3: `OP_STEP_UP`**：`!held` → 中止 `未按下`；写 virt up；`held=0`；步日志 `op 步 k/n 弹起`。
- [ ] **Step 4: 按住期门禁**：点按/滑动/按下 在 `held` 时 → 中止 `槽占用`（步入口统一判）。
- [ ] **Step 5: 收尾**：`op_finish` 与 abort 路径：`held` → 释放 + `op 收尾 松开`（帧窗避让照既有机制：abort 走 stop_pending 口径，不得在帧窗内直写 `g.virt`）。
- [ ] **Step 6: 门**：默认 md5 不变；UI 新 md5；宿主仿真 4 组：按下→等待→弹起 正序 / 未按先弹 / 按住中点按 / 收尾释放。
- [ ] **Step 7: 提交**：`feat: 执行器 —— 按下/弹起步骤（持有态/槽占用/未按下/收尾释放）`。

### Task 2.3: 条件步（区域判断 / 开关判断）

**Files:**
- Modify: `src/vt_ops.c`

- [ ] **Step 1: `OP_STEP_COND_REGION`**：解析点；`region_lock` 内查 `ref`（不存在 → 解锁后中止 `区域不存在`）；`region_hit` 判定（**停用 = 不命中**，spec §3.1）；解锁后：成立 → 继续；不成立 → 记 `op 条件 区域判断 <ref> 不成立 → <中止|跳过下一步>`，`a3=0` → 中止 `条件不成立`；`a3=1` → 步序额外 +1。
- [ ] **Step 2: `OP_STEP_COND_TOGGLE`**：查 `ref`；不存在 → `区域不存在`；`kind!=1` → `非开关型`；成立 = `toggle_on==1`；不成立处理同上（词=开关判断）。
- [ ] **Step 3: 步日志**：`op 步 k/n 区域判断 <ref> x,y` / `op 步 k/n 开关判断 <ref>`。
- [ ] **Step 4: 跳过边界**：末步跳过 = 正常完成；跳过步不执行不求值。
- [ ] **Step 5: 门**：默认 md5 不变；UI 新 md5；宿主仿真 ≥7 组：矩形 / 圆 / 停用 / 不存在 / 非开关型 / 两档失败 / 末步跳过。
- [ ] **Step 6: 提交**：`feat: 执行器条件步 —— 区域判断/开关判断 + 中止/跳过下一步`。

### Task 2.4: 胶水 API v2

**Files:**
- Modify: `src-ui/ui_glue.c`, `src-ui/ui_stubs.c`, `scripts/funcdoc_data.py`

**Interfaces:**
- Produces: `int vtouch_get_op_step(int i, int s, int *type, int *a1, int *a2, int *a3, int *a4, int *ms, char *ref, int refn);`（尾部扩参；`ref` 空写空串；`refn<=0` 或 NULL 可省略）。

- [ ] **Step 1: funcdoc 文案先写进 `funcdoc_data.py`**（签名/参数/返回/note 同步）。
- [ ] **Step 2: `ui_glue.c` 实现扩参**（读 `steps[s].ref`，strnlen 防御照款）；`ui_stubs.c` 镜像。
- [ ] **Step 3: 面板调用点同步**（`vtouch_ui.cpp` 唯一调用者，编译期暴露）。
- [ ] **Step 4: 门**：real 面板 ×2 md5 一致（记录）；stub rc=0；funcdoc apply + `--check` 0/0；默认核心 md5 不变。
- [ ] **Step 5: 提交**：`feat: 胶水 —— vtouch_get_op_step 带区域引用（ref）`。

### Task 2.5: 批 2 收尾

- [ ] **Step 1: 全门**（同 T1.3 口径）+ 报告 `task-2-report.md`。
- [ ] **Step 2**: 无文件改动则无提交。

---

## 阶段 3 —— 面板（批 3）

> 开工前：全量备份，记恢复点。

### Task 3.1: 步骤编辑器 v2（7 类型 / 变量切换 / 条件参数）

**Files:**
- Modify: `src-ui/vtouch_ui.cpp`

- [ ] **Step 1: 类型选择器扩 1..7**（名：点按 / 滑动 / 等待 / 按下 / 弹起 / 区域判断 / 开关判断）；每类型字段布局（弹起无字段）。
- [ ] **Step 2: 变量切换**：坐标/时长格加 [变量] 按钮 → 弹 5 项中文名列表（触发按下x / 触发按下y / 触发弹起x / 触发弹起y / 触发时长）+「数值」回退；选中存 `-1..-5`，显示中文名（面板本地表）。
- [ ] **Step 3: 条件参数**：区域列表（可滚动；开关判断只列 `kind==1`）；不成立行为两键（中止 / 跳过下一步）。
- [ ] **Step 4: 取点接入新字段**（点 [取点] 后点屏 → 填当前坐标格并切回字面值）。
- [ ] **Step 5: 步骤行摘要显示**（含变量名 / 区域名）。
- [ ] **Step 6: 门**：real ×2 一致、stub rc=0、默认不变、funcdoc 0/0。
- [ ] **Step 7: 提交**：`feat: 面板 —— 步骤编辑器 v2（7 类型/变量选择/条件参数）`。

### Task 3.2: 取点捕获标记 + 说明页

**Files:**
- Modify: `src-ui/vtouch_ui.cpp`

- [ ] **Step 1: 标记**：`pick_seq` 变化时存 `{x,y,expire}`（逻辑坐标）；渲染循环画十字 + 坐标文字（~2 秒；逻辑→当前屏换算复用区域轮廓的现成换算）；**纯绘制**（不吞触摸、不写共享内存）。
- [ ] **Step 2: 说明页**：左侧菜单加「说明」；可滚动；文案 = spec §5 逐条（12 条，逐字）。
- [ ] **Step 3: 门**：real ×2 一致、stub rc=0、默认不变、funcdoc 0/0。
- [ ] **Step 4: 提交**：`feat: 面板 —— 取点捕获标记 + 说明页（术语表）`。

### Task 3.3: ops.conf v2 落盘

**Files:**
- Modify: `src-ui/vtouch_ui.cpp`

- [ ] **Step 1: 写端**：`#vtouch-ops v2`；step 行 7 字段（`step <type> <a1> <a2> <a3> <a4> <ms> <ref>`，`ref` 空写 `-`；变量照写负数）。
- [ ] **Step 2: 读端**：接受 v1（6 字段：`a3=0`、`ref=""`）与 v2；坏行单跳 / 只补缺口径不变。
- [ ] **Step 3: 门**：real ×2 一致、stub rc=0、默认不变；宿主仿真读端（v1 样本 → 默认值正确）。
- [ ] **Step 4: 提交**：`feat: 面板 —— ops.conf v2（ref/七字段；读端兼容 v1）`。

### Task 3.4: 批 3 收尾（README/AGENTS + 全门）

**Files:**
- Modify: `README.md`, `AGENTS.md`（**两份**：仓库根 + `C:/Users/21102/AGENTS.md`）

- [ ] **Step 1: README**：操作编辑器 v2 节（变量表 / 新步骤 / 条件步 / 取点标记 / 说明页 / ops.conf v2）；`文件:行号` 引用现读。
- [ ] **Step 2: AGENTS（两份逐字节同步）**：操作模型口径增量（变量编码、按下弹起、条件步、契约 v5、日志词）。
- [ ] **Step 3: 全门**：双构建 ×2 + funcdoc + `python scripts/pack_client.py` + `python scripts/ci_check.py --core build/vtouchd_ui` 全绿；md5 全表记录；AGENTS 两份 md5 对齐。
- [ ] **Step 4: 提交**：`docs: 批 3 收尾 —— README/AGENTS（v2 口径）+ 全门`。

---

## 阶段 4 —— 测试阶段（入口索引）

> 实现阶段不接设备（用户定）；真机测试按 spec §9 清单在用户配合下执行（agent 走 adb、用户点触）。
> 范围：v2 全部新功能 + v1 遗留两小项（运行中停止 / 取点面板内取消）+ 终态构建重部署（顺测 su 更新路径）。
