# UX_BATCH_v39 — 脚本编辑器取点（帧上 + 屏幕，两键）（2026-10-10 深夜）

用户需求：「想在编辑脚本的时候可以取点」→ 拍板形态 = **两种都要**（辅助条两枚键：「帧上取点」「屏幕取点」）。

## 口径
- 两键取到的＝**竖屏逻辑坐标**（与步骤坐标/取色卡同口径：`Matcher.frameToLogic`），在**光标处插入「x y 」**（尾随空格：续写参数不断词；有选区则替换）。
- [帧上取点]：与块编辑 [取点 x,y] 同款 `MODE_PICK_POINT` 帧会话（帧源行 [用帧▾]/[去抓帧]、放大镜精调）→ [确认] 插入；取消不插入。
- [屏幕取点]：全屏遮罩武装 + 退后台 → 到目标画面点一下（点在遮罩上，**不误触游戏**）→ 抓帧算逻辑坐标 → **自动回编辑器插入** + toast；遮罩 [取消] = 也回编辑器（不插入）；用完悬浮窗自动收起（与武装抓帧同语义）；悬浮窗未开也能用（startPick 直接起服务）。

## 实现（5 文件）
- `ScriptEdit.java`：KEYS 增组 {帧上取点, 屏幕取点}（函数组与缩进组之间）；`enablePickKeys(frame, screen)` 注入回调（同 Forms.Num.enablePick 口径；未挂时点击提示）；`insertPickedPoint(x,y)` 插入「x y 」（复用既有 insert：替换选区 + 光标随动 + ensureCursorVisible）。
- `OpsPage.java`：接线 `pickScriptFrame()`（VisualPage.open MODE_PICK_POINT，Done.onPointPicked→insertScriptPick）/`pickScriptScreen()`（`CaptureChip.startPick(act,"script")` + moveTaskToBack，同 [去抓帧] 路）；`insertScriptPick(lx,ly)`＝活脚本会话则插入（MainActivity 与帧会话回填共用入口）。
- `MainActivity.java`：pick 意图新增 "script" 档——会话活着 → `shell.opsPage.insertScriptPick` + toast「已插入脚本（x,y）」；会话不在 → toast 报坐标不插入；冷启动（onCreate）同档只报告（「请重新打开脚本编辑插入」）。
- `CaptureChip.java`：接上 v7 悬空基础件——`startPick` 链终点从「色卡」分出脚本档：doPick 按 pickKind 分流（script=不落色卡、不 stash、直回编辑器）；遮罩提示文案分档；[取消] 分档（script=也回编辑器）；`launchEditor` 增带逻辑坐标（extra `pickLx/pickLy`）重载 + into=2「纯返回」档。
- `TestBench.java`：T6 双视图段 +g) 取点回填核（两键真点按回调、光标处插入（尾部/行中）、OpsPage 活会话插入、无会话拒）；T6 预览段 t1 +两键关键文本断言。

## 已知边界
- 屏幕取点回跳插入依赖「脚本编辑会话还在」（常态=同实例回跳没问题；进程被杀 → 冷启动档只 toast 报坐标）。
- 与 v8.4「取点从悬浮窗移除」不冲突：本批量入口在**编辑器**（武装式），悬浮窗面板仍无取点键。

## 验证
- 设备测试台 **87·0·0（32.9s）**；`取点键=true`；T6 预览行含两新键断言；装机 md5 `79730c6f2950fafab709f0e2b168bc1c` 逐字节；impeccable detect=`[]`。
- 快照 `src.bak-v39`；回滚 `src.bak-v38`；APK `out/vtouch-editor-v39.apk`；工具 `~/vtouch-editor-run-v39.sh` + `wait-gate-v39.sh`。
- 手测清单（真机交互链，需用户）：
  1) [帧上取点]：操作→[脚本]→点 [帧上取点] → 会话里选帧/去抓帧 → 点画面 →[确认] → 坐标插入光标处（取消 = 不插入）。
  2) [屏幕取点]：点 [屏幕取点] → app 退后台 → 到目标画面点一下 → 自动回编辑器、坐标插入 + toast；再来一次按遮罩 [取消]（应回编辑器、不插入）。
