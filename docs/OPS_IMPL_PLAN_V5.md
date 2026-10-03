# OPS_IMPL_PLAN_V5 — 计算步骤（v5）实施计划

> **Spec（约束权威）**：`docs/OPS_PLAN_V5.md`（行为/文案唯一来源；与本计划冲突时以 spec 为准）。
> 执行方式：Subagent-Driven（每任务：精确简报 → 实现子代理 → 任务评审 → 修复环；控制方裁决并记账）。
> 任务号是精确号（1.1 / 2.1 / 3.1 / 3.2 / 4.1 / 5.1）；台账 = `.superpowers/sdd/OPS_IMPL_PLAN_V5/progress.md`。

## Global Constraints（所有任务适用）

- **构建门**（每任务收尾必跑，rc 全 0）：
  - `sh scripts/build.sh`（默认核心；**允许 md5 变化**——state 结构增长所致，报告记录新旧值；行为不得变）；
  - `sh scripts/build.sh ui`（UI 核心）与 `VTOUCH_UI_CORE=real sh scripts/build_ui.sh`（面板 real；我方文件 0 告警）；
  - `python scripts/apply_funcdoc.py --check` → 「共调整 0 处、不变式全满足」；
  - `python scripts/ci_check.py` → 10/0（打包产物变化后需先 `python scripts/pack_client.py` / `pack_su.py`——**由收尾任务 5.1 统一做**，中间任务 ci 允许因产物未重打而红，但需在报告注明）。
- **funcdoc 纪律**：`src/` 下每个函数（含 static）定义正上方紧贴唯一 Doxygen 块（含 `(vtouch-doc: 名字)`）；文案唯一来源 `scripts/funcdoc_data.py`（新增函数必须加条目）。
- **VT_UI 守卫**：默认（无面板）构建中 `vt_ops.c`/`vt_shm.c`/`vt_panel.c` 是空 TU；`vt_expr.c` 同样整体 `#ifdef VT_UI`。
- **C 口径**：`-O2 -Wall -Wextra -Werror -D_GNU_SOURCE`；注释/日志中文；日志词照 spec §7 逐字。
- **行尾**：`src-ui/vtouch_ui.cpp` 全 CRLF（编辑工具注意）；`src/*.c` 保持原行尾。
- **最小 diff**：只动本任务点名的文件/位置；不改 WS 协议；不动无关代码。
- **提交**：master 直做（v4 先例）；**不推送**（推送由用户拍板）；每任务一提交（或一小组提交），提交信息中文、带验收摘要。
- **报告**：写到工作区 `task-<N>-report.md`（状态/提交/测试证据/披露），返回只给状态+提交+一行测试摘要。

## 背景（给所有实施者）

vtouch = Android root 触摸合成引擎（EVIOCGRAB + uinput），带原生 ImGui 面板；「操作」= 面板编辑的步骤序列（点按/滑动/等待/按下/弹起/区域判断/开关判断/跳转 8 类），核心主线程执行。v5 新增第 9 类**「计算」**：`结果槽 = 表达式`。相关现状：

- 变量负数编码：step 字段 ≥0 = 字面值，-1..-5 = 触发变量（tdx/tdy/tux/tuy/tms）。解析在 `src/vt_ops.c:452-466`（`idx = OP_VAR_TDX - v`，mask 判无值）。
- 触发快照：`struct vt_trig_data { unsigned mask; int dx,dy,ux,uy,ms; }`（`src/vt_internal.h`），起跑时整组拷入，不回填。
- 步骤结构：`struct vt_step`（`src/vt_internal.h`，type + a1..a4 + ms + j1 + j2 + ref[16]）；`struct vt_op` = name+step_count+gate+auto_off+steps[32]；ops[16] 在 `struct vt_state`（区 A，尺寸随 sizeof 动态）。
- 契约：`VT_SHM_VERSION`（`src/vt_shm.h:29`，当前 6）。
- 编辑期校验：`op_valid`（`src/vt_ops.c:55`，逐类型 case）；拒收日志 `op 被拒 <名>: <原因>`。
- 执行器：核心主线程；按住期允许集在 `src/vt_ops.c:308` 附近（等待/弹起/条件步/跳转）；中止词函数在 vt_ops.c。
- 面板：`src-ui/vtouch_ui.cpp`——本地副本 `g_ope_steps[32][8]` + `g_ope_refs[32][16]`（:2951）；`ope_fidx[8][5]` 字段表（:3132）；`ope_vname_tab`（:3173）；加步区两行 4+4（`draw_op_edit` 内，`＋点按`…`＋跳转`）；子层 `draw_num_edit`（数字键盘）/`draw_char_kb`（字符键盘，含矮屏自适应+拖滚兜底 `g_kb_sc`/`SCR_KB`/`g_zone_kb`）；说明页 `g_help_lines[]`（15 条）；ops.conf 读写 `save_ops`/`load_ops`（`#vtouch-ops v3`，9 字段）。
- 胶水：`src-ui/ui_glue.c`（面板↔核心 API：`vtouch_get_op_step` :435、`vtouch_op_put` 等）。

---

## Task 1.1 — 表达式引擎 `src/vt_expr.c`（+ 头 + 宿主单测）

**产出**：新文件 `src/vt_expr.c` + `src/vt_expr.h`；宿主自测通过；funcdoc 条目。

**接口（精确；写进 `src/vt_expr.h`）**：
```c
#define VT_EXPR_OK        0
#define VT_EXPR_NO_VAR   (-1)   /* 变量无值 */
#define VT_EXPR_NO_SLOT  (-2)   /* 结果无值 */
#define VT_EXPR_BAD      (-3)   /* 表达式错 */
/* 0 = 合法；非 0 = 非法，why 填短中文原因（供面板显示/核心拒收日志）。 (vtouch-doc: vt_expr_check) */
int vt_expr_check(const char *s, char *why, size_t whycap);
/* 求值：trig_vals[0..4] = tdx,tdy,tux,tuy,tms；trig_mask 位 0..4 同序（1=有值）。
 * slots[0..3] = r1..r4；slot_mask 位 0..3（1=已写）。out 收 double。
 * 返回 VT_EXPR_OK / NO_VAR / NO_SLOT / BAD。 (vtouch-doc: vt_expr_eval) */
int vt_expr_eval(const char *s, const int trig_vals[5], unsigned trig_mask,
                 const double slots[4], unsigned slot_mask, double *out);
```
- **零核心依赖**：只 include `<math.h> <string.h> <stddef.h> <stdint.h>` + 自家头（宿主 gcc 可直接编译——本机 `/d/C/mingw64/bin/gcc` 可用）。`vt_expr.c` 整体 `#ifdef VT_UI` 包裹（默认构建空 TU）。
- **语法/语义/边界**：逐字照 spec §2（递归下降；`+ - * /`；`atan2 sin cos abs min max sqrt`（度）；小数；空白忽略；长度 ≤63、嵌套 ≤8、token ≤128；除零/域错/非有限/arity 错 → BAD；未写槽 → NO_SLOT；mask 缺位 → NO_VAR；`(0,0)` 的 atan2 = 0）。
- **`vt_expr_check` 的 why 文案**（短中文，供面板直接显示）：如「表达式为空」「超过 63 字符」「未知名字」「括号不配对」「参数个数不对」「除零」「负数开方」「表达式过长」等（实现者自定措辞，保持短、准）。
- **宿主单测**（不入库，放 `build/test_vt_expr.c`）：覆盖——四则/优先级/一元负/括号嵌套；`atan2(0,-1)=180`、`sin(90)=1`、`cos(180)=-1`、`sqrt(9)=3`、`min/max/abs`；小数；错误面（除零/域/arity/未知名/未闭合括号/超长/超深/超 token）；mask 缺位→NO_VAR、未写槽→NO_SLOT、已写槽参与运算；边界样例 `tdx + cos(atan2(tuy-tdy, tux-tdx)) * 300`。编译：`gcc -O2 -Wall -Wextra -Werror -DVT_UI -Isrc build/test_vt_expr.c src/vt_expr.c -lm -o build/test_vt_expr.exe && ./build/test_vt_expr.exe`（全过打印 `PASS n/n`）。
- **funcdoc**：`vt_expr_check`/`vt_expr_eval` 加 `scripts/funcdoc_data.py` 条目 + 源码 Doxygen 块；`apply_funcdoc.py --check` 0/0。
- **验收**：默认构建 md5 **不变**（空 TU）；`build.sh ui` rc=0；宿主单测全过；funcdoc 0/0。

## Task 2.1 — 契约 v7 + 核心集成（vt_internal.h / vt_shm.h / vt_ops.c / ui_glue.c / build_ui.sh）

**产出**：契约版本 7；核心支持「计算」步与槽引用；胶水 API 扩展；面板最小接线（编译通过，数据接线在 3.1）。

**点**：
1. `src/vt_internal.h`：
   - `#define OP_STEP_CALC 9`（注释：计算，a1=槽号 1..4、expr=表达式）；
   - `#define OP_VAR_R1 (-6) … OP_VAR_R4 (-9)`（结果槽引用；`OP_VAR_N` 保持 5 不动——它是触发变量数）；
   - `#define VT_EXPR_MAX 63`；
   - `struct vt_step` += `char expr[VT_EXPR_MAX + 1];`（含注释）；
   - `#include "vt_expr.h"`（或前置声明——按文件现有风格）。
2. `src/vt_shm.h`：`VT_SHM_VERSION 6u → 7u` + 版本注释行 `* 7 = v5 操作扩展：step.expr（计算步骤表达式）。`。
3. `src/vt_ops.c`：
   - `op_valid`：+ `case OP_STEP_CALC`——a1 ∈ 1..4；expr 非空且过 `vt_expr_check`（不过 → 拒收原因 `表达式错: <why>`）；a2/a3/a4/ms/j1/j2 必须 0、ref 必须空；**其余所有 case** 加防御：expr 非空 → 拒收 `表达式错`。
   - 允许变量的字段判据（-5..-1 → **-9..-1**）。
   - 解析助手（:452-466）：签名加 `lo, hi`；-1..-5 照旧；**-6..-9 = 槽值 llround 后夹取 [lo,hi]**（lo/hi 由调用点给：坐标 0..g_w-1 / 0..g_h-1；时长按类型档）。调用点逐处给域。
   - 执行器：运行上下文加 `double slot[4]; unsigned slot_mask;`，**起跑清零**；步分发 + `OP_STEP_CALC`：求值（trig_vals 从快照取）→ 写槽 → 日志 `op 步 k/n 计算 r1 = <int>`（llround）；错误映射中止词：`表达式错` / `结果无值`（新词）/ `变量无值`（沿用）；TRACE（`VTOUCH_OPS_TRACE`）加 `op 计算 r1 = <expr> = <int>`。
   - **按住期允许集** += 计算（:308 附近的口径）。
   - 步类型清单注释（:199 附近）补「计算」。
4. `src-ui/ui_glue.c`：
   - `vtouch_get_op_step` 加 `char *expr, int exprn`（尾参）；put 侧（`vtouch_op_put` 链）同样加 expr 数组参；
   - 新增 `vtouch_expr_check(const char *s, char *why, int whycap)`（转发 `vt_expr_check`）；
   - 面板侧声明处（`src-ui/vtouch_ui.cpp` 内 vtouch_* 声明点，grep 找）同步签名；
   - **面板调用点最小接线**：op_edit_save / 方案重放等 `vtouch_op_put` 调用点传「空表达式数组」占位（编译通过即可；真实数据 3.1 接）。
5. `scripts/build_ui.sh`（real 分支）：加一行编译 `src/vt_expr.c`（`-DVT_UI -DVT_UI_PANEL -Isrc`，obj 入 `build/ui/obj/vt_expr.o`，链接清单同步）。
6. `scripts/funcdoc_data.py`：vt_ops.c 如有新函数则加条目。
- **验收**：默认核心可编译（md5 变化记录）；`build.sh ui` rc=0；面板 real rc=0；funcdoc 0/0。**真机冒烟留 5.1**（契约 v7 需核心+面板同升）。

## Task 3.1 — 面板：存储与管线（`src-ui/vtouch_ui.cpp`）

**产出**：ops.conf v4 读写（兼容 v1/v2/v3）；表达式随编辑/保存/方案重放全链传递；加步 3×3 + 「＋计算」；摘要/预览/变量列表扩展。

**点**：
1. `#define OPS_EXPR_MAX 63`（面板独立定义，注释「同核心 VT_EXPR_MAX」）+ `g_ope_exprs[OPE_MAX_STEPS][OPS_EXPR_MAX + 1]`。
2. `save_ops`：`#vtouch-ops v4`；step 行 **10 字段** `step <type> <a1> <a2> <a3> <a4> <ms> <ref> <j1> <j2> <expr>`（expr 空写 `-`）。
3. `load_ops`：v4 解析；缺 expr（v1/v2/v3 行）→ 空；类型感知翻译照旧。
4. `op_edit_open` 读入 exprs；`op_edit_save` 经扩展后的 `vtouch_op_put` 传 exprs；**方案重放**（scheme_switch 的逐条重放操作路径）确认 expr 随行（读 conf → put 全字段）。
5. **加步区 3×3**：`[＋点按 ＋滑动 ＋等待] [＋按下 ＋弹起 ＋跳转] [＋区域判断 ＋开关判断 ＋计算]`；bw3 = (cw − 2×12)/3；栈位：add_y3/add_y2/add_y1 三行（`draw_op_edit` 内 by_bottom/done/autooff/gate/add 链加一行）；**自适应阈值行数 4→5 同步**（保持两档公式形态：`140 + bh_done + N×(bh_row+12)`、N=5；紧凑档同）。
6. 「＋计算」：新步 `{type=9, a1=1, 其余 0}`、expr 空，加完**立即开表达式子层**（`g_ope_se=i; g_ope_ex=1;`）。
7. 步行（`ope_step_row`）：计算步 `[参数]` → 开表达式子层（不开数字键盘）；`ope_step_text`/`ope_preview_row` 摘要 = `计算 → r1 = <表达式>`。
8. 变量列表（`draw_ope_vlist`）：5 触发变量 + **4 结果槽（结果1..结果4）** + 「数值」+ [取消]；`ope_vname`/`ope_vname_tab` 扩展（-9..-1）；`ne_cell_text` 显示扩展；面板字段校验的变量域扩到 -9..-1；矮屏档位保证 9+2 条全在屏内（不够加档或双列——验收=全可见）。
9. 表达式子层状态（本任务只建状态与入口，UI 在 3.2）：`g_ope_ex`、缓冲 `g_ope_expr_buf[OPS_EXPR_MAX+1]`、槽选择 `g_ope_expr_slot`；进入时从 `g_ope_exprs[g_ope_se]` 快照。
- **验收**：`build.sh ui` rc=0；面板 real rc=0；手工构造 v3 老 conf 重启加载无失配（静态走查/仿真即可，真机留 5.1）。

## Task 3.2 — 面板：表达式子层 UI + 说明页 16 条

**产出**：`draw_ope_expr`（整屏子层）+ 说明页 16 条逐字。

**点**：
1. `draw_ope_expr`（照 `draw_num_edit` 的整屏口径 + `draw_char_kb` 的矮屏自适应/拖滚兜底——复用 `g_kb_sc`/`g_zone_kb`/`SCR_KB` 与裁剪+隔帧重置）：
   - 顶：`第 N 步 · 计算 · 表达式`；**槽 chips** `[r1][r2][r3][r4]`（单选，选中高亮；写 `g_ope_expr_slot`）。
   - 中：表达式显示框（`g_ope_expr_buf` 或「(空)」）+ 右侧 `[清空]`（框宽 = cw − (btnw+12) 模式）。
   - 字符键 6×3：`1..0`、`+ - * / ( ) .`、`退格`（追加式；退格删末字符）。
   - 插入 chips 2×8：`tdx tdy tux tuy tms r1 r2 r3` / `r4 atan2( sin( cos( abs( min( max( sqrt(`。
   - 底：`[取消][确定]`；确定 → `vtouch_expr_check`（不过：红字 why、层不关）；过：写 `g_ope_exprs[g_ope_se]` + 日志 `op edit 计算 第 N 步 r1 = <expr>` + 关层；取消 → 丢弃 + 日志 `op edit 表达式取消 第 N 步`。
   - 子层分发：`draw_op_edit` 顶部（`g_ope_pv`/`g_ope_kb`/`g_ope_vl`/`g_ope_rl`/`g_ope_se` 一串里）加 `if (g_ope_ex) { draw_ope_expr(); return; }`（在 `g_ope_se` 之前）。
2. 说明页：`g_help_lines[]` → **16 条**；第 12 条更新、第 16 条新增——**逐字**照 spec §9（第 12 条含 `结果无值 / 表达式错` 两词；第 16 条整句照抄）。
- **验收**：`build.sh ui` rc=0；面板 real rc=0；说明页 16/16 与 spec §9 逐字节一致（报告附比对命令与结果）。

## Task 4.1 — 文档同步（README / AGENTS 双份）

**产出**：README.md + 仓库根 AGENTS.md + `C:/Users/21102/AGENTS.md` 三份同步（后两者逐字节一致，md5 自查）。
- 更新点（照 spec 与实现）：步骤类型 8→9（+计算）；ops.conf v3→v4（10 字段、expr、兼容 v1/v2/v3）；契约 v6→v7；中止词 +表达式错/结果无值；加步 3×3；说明页 16 条；日志增量（`op 步 k/n 计算` / TRACE `op 计算`）；「操作表落盘」段、`VT_SHM_VERSION` 段、面板段（如有固定表述）。
- **验收**：三份 md5 对齐（`md5sum` 回读）；README 内引用行号抽查。

## Task 5.1 — 收尾：全量门 + 重打包 + 真机端到端 + 台账（控制方/收尾子代理）

- 全量门（real ×3 / 默认 / funcdoc / syntax）+ `pack_client.py`/`pack_su.py` + `ci_check.py` 10/0 + 五产物 md5 全表。
- 真机（sendevent 注入法）：部署 → 构造测试 op（经面板编辑或直写 ops.conf v4）：
  a) 算术直通（`r1 = tdx + 100` → 日志值 = 触发点 +100）；b) 函数/角度（atan2 例，验证度数与符号）；c) `变量无值`（按下触发引用 tux）；d) `结果无值`（先引用未写槽）；e) v3 老 conf 兼容加载照跑；f) 按住期「计算」不中止；g) 槽当坐标端到端（点按落点经日志验证）。
- 台账 + 提交 + （用户拍板后）推送。

---

## 预检要点（控制方已核）

- 串行依赖：1.1 → 2.1 → 3.1 → 3.2 → 4.1 → 5.1；同文件（`vtouch_ui.cpp`）串行。
- 接口对账：1.1 的 `vt_expr_check/eval` 签名 → 2.1 调用；2.1 的胶水签名 → 3.1 调用；3.1 的 `g_ope_ex*` 状态 → 3.2 使用。
- 风险点：契约 v7 中间态（核心新面板旧 → 拒启；开发期同树同升即可）；默认核心 md5 变化（有意，记录）。
