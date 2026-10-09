# OPS_V6 控制流 —— 实施计划（核心 → 面板 → app）

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans（已拍板 **Native 逐任务**）。任务按序执行；每步 checkbox 跟踪，任务完成 = 末步验收全过。

**Goal:** 操作模型加原生结构化控制流（循环/条件块/跳出/嵌套 = 类型 12..18），契约升 v12、旧数据零迁移；编辑器落入形态 C（色带块）。

**Architecture:** 核心在既有扁平步骤语义上加"块结构"——结构行复用 `step` 行格式（文件版本号不动）；执行器加配对表 + 块栈；app 扩模型/校验/块编辑器；面板只镜像常量。三侧同尺校验。

**Spec:** `docs/OPS_PLAN_V6.md`（字段映射 §3.2 / 运行时 §3.3 / 校验 §3.4 / 兼容矩阵 §3.6 / 验收门 §5 / 拍板表 §6）

**Tech Stack:** C（设备侧 clang 构建）/ C++（面板 clang++ -fsyntax-only 自检）/ Java（javac+d8 手写管线）。

## Global Constraints

- **文件格式不变**：`#vtouch-ops v5` 与 step 行格式、版本号都不动；新结构 = 类型 12..18 的标准 step 行。
- **VT_UI 守卫**：ops 相关 C 代码在守卫内；`vt_internal.h` / `vt_shm.h` 属共享路径——改动=有意变更，任务报告显式记录（默认核心二进制会变，属预期）。
- **构建门（设备侧）**：`sh ~/vtouch-devbuild.sh`（clang -Werror 全编）每任务尾必过；面板另有 `sh ~/vtouch-ui-check.sh`；默认核心（无面板）用 `clang -O2 -Wall -Wextra -Werror -D_GNU_SOURCE src/*.c -L/system/lib64 -lm -o build/vtouchd` + `python3 ~/vtouch-rpath-nuke.py`（md5 与基线对比、差异须能解释）。
- **部署链**：核心/面板 `python3 scripts/pack_su.py` → `su cp build/vtouch.sh /sdcard/vtouch.sh`（禁止只换 /data/local/tmp）；app `bash ~/vtouch-editor/build.sh` → `su -c 'pm install -r …'` → `force-stop`。
- **funcdoc 链**：核心新增/改函数按仓库体例（Doxygen + `(vtouch-doc: 名字)`）；文案先入 `scripts/funcdoc_data.py` → `python3 scripts/apply_funcdoc.py` → `--check` 必须"共调整 0 处"。
- **旧数据零迁移**：每次部署后回归 live 操作（加载无拒收、旧操作可跑）。
- **真机纪律**：起核前查前台（阅读/视频在前→不起，交用户）；不注入用户屏幕；动 live conf 前先备份、测完"还原+touch 热载"。
- **提交**：每任务尾 `git add -A && git commit`；开工前先把工作树现存未提交项（.gitignore / AGENTS.md）单独提交归拢；app 侧（非 git）改动前备份 `app/src → src.bak-v15`。

## Review Focus（spec 未写、最可能咬人的点——均已配测试）

1. **按住中跳出**：循环内"按下未弹起"时触发 break/循环退出——收尾必须释放虚拟槽（否则触摸卡死）→ Task 2 · zt6。
2. **无限循环停止**：面板 [停止] 在任意块深度立即中止 → Task 2 · zt3。
3. **结构行不占旧跳转守卫**：块循环回跳不增加 `R.jumps`（防"循环 300 轮→跳转超限"误伤）→ Task 2 · zt1（轮数 300）。
4. **判定异常口径**：到条件/条件块的判定异常（模板不存在 / 无画面）**与既有条件步同口径=中止**（挂机时快速暴露问题，不得静默空转）→ Task 2 · zt8。
5. **空体块**：`12` 紧邻 `13` 空循环合法且即刻按预算收敛（不卡顿）→ Task 2 · zt7。

---

## Task 1：契约 v12 + 校验层（P0）

**Files:**
- Modify: `src/vt_internal.h`（:79-80；:82-95 后追加常量；:111 旁；:182-195 注释）
- Modify: `src/vt_shm.h`（:30；:111-113）
- Modify: `src/vt_ops.c`（`op_valid` :99 起）
- Modify: `src-ui/vtouch_ui.cpp`（:909）
- Modify: `scripts/funcdoc_data.py`
- Test: 设备热载（备份-测试-还原）

**Interfaces:**
- Produces: `OP_STEP_LOOP=12 / OP_STEP_ENDLOOP=13 / OP_STEP_IF=14 / OP_STEP_ELSE=15 / OP_STEP_ENDIF=16 / OP_STEP_BREAK=17 / OP_STEP_CONT=18`、`VT_CTRL_MAX_DEPTH 8`、`MAX_STEPS 64`、`VT_SHM_VERSION 12` —— 后续所有任务引用这些名字。

- [x] **Step 1: vt_internal.h 常量。** MAX_STEPS 32→64；`OP_STEP_FINDCOLOR 11` 后追加 7 个类型常量（每行注释指向 `OPS_PLAN_V6 §3.2`，字段摘要一行）；`VT_OPS_JUMP_MAX` 旁加 `#define VT_CTRL_MAX_DEPTH 8 /* 控制流块最大嵌套深度（OPS_PLAN_V6 §3.4） */`；vt_step 类型枚举注释补"12..18 = 控制流（OPS_PLAN_V6 §3.2）"。
- [x] **Step 2: vt_shm.h。** `VT_SHM_VERSION 11u → 12u`（注释链补"12 = 控制流块（steps 64）"）；区 B `_Static_assert` 8192→12288、上方注释同步（"装进三页"）。
- [x] **Step 3: 面板常量。** `src-ui/vtouch_ui.cpp:909` `OPS_MAX_STEPS 32→64`（含注释）。
- [x] **Step 4: op_valid 结构校验。** 按 spec §3.4 在 `op_valid` 里实现（单次预扫描 + 局部栈）：
  - 预扫：`depth`、块头栈 `[VT_CTRL_MAX_DEPTH]`、每步 `parent[i]`（直接父块头索引，-1=顶层）、`hasBlocks` 标记。12/14 push（depth>8 → 拒 `嵌套超深`）；13 pop（顶非 12 → `块配对错误`）；15（顶非 14、或重复 else → `块配对错误`）；16 pop（顶非 14 → `块配对错误`）；17/18（栈内无 12 → `跳出不在循环内`）；扫毕栈非空 → `循环未闭合`。文案带步号（`第 N 步 …`）。
  - `hasBlocks` 时：跳转步（8）目标、条件步（6/7）档位=跳转时 j1/j2、以及档位=跳过下一步（1）的被跳过行（`i+1<n` 时）——目标/被跳过行是结构行（12..18）或 `parent` 与起点不同 → 拒 `跳转跨越块边界`。（目标 0=结束不受限。）
  - 字段域（spec §3.2 表）：12（模式 0..3；模式 0：a2∈1..99999 且余字段空；模式 1：a2∈1000..86400000；模式 2：全 0；模式 3：判定类型/参数按表域）、13/17/18（全 0/空）、14（类型 0..3 + 参数域）、15/16（全 0/空）。
- [x] **Step 5: funcdoc。** 更新 `scripts/funcdoc_data.py` 中 `op_valid` 文案（类型清单 1..18）；`python3 scripts/apply_funcdoc.py` → 复跑 `--check` 期望"共调整 0 处"。
- [x] **Step 6: 构建门。** a) `sh ~/vtouch-devbuild.sh` 绿；b) 默认核心命令（见 Global Constraints）构建绿，md5 与改动前基线对比并**记录差异原因**（共享头 MAX_STEPS 所致，预期）。
- [x] **Step 7: 部署。** 查前台 → `pack_su.py` → `su cp` 到 `/sdcard/vtouch.sh` → `sh /sdcard/vtouch.sh stop` → `status` → `start`；核对新面板自解包日志与 `engine=on`。
- [x] **Step 8: P0 验收（备份-测试-还原）。** a) 备份 `ops.conf → ops.conf.bak-planv6`；b) 追加 5 条测试操作：`zt1`（正例：循环×2 含等待）、`zt2`（未闭合）、`zt3`（交叉嵌套）、`zt4`（跳出悬空）、`zt5`（跨块跳）；c) `su -c 'touch ops.conf'` 热载；d) 日志核：zt1 `op 编辑 put`、zt2..5 各一条 `op 被拒`（原因词：循环未闭合/块配对错误/跳出不在循环内/跳转跨越块边界）；e) **还原备份** + touch 热载 + 确认全量恢复（回归：无拒收）。
- [x] **Step 9: 提交。** `git add -A && git commit -m "feat(core): 契约 v12 + 控制流校验层（类型 12..18）"`。

## Task 2：核心执行器（块栈 + 12..18）（P1）

**Files:**
- Modify: `src/vt_ops.c`（R 结构 :514 区；执行分支 :1455-1670；`vt_ops_run` :1841；日志区）
- Modify: `scripts/funcdoc_data.py`
- Test: 设备真机（zt 操作集 ×8）

**Interfaces:**
- Consumes: Task 1 全部常量。
- Produces: R 内 `blk[]/blk_n/pair[]`（文件内静态，无跨文件接口）。

- [x] **Step 1: R 结构扩展**（:514 区）：`struct { int head, mode, remain; uint64_t start_ms; } blk[VT_CTRL_MAX_DEPTH]; int blk_n; int pair[MAX_STEPS];`（注释指向 spec §3.3）。
- [x] **Step 2: `op_pair_build()`**（新静态，funcdoc 体例）：扫描 steps 填 `R.pair`（12↔13、14↔[15]↔16，合法输入由校验层保证）；`vt_ops_run`（:1841）清零 `blk_n` 后调用。
- [x] **Step 3: `op_begin_step` 分支（12..18）**，按 spec §3.3：
  - 12：回跳到达（`blk_n>0 && blk[top].head==i`）→ 模式 0：`remain--`，`>0` 进体否则弹栈+跳 `pair(i) 之后`；模式 1：`now-start<V` 进体否则同；模式 2：进体；模式 3：求值判定——成立→弹栈+跳 13 之后；不成立→进体。首次到达→压栈初始化（remain/start）。
  - 13：跳到栈顶 `head` 处（`R.step = head-2; PH_WAIT`，同 `op_jump_apply` 落位法，**不加 `R.jumps`**）。
  - 14：求值 → 成立进体；不成立跳 `15+1`（有 else）或 `16 之后`。15：跳 `16 之后`。16：无动作。
  - 17：弹栈至最近循环 → 跳其 `pair(13) 之后`；18：跳其 `13` 处。
  - 判定求值：区域判断/开关判断复用既有判定代码路径；找图/找色走 `vis_exec_find`（命中照写 fx/fy）；**异常口径与既有条件步一致（中止）**。
- [x] **Step 4: 日志**（spec §3.8）：TRACE 五类（轮/退出原因/条件块/跳出/继续）；循环回跳默认静默。
- [x] **Step 5: funcdoc + 构建门**：`op_pair_build` 等新函数文案入 `funcdoc_data.py` → apply → `--check` 幂等；`sh ~/vtouch-devbuild.sh` 绿。
- [x] **Step 6: 部署**（同 Task 1 Step 7）。
- [x] **Step 7: P1 真机判据（zt 集，全部日志核）**：
  - zt1：次数×3 + 等待 100ms → `op 完成` 用时 ≈300ms；**另造 300 轮版**（次数 300 + 等待 0）→ 完成且**无 `跳转超限`**（守卫不受新语义影响）；
  - zt2：时长 3s → 用时 ≈3s；
  - zt3：无限空循环 → 起跑后 [停止] ≤1s 停（`op 完成/中止` 日志）；
  - zt4：到条件·区域判断（zt 测试区域，判定点=其中心）→ 立刻退出；
  - zt5：嵌套 2 层 + 内层跳出 → TRACE 轮序正确；
  - zt6：按住（按下 300,400）→ 循环 { break } → 收尾 `op 收尾 松开`、无槽占用中止；
  - zt7：空体循环×5 → 即时完成；
  - zt8：到条件·找图（不存在的模板）→ 中止 `模板不存在`（口径与条件步一致）；
  - zt9（回归）：旧 zt0（含跳转步旧操作）照跑。
  - 驱动方式：起核（查前台）→ 面板 [运行]（一次点按：注入或请用户点）→ 日志核。测试用 zt 区域/操作照旧"备份-还原"。
- [x] **Step 8: 提交。** `feat(core): 控制流执行器（块栈 + 类型 12..18）`。

---

## Task 3：app 模型 + 测试台（P2）

**Files:**
- Backup: `cp -r ~/vtouch-editor/app/src ~/vtouch-editor/app/src.bak-v15`（开工第一步）
- Modify: `~/vtouch-editor/app/src/com/vtouch/editor/Ops.java`
- Modify: `~/vtouch-editor/app/src/com/vtouch/editor/TestBench.java`

**Interfaces:**
- Consumes: spec §3.2 字段映射（与核心同尺，逐字段镜像）。
- Produces: `Ops.T_LOOP=12 … T_CONT=18` 常量、扩展的 `Ops.validate`、测试台新检查「控制流模型」—— Task 4 复用。

- [ ] **Step 1: 备份** `app/src → src.bak-v15`。
- [ ] **Step 2: Ops.java 模型层。** `MAX_STEPS 64`；常量 `T_LOOP..T_CONT`；`typeName()` 加 7 个名字（循环/循环结束/条件块/否则/条件块结束/跳出循环/继续循环）；`Step.summary()` 新分支（如 `循环 · 持续 30 分钟` / `如果 · 找图 t1` / `结束循环` / `跳出循环`）；`parseStep` 的 raw 边界 `>11 → >18`；`serialize` 核对（type 数字照写，无分支改动）；`validate` 加结构校验（镜像 Task 1 Step 4：配对/深度/17-18/跨块跳/字段域，文案同词表）。
- [ ] **Step 3: TestBench 新检查「控制流模型」。** 断言组：a) 往返（含块 conf：parse → serialize → 再 parse，字段逐项一致）；b) validate 正例过；c) 未闭合 / 交叉嵌套 / 跳出悬空 / 跨块跳 各拒；d) 深度 9 拒；e) 字段域（次数 0 拒；时长 86400001 拒）。
- [ ] **Step 4: 构建装测。** `bash ~/vtouch-editor/build.sh` → `su -c 'pm install -r …'` → force-stop → 测试台全绿（63 → +1 项）。
- [ ] **Step 5: 存档。** out/vtouch-editor.apk 照录；进度记账。

## Task 4：app 编辑器 · 形态 C（P3）

**Files:**
- Modify: `Ui.java`（色带件）/ `StepEdit.java`（块头尾与属性编辑）/ `OpsPage.java`（插入/移动/删除）/ `StepFlow.java`（块树）/ `TestBench.java`（渲染件与断言）

**Interfaces:**
- Consumes: Task 3 的 `Ops.T_*` 与 validate。
- Produces: 块编辑 UI 全部入口（用户可见面）。

- [ ] **Step 1: Ui 色带件。** 循环块 = 琥珀 6% 叠色底 + 左 3dp 色条；条件块 = slate 4% 底 + 左条；嵌套叠深。块头/尾行文本件。
- [ ] **Step 2: StepEdit 块渲染 + 属性编辑。** 块头行（类型 + 参数摘要，琥珀粗体）；点击块头 → 属性编辑器：循环（模式 Seg [次数/时长/无限/到条件] + 值数字步进；时长按分钟输入、存 ms）；到条件/条件块 = 判定编辑（复用四类判定组件）。
- [ ] **Step 3: OpsPage 交互。** 块头 [＋] = 块尾插步；块尾 [✕] = 删整块（确认）；[↕ 移动] 支持"移出块"；bottomBar 语境（未选中→操作末尾；选中块→进该块）。
- [ ] **Step 4: StepFlow 块树。** 缩进 + 色条，与编辑页一致。
- [ ] **Step 5: TestBench。** 渲染件 `edit_ops_flow.png`（普通+嵌套）/ `edit_ops_nested.png`（选中态）；关键文本断言；双档溢出核（竖/横）；块编辑流断言（加块/改模式/进块加步/删块，API 直驱）。
- [ ] **Step 6: impeccable 流程。** `impeccable context` → 按 `reference/layout.md + craft-floor.md + android.md` 设计审查 → 实现 → `impeccable detect --json <改动文件>` 期望净。
- [ ] **Step 7: 构建装测 + 渲染目检。**（拉渲染件人工/vision 检查）→ 预留用户上机手测点。
- [ ] **Step 8: 存档。** 备份 `src.bak-v15` 保持；APK 存档。

## Task 5：端到端 + 回归 + 文档（P4）

- [ ] **Step 1: 真机全链。** 起核（查前台）→ app 编「挂机脚本」（循环·时长 + 条件块·找图 + 点按 fx,fy）→ 写盘 → 面板热载 1–2s → [运行] 观测 ≥2 分钟 → [停止]；记录日志片段。
- [ ] **Step 2: 回归。** 旧操作照跑；旧方案切换；核心 v12 + 旧 conf；新 app + 旧 conf。
- [ ] **Step 3: 文档。** `AGENTS.md` 操作口径节加"控制流"小节（类型表 + 守卫口径 + 兼容矩阵引 OPS_PLAN_V6）；编辑台侧相应记录。
- [ ] **Step 4: skill 更新。** `vtouch-project`：核心 v6 控制流现状 + 编辑台 v15（块编辑器）现状。
- [ ] **Step 5: 归档 + 报告。** 核心 git 收尾提交；app 产物存档；给用户的验收点清单（上机步骤 + 预期）。

---

## Self-Review（发布前自查）

1. **Spec coverage**：§3.1/§3.2/§3.3→T1+T2；§3.4→T1（核心）+T3（镜像）；§3.5→T1；§3.6→T1/T5；§3.7→T4；§3.8→T2；§5 P0..P4→T1..T5。无缺口。
2. **Step scan**：每步=一个动作+可检查结果；实现步仅在"签名与测试决定不了处"给算法（op_valid 扫描、执行器分支），其余只给接口与值。
3. **Type consistency**：`OP_STEP_LOOP..CONT`、`VT_CTRL_MAX_DEPTH`、`blk/pair`、zt1..zt9 在任务间一致。
4. **Review Focus**：5 条均已挂到具体测试（zt1/zt3/zt6/zt7/zt8）。
5. **Proportion**：计划与 spec 体量相当，无代码转抄。
