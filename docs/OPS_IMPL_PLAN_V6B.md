# OPS_V6B 实施计划 —— 控制流补齐（核心 → 面板零改 → app）

> **状态：Task 1–5 全部完成，真机验收全过（2026-10-09 晚）**——验收记录 `OPS_V6B_ACCEPTANCE.md`（P0 9/9 + P1 逐项过）；核心 97880366 / 编辑器 v23 已部署；编辑器侧上机手测挂账。

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans（已拍板 **Native 逐任务**，2026-10-09 用户「开始」按 §6 推荐默认全通过）。任务按序执行；每步 checkbox 跟踪，任务完成 = 末步验收全过。
> 账本：`~/vtouch-editor/.v6b-progress.md`（每任务一行 + Ruling 行）。

**Goal:** 控制流补齐 = 多条件并或（子条件行 19）+ 终止（20）+ 跳出/继续 N 层（17/18 扩 a1）+ 卡片/总览双视图缩进对齐；行格式/契约/面板零改动、旧数据零迁移。

**Architecture:** 核心 `op_valid` 扩校验（19 链 + 20 + 17/18 层数）；执行器加组合求值（短路、且>或）+ 新 case；app 模型镜像 + 块编辑器条件列表 + 两视图对齐修复（总览渲染合源、嵌套色带逐级）。面板源码零改动（透传已核）。

**Spec:** `docs/OPS_PLAN_V6B.md`（§3.1 字段/求值、§3.2 终止、§3.3 N 层、§3.4 对齐、§3.5 兼容、§5 验收门、§6 拍板已过）

**Tech Stack:** C（设备侧 clang 构建）/ Java（javac+d8 手写管线）/ ImGui 面板（零改动，跑自检留痕）。

## Global Constraints

- **文件格式不动**：`#vtouch-ops v5`、step 行 10 字段、契约版本都不动；19/20 = 新类型号标准行。
- **VT_UI 守卫**：ops C 代码在守卫内；`vt_internal.h` 共享路径改动=有意变更（默认核心 md5 会变，记录差异原因）。
- **构建门（设备侧）**：`sh ~/vtouch-devbuild.sh`（每任务尾必过）；面板 `sh ~/vtouch-ui-check.sh`（本批应零改动，跑留痕）；默认核心构建 + `python3 ~/vtouch-rpath-nuke.py` + md5 对账。
- **部署链（血泪）**：核心/面板 `python3 scripts/pack_su.py` → `su cp build/vtouch.sh /sdcard/vtouch.sh`（**禁止只换 /data/local/tmp**）；app `bash ~/vtouch-editor/build.sh` → `su -c 'pm install -r …'` → `force-stop`。
- **funcdoc 链**：新/改函数文案先入 `scripts/funcdoc_data.py` → `python3 scripts/apply_funcdoc.py`（先 `--check` 审单）→ 复跑 `--check` 须「共调整 0 处」。
- **旧数据零迁移**：每次部署后回归 live 操作（无拒收、旧操作可跑）。
- **真机纪律**：起核/重启核前查前台（阅读/视频在前→不动，交用户）；**用户在操作时不做输入注入**；动 live conf 前备份、测完还原+touch；起核测试用 zt 操作照「备份-还原」制。
- **提交**：每任务尾 `git add -A && git commit`；app 侧（非 git）改动前备份 `app/src → src.bak-v6b`（Task 3 第一步）；注释标签用 **v6B**（与本文档同源；勿占用数字批号）。
- **测试台**：当前 70 项基线；每任务跑 `am start-foreground-service -n com.vtouch.editor/.ChipService`（`.RenderService` 同款）验证，**读结果前核对 test_report.txt 时间戳**；跑前唤醒屏幕。

## Review Focus（spec 未写、最可能咬人的点——均已配测试）

1. **短路 + 判定异常**：被短路跳过的找图不写 fx/fy、不触发异常；被求值的异常=中止 → Task 2 · zt10。
2. **终止·完成 vs 中止的收尾差异**：autooff 只在完成侧翻 → Task 2 · zt12/zt13。
3. **N 层跳出栈不足**：a1 ≤ 当前嵌套深度由校验层拦；执行器防御（不卡死）→ Task 1/2 · ztc7/zt14。
4. **19 链与「跳过下一步/跳转」**：结构行集合 12..19 全覆盖，漏一处 = 跨块跳漏洞 → Task 1 负例。
5. **对齐合源后的回归**：总览旧渲染行为（▼/▸、收起件）不得回退 → Task 4 · 渲染件对账。
6. **子句组编辑的步号位移**：`setClauseGroup` splice 不得破坏既有步骤对象身份/顺序 → Task 3 断言（保序）。

---

## Task 1：核心校验层（P0）

**Files:**
- Modify: `src/vt_internal.h`（:98-108 类型注释 + 新常量；:124-125）
- Modify: `src/vt_ops.c`（`op_valid` :217-320 扫描 + :564-620 字段域；`op_ctrl_cond_bad` :83）
- Modify: `scripts/funcdoc_data.py`
- Test: 设备热载（备份-测试-还原）

**Interfaces:**
- Produces: `OP_STEP_CLAUSE=19` / `OP_STEP_STOP=20` / `VT_CTRL_CLAUSE_MAX=4` —— 后续任务引用。
- Produces（校验口径）: 拒词 `子条件位置错误` / `子条件超限` / `跳出层数非法`。

- [ ] **Step 1: vt_internal.h。** 追加常量（注释指向 OPS_PLAN_V6B §3.1/§3.2/拍板①）；类型范围注释 12..18 → 结构 12..19 + 20 终止。
- [ ] **Step 2: op_valid 结构扫描。** 子条件链追踪（`clauses` 计数：LOOP/IF → 1；CLAUSE → 位置检查〔前行须 14 / 12mode3 / 19〕+ 上限 4；其余 → 0）；17/18 层数于扫描内校验（`a1 < 0 || a1 > 当前循环栈深` → 拒 `跳出层数非法`）；`has_blocks` 范围 → 12..19。
- [ ] **Step 3: op_valid 字段域。** 新增 `case OP_STEP_CLAUSE`（a1∈{0,1}、j2∈{0,1}、判定参数走 `op_ctrl_cond_bad`）；`case OP_STEP_STOP`（a1∈{0,1}、余字段 0/空）；LOOP 模式3 j2∈{0,1}、IF a4∈{0,1}（原恒 0 位改取反域）；expr 防御白名单 + CLAUSE；结构行集合 12..18 → 12..19（:635/:645）。
- [ ] **Step 4: funcdoc。** `op_valid` 文案更新（类型清单 1..20、链规则、层数）；`apply_funcdoc.py --check` 审单 → apply → 复跑 `--check` 幂等。
- [ ] **Step 5: 构建门。** `sh ~/vtouch-devbuild.sh` 绿；默认核心构建绿 + md5 对账（差异=共享头，记录）；`sh ~/vtouch-ui-check.sh` 零改动留痕。
- [ ] **Step 6: 部署。** **先查前台** → `pack_su.py` → `su cp` 到 `/sdcard/vtouch.sh` → `sh /sdcard/vtouch.sh stop` → `status` → `start`；核对日志 `engine=on` + 面板自解包。
- [ ] **Step 7: P0 验收（备份-测试-还原）。** 备份 `ops.conf → ops.conf.bak-v6b`；追加 ztc 集：ztc1 正例（14 + 19×2 且/或 + 体 + 16）、ztc2 19 无头、ztc3 头+4 子句超限、ztc4 12mode1 + 19、ztc5 19.j2=2、ztc6 20.a1=2、ztc7 17.a1=3（单循环）、ztc7b 17.a1=2（双层嵌套）应过；`su -c 'touch ops.conf'` 热载；日志核（put/被拒词）；**还原备份 + touch 热载 + 确认无拒收**。
- [ ] **Step 8: 提交 + 账本。**

## Task 2：核心执行器（P1）

**Files:**
- Modify: `src/vt_ops.c`（R 结构 :842 区；`op_pair_build` :1191；新 `op_ctrl_eval_list`；`op_begin_step` :2037/:2096 求值点 + 新 case 19/20 + :2120-2145 层数；日志）
- Modify: `scripts/funcdoc_data.py`
- Test: 设备真机（zt 集扩展 ×7）

**Interfaces:**
- Consumes: Task 1 全部常量与校验。
- Produces: `R.cl_end[]`（头→链尾）+ `op_ctrl_eval_list(head)`。

- [ ] **Step 1: R 结构 + `op_pair_build`。** 加 `cl_end[MAX_STEPS]`（无链 = 头自身；顺扫）。注释指向 spec §3.1。
- [ ] **Step 2: `op_ctrl_eval_list(head)`。** 扫链（头 + 19 行）→ 短路求值（spec §3.1 伪码：且优先、左假右跳、已真停）→ 取反（`!hit`）→ 异常口径=中止（返回 -1 透传）。TRACE 每条求值行 `op 子条件 <名> 第 K 条 <且|或> <成立|不成立>`。
- [ ] **Step 3: `op_begin_step` 分支。** LOOP 模式3（:2046）与 IF（:2098）改调 eval_list；新 `case OP_STEP_CLAUSE`（纯前进 PH_WAIT，防御）；新 `case OP_STEP_STOP`（a1=0 → `op_finish()` / a1=1 → `vt_ops_abort("终止")`；TRACE `op 终止 …`）；BREAK/CONT：弹 N-1 层后按现语义。
- [ ] **Step 4: funcdoc + 构建门**（同 Task 1 Step 4/5）。
- [ ] **Step 5: 部署**（同 Task 1 Step 6）。
- [ ] **Step 6: P1 真机判据（备份-还原制；起核前查前台）。**
  - zt10：组合·且/或 + 短路（TRACE 核被短路子句**无**求值行）；zt11：取反；zt12：终止·完成（`op 完成` + autooff 翻关）；zt13：终止·中止（`op 中止 原因=终止`、autooff 不动）；zt14：跳出 2 层；zt15：继续 2 层；zt16：旧数据回归（17/18 a1=0 照跑 + 旧操作照跑）。
  - 驱动 = 面板 [运行]（注入或请用户点；坐标口径查既往记录）→ `grep -E "op |终止|子条件" /data/local/tmp/vt_ui_core.log` 核。
- [ ] **Step 7: 提交 + 账本。**

## Task 3：app 模型 + 测试台（P2）

**Files:**
- Backup: `cp -r ~/vtouch-editor/app/src ~/vtouch-editor/app/src.bak-v6b`（开工第一步）
- Modify: `Ops.java`（常量/typeName/摘要/parseStep 边界 >20/validate 镜像/`clauseChain`·`setClauseGroup`·`headSummary`）
- Modify: `TestBench.java`（控制流模型检查扩展）
- Test: 测试台全绿 + APK 装机

**Interfaces:**
- Consumes: spec §3.1-§3.3（与核心同尺）。
- Produces: `T_CLAUSE=19` / `T_STOP=20` / `setClauseGroup` / `clauseChain` —— Task 4 复用。

- [ ] **Step 1: 备份 src.bak-v6b。**
- [ ] **Step 2: Ops.java。** 常量；`typeName`（子条件/终止）；摘要（19 子句语/20 终止/18 文案「跳过本轮剩余 → 下一轮」）；parseStep raw 边界 `>18 → >20`；validate：链校验（位置/上限/取反域/层数/集合 12..19）+ STOP 字段域；块助手：`clauseChain(steps, head)`、`headClauseCount`、`setClauseGroup(steps, headIdx, header, clauses)`（保序 splice + 上限）；`insertStep` 核对（19 不经此路径）。
- [ ] **Step 3: TestBench 控制流检查扩展。** 往返（含 19/20）；链正例/负例（位置错/超限/12m1 跟 19）；取反域；17/18 层数（超深拒）；`setClauseGroup` 直驱（加/删/改 + 保序断言）；20 字段域。
- [ ] **Step 4: 构建装测。** `bash build.sh` → install → force-stop → 测试台全绿（70 → +新项）。
- [ ] **Step 5: 存档 + 账本**（APK v23）。

## Task 4：编辑器 UI + 双视图对齐（P3）

**Files:**
- Modify: `StepEdit.java`（条件列表：子句 Seg/取反/增删；终止模式 Seg；跳出/继续层数）/ `OpsPage.java`（19 子行渲染 + 嵌套壳色带步进 + 移动禁用 + 组替换接线）/ `StepFlow.java`（渲染合源 + 19 子行）/ `Forms.java`（typeSheet + 终止）/ `Ui.java`（壳偏移助手）/ `TestBench.java`（渲染件 + 对齐矩阵）

**Interfaces:**
- Consumes: Task 3 的模型/助手。
- Produces: 用户可见面全部入口 + 对齐断言矩阵。

- [ ] **Step 1: 终块编辑器。** StepEdit：块头（14 / 12mode3）改「条件列表」（条件1 + 子条件：`且/或` Seg + 判定编辑器 + 取反 SwitchRow + [✕]；[＋ 加子条件] ≤4）；终止模式 Seg；跳出/继续层数步进。
- [ ] **Step 2: 列表 + 组替换。** OpsPage：19 子行（`N. 且 · …` 暗色、块头层）+ 点入块编辑器；块头保存走 `setClauseGroup`；[↕移动] 对 19/带子句头禁用（DIM+toast）；嵌套壳 `marginLeft=14dp×级` + 行缩进补偿（文本基线 82/138/194px 不变）。
- [ ] **Step 3: 总览合源。** `StepFlow.build` 改 `depthsOf` 驱动（+19 特例：块头层）；同源色带/指示线；▼/▸、收起件行为回归核对。
- [ ] **Step 4: 类型弹层 + 文案。** typeSheet 加「终止」；跳出/继续描述微调。
- [ ] **Step 5: TestBench 对齐矩阵。** 渲染件（`edit_ops_clause.png` 等）+ 像素剖面断言（同层集合、+56px/级、色带步进、文本基线）+ 关键文本断言；竖/宽双档。
- [ ] **Step 6: impeccable 流程 + 构建装测 + 渲染目检**（拉件人工/vision）。
- [ ] **Step 7: 存档 + 账本**（APK 重存）。

## Task 5：端到端 + 回归 + 文档（P4）

- [ ] **Step 1: 真机全链。** app 编「组合条件 + 终止」挂机脚本 → 写盘 → 热载 1–2s → [运行] 观测 → [停止] 日志片段。
- [ ] **Step 2: 回归。** 旧操作照跑；旧方案切换；新核心 + 旧 conf；新 app + 旧 conf。
- [ ] **Step 3: 文档。** `AGENTS.md` 操作口径节更新（类型表 19/20、链规则、短路口径、兼容矩阵）。
- [ ] **Step 4: skill 更新。** `vtouch-project` 增 V6B 现状段。
- [ ] **Step 5: 归档 + 报告 + 用户上机验收点清单。**

---

## Self-Review（发布前自查）

1. **Spec coverage**：§3.1→T1+T2+T3+T4；§3.2→T1(域)+T2(执行)+T3+T4；§3.3→同；§3.4→T4；§3.5→T1/T5；§3.6→T4；§5 P0..P4→T1..T5。无缺口。
2. **Step scan**：每步=一个动作+可检查结果。
3. **Type consistency**：`OP_STEP_CLAUSE/STOP`、`VT_CTRL_CLAUSE_MAX`、`cl_end`、`setClauseGroup`、ztc/zt 命名跨任务一致。
4. **Review Focus**：6 条均挂到具体测试（ztc7/zt10/zt12/zt13/对账件/保序断言）。
5. **Proportion**：与 spec 体量相当。
