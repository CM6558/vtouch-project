# VTouch AutoJs6 应用插件 APK

把 vtouch SDK 打包成 AutoJs6 **应用插件**：安装 APK 后，任意脚本 `plugins.load('org.vtouch.plugin')` 即可拿到 `VTouch` 构造函数，不依赖"项目"目录结构。

## 机制（源码确认，AutoJs6 6.7.0）

`plugins.load('含点号且非 .js 结尾的名称')` 按 **应用插件** 处理，完整链路：

1. **插件中心授权检查**（6.7.0+）：`PluginTrustManager.isAuthorized()` 要求签名指纹在官方/信任白名单，或用户在 AutoJs6 **插件中心**（主界面 → 左滑抽屉 → 插件中心 → VTouch Plugin → 授权）手动授权过。未授权抛 `PluginLoadException: 未在插件中心获得授权`。**更换签名后必须重新授权。**
2. `createPackageContext(pkg, INCLUDE_CODE | IGNORE_SECURITY)` —— **APK 必须已安装**。
3. 读插件 APK 的 meta-data `org.autojs.plugin.sdk.registry` → 注册类全名。
4. 反射调用注册类的静态方法 `loadDefault(Context, Context, Object, Object)`，返回实例**必须是 `android.content.ServiceConnection`**（硬性类型检查，否则抛 "Plugin instance must be type of android.content.ServiceConnection"）。
5. 反射调 `getAssetsScriptDir()` / `getVersion()`；**`getVersion() < 2` 时跳过 Service 绑定**（`bindService` 仅 version>=2 触发），纯脚本插件返回 1 即可，无需实现 Service/AIDL。
6. 把 APK `assets/<scriptDir>/` 复制到 AutoJs6 缓存，`require` 其中的 **`index.js`**。
7. `index.js` 导出 `function (plugin) {...}`；AutoJs6 以插件 Java 实例为参数调用它，**返回值就是 `plugins.load()` 的结果**。

对应源码：
- `org/autojs/autojs/core/plugin/Plugin.kt`（反射加载 + ServiceConnection 强转）
- `org/autojs/autojs/core/plugin/center/PluginTrustManager.kt` + `PluginAuthorizationStore.kt`（授权）
- `org/autojs/autojs/runtime/api/Plugins.kt`（启用/授权检查 + assets 复制 + bindService）
- `org/autojs/autojs/runtime/api/augment/plugins/Plugins.kt`（包名判定 + 调用胶水层）

## 生命周期（真机验证教训）

AutoJs6 脚本主体结束后事件循环仍保持（WebSocket/timers 存活）。**vtouchws 是单客户端 WebSocket 服务端**——脚本结束后若不关闭连接，旧连接占用端口导致下次 `connect` 失败（表现为"有时能连上、有时报错"）。业务完成后必须显式：

```js
client.close();      // 关闭 WebSocket（释放单连接）
vt.stopService();    // 杀掉 vtouchmerge/vtouchws，释放 EVIOCGRAB
```

## 工程结构

```
plugin-apk/
├── AndroidManifest.xml          # meta-data 注册入口 + VTouchService 托管服务
├── build.sh                     # 命令行构建（aapt2 + javac + d8 + zipalign + apksigner）
├── src/org/vtouch/plugin/
│   ├── VTouchPlugin.java        # 注册类：loadDefault + Java API（start/stop/runCommand/isReady）
│   └── VTouchService.java       # 后台托管服务（root 拉起 vtouchmerge/vtouchws）
└── assets/
    ├── vtouch/index.js          # 胶水层（由 scripts/build_sdk.py 生成）：SDK + 插件 Java API 优先覆盖
    ├── vtouchmerge              # native 合并器（构建时从 sdcard/vtouch-merge 打入, 运行时自动释放）
    └── vtouchws                 # native WebSocket 桥（同上）
```

> SDK 单源化：`clients/plugins/vtouch.js`、本 assets/index.js
> 均由 `scripts/vtouch-sdk.src.js`（可读主源）经 `scripts/build_sdk.py` 生成，改 SDK 只改主源。

## 构建

> **状态：本机已实构建验证**（Windows Git Bash，2026-09）。产物 `out/vtouch-plugin.apk` 已验证：签名有效、包名 `org.vtouch.plugin`、meta-data 正确、`assets/vtouch/index.js` 为正斜杠条目且与源文件字节一致、classes.dex 一致。**注意：native 自动释放（assets 内嵌 vtouchmerge/vtouchws）为本机无 Android SDK 环境下的代码级改动，构建/真机验证未执行**——请以 GitHub Actions 产物为准。GitHub Actions 工作流位于仓库根 `.github/workflows/build-vtouch-plugin-apk.yml`。

**本机**（需 JDK 17+ 和 Android SDK build-tools / platforms）：

```sh
export ANDROID_HOME=/path/to/android-sdk
export JAVA_HOME=/path/to/jdk17
bash clients/plugin-apk/build.sh
# 产物: out/vtouch-plugin.apk
```

**GitHub Actions**（无需本地工具链，推荐）：

```yaml
# .github/workflows/build-apk.yml
name: build-apk
on: [push, workflow_dispatch]
jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-java@v4
        with: { distribution: temurin, java-version: 17 }
      - uses: android-actions/setup-android@v3
      - run: bash clients/plugin-apk/build.sh
      - uses: actions/upload-artifact@v4
        with: { name: vtouch-plugin, path: clients/plugin-apk/out/vtouch-plugin.apk }
```

## 使用

```sh
adb install out/vtouch-plugin.apk
# 无需提前推送 vtouchmerge/vtouchws 二进制: 首次脚本运行时插件自动从 APK assets 释放到
# /data/local/tmp（需 root; 释放后永久生效, 二次脚本零开销）
```

```js
// 任意脚本，无需项目结构（v2：自动连接 + 一行一个动作）
var VTouch = plugins.load('org.vtouch.plugin');
var vt = new VTouch();                    // 自动释放二进制(如需) + 拉起服务 + 自动连接
vt.tap(540, 1200);                        // 点击
vt.swipe(200, 200, 1500, 2000, 1000);     // 滑动
// 退出自动 close + stopService
```

未安装插件时 `plugins.load` 抛 `PluginLoadException`（可 try/catch 回退到项目插件或 require）。

## v2 架构（vtouchmerge 新架构）

- SDK v2：`new VTouch()` 自动连接（先确保服务就绪再连 WebSocket，省去首次失败重试），
  便捷 API `vt.tap/vt.swipe/vt.down/vt.move/vt.up` 一行一个动作，发送队列自动缓冲；
  脚本退出自动 `close() + stopService()`。
- 插件 Java API 优先：胶水层覆盖 `startService/stopService`，走 `VTouchPlugin` 的
  Java 实现（root 进程内拉起/停止 vtouchmerge + vtouchws），失败自动回退 shell。
- `VTouchService`：可选的后台托管组件，供显式 `startService` 场景
  （`org.vtouch.plugin.action.STOP` 停止）。
- 托管的是 vtouchmerge 新架构（合并器 + WebSocket 桥），非旧 vtouchd。

## 与项目插件对比

| | 项目插件 (plugins/vtouch.js) | 应用插件 APK (本工程) |
|---|---|---|
| 构建 | 无，push 即用 | 需要 Android 工具链（或 GitHub Actions） |
| 加载 | `plugins.load('vtouch')`，依赖项目目录识别 | `plugins.load('org.vtouch.plugin')`，任意脚本可用 |
| SDK 更新 | 换文件即可 | 重新打包 + 重装 APK |
| 分发 | 拷贝 JS 文件 | 安装 APK |
| 额外能力 | 无 | 可附带 Java/Service/native 库 |
| 源码可见性 | assets 中 JS 明文 | assets 中 JS 仍明文（解包可见），隐藏效果有限 |

## 注意

- 签名：任意自签即可，AutoJs6 源码未校验插件签名。
- 缓存：每次 `load` 重新复制 assets 到 AutoJs6 缓存目录，更新插件后重装 APK 即生效。
- 胶水层内的 SDK 代码运行在当前脚本的 Rhino 上下文中，`device/shell/WebSocket/events/threads/sleep` 全局对象均可用。
- `VTouchPlugin.java` 的 `loadDefault` 参数类型必须写 `android.content.Context`（AutoJs6 用精确类型反射查找方法）。
