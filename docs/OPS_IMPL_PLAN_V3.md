# 操作编辑器 v3 实施计划（坐标同编 / 条件分支 / 跳转步 / 预览与整屏编辑）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 给 v2 操作编辑器加：坐标对同屏编辑 + 取点一次填一对、条件步两侧对称四档（继续/跳过/跳转/中止，跳转目标 0=结束）、新步骤类型「跳转」+ 防死循环守卫、编辑层整屏（取点自动收起）+ [预览] 页（清单 + 小地图）；契约 v5→v6、ops.conf v3。

**Architecture:** 契约 v6 在批 1 一次落全（`vt_step` +j1/j2、枚举、版本）；执行器把条件步「不成立处理」改造为**两侧通用的档位应用**（CONT/SKIP/JUMP/ABORT + 跳转守卫），新增跳转步（目标 0 = 直接 `op_finish()`）；面板数据模型 `[6]→[8]`、胶水 flat 6→8/步、ops.conf 9 字段（读端类型感知翻译）；整屏/收起/预览全为面板侧（核心零新增接口）。

**Tech Stack:** C（NDK r27d 交叉编译，`-O2 -Wall -Wextra -Werror -D_GNU_SOURCE`）、C++/ImGui v1.91.8 面板（`src-ui/`）、toybox sh、Python 3 工具脚本。设备：OnePlus PJZ110（ColorOS / Android 16，KernelSU）。

**Spec:** `docs/OPS_PLAN_V3.md`（设计定稿）。执行时**两份一起读**；契约细节（字段/格式/日志行/文案）以 spec 为准，本计划写"怎么做"。

## Global Constraints

- 编译：`src/*.c` 通配一起链（`scripts/build.sh`），新函数无需改构建脚本。
- **默认（无 UI）构建必须保持可编译**：所有新增核心代码放 `#ifdef VT_UI` 分支内（同 v1/v2 口径）。
- 告警零容忍：`-Wall -Wextra -Werror`；单文件秒查 `sh scripts/check_syntax.sh <文件>`。
- 函数文档唯一来源 `scripts/funcdoc_data.py`：**每个新/改函数的文案先写进库**，再 `python scripts/apply_funcdoc.py`；`--check` 必须 **0 处**。
- Shell：`#!/system/bin/sh`（toybox）；产物 `sh -n` 通过；LF 断言用**字节判据**，不用 `grep -c $'\r'`（MSYS 空转）。
- 契约：**WS 协议零改动**；不加线程、不加命令通道；坐标一律**竖屏逻辑坐标**；**`VT_SHM_VERSION` 5→6 在批 1 T1.1 一次落全**（spec §6），批 2/3 只加逻辑/UI。
- **档位枚举逐字**：`OP_COND_ABORT=0 / OP_COND_SKIP=1 / OP_COND_CONT=2 / OP_COND_JUMP=3`。
- **跳转目标值域逐字**：**0..步数**（0 = 结束 = 直接正常完成；1..步数 = 目标步骤）。
- **跳转守卫**：`VT_OPS_JUMP_MAX=200`；条件跳转 + 跳转步共用计数；「结束」不占计数、不判上限。
- **日志词逐字**（spec §8 + 本计划细化）：
  - 条件不成立（恒打）：`op 条件 <区域判断|开关判断> <ref> 不成立 → <继续下一步|跳过下一步|跳到第 N 步|跳到结束|中止>`；
  - 条件成立（档位 ≠ 继续才打）：`op 条件 … 成立 → <跳过下一步|跳到第 N 步|跳到结束|中止>`；
  - 步级：`op 步 k/n 跳转 第 N 步` / `op 步 k/n 跳转 结束`；
  - TRACE：`op 跳转 <名> 第 A 步 → 第 B 步`（B=0 打 `→ 结束`）；
  - 中止原因新词：`条件中止`（成立侧）/ `跳转超限`；取点：`取点 回填 第 N 步 参数 x,y = a,b`。
- **面板数据模型 [8]**：`g_ope_steps[i][8] = {type, a1, a2, a3, a4, ms, j1, j2}`（与核心 `vt_step` 字段顺序一致，ref 另存 `g_ope_refs`）。
- **ops.conf v3**：`#vtouch-ops v3`；step 行 9 字段 `step <type> <a1> <a2> <a3> <a4> <ms> <ref> <j1> <j2>`；读端 v1(6)/v2(7)/v3(9)；**类型感知翻译**（v1/v2 行的条件步 a4→`OP_COND_CONT`；v1 滑动行 a3/a4 照读**不动**）。
- **说明页 v3**：唯一来源 = `docs/OPS_PLAN_V3.md` §9（13 条）；重生成流程照 v2（机器提取 + 逐字节复核；改文案先改 spec 再重提）。
- **md5 口径**：T1.1 起共享结构变化 ⇒ 默认核心 md5 **变更属有意**（记录新值+理由）；此后纯 VT_UI 任务默认 md5 **应不变**（守卫纪律最强信号，变了就停）；面板任务 real md5 每任务记录。
- 提交：中文信息写到 `build/_msg_v3_<x>.txt`（UTF-8）后 `git commit -F`；**每任务一提交、不自动 push**。
- 每批开工前：**全量备份 + 恢复点**（`build/_backup_full_<TS>/`）。
- 测试口径（用户定）：**实现阶段不接设备**；真机测试集中到 spec §10（阶段 4 只是入口索引）。
- 本计划所有函数名/字段名/常量名**逐字使用**（后续任务按名字互认，别改拼写）。

---

## 文件结构（新增/修改地图）

| 文件 | 动作 | 职责 |
|---|---|---|
| `src/vt_internal.h` | 修改 | `OP_STEP_JUMP`；`OP_COND_CONT/JUMP`；`VT_OPS_JUMP_MAX`；`vt_step.j1/j2`；注释表 |
| `src/vt_shm.h` | 修改 | `VT_SHM_VERSION` 5→6 + 注释行 |
| `src/vt_ops.c` | 修改 | op_valid v3；条件四档通用应用；跳转步；守卫；TRACE |
| `src-ui/ui_glue.c` | 修改 | `vtouch_op_put` flat 8/步；`vtouch_get_op_step` 扩 j1/j2 |
| `src-ui/ui_stubs.c` | 修改 | 同上镜像 |
| `src-ui/vtouch_ui.cpp` | 修改 | 模型 [8]；conf v3；行控件；加步；参数弹层；整屏/收起；预览；说明页 |
| `scripts/funcdoc_data.py` | 修改 | 新/改函数文案（先文案后代码） |
| `README.md` / `AGENTS.md`（两份） | 修改 | v3 口径（批 3） |

---

## 阶段 1 —— 契约 v6 + 执行器 v3（批 1）

> 开工前：全量备份到 `build/_backup_full_<TS>/`，记恢复点。

### Task 1.1: 契约 v6（step.j1/j2 / 枚举 / 版本 / op_valid v3）

**Files:**
- Modify: `src/vt_internal.h`, `src/vt_shm.h`, `src/vt_ops.c`（仅 `op_valid`）, `scripts/funcdoc_data.py`（op_valid note）

**Interfaces:**
- Produces（逐字，后续任务全部按这些名字）：

```c
/* vt_internal.h 追加 */
#define OP_STEP_JUMP    8
#define OP_COND_CONT    2
#define OP_COND_JUMP    3
#define VT_OPS_JUMP_MAX 200

struct vt_step { int type; int a1, a2, a3, a4; int ms; int j1, j2; char ref[REGION_ID_MAX + 1]; };
```

- `vt_shm.h`：`#define VT_SHM_VERSION 6u` + 注释行（`6 = v3 操作扩展：step.j1/j2（条件双分支 + 跳转步）`）。
- **op_valid v3 规则**（逐条）：
  - `type ∈ 1..8`；未知类型拒。
  - 条件步（6/7）：`a3,a4 ∈ 0..3`；`a4 == OP_COND_JUMP` → `j1 ∈ 0..step_count`，否则 j1 忽略；`a3 == OP_COND_JUMP` → `j2 ∈ 0..step_count`，否则 j2 忽略；坐标（区域判断 `a1,a2`）/`ref` 规则照 v2 不动。
  - 跳转步（8）：`a1 ∈ 0..step_count`；其余字段忽略。
  - 其余步（1..5）：照 v2 不动。
  - 名字/门控防御照 v1 不动。

- [ ] **Step 1: 改 `vt_internal.h` / `vt_shm.h`**（常量、`vt_step` 加 `j1, j2` 于 `ms` 与 `ref` 之间；注释表更新为 spec §6.1 字段语义表口径）。
- [ ] **Step 2: 核实 `vtouchd.c` 初值表**（v3 无新顶层状态字段则**不动**；若动了必须记理由——初值表两份初始化表达式必须逐字一致）。
- [ ] **Step 3: 改 `vt_ops.c` `op_valid`**（逐条落实上表；`why` 人话文案同步各档，如 `第 %d 步档位非法` / `第 %d 步跳转目标越界`）。
- [ ] **Step 4: 门**：`sh scripts/build.sh`（**默认 md5 变更属预期**——共享结构改了；新值+理由记报告）+ `sh scripts/build.sh ui`（新 md5 记录）；`python scripts/apply_funcdoc.py --check`（0 处）；`op_valid` 文档块/`funcdoc_data.py` 文案同步（先文案后代码）。
- [ ] **Step 5: 实测尺寸记录**（spec §6.1）：从启动日志 `共享内存就绪 …` 读区 A/total 新值，写进报告。
- [ ] **Step 6: 提交**：`feat: 契约 v6 —— step.j1/j2/跳转步枚举/op_valid v3`。

### Task 1.2: 执行器 v3（条件四档 / 跳转步 / 守卫 / TRACE）

**Files:**
- Modify: `src/vt_ops.c`, `scripts/funcdoc_data.py`

**Interfaces:**
- Consumes: `OP_COND_*`、`OP_STEP_JUMP`、`VT_OPS_JUMP_MAX`（T1.1）。
- Produces（建议名，实现可调整但报告说明）：`R.jumps`（起跑清零）；`op_cond_apply(const struct vt_step *st, int hit, const char *word)`（两类型共用，替代 `op_cond_fail`）；`op_jump_apply(int target)`（守卫 + 0=结束→`op_finish()` / 否则 `R.step = target-1`）。

- [ ] **Step 1: 条件两侧应用改造**：`op_cond_fail` → `op_cond_apply(st, hit, word)`：按侧取档位（成立=`a4`/目标=`j1`；不成立=`a3`/目标=`j2`）执行 CONT/SKIP/JUMP/ABORT；日志按 Global Constraints 词表（不成立恒打、成立 ≠ 继续才打）；SKIP 机制照 v2（额外 +1 + PH_WAIT）；ABORT：不成立 `条件不成立` / 成立 `条件中止`；两处调用点（区域判断/开关判断）同步。
- [ ] **Step 2: 跳转步**：`op_begin_step` 增 `case OP_STEP_JUMP`：目标 0 → `op_finish()` 直接返回；否则守卫 → `R.step = 目标-1` → `PH_WAIT` + `deadline=t0`；步级 INFO `op 步 k/n 跳转 第 N 步|结束`。
- [ ] **Step 3: 守卫**：跳转计数（条件跳转 + 跳转步共用，起跑清零）；超限 → 中止 `跳转超限`；「结束」不占计数不判上限。
- [ ] **Step 4: TRACE 行**：`op_trace_on()` 时每跳打 `op 跳转 <名> 第 A 步 → 第 B 步`（B=0 打 `→ 结束`）。
- [ ] **Step 5: 门**：默认核心 md5 **不变**（本任务全在 VT_UI 守卫内——变了就是泄漏，停）+ UI 核心新 md5；**宿主仿真**（`build/` 下自建生成器 + MinGW gcc，套路同 v2 `_sim_gen*`，不入库）断言贴报告：
  - 条件四档 × 两侧穷举（成立侧四档 / 不成立侧四档）；
  - 跳过边界（末步跳过 = 正常完成）；跳过步不执行不求值；
  - 跳转前 / 后（循环）/ 自环；目标 0（结束 → 正常完成、auto_off 生效、不占计数）；
  - 守卫：201 跳 → `跳转超限`；
  - op_valid：档位越界 / 跳转目标越界（含负值）→ 拒收。
- [ ] **Step 6: 提交**：`feat: 执行器 v3 —— 条件四档/跳转步/跳转守卫 + TRACE`。

### Task 1.3: 批 1 收尾

- [ ] **Step 1: 全门**：双构建 ×2（real 面板两遍 md5 一致）+ funcdoc 0/0 + `git status` 清点。
- [ ] **Step 2: 报告** `task-1-report.md`（改动 / 门输出 / md5 集：默认新值+理由、UI、real）。
- [ ] **Step 3**: 无文件改动则无提交（报告进工作区）。

---

## 阶段 2 —— 面板 + 胶水（批 2）

> 开工前：全量备份，记恢复点。

### Task 2.1: 数据层 v3（模型 [8] / 胶水 put8 + get 扩 / ops.conf v3 / 保存预检）

**Files:**
- Modify: `src-ui/vtouch_ui.cpp`, `src-ui/ui_glue.c`, `src-ui/ui_stubs.c`, `scripts/funcdoc_data.py`

**Interfaces:**
- `g_ope_steps[OPE_MAX_STEPS][8]`（`{type, a1, a2, a3, a4, ms, j1, j2}`；`g_ope_refs` 不变）。
- `int vtouch_op_put(const char *name, const char *gate, int autoff, const int *steps8, const char (*refs)[REGION_ID_MAX + 1], int nsteps, int *out_err);`（flat **8/步**；步数组顺序逐字：`t,a1,a2,a3,a4,ms,j1,j2`）。
- `int vtouch_get_op_step(int i, int s, int *type, int *a1, int *a2, int *a3, int *a4, int *ms, char *ref, int refn, int *j1, int *j2);`（尾部扩参；`j1/j2` 可 NULL）。
- `OPS_CONF_VER 3`；写端 9 字段；读端 6/7/9 字段 + **类型感知翻译**；版本门接受 1/2/3。

- [ ] **Step 1: funcdoc 文案先写进 `funcdoc_data.py`**（两函数签名/参数/note 同步 v3）。
- [ ] **Step 2: `ui_glue.c` 实现**：`vtouch_op_put` flat 步长 6→8（组装 `op.steps[i].j1/j2`）；`vtouch_get_op_step` 读 `steps[s].j1/j2`；`ui_stubs.c` 镜像。
- [ ] **Step 3: 面板模型与调用点全量迁移**（`g_ope_steps` [6]→[8]；表函数 `ope_fidx` [7]→[8]（跳转行）/ `ope_nfields` / `ope_tname` / `ope_flabel` 同步；`op_edit_open` / `op_edit_save` / `load_ops` / `ops_load_put` / 操作卡片摘要——编译期暴露，一处不漏）。
- [ ] **Step 4: ops.conf v3**：写端 `#vtouch-ops v3` + 9 字段行（`ref` 空写 `-`；变量照写负数）；读端解析 6/7/9 字段（`sscanf` 9 转换；6/7 字段行缺省 `j1=j2=0`；**`t==6||t==7` 且旧行 → `a4=OP_COND_CONT`；滑动步 a3/a4 照读不动**）；版本门 `ver != 1 && ver != 2 && ver != OPS_CONF_VER` → 丢弃清空改写（照现有口径）。
- [ ] **Step 5: 保存预检**：`op_edit_save` 增——条件步任一侧档位 = 跳转 → 对应目标 ∈ 0..nsteps；跳转步 `a1 ∈ 0..nsteps`；不过就地提示拒收（文案人话，如 `第 %d 步跳转目标超出步数`）。
- [ ] **Step 6: 门**：real 面板 ×2 md5 一致（记录）；stub rc=0；默认核心 md5 不变；funcdoc apply + `--check` 0/0；**宿主仿真**：conf 读入三版本样本（v1 滑动行 a4 不被改写 / v2 条件行 a4→继续 / v3 行往返）+ 写端 9 字段逐字。
- [ ] **Step 7: 提交**：`feat: 面板数据层 v3 —— 模型 j1/j2 + 胶水 flat8 + ops.conf v3（类型感知读）`。

### Task 2.2: 参数弹层 v3（全字段一屏 / 本地缓冲原子落 / 取点填对）

**Files:**
- Modify: `src-ui/vtouch_ui.cpp`

- [ ] **Step 1: 全字段一屏**：打开 `[参数]` → 该步全部数值字段一次列出（格 = 标签 + 值文本；点格 = 激活，激活格高亮；键盘编激活格；[变量] 仅激活格可变量时显示；[取点] 仅激活格是坐标格时显示）。
- [ ] **Step 2: 本地缓冲原子落**：进层快照全部字段到 `g_ne_vals[]`（每格 `{literal/var, 文本}`）；[完成] **全字段校验全过才一次写回**（不过 → 提示该格 + 停留）；[取消] **全丢**；层内不写 `g_ope_steps`（取点/变量只改缓冲）。
- [ ] **Step 3: 取点填对**：取点结果把激活格所在**坐标对两格同填**（字面值；中断变量态）；十字标记照旧（既有链路）；日志 `取点 回填 第 N 步 参数 x,y = a,b`。
- [ ] **Step 4: 条件目标格键盘模式**：j1/j2 目标编辑复用本弹层（标签「成立目标 / 不成立目标」，范围 0..32，0 显示「结束」；无变量/取点）—— 供 T2.3 行控件目标格调用。
- [ ] **Step 5: 门**：real ×2 一致、stub rc=0、默认不变、funcdoc 0/0。
- [ ] **Step 6: 提交**：`feat: 面板 —— 参数弹层 v3（全字段一屏/原子落/取点填对）`。

### Task 2.3: 步骤行控件 v3（条件两钮 + 目标格 / 跳转行 / 加步 8 类型 / 摘要）

**Files:**
- Modify: `src-ui/vtouch_ui.cpp`

- [ ] **Step 1: 条件步行内控件**：成立 / 不成立两枚**循环钮**（`继续下一步 → 跳过下一步 → 跳到… → 中止` 循环；文字随档位）；档位 = 跳到… 时该侧出现**目标格**（显示 `第 N 步` / `结束`；点它开数字键盘编 0..32 —— 复用 T2.2 的目标模式，槽位 = j1 / j2）。
- [ ] **Step 2: 跳转步行**：`[参数]` = 1 格「目标」（0..32，0 显示「结束」）+ 行摘要。
- [ ] **Step 3: 加步按钮**：两行 4+4 —— `＋点按＋滑动＋等待＋按下` / `＋弹起＋区域判断＋开关判断＋跳转`（新步默认：跳转目标 = 1；条件步 = 成立继续 / 不成立中止）。
- [ ] **Step 4: 摘要文本 v3**（`ope_step_text`）：条件步 `x,y · <ref> · 成立 → … / 不成立 → …`（跳转目标打 `跳到第 N 步` / `跳到结束`）；跳转步 `跳到 第 N 步` / `跳到 结束`。
- [ ] **Step 5: 门**：real ×2 一致、stub rc=0、默认不变、funcdoc 0/0。
- [ ] **Step 6: 提交**：`feat: 面板 —— 条件四档行控件 + 跳转步编辑（加步 8 类型）`。

### Task 2.4: 编辑层整屏 + 取点自动收起 + 手动收起

**Files:**
- Modify: `src-ui/vtouch_ui.cpp`（`ui_glue.c` 如矩形上报需要改则一并）

- [ ] **Step 1: 整屏绘制**：操作编辑覆盖层及全部子层（参数/变量/区域列表/名字键盘/预览）改按**当前屏全屏 rect** 绘制（面板主窗口 / 列表页 / 区域页不变）。
- [ ] **Step 2: 吞触摸矩形三态**：`vtouch_ui_publish_rect` 按状态切换 —— 编辑层打开 = 全屏；编辑层收起（取点 / 手动）= 底部收起条；编辑层关闭 = 面板窗口（现状）。
- [ ] **Step 3: 取点自动收起**：进取点态 → 编辑层收成底部条「点屏幕上目标位置 · 点这里取消」（条上点按 = `vtouch_pick_cancel` + 退取点态）；回填 / 取消 / **面板自带 20s 兜底计时**超时 → 自动弹回编辑层（参数层保持）。
- [ ] **Step 4: 手动收起**：编辑层头 [收起] 钮（状态全保留）；收起条点按 = 展开。
- [ ] **Step 5: 门**：real ×2 一致、stub rc=0、默认不变、funcdoc 0/0。
- [ ] **Step 6: 提交**：`feat: 面板 —— 编辑层整屏 + 取点自动收起（三态吞触摸矩形）`。

### Task 2.5: 预览页（清单 + 小地图）

**Files:**
- Modify: `src-ui/vtouch_ui.cpp`

- [ ] **Step 1: [预览] 入口 + 全屏只读层**（编辑层头按钮；[关闭] 返回）。
- [ ] **Step 2: 步骤总览**：每步一行（类型 + 参数摘要，逐字同 `ope_step_text`）；条件步两行分支（`├ 成立 → …` / `└ 不成立 → …`）；跳转行 `→ 第 N 步` / `→ 结束`。
- [ ] **Step 3: 小地图**：竖屏逻辑比例图（`g_w × g_h` 等比缩入可用区域）；点按 = 蓝点 / 按下 = 橙点 / 判定点 = 紫叉 / 滑动 = 带箭头连线；每点旁标步号；越界坐标贴边并标注。
- [ ] **Step 4: 门**：real ×2 一致、stub rc=0、默认不变、funcdoc 0/0。
- [ ] **Step 5: 提交**：`feat: 面板 —— 操作预览页（步骤清单 + 坐标小地图）`。

### Task 2.6: 批 2 收尾

- [ ] **Step 1: 全门**（同 T1.3 口径）+ 报告 `task-2-report.md`。
- [ ] **Step 2**: 无文件改动则无提交。

---

## 阶段 3 —— 文档与收尾（批 3）

> 开工前：全量备份，记恢复点。

### Task 3.1: 说明页 v3（13 条）+ README/AGENTS v3 口径

**Files:**
- Modify: `src-ui/vtouch_ui.cpp`（说明页数组）, `README.md`, `AGENTS.md`（**两份**：仓库根 + `C:/Users/21102/AGENTS.md`）

- [ ] **Step 1: 说明页重生成**：从 `docs/OPS_PLAN_V3.md` §9 机器提取 13 条（流程照 v2：去标记/拼行规则同款，**逐字节复核**）；说明页注释的"唯一来源"改为 §9（v3）。
- [ ] **Step 2: README**：操作编辑器 v3 节增量（条件四档 / 跳转步（0=结束、200 上限）/ 坐标同编与取点填对 / 整屏 + 取点自动收起 + 手动收起 / [预览] 页 / ops.conf v3 / 契约 v6 / 日志新词）；`文件:行号` 引用现读。
- [ ] **Step 3: AGENTS（两份逐字节同步）**：操作模型口径增量（档位枚举、跳转语义、0=结束、守卫、conf v3 翻译规则、契约 v6、说明页来源）。
- [ ] **Step 4: 门**：real ×2 一致、stub rc=0、默认不变、funcdoc 0/0；AGENTS 两份 md5 对齐。
- [ ] **Step 5: 提交**：`docs: 说明页 v3 + README/AGENTS（v3 口径）`。

### Task 3.2: 批 3 收尾（全门 + 产物）

- [ ] **Step 1: 全门**：`check_syntax` 全绿；双构建 ×2；funcdoc 0/0；`python scripts/pack_client.py`（onefile 新 md5 记录）；`python scripts/ci_check.py`（10/0）；AGENTS 双份 md5 对齐。
- [ ] **Step 2: md5 全表**（默认 / UI / real 三件 / onefile / AGENTS）写报告 `task-3-report.md`。
- [ ] **Step 3: 提交**（若有文件改动；否则仅报告）。

---

## 阶段 4 —— 测试阶段（入口索引）

> 实现阶段不接设备（用户定）；真机测试按 spec §10 清单在用户配合下执行（agent 走 adb、用户点触）。
> 范围：v3 全部新功能（条件四档 / 跳转含结束与循环 / 取点填对 / 整屏与收起 / 预览 / 说明页）+ v1/v2 遗留项 + 终态构建重部署（顺测 su 更新路径）。
