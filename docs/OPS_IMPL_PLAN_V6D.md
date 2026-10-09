# OPS_IMPL_PLAN_V6D —— 提示步骤（实施计划）

拍板：2026-10-09 夜「按推荐」（OPS_PLAN_V6D §6 九项全按推荐默认）。执行方式：**Native 逐任务**（单设备）。
账本：`~/vtouch-editor/.v6d-progress.md`。基线：核心 `fb23f75c`（v6C）+ 编辑器 v27。

## Task 1 —— 契约 + 校验层

- `src/vt_internal.h`：`OP_STEP_TOAST 21`；`struct vt_state` 加 `toast_seq` / `toast[256]`（含内存序写注：先写文本 → release 存 seq）。
- `src/vt_shm.h`：`VT_SHM_VERSION` 13 → 14 + 历史注一行。
- `src/vt_ops.c`：新静态 `op_toast_tpl_check`（模板语法：`{名}` 配对、名 ∈ 预置 7 + op->vars、`{{`/`}}` 转义、字节 ≤63、占位 ≤16）；`op_valid` `:380` switch 加 case 21（字段域 + 模板检查，拒词 `模板错: …`）。
- funcdoc 词条（op_valid / 新 helper）+ apply 链幂等绿。
- **验收**：设备热载 P0 五门（ztd1 put / ztd2 未闭合拒 / ztd3 未知名拒 / ztd4 空模板拒 / ztd5 残留字段拒）。

## Task 2 —— 执行器

- `src/vt_ops.c`：新静态 `op_toast_render`（逐段渲染；预置按 trig/res 掩码、自定义按 vmask；无值 → 中止口径；llround+夹取；溢出截断 255B）；`op_begin_step` `:2042` switch 加 case 21（写 state 文本 + release seq + 恒打日志 `op 提示 <文本>` + `PH_WAIT` 单拍）。
- devbuild 绿；负载有界（单步 ~µs 级 + 一次 memcpy ≤256B）。
- **验收**：ztd6（calc e9=5 → `第 {e9} 次` → 日志文本对）/ ztd7（纯文本）/ ztd8（`{tdx}` 手动跑 → 中止 `变量无值`）。

## Task 3 —— 面板

- `src-ui/ui_glue.c`：`vtouch_toast_seq()` / `vtouch_toast_take(buf,cap)`（acquire 读 seq vs 本地 seen；对照 `vt_shm_ui_test_take`）。
- `src-ui/vtouch_ui.cpp`：主循环新 seq → 显示窗口（单调钟 + ms，默认 2000）→ 顶前景绘制（顶部居中、深底 70% 黑、圆角、白字、末 300ms 淡出）；文本变化时 `ui_glyph_scan(text)`（动态字形）。
- **验收**：`sh ~/vtouch-ui-check.sh` 语法绿 + devbuild 绿；绘制效果留 T5 目视。

## Task 4 —— 编辑器

- `Ops.java`：`T_TOAST = 21`、typeName、parseStep 闸 `>20`→`>21`、validate 镜像模板校验、summary。
- `StepEdit.java`：TYPE_NAMES/TYPE_IDS + build case（模板 Text + [插入变量] + 时长 Num + 说明 + 实时预览行）。
- `Forms.java`：typeSheet 条目。
- `TestBench.java`：控制流 v6D 用例（模型往返 / validate 正负例 / 摘要 / condErr 镜像）+ 渲染件 `edit_step_prompt.png`（入 shots-pull 清单）。
- **验收**：javac 绿、测试台 x/0/0、拉件目检。

## Task 5 —— 真机验收

- P0（导入日志五门）+ P1 日志（ztd6-8）+ **P1 目视（用户确认：浮层出现 / 位置 / 时长淡出 / 覆盖 / 中文字形）**。
- 收场：双侧还原 + 无 TRACE 干净核；验收记录 `docs/OPS_V6D_ACCEPTANCE.md`。

## 终审

- 独立子代理只读评审（核心 diff + 面板 diff + 编辑器 diff，≤12 findings）；需修项一次修完复测。
