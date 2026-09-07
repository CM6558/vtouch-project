# VTouch AutoJs6 应用插件 APK

把 vtouch SDK 打包成 AutoJs6 **应用插件**：安装 APK 后，任意脚本 `plugins.load('org.vtouch.plugin')` 即可拿到 `VTouch` 构造函数，不依赖"项目"目录结构。

## 机制（源码确认，AutoJs6 6.x）

`plugins.load('含点号且非 .js 结尾的名称')` 按 **应用插件** 处理，完整链路：

1. `createPackageContext(pkg, INCLUDE_CODE | IGNORE_SECURITY)` —— **APK 必须已安装**。
2. 读插件 APK 的 meta-data `org.autojs.plugin.sdk.registry` → 注册类全名。
3. 反射调用注册类的静态方法 `loadDefault(Context, Context, Object, Object)`（参数类型必须精确匹配 `android.content.Context`）。
4. 返回对象上反射调用 `getAssetsScriptDir()` / `getVersion()` —— **鸭子类型即可，无需继承任何 Plugin SDK 基类**（Auto.js 4.x 时代的 `Auto.js-Plugin-SDK` 仓库已下线，本工程零依赖实现）。
5. 把 APK `assets/<scriptDir>/` 复制到 AutoJs6 缓存，`require` 其中的 **`index.js`**。
6. `index.js` 导出 `function (plugin) {...}`；AutoJs6 以插件 Java 实例为参数调用它，**返回值就是 `plugins.load()` 的结果**。

对应源码：
- `com/stardust/autojs/core/plugin/Plugin.java`（反射加载）
- `com/stardust/autojs/runtime/api/Plugins.java`（assets 复制 + index.js 路径）
- `org/autojs/autojs/runtime/api/augment/plugins/Plugins.kt`（包名判定 + 调用胶水层）

## 工程结构

```
plugin-apk/
├── AndroidManifest.xml          # meta-data 注册入口 org.vtouch.plugin.VTouchPlugin
├── build.sh                     # 命令行构建（aapt2 + javac + d8 + zipalign + apksigner）
├── src/org/vtouch/plugin/
│   └── VTouchPlugin.java        # 注册类：loadDefault + getAssetsScriptDir + getVersion
└── assets/vtouch/
    └── index.js                 # 胶水层：module.exports = function(plugin){ ...SDK...; return VTouch; }
```

## 构建

> **状态：本机已实构建验证**（Windows Git Bash，2026-09）。产物 `out/vtouch-plugin.apk` 已验证：签名有效、包名 `org.vtouch.plugin`、meta-data 正确、`assets/vtouch/index.js` 为正斜杠条目且与源文件字节一致、classes.dex 一致。GitHub Actions 工作流位于仓库根 `.github/workflows/build-vtouch-plugin-apk.yml`。

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
```

```js
// 任意脚本，无需项目结构
var VTouch = plugins.load('org.vtouch.plugin');
var vt = new VTouch().connect(function (client) {
    threads.start(function () {
        client.finger().swipe(200, 200, 1500, 2000, 1000);
    });
});
```

未安装插件时 `plugins.load` 抛 `PluginLoadException`（可 try/catch 回退到项目插件或 require）。

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
