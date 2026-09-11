# vtouch：Android 触摸合并系统

通过用户态 `EVIOCGRAB` + `uinput` 将真实触摸与模拟触摸合并为单一统一触摸设备，供 AutoJs6 / 第三方程序通过 WebSocket 调用；**区域匹配、UI 管理面板、触摸注入全部可单进程运行**。

- 用户态实现，无需内核模块（.ko）
- 真实触摸与模拟触摸共存、互不干扰
- WebSocket 接口（`ws://127.0.0.1:27183`）
- 动态发现触摸设备，不硬编码 event 节点
- 自动坐标转换（逻辑屏幕 ↔ 原始触摸轴）
- 原生 region 区域匹配（≤32 区域，五事件监听，**纯监听不代点**；同 id 重复添加=更新属性）
- **单进程 UI 整合**：vtouchd 核心编译为 C 库（`vtouch_init/poll_step/region_*/set_callbacks`），可嵌入 app_process 渲染进程（SurfaceControl 图层 → EGL GLES2 → Dear ImGui），UI 与触摸/匹配/WS 全部同进程直连，无 socket 转发

## 组件

| 组件 | 说明 |
|------|------|
| `src/vtouchd.c` | 核心：EVIOCGRAB 采集 + uinput 注入 + region 匹配 + WS 服务器（127.0.0.1:27183）。**库化接口**：`vtouch_init/vtouch_poll_step/vtouch_cleanup`（嵌入用）、`vtouch_region_clear/count/add/get_region`（面板直读写）、`vtouch_set_callbacks`（触摸/事件进程内回调）；命令行入口保留（`-w -h [-v] [-p] [-ui]`） |
| 渲染进程（app_process + `libtestimgui.so`） | **单进程管理面板**：Java 仅 ~80 行拿 SurfaceControl 图层（隐藏 API 反射），其余全 C++——EGL GLES2 + Dear ImGui 渲染；vtouchd 核心同进程（JNI 直调）。功能：区域表格（名称/形状/坐标/开关/显隐/删除）、＋矩形/＋圆形框选、事件日志、全屏透明 overlay（区域描边实时着色：**触摸在内绿/在外红**、命中亮绿闪烁） |
| `clients/vtouch_bundle.js` | **构建产物**：内嵌 vtouchd 二进制的 AutoJs6 单文件（自释放+连接+协议封装+JS 管理 UI），由 GitHub Actions 生成 |
| `clients/vtouch_touchback.js` | 监听+回触示例（区域触发→虚拟上滑/点按） |

## 部署

二进制不随仓库分发，由 GitHub Actions 构建：

1. 推送源码（`sync_auto.py` 或 git）→ **Build vtouch (Android NDK)** 工作流自动运行
2. 从 artifact 下载 `vtouch_bundle-arm64.js`（真机）/ `vtouch_bundle-x86_64.js`（AVD 测试）
3. 放到手机 `/sdcard/vtouch_bundle.js`，运行任意示例脚本即可（bundle 自释放 vtouchd）

## 区域监听示例

```javascript
var vt = require("/sdcard/vtouch_bundle.js");
vt.uiStart();                      // 起 ImGui 面板（面板 = UI + daemon，首次自动释放 dex/so）
var c = vt.connect();              // 连 ws://127.0.0.1:27183（别另调 ensure()，会抢端口）
var rs = vt.rgList(c);             // 回读面板里的区域（面板是唯一归属，必须在开收包循环前调）
vt.rgPush(c, rs);                  // 整表下发（内部先 region clear）；面板改过的区域照旧保留

var eng = vt.createEngine(rs, {
    onDown: function (r) { log("down  " + r.id); },
    onEnter: function (r) { log("enter " + r.id); },
    onExit:  function (r) { log("exit  " + r.id); }
});
vt.sub(c);                         // 订阅物理触摸流
threads.start(function () {        // 读线程：recv() 非阻塞，空转让一下
    for (;;) { var l = null; try { l = c.recv(); } catch (e) { break; }
               if (l) { var ev = vt.parseEv(l); if (ev) eng.feed(ev); } else sleep(8); }
});
```

完整协议见 [docs/VTOUCH_PROTOCOL.md](docs/VTOUCH_PROTOCOL.md)。

## UI 渲染架构（单进程整合）

```
app_process（Java ~80 行: SurfaceControl 反射拿图层）
└── JNI → libtestimgui.so（C++）
     ├── vtouchd 核心（同进程 C 库）: EVIOCGRAB 触摸 + uinput 注入 + region 匹配 + WS 服务器
     ├── 触摸回调 vtouch_set_callbacks → 面板 io / 框选 / overlay 着色（内存直连，无 socket）
     ├── 事件回调 region_ev → 日志 + 命中闪烁（无 WS 客户端也通知面板）
     └── EGL GLES2 + Dear ImGui：管理面板 + 全屏透明 overlay
```

- **无两个进程、无 socket 转发、无"链接不上"**：面板与核心是同一程序的内部调用
- WS 服务器保留：AutoJs6/外部程序照常连接收 `region_ev`/`pev`，配置通过 `region add/list/clear` 与面板共用同一张表
- 渲染要点：透明合成（`glClearColor(0,0,0,0)` + RGBA8888）、全屏图层（`wm size` 动态解析）、CJK 字体（NotoSansCJK）
- 性能：**脏检查渲染**（静态跳过合成省 CPU；触摸/事件/配置变化立即渲染）；AVD 用 `-gpu host`（composer 软合成是瓶颈，host GPU 后帧率 60）

## 构建

GitHub Actions（`.github/workflows/build.yml`）用 Android NDK r27d 交叉编译 vtouchd（arm64 + x86_64），再跑 `scripts/build_bundle.py` 生成双 ABI bundle，产物上传 artifact。

## 目录

```
src/                  C 源码（vtouchd.c：核心 + 库化接口）
clients/              AutoJs6 脚本（bundle 构建产物、示例）
scripts/              构建 / 同步脚本
tests/                WS 冒烟测试
docs/                 文档（协议 / UI 方案调研）
extension/sync-ext/   Chrome 扩展（一键网页同步通道）
```

## 一键同步（本地 → GitHub，零 git push）

本地改动无需 `git push`，两种通道自动选择：

```sh
# 一步同步 (有 token 走 GitHub REST API, 秒级静默; 无 token 自动回退 Chrome 扩展通道)
python scripts/sync_auto.py

# 完全静默后台守护 (开机自启, 每 N 秒检测变化自动同步)
python scripts/sync_auto_install.py install --watch 300   # 注册开机自启
python scripts/sync_auto_install.py uninstall             # 注销
```

**API 通道（推荐）**：GitHub fine-grained PAT 存 `D:\MYP\sync-config.json`（`{"token": "github_pat_..."}`）
或环境变量 `VT_SYNC_TOKEN`。需 `Contents: Read and write` 权限（仅 vtouch-project 仓库）。
秒级、免 Chrome、天然支持删除，`409` 时跳过（以远程为主）。

**扩展通道（回退）**：`chrome://extensions` → 开发者模式 → 加载已解压的扩展 → `extension/sync-ext`。
Chrome 需已登录 GitHub。扩展经代理隧道连本机局域网 IP（默认 `10.164.120.30`，
见 `extension/sync-ext/offscreen.js` 与 `manifest.json`，换机器需改）；`127.0.0.1` 会被系统代理 CONNECT 拦截。

状态缓存：`D:\MYP\sync-state.json`（sha256 增量，内容未变不提交）；日志：`D:\MYP\sync-auto.log`。
