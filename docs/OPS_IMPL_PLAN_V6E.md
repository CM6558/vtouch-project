# OPS_IMPL_PLAN_V6E —— 框区域步骤实施计划（Task 1–5）

依据 `OPS_PLAN_V6E.md`（§6 九项已拍板：全按推荐）。Native 逐任务；每任务 = 代码 + 构建/检查绿，Commit 可回溯。

## Task 1 — 核心常量与契约

- `src/vt_internal.h`：`#define OP_STEP_HL 22`；`struct vt_state` 加 `int hl_seq; char hl_name[VT_NAME_BUF]; int hl_ms;`（注释：核心文本先写、ms 再写、seq 最后 release；面板 acquire 认领+读期重读，同 toast 通道模式）。
- `src/vt_shm.h`：`VT_SHM_VERSION` 14 → 15 + 历史注（v15：hl_seq/hl_name/hl_ms 框区域高亮通道）。
- 门：devbuild 语法过（无功能）。

## Task 2 — 核心校验 + 执行

- `src/vt_ops.c` `op_valid` 加 `case OP_STEP_HL`：ref 必填且过 `vt_id_ok`（拒词 `区域引用非法`）；ms 0..60000（拒词 `时长越界`）；a1..a4/j1/j2 全 0 / expr 空（拒词 `字段必须为 0`）；`OP_VALID_DOC` 域更新。
- `src/vt_ops.c` `op_begin_step` 加 `case OP_STEP_HL`：`g.regions[]` 按名查 → 找不到 `vt_ops_abort("区域不存在")`；找到 → `hl_name` 文本先写、`hl_ms` 再写（0→3000）、`hl_seq++`（release）；日志 `op 框区域 <id>（<ms>ms）`；单拍顺延（PH_WAIT）。
- `scripts/funcdoc_data.py`：`op_valid` / `op_begin_step` 词条更新。
- 门：funcdoc 幂等 + devbuild 绿 → 核心 `hl_*` 构建。

## Task 3 — 面板

- `src-ui/ui_glue.c`：`vtouch_hl_seq(void)` / `vtouch_hl_take(char *id, int cap, int *ms)`（对照 toast 两件套）。
- `src-ui/ui_stubs.c`：同款空实现。
- `src-ui/vtouch_ui.cpp`：原型 + `draw_region_hl(sw, sh)`（seq 变化认领 → 窗口= now+ms(0→3000) → 每帧按名查区域〔`vtouch_get_region` 循环〕→ 复用 :2277-2285 换算〔矩形 min/max、圆换圆心〕→ 琥珀描边 lw5 + 12% 填充 + 呼吸脉冲 + 左上 id 标签〔深底白字，`ui_glyph_scan` 认字〕 + 末 300ms 淡出；**不经 `g_ov_show` 闸**）→ `draw_frame` 里 `draw_toast` 旁调用。
- funcdoc 词条（`vtouch_hl_take`）。
- 门：`~/vtouch-ui-check.sh` 全绿（error=0）+ 全链 devbuild 绿。

## Task 4 — 编辑器（v29 目标）

- `Ops.java`：`T_HL = 22`、TYPE_NAMES「框区域」、解析闸 `>21`→`>22`、validate 镜像（ref 必填 `区域引用非法` / ms `时长越界` / 余字段）、summary「框区域 · <id>（<秒> 秒）」、expr 白名单不含 22（expr 必须空 ✓ 已有 pre-gate 处置）。
- `StepEdit.java`：TYPE_NAMES/TYPE_IDS + build case（区域胶囊 = 全部 regionIds〔含停用〕+ 时长 Num「显示时长 ms（0 = 默认 3 秒）」+ 说明行）+ read 接线。
- `Forms.java`：typeSheet 加「框区域」（基础组）。
- `TestBench.java`：⑤ 框区域核（往返 / 校验镜像正负例 / 摘要 / 渲染件 `edit_step_box.png`）→ **78 项**。
- 门：javac + 构建装机 + 测试台 78/0/0 + 渲染件目检。

## Task 5 — 真机验收（TRACE 起核 + 双侧载荷 + 用户目视）

- P0（导入日志）：zte1 合法 put；zte2 ref 非法拒 `区域引用非法`；zte3 残留字段拒 `字段必须为 0`；zte4 ms 越界拒 `时长越界`。
- P1（日志+目视）：zte5 `框区域 c1 3000` → 琥珀高亮约 3 秒 + 自动消失；zte6 `{不存在}` → 中止 `区域不存在`；zte7 圆区域照画；**显隐两态**各跑一次（隐=只高亮框；显=叠加）。
- 收场：双侧还原（`.bak-v6e-pre`）+ 无 TRACE 干净核 + 验收记录 `docs/OPS_V6E_ACCEPTANCE.md` + 双仓提交 + 技能/账本。
