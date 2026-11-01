# UX 批次 v26 —— 切页保留编辑 / 移动全覆盖 / 添加即定位（2026-10-09）

用户三条反馈的落地记录（编辑器 APK，`com.vtouch.editor`；APK 存档 `out/vtouch-editor-v26.apk`，
装机 md5 `5c43bcf78fe23501af7dbd0eba7585ff`，逐字节一致）。

## 目标与改动

### 1. 切换页签不再丢失「正在编辑的内容」
- 真因：`Shell.showTab()` 每次切页都调 `refreshCurrent()` → 各页 `applyLoad()` 无条件
  `editing = null` → 编辑态被清掉、回列表（连点当前页签也会触发）。
- 改法：
  - `Shell.showTab()`：目标页有编辑会话（`pageBusy(i)`，区域/操作两页）→ **跳过数据重载**，
    仅重挂视图 + 刷头部。视图是同一批对象，切走再切回原位保留（含未提交的输入）。
  - `Shell.refreshCurrent()`（⟳）：编辑中 → toast「编辑中 · 已保留现场（完成或取消后刷新）」，
    不重载；返回 boolean 供按钮区分提示。
  - 兜底：`OpsPage.applyLoad()` / `RegionsPage.applyLoad()` **不再清空 editing/stepEditing**
    （其他刷新路径调用时也保持现场）。
- 锚点：`Shell.showTab/refreshPage/pageBusy/refreshCurrent`、`OpsPage.busy/applyLoad`、
  `RegionsPage.busy/applyLoad`。

### 2. 移动全覆盖（逻辑所属一起移动）
- 口径（拍板默认值，用户可改）：
  - **普通步骤**：移动自身（行为同旧版，落点仍可选全部合法位）。
  - **块头（循环/条件块）**：`[↕ 移动]` = **整块移动**——子条件 + 块体 + 否则 + 块尾
    一起走（单元 = `pairTail` 配对区间）；对话框标题标「整块（N 步）」。
  - **子条件行（19）**：`[↕ 移动]` = 组内换序（第 j 条 ↔ 第 j 条；求值序 = 短路序）。
  - **块尾 / 否则行**：结构标记行，**无独立移动按钮**（随块头整块移动）——如需要可再补。
  - 落点语义 = 「单元首行最终落位」；`k=idx+1`（原位）与越界不列出。
- 护栏：`Ops.structureErr()`（= 结构扫描 `structureScanErr` + 跨块跳 `crossBlockErr`，
  从 `validate` 抽出、文案不变）对移动**前后**预检——会把原本完好的结构弄坏 → 回滚 +
  toast 给原因（如「跳出不在循环内」「子条件位置错误」）；原本就破损的数据不拦。
- 锚点：`OpsPage.moveUnitTo/moveClauseTo/moveStepDialog/clauseMoveDialog`、
  `Ops.unitTail/structureScanErr/crossBlockErr/structureErr`（`Ops.validate` 改为复用同函数）。

### 3. 添加步骤后自动定位到新步骤（移动后同）
- `OpsPage.focusStep(idx)`：记录 `focusIdxRequest`，下一帧做**最小滚动**（12dp 边距，
  目标行滚入视野；已可见则不动）；总览模式下目标在收起块里 → 先自动展开。
  **时间护栏**：`View.post` 回调可能早于本帧布局（首帧 getHeight()==0）——行在但未量好时
  保留请求并重投（≤10 次），量好即滚（否则设备端会静默不生效；测试台有「未量测重试核」）。
- 触发点：底部 [＋ 添加步骤]、块头 [＋]、否则行 [＋]（统一走新抽出的
  `insertNewStep()`），以及 `moveUnitTo/moveClauseTo` 成功之后。
- 锚点：`OpsPage.insertNewStep/focusStep/applyFocusNow/computeFocusScrollY/
  focusMetricsForTest`、`StepFlow.build(…, rowSink)` 行视图表重载。

## 验证（证据）

- 测试台：**75/0/0**（v26 – 72 基线 + 3 新核），干净轮 29.2s；
  新核 = 「移动核 v26」「会话保留 v26」「添加定位 v26」（含直驱断言与回滚断言）。
- 渲染目检：`edit_ops_blocks.png`（块头 [↕ 移动]+[＋]、子条件行 [↕ 移动]，无版式挤压）、
  `move_dialog.png`（「移动『3. 条件块』整块（5 步）到…」文案正确）。
- 构建：`bash build.sh` 一次通过；装机 md5 与 out 存档一致（逐字节）。
- 环境备注：跑测试台期间息屏（LcdOff）会触发 ColorOS 速冻（连可见浮窗也冻，
  `freeze uid: 10249 … scene: LcdOff`）→ 套件卡半路 / mainSync 超时 FAIL。可靠跑法 =
  临时 `screen_off_timeout 600000` + 运行期保活循环（`input keyevent 224`，≤5s 间隔），
  跑完还原（本次已还原 120000）。

## 待用户上机手测

1. 编辑操作（或步骤）中途切到别的页签再切回 → 应仍是编辑页；点 ⟳ 应提示「已保留现场」。
2. 「1. 循环」/「3. 条件块」的 [↕ 移动] → 对话框标「整块（N 步）」→ 移到末尾后整块
   （含子条件/否则）一起走；子条件行 [↕ 移动] 组内换序。
3. 添加步骤（含块内 [＋]）保存后 → 列表自动滚到新步骤；移动后也自动滚到落位。
