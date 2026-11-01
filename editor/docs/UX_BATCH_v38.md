# UX_BATCH_v38 — 脚本编辑器双指捏合缩放（2026-10-10）

用户原话：「脚本编辑器横向滚动不是很好使，希望可以有缩放能力」→ 拍板形态 = **双指捏合缩放**（只加手势、不加任何控件；捏完小提示字号）。

## 背景（真因）
- 代码区现状：`setHorizontallyScrolling(true)` 不换行；横向**没法拖拽滚**（拖拽=选字），只能靠光标移动带动视口 →「不好使」。
- 缩放的定位 = 改**代码字号**（缩小时长行自然收进屏内，就不需要横滚；放大便阅读），行号槽同比例跟缩。

## 实现（ScriptEdit.java；仅编辑器侧，核心/数据格式未动）
- **捏合派发层 PinchCard**（代码卡片容器，替代普通 LinearLayout）：`dispatchTouchEvent` 先喂 `ScaleGestureDetector`；**第二指落下即接管**（吞到全抬指；接管瞬间给子层补发 CANCEL——不留半截选字）；单指原样下传（选字/光标/拖拽滚动一切照旧）。缩放对「编区/行号槽/空白」一视同仁。
- **SD 惰性构造**（`ensurePinch`）：ScaleGestureDetector 内部 `new Handler`，要求带 Looper 的线程。真实触摸必达主线程；离屏核 probe 线程无 Looper——构造会抛「Can't create handler」（首轮实测炸掉「T6 双视图」段）→ 触摸路径首次使用时才建 + 失败降级标记（不崩、该实例无缩放）。
- 字号模型：**9..24sp，0.5 步进量化**；默认 13sp=原值；捏合累计值（`zoomAcc`）**不量化**，防小步缩放被四舍五入吃掉；prefs（`"ui"`/`"scriptZoomSp"`）全局记忆；手势结束 toast「字号 N」（`fmtSp` 去尾零）。
- 视口锚定：手势起点所在行（行数不随字号变，行号恒有效），每步改尺后经 `edit.addOnLayoutChangeListener` 重锚。
- 行号槽：`GutterView.setFontSp(sp)` 按 **11:13 原比例**跟缩；行基线取自 EditText Layout、不漂移。

## 测试核（并入 T6 既有两行 rec——87 项基线不变）
- 「T6 脚本编辑预览」行 +`缩放=true（13→15.0 读数=true/true 单指=true 钳制=true 行号=true 记忆=true）`：
  - **真手势路径**＝合成双指 MotionEvent（DOWN→POINTER_DOWN→MOVE×3 跨度 400→640px→POINTER_UP→UP，直投 View 层，不进系统输入）→ 13→15.0 生效且落 0.5 步进；
  - 单指回归（捏合后单指拖拽仍滚动=无卡死状态）；钳制 9/24（直驱同 clamp/quant 路径）；行号槽宽跟缩；记忆真写路径（落盘 → 新实例读回 15 → 复原用户 pref）；
  - 渲染件 `edit_script_zoom.png`（19.5sp）+ 溢出核无。
- 「T6 双视图」行恢复全绿（SD 惰性构造修复）。
- **像素对账**（13sp vs 19.5sp 渲染件）：字形墨高 42→64-72px、行距 76-79→106-111px（≈1.5×，含常量行距 16px）；行号列与代码行基线对齐（目检 + 行号槽宽断言）。

## 血泪（可复用）
1. **ScaleGestureDetector 不能在无 Looper 线程构造**（离屏核 probe-run 线程）——触摸类对象一律惰性构造。
2. **捏合挂点别选 EditText**：短文本时手指落在编辑区下方空白 → 事件根本不到 EditText（首轮合成捏合 13→13.0 全无效果）。挂「容器 `dispatchTouchEvent`」层：卡内通吃、且不受子层 `requestDisallowInterceptTouchEvent` 干扰。
3. 渲染件**母体不同**（sw 含页面栏 / swz 纯编辑器）时，版面不能用绝对 y 直接比——用行距/墨高这类**无偏移判据**。

## 证据
- 装机 md5 `309b391a12cd5eca212c8185335f074a`（与构建产物逐字节一致）；测试台 **87·0·0**（mode=all，32.0s）；impeccable detect=`[]`。
- 快照 `src.bak-v38`（=现源码，diff 逐字节）；回滚点 `src.bak-v37b`；APK `out/vtouch-editor-v38.apk`。
- 工具：`~/vtouch-editor-run-v38.sh` + `~/vtouch-shots/v8t6/wait-gate-v38.sh`；pull 清单已含 `edit_script_zoom.png`。
