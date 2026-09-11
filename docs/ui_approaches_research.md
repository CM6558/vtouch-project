# vtouchd 管理 UI 调研报告（Phase 1 产出）

> **历史存档（已被取代，勿当现状）**：本报告是 2026-09-11 的选型调研原始记录，其中 floaty 载体、AutoJs6 侧 `UI_SRC`/`eval` 方案、WebUI 方案均**未采用且源码已删除**。
> 结论已落地为 `src-ui/` 的 app_process + composer 图层 + ImGui 面板（见 `PROJECT_STRUCTURE.md` 与技能 `vtouch-autojs6`）；文中提到的 `storages vtouch_regions` 也已废除，区域唯一归属是面板的 `regions.conf`。


日期：2026-09-11
范围：AutoJs6 环境 / 独立 App 的悬浮管理 UI 实现方案（网上实践）
前提：vtouchd 二进制（触摸合并/区域检测/WS）与 AutoJs6 事件侧不动；UI 是 vtouchd 的独立管理器，随时悬浮、含即时预览、样式美观、性能兼容兼顾。

## 1. 关键结论（有证据）

1. **AutoJs6 6.x 的 floaty 不支持 WebView** —— 载体 A（floaty 内 WebView）出局：
   - AutoJs6 文档：WebView 只以 `web.newInjectableWebView()` 存在，绑定 `activity.setContentView()`（ui 界面），**floaty 的 XML 布局体系没有 webview 元素**（`ui.md` 全文检索 0 命中）
   - issue #467：`ui.inflate` 返回的 View 无法给 `floaty.window` 作为参数（closed）——floaty 与 ui View 体系兼容性有已知缺陷
   - issue #273：floaty window 无法加载 android layout 文件（closed bug）
   - 结论：想用 Web 技术渲染 UI，必须**跳出 AutoJs6 的 floaty**。

2. **Auto.js 生态里"Web 技术做界面"是成熟实践**：
   - `autox-community/autojs-web`（65★）：HTML/CSS/JS（vue3+vant 示例）作为 AutoX 界面，**JS↔Rhino 双向通信用拦截 `onConsoleMessage`（console.log）**实现（并发安全；`shouldOverrideUrlLoading` 拦截无法并发，被弃用）——这是 WebView 壳与脚本引擎通信的参考实现
   - `xxxxue/autox-super-kit`：Web 框架（React/Vue）+ TypeScript 写界面，可编译 Dex
   - 适用：Rhino 引擎的 auto.js 系（AutoX 等）。注意：这些是 **ui 界面（activity）**方案，非 floaty。

3. **悬浮工具箱（overlay）原生实现有成熟案例**：
   - `zennqiitagpu/OverlayPowerToys`：Jetpack Compose 悬浮工具箱（便签/绘图/计算器/浏览器，Android 10+，最小化悬浮球）——证明"随时悬浮 + 最小化小球"交互模式可行；纯 Compose 性能好，但样式开发成本高
   - `mmwtl/AtlasAppWidget`：Android 11 悬浮快捷面板（车载竖屏）
   - 模式共性：前台服务 + `TYPE_APPLICATION_OVERLAY` + 可拖动悬浮球 + 点击展开面板

4. **WebView 壳 App（HTML/CSS/JS 前端 + Android 壳）**：样式自由度最高（任意 Web 技术栈）、渲染由 Chromium 自管（独立于 AutoJs6 的 ScriptCanvasView 线程池——不继承 floaty 每帧重绘问题）、随时可用（独立进程/生命周期）。**主候选**，待 AVD 实测 CPU。

## 2. 候选载体对比

| 载体 | 随时悬浮 | 样式自由度 | 性能预期 | 代码量 | 关键风险/证据 |
|---|---|---|---|---|---|
| A. AutoJs6 floaty + canvas（现状） | 需 AutoJs6 跑脚本 | 低（xml 控件） | 差（已知 114-141% CPU，平台每帧重绘） | 已实现 | 性能痛点即本项目动因 |
| A'. AutoJs6 floaty 内 WebView | 同上 | 高 | 未知 | 中 | **floaty 无 webview 元素 + ui.inflate→floaty 有 bug（#467）——基本不可行** |
| **B. 独立 WebView 壳 App（APK）** | **是（前台服务+overlay）** | **最高（HTML/CSS/任意框架）** | 好（Chromium 自管渲染） | 中（壳工程 + 前端） | 需装 APK；cleartext（ws://）需配置；悬浮权限 |
| C. 独立原生 App（Compose） | 是 | 中（Compose 主题系统） | 最好 | 高 | OverlayPowerToys 已验证模式；样式迭代慢 |
| D. 桌面浏览器管理页 | 否（开发期） | 最高 | 无关（PC 渲染） | 低 | 真机日常不便；可作附加调试通道 |

## 3. UI 与 vtouchd 的交互（复用现有 WS 协议，二进制零改动）

```
独立 UI (B/C/D)  ──ws://127.0.0.1:27183──▶  vtouchd
  region clear / region add <id> <type> <a1..a4> <en>   (配置)
  sub → 收 pev（物理手指）/ region_ev（区域事件）          (即时预览)
  res（分辨率/raw 范围）                                  (框选参考)

AutoJs6 bundle  ──ws://127.0.0.1:27183──▶  vtouchd
  收 region_ev → 五事件回调（业务）——事件侧不动
```

**前置**：vtouchd 需支持多客户端（现单连接踢旧）——最小协议层改动（连接表，触摸/匹配逻辑不动），需用户确认边界。

## 4. UI 能力清单（新 UI 必须全量继承，对照 build_bundle.py UI_SRC）

| 能力 | 现状实现（UI_SRC） | 新 UI 要求 |
|---|---|---|
| overlay 区域预览（红描边/命中 400ms 绿闪加粗） | `ovShow`/draw | ✓ + 深浅色适配 |
| 手指蓝点 + 槽号 | `ovShow`/draw fs 循环 | ✓ |
| 区域列表（名称/形状/坐标/开关●○/显隐/删除） | `uiRefreshDo` + `uiRowXml` | ✓ 动态行（>8 不限） |
| ＋矩形/＋圆形框选（拖框/圆心拖半径） | `capStart`/`capClose` | ✓ |
| 预览总开关 | `ovPreview` | ✓ |
| 最小化小球（可拖、点恢复） | `uiMin` | ✓ |
| 标题栏拖动/关闭 | `uiDragBar`/`btnQuitTop` | ✓ |
| 旋转适配（竖屏归一 C2P/P2C、方向事件+轮询、尺寸自适应） | `rotSync`/`ovSync`/`vtC2P/vtP2C` | ✓ |
| 事件实时日志区 | （无——增强项） | 新增 down/up/enter/exit/move 滚动日志 |
| 配置持久化 | `storages("vtouch_regions")`（AutoJs6 侧） | App 侧 SharedPreferences/文件 + 兼容旧存储 |
| 样式 | 白色卡片 Corporate Clean | 现代主题：深浅色切换/渐变/圆角阴影/动效 |

## 5. 样式参考（调研后选型）

- **shadcn/ui**（浅深色主题 + Tailwind，组件语义化）——推荐管理页主体
- **Material 3**（Android 原生观感，动态色）——Web 侧可用 MUI/M3 Web
- **daisyUI**（Tailwind 组件库，主题切换极简）
- 悬浮球/面板动效参考：OverlayPowerToys（Compose）交互模式

## 6. 技术注意点（实现期）

- **cleartext**：Android 9+ 默认禁明文流量，`ws://127.0.0.1` 需 `android:usesCleartextTraffic="true"` 或 network_security_config 放行 localhost
- **悬浮权限**：`SYSTEM_ALERT_WINDOW`（API 30+ 需用户在设置/悬浮窗白名单授予）；AutoJs6 已声明，独立 App 需自行声明 + 引导授权
- **WebView WS**：HTML 内 `new WebSocket('ws://127.0.0.1:27183')` 同 App 进程直连，无跨域问题；断线自动重连（参照 bundle bootWatch）
- **前台服务**：悬浮窗常驻需前台服务（`foregroundServiceType`）+ 通知；Android 11 后台启动限制
- **ADB 测试期**：独立 App 可用 `adb install`；真机长期使用需用户接受安装

## 7. 待实测（Phase 2）

1. 载体 B 最小 APK：WebView 管理页（HTML 卡片 + WS 配置/订阅/事件日志）+ overlay 预览 → AVD 装、截图、注入触摸验证 region_ev 到达 + `top` CPU
2. vtouchd 多客户端最小改造（TDD：ws_multi.py）→ UI 与 AutoJs6 并存
3. 对比基线（现状 floaty canvas CPU）→ 评估矩阵（性能 0.3 / 随时可用 0.25 / 样式 0.2 / 代码量 0.15 / 真机风险 0.1）

## 8. 来源链接

- AutoJs6 issues：#467（ui.inflate→floaty）、#273（floaty 加载 layout bug）、#437（floaty 无法关闭）
- AutoJs6 文档（本地 D:\MYP\autojs6-docs）：`floaty.md`、`web.md`、`injectableWebViewType.md`（WebView 仅 activity 绑定）
- `github.com/autox-community/autojs-web`（HTML 界面 + console.log 桥接）
- `github.com/xxxxue/autox-super-kit`（React/Vue + TS 界面）
- `github.com/zennqiitagpu/OverlayPowerToys`（Compose 悬浮工具箱）
- `github.com/mmwtl/AtlasAppWidget`（Android 11 悬浮面板）
- 样式：shadcn/ui、Material 3、daisyUI（官网/文档）
- CSDN：`blog.csdn.net/weixin_33919950/article/details/923495`（Android 悬浮窗开发全解析：权限适配到性能优化）
- CSDN：`blog.csdn.net/dh1021927867/article/details/158807757`（UniApp 实现 Android 悬浮窗，支持 WebView）
