# vtouch：Android 触摸合并系统

通过用户态 `EVIOCGRAB` + `uinput` 将真实触摸与模拟触摸合并为单一统一触摸设备，供 AutoJs6 / 第三方程序通过 WebSocket 调用。

- 用户态实现，无需内核模块（.ko）
- 真实触摸与模拟触摸共存、互不干扰
- WebSocket 接口（`ws://127.0.0.1:27183`）
- 动态发现触摸设备，不硬编码 event 节点
- 自动坐标转换（逻辑屏幕 ↔ 原始触摸轴）
- 原生 region 区域匹配（≤32 区域，五事件监听，纯监听不代点）

## 组件

| 组件 | 说明 |
|------|------|
| `src/vtouchd.c` | 单进程合并器+WebSocket+区域匹配（EVIOCGRAB + uinput + 127.0.0.1:27183） |
| `clients/vtouch_bundle.js` | **构建产物**：内嵌 vtouchd 二进制的 AutoJs6 单文件（自释放+连接+协议封装+管理 UI），由 GitHub Actions 生成 |
| `clients/vtouch_touchback.js` | 监听+回触示例（区域触发→虚拟上滑/点按） |

## 部署

二进制不随仓库分发，由 GitHub Actions 构建：

1. 推送源码（`sync_auto.py` 或 git）→ **Build vtouch (Android NDK)** 工作流自动运行
2. 从 artifact 下载 `vtouch_bundle-arm64.js`（真机）/ `vtouch_bundle-x86_64.js`（AVD 测试）
3. 放到手机 `/sdcard/vtouch_bundle.js`，运行任意示例脚本即可（bundle 自释放 vtouchd）

## 区域监听示例

```javascript
var vt = require("/sdcard/vtouch_bundle.js");
eval(vt.uiSource);   // 可选：管理 UI（框选添加区域）
vt.connect({
    onDown:  function (region, f) { log("down  " + region.id + " s" + f.slot); },
    onMove:  function (region, f) { log("move  " + region.id + " s" + f.slot + " " + f.x + "," + f.y); },
    onUp:    function (region, f) { log("up    " + region.id + " s" + f.slot); },
    onEnter: function (region, f) { log("enter " + region.id + " s" + f.slot); },
    onExit:  function (region, f) { log("exit  " + region.id + " s" + f.slot); }
});
```

完整协议见 [docs/VTOUCH_PROTOCOL.md](docs/VTOUCH_PROTOCOL.md)。

## 构建

GitHub Actions（`.github/workflows/build.yml`）用 Android NDK r27d 交叉编译 vtouchd（arm64 + x86_64），再跑 `scripts/build_bundle.py` 生成双 ABI bundle，产物上传 artifact。

## 目录

```
src/                  C 源码（vtouchd.c 单一二进制）
clients/              AutoJs6 脚本（bundle 构建产物、示例）
scripts/              构建 / 同步脚本
tests/                WS 冒烟测试
docs/                 文档
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
