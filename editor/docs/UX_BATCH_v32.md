# UX 批次 v32（2026-10-10）—— 变量名显示 / 新增块带子条件 / 按钮分级规范

三条用户反馈一回批：①引用变量应显示变量名（非编码 id）；②新增条件块同页配好的子条件 [保存] 后丢失；
③按钮臃肿——要按功能/重要程度分级，部分去掉多余内边距（重点：条件块编辑界面）。
UI 部分按项目点名口径走 impeccable 技能流程（context → craft-floor/android 参考 → 改 → 渲染件核 → detect）。

## ① 变量名显示（Ops.java / Forms.java / StepEdit.java）

- 根因：负值字段（-25..-1 = 变量引用）在列表/总览/摘要里直接打编码；编辑器框内也显示编码。
- 修（显示面）：`Ops.Step.nv()/msCell()`（Ops.java「v32」节）——摘要统一「负值 = 变量名」：
  T_TAP/T_SWIPE/T_WAIT/T_DOWN/T_BOX + condText 区域判断 @p1,p2（毫秒字段变量时不再缀 ms）。
  例：`点按 fx,fy  50ms`、`框矩形 (fx,fy)–(dx,dy)（3 秒）`、`如果 · 区域判断 r9 @fx,fy`。
- 修（输入面）：`Forms.Num` 存「编码⇄名」对照（enableVar 传入），`setValue()` 负值显名、
  `value()`/`refreshVarDisplay()` 名字→编码回解（预置 tdx..fy + 自定义槽名，中文名可用）；
  `bump()`（± 步进）对变量名 no-op（换变量走「变量」键）；`StepEdit.iv()` 改走 `n.value()`。
- 测试核：变量名显示核 v32（fx 显名 / 中文名往返 / 无槽名保持编码 / 摘要显名）。

## ② 新增块带子条件随插（OpsPage.java / StepEdit.java）

- 根因：`renderStepEditor` 保存分支 `stepIndex >= 0` 才处理 `collectedClauses()`；**新增步（-1）直接
  `insertNewStep(se)`，子条件箱被整体丢弃**。
- 修：保存逻辑抽为 `saveStepEdit()`（[保存] 按钮与测试直驱共用）；`insertNewStep(at,se,ctx,clauses)`
  重载——块头 + 结构配套 + 子条件合计预检（canClause / CLAUSE_MAX / MAX_STEPS），插入后
  `setClauseGroup` 一次落位，异常回滚；toast 带「（子条件 N 条）」。
- 测试核：新增块带子条件核 v32（同页加箱 → 保存随插 [IF,19,ELSE,ENDIF] / 编辑组替换保序 /
  次数循环拒挂 / 渲染件 edit_ops_addclause.png + 关键文本断言）。
- 新钩子：`startNewStepForTest` / `stepEditorForTest` / `clauseAddForTest`。

## ③ 按钮分级规范 v32（impeccable · Operate + android.md 口径）

层级（由重到轻；实心同级唯一、其余全靠描边/文字/着色表达状态）：

| 层级 | 形态 | 用法 | 实现 |
|---|---|---|---|
| 主操作 | 琥珀实心深字 | 每屏/每卡唯一主按钮（保存 / 完成 / ＋新建操作 / ＋添加步骤） | btnPrimary / btnSmallPrimary |
| 次操作 | 1dp 描边透明底 | 页面级次要动作（取消 等） | btnSmall / btn |
| 工具/行内 | 文字键（无盒，涟漪） | 工具栏、列表行（移动/复制/编辑/删除 ✕） | btnFlat |
| 危险 | 文字键或 20% 柔色底 | 删除类（整块删除确认用 tonal） | btnSmallTonal / ✕red |
| 输入辅助 | 细描边小键 | 字段行「变量 / 取点」 | **新增 Ui.btnAux** |
| 分段选择 | 选中=琥珀浅底粗字；未选=纯文字 | 判定类型/模式/比较符/层档 | **Forms.Seg 重设计** |

本批落地：
- `Forms.Seg`：去盒子化（原「项项带描边盒子」= 臃肿源）——未选纯文字、选中琥珀 0x2E 浅底 + BOLD；
  minHeight 40dp（触控底线）；**权重 = 标签字数 + 1**（等宽格在窄容器裁 4 字标签，见「坑」）；
  字号 13→12。
- `Forms.Num`：± 44×46 → 40×40、字形 20→18；「变量/取点」实底盒子 → `Ui.btnAux` 细描边小键。
- `Forms.Chips`：内边距 14/9 → 12/7、字号 12.5（保留胶囊形态 = 标准选择控件）。
- `OpsPage` 列表头 [＋ 新建操作] → 实心主按钮（页面唯一主操作）。
- 已去盒子化不动的：v13.6 画布工具行、壳层页签。条件块编辑界面（CondBox）随 Seg + btnAux 全局受益。

### 坑（重要）

1. **`TextView.setSingleLine(true)` 在离屏软件渲染下会让整格文字不绘制**（加权 LinearLayout 内实锤：
   seg 探针 V0/V4 txt=0px；去掉即恢复 899px；`setMaxLines(1)` 走另一条布局路径渲染正常）。
   需要单行：用 `setMaxLines(1)`，勿用 setSingleLine/ellipsize 组合。
2. **等宽 Seg 格在窄容器会裁 4 字标签尾字**（子条件卡内格宽 195px < 文本 172+32px）。
   修 = 权重按字数（len+1，实测最紧余量 +13px）；并留**永久体检核**：
   `StepEdit.segFitDetailForTest()` 逐格比较「可用宽 vs measureText」，并入「条件列表编辑器」检查
   （报告行显式带 `判定行=适配(最紧余量 Npx)`）。
3. 视觉模型对「有没有文字/裁没裁字」在小字号下不可靠（本轮两次误判方向），判据一律：
   像素剖面（DIM/琥珀色像素行带/字距链）+ **设备侧几何体检**（格宽 vs measureText）。

## 验收证据（2026-10-10）

- 测试台 **83/0/0**（mode=all，~27s；+变量名显示核、+新增块带子条件核；判定行体检并入既有检查）。
- 报告行：`判定行=适配(最紧余量 13px) · 溢出核 无`；`变量名显示核 v32` 全 true；`新增块带子条件核 v32` 全 true。
- 渲染件：edit_ops_clause.png（三处判定行逐字完整、无空色块）、edit_ops_addclause.png（如果·变量 fx > 0 → 且 · 变量 fx > 0 → 否则 → 结束）、edit_step.png、render_p1.png（＋新建操作 = 实心主键）。
- 装机对账：out/vtouch-editor-v32.apk md5 = 914067f6...（逐字节已装设备）；impeccable detect = []。
- 新钩子（供后续回归）：`OpsPage.saveStepEdit/insertNewStep(4参)/startNewStepForTest/stepEditorForTest`、
  `StepEdit.clauseAddForTest/segFitDetailForTest`、`Forms.Seg.itemsCountForTest/itemForTest`。

待用户上机手测：①变量字段显名与回解（含中文名）；②新增条件块同页配子条件保存；③按钮观感
（Seg 无盒、变量/取点小键、± 变小、新建操作实心）。
