# UX_BATCH_v35 —— T6 双视图编辑器（V8 操作语言化 · 编辑器侧）

> 批次：v35（2026-10-10 晚；T6.1–T6.4，V8 计划最后一批）。设计纪要 + 实现口径 + 验收记录。
> 上位：`~/vtouch-project/docs/OPS_PLAN_V8.md` §6/§8、`VT_SCRIPT_V1_SPEC.md`（r8 覆盖清单）、`OPS_IMPL_PLAN_V8.md` T6。

## 1. 批次目标

迁移（T5）后所有操作 = v8（元数据 + `progs/*.vts/.vtbc`），编辑器 v34 只能存不能编。本批把编辑器升级为**双视图**：

- **T6.1 AST → 块渲染**：脚本「投影」回块编辑器（目标模型 = 步骤表，T5.1 迁移域全量，复用现卡片/块渲染链）；
- **T6.2 文本编辑器**：语法高亮 / 行号槽 / 错误行标红 / 手机输入辅助条；
- **T6.3 模式锁 + 转换**：`#模式 块|脚本` 侧线（保存时定锁）；[转脚本] 恒可、[转块] 仅可视化子集内；
- **T6.4 测试台核 + 渲染件 + 上机手测**（本批执行前三 + 交底）。

## 2. 设计决策（推荐默认已采纳并落地）

| 决策 | 取值 | 理由 |
|---|---|---|
| 投影目标 | **步骤表模型**（23 类 + 子条件链） | 计划原话「复用现卡片/块渲染链」；与迁移域精确互逆 |
| 子集判定 | `VtToSteps.project` 内置**固定点自检**（投影产物再翻译逐字一致才通过） | 宁缺毋滥：系统性错映射会被语料核兜住；运行时双保险 |
| 模式持久 | `.vts` **头注释 `#模式 块/脚本`**（保存时归一为首行唯一一条） | 跨重装/跨工具（AI/手工）可见；迁移产物缺省 → 按「可投影→块，否则脚本」推导 |
| 脚本视图保存 | 文本**原样**（注释保留）+ 侧线归一 | 注释不丢；AI 友好 |
| 块视图保存 | steps → 规范脚本（`VtFromSteps`）→ 编译落盘 | 「块→脚本」恒可；缺省参数补显/显式默认折叠 = 规范形态 |
| 含注释脚本 | 投影**拒绝**并提示（「先删注释或留在脚本模式」） | 块侧无注释载体，防静默丢注释 |
| 脚本编辑载体 | 页内宿主 `editorHost`（与列表互斥）+ **自绘行号槽** | 不套 ScrollView（避拖拽冲突）；行号用 EditText 实际 `Layout.getLineBaseline` 对齐（中英混排不漂） |
| 高亮 | 轻量扫描器（保留字=琥珀 ACCENT / 字符串=绿 OK / 注释=灰 FAINT） | 打字中途（未闭合串/错）也稳定；与词法器同规则（最长匹配） |
| 错误面 | `VtCheck` 防抖 300ms → 行底浅红 + 行号红 + 底部错误条（可点跳行） | |
| 辅助条 | 底部横滚文字键（语句词 + 符号；`( { [ "` 成对插入、光标居中） | 手机输入辅助（块仍是一等公民，文本侧少手打） |
| 新建操作 | 默认块视图；会话内 [转脚本] 可切 | 不改变既有创建流 |

## 3. 覆盖清单（v1）与拒绝词（= SPEC §8 r8）

- **块视图全量**：点按/滑动/等待/按下/弹起/赋值（v5 表达式域）/如果·否则（判定原子 + 且·或·取反 DNF 子条件）/循环×4（次数/时长/无限/直到）/跳出·继续（层数）/终止/提示/框区域/框矩形。
- **仅脚本**：`跳转`（兼容语法）。**v1 未覆盖（脚本侧已全量可用）**：函数/表/返回/调用语句、`当`、for 计数循环。
- **字段域拒绝词**（`第 N 行: …`）：负数字面量（与变量编码冲突）/ 复合表达式（% / 字符串 / 比较链）/ 自定义变量 >16 / 变量名 >15 字 / 注释 / 子条件 >4 / 嵌套 >8 / 小数 / 颜色越界 / 「当」循环 / for / 表读写 / 空脚本。
- **可接受规范化**（保存输出规范形）：缺省参数补显（点按 ms=50、滑动时长=300）、显式默认折叠（找图 `阈值=8`、`停1=0`）、组合条件德摩根 DNF 展开。

## 4. 实现地图

- `VtToSteps.java`（新）：投影器。`project()` = 注释扫描 → 解析 → VtCheck → 投影 → 固定点自检；字段原子 = 整数≥0 字面量或单变量引用；判定原子 5 类（在区域/开关开/找图/找色/找色点/变量比较）+ DNF（德摩根）；槽位=首赋值序；限制 16 槽/15 字名/4 子条件/8 层/512 步。
- `ScriptStore.java`（新）：`modeOf/withPragma`（侧线）；`prepare/prepareRaw`（检查→编译→字节）；`updateMetaLine`（行级元数据更新）；`regionRefs/remapRegions`（AST 级区域引用，CrossCopy 用）。
- `ScriptEdit.java`（新，UI）：[meta 行 | 代码卡（槽 + EditText）| 错误条 | 辅助条]；EditText 无换行 + 内部横竖滚动；`Ed` 子类 onScrollChanged → 槽重绘；测试钩子全套（span 计数/错误行/滚动/键数）。
- `OpsPage.java`：`editMode/scriptEdit/scriptSeed/editorHost` 会话态；`startEdit` 路由（v8 读源 → 侧线 ?? 可投影推导）；`renderScriptEditor`（bar=[状态][转块][取消][保存]）；`switchToScript/switchToBlock`；`commitEdit` 双路归一（v8 记录 = 元数据+产物，steps 恒空）；`saveV8`（.vts/.vtbc/conf + 备份 + 镜像）；`deleteOp` 产物随删（`saveEx`）；列表卡「脚本 N 行」（refresh 批量 `wc -l`）；属性对话框（门控/自动关，与块页同源组件）。
- `CrossCopy.java`（v8 补链）：`Vts` 脚本源口；`opRegionRefs(o,vts)`（脚本级区域扫描 → 依赖携带）；`remapScriptOp`（改名改写 + 重编译 + 记录哈希/大小更新 + 降级警告）；`Artifact` 清单（verbatim / 重编译）；`execute` 产物**先落**（conf 写成功即以产物齐备为前提）+ 警告进 toast。
- `VtPrint.java` `+printExpr`；`OpsV8.java` `+srcPath/readSrc/writeSrc`。
- `TestBench.java`：主流程 **19** 数据步（+「T6 双视图」）；`dualViewChecks`（投影往返 / 侧线 / OpsPage 端到端四场景 / live 只读核）；`crossCopyChecks` +用例 6（v8 携带 + 改名改写重编译）；`extraChecks` +3 渲染件。
- 离机核：`VtToStepsCheck.java`（66 例）入 `~/vtchk-host/run.sh` → **六连核**。

## 5. 验收记录（T6.4）

- **离机核**：`bash ~/vtchk-host/run.sh` **六连核全绿**（HostCheck / VtScriptCheck / OpsV8Check / VtFromStepsCheck 38 / VtMigrateCheck 18 / **VtToStepsCheck 66**；真实产物 out-mig 8 + out-mig-zt 42 件逐字往返、负例 5 件仍拒）。
- **装机**：`5ff29f71173bbd6693a1aea2a1f9d511`（`pm install -r` 后回读 base.apk 逐字节一致）；`src.bak-v35` + `out/vtouch-editor-v35.apk` 已归档。
- **设备测试台两轮**（起跑门 = 白名单前台+亮屏条件守候 `~/vtouch-shots/v8t6/wait-gate.sh`）：
  - 轮1（`cb3afd52`）：**85/0/2** —— 两失败均测试代码自伤：c5b 用例拿合法函数脚本当非法例；`assertOverflow` 对辅助条横滚内容误报。
  - 轮2（`5ff29f71`，终版）：**87/0/0（30.7s）** ✅（T6 双视图全项 true 含非子集拒/块保存/live 2/2 可投影；脚本编辑预览全 true 溢出核无；跨方案复制含 v8 携带+改写 true；零回归）
- **渲染件目检**：`edit_script.png` / `edit_script_err.png` / `edit_script_long.png` 三件过（行号-代码对齐、错误行三层标记、滚动态行号连续；像素剖面核色：琥珀 8606 / 绿 162 / 错误红 724 / FAINT 1521）。
- **收场**：scratch 即清、无窗口残留、服务自停、核心 `3a6e723c` 未动、用户前台不受扰。
- 证据档案：`~/vtouch-shots/v8t6/`（evidence.txt + test_report.txt / test_run.txt + 全量渲染件）；验收文档 `~/vtouch-project/docs/OPS_V8_T6_ACCEPTANCE.md`。

## 6. 血泪 / 坑（本批）

- **`VtPrint` 打字符串优先 `raw`**——改 `segs` 不生效，必须同步 `raw`（CrossCopy 改写踩过，投影器不受影响因其读 segs）。
- 投影器 `raw()` 忘初始化 `r.op` → NPE（首跑即逮）。
- TestBench：`extraChecks` 的匿名 `run()` 作用域里变量名别撞（`se` 已占）。
- 行号槽对齐必须用 `Layout.getLineBaseline(i)`（实际基线）；自算行高在中文回退字体下会漂。
- **EditText 拖拽 = 选字，不滚动**（内滚方案拖不动正文）→ 滚动模型改**外层 ScrollView** 承载 + `OnTouchListener` MOVE 时 `requestDisallowInterceptTouchEvent(false)` 重允许拦截 + `ensureCursorVisible()` 跟随光标（合成 MotionEvent 直投设备核已证：拖拽滚动=true）。
- `assertOverflow` 须豁免**横滚容器（HorizontalScrollView）的直系子级**——内容超宽是滚动语义，非布局错误。
- 测试台失败的「调试位」要打进 rec 行（本轮靠自增调试位一次定位 c5b 自伤）；轮1 的假失败演示了**断言样例自身合法性**要先核（拿合法脚本当非法例）。

## 7. 挂账（T6 后）

- v5 步骤读路径退役评估 + `VTOUCH_UI_OPS_SAVE_TEST` 钩 / 起跑旗标退役——面板/核心侧，Windows 构建批次处理（本批不动）。
- 块视图覆盖扩展（函数/表/返回/调用/当/for）——脚本侧已全量可用，块侧按批补。
- 打开 v8 操作时同步 su 读源（~100ms 级）；如需可改异步载入（观察项）。
- 错误行底衬目前只到行文本范围（非整行通栏）——观感观察项。
