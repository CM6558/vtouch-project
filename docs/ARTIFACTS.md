# 编译产物说明：哪个才是手机能跑的最小包

**一句话**：手机侧只要**一个文件** —— `clients/vtouch_bundle.js`（约 **754 KB**）。它是自包含的：里面按 base64/gzip 内嵌了 arm64 `vtouchd`、面板 `classes.dex` + `libtestimgui.so`，再加上整套 JS 库；运行时自己释放到 `/data/local/tmp/` 并拉起进程。**其它所有产物都是构建中间件或 CI 归档件，不用上手机。**

## 1. 最小可运行集合

| 需要 | 说明 |
|---|---|
| `clients/vtouch_bundle.js` | **必须**。推成 `/sdcard/vtouch_bundle.js`；业务脚本 `require("/sdcard/vtouch_bundle.js")` |
| 业务脚本（如 `clients/vtouch_region_min.js`） | 必须，但那是你自己的逻辑，放哪都行（`/sdcard/`、AutoJs6 项目目录…） |
| 区域定义 | **不是文件**：首次在面板里框选建区域（落盘到设备侧 `regions.conf`），或走 WS 的 `region add` |
| AutoJs6（已授予 root）+ Android root | 运行前提。bundle 用 AutoJs6 的 `shell(cmd, true)` 跑 `pidof` / `kill -9` / `app_process` |

部署（三步，含回读校验）：

```sh
adb push clients/vtouch_bundle.js /sdcard/
adb shell md5sum /sdcard/vtouch_bundle.js        # 回读对账，别信 push 回执
# 在 AutoJs6 里跑你的脚本；起来后确认：
adb shell su -c "pidof vtouch-ui"                # 有 pid = 面板在跑 = 抓着 EVIOCGRAB
```

## 2. 本机构建产物（都在 `build/`，已 gitignore，删了能重建）

| 产物 | 大小（本机实测） | 内容 / 作用 | 谁消费 |
|---|---|---|---|
| `build/vtouchd` | 31,968 B | arm64 ELF，纯 daemon（无面板） | 被 `build_bundle.py` 内嵌 |
| `build/ui/classes.dex` | 4,384 B（干净构建） | 面板 Java 层 `VTouchUI`（d8 产物） | 内嵌 → 设备 `/data/local/tmp/vtouch-ui/classes.dex` |
| `build/ui/libtestimgui.so` | 936,360 B | 面板本体（ImGui + 核心 C 库，EGL/GLES2） | 内嵌（gzip）→ 设备 `/data/local/tmp/vtouch-ui/libtestimgui.so` |
| `build/ui/{obj,classes,dex}/`、`build/ui/md5.txt` | — | 编译中间态（`.o`/`.class`/md5 清单） | 可忽略 |
| `clients/vtouch_bundle.js` | **753,715 B** | **最终交付物**（自释放上面三样 + JS 库） | 推手机 |

`clients/vtouch_bundle.js` 内嵌对账（当前在用的这份）：

| 内嵌项 | 大小 | md5 |
|---|---|---|
| `vtouchd`（arm64） | 31,968 B | —（bundle 内只存 size + b64） |
| 面板 `classes.dex` | 6,712 B | `271cf78e6cd30261deca5a7ae9d5b7db` |
| 面板 `libtestimgui.so` | 936,360 B（gz 441,237 B） | `da1f09407856db4a373d88c84da8eda5` |

> **重建后 dex 会变小**：现在重跑 `build_ui.sh` 得到的是 4,384 B（旧 `build/ui/classes` 目录里残留过已删除的 `SCProbe.class`，随源码清理一起去掉了）。
> 设备侧 `uiStart()` 按 md5 判断要不要重新释放，所以拿到新 bundle 直接跑即可，不用手清 `/data/local/tmp/vtouch-ui/`。
> `libtestimgui.so` 是字节可复现的：重编后 md5 与上表一致。

## 3. 设备侧运行时会创建（不是随包分发的文件）

```
/data/local/tmp/vtouchd                         headless 模式的 daemon 二进制
/data/local/tmp/vtouch-ui/classes.dex           面板 Java 层（从 bundle 释放）
/data/local/tmp/vtouch-ui/libtestimgui.so       面板本体（从 bundle 释放）
/data/local/tmp/vtouch-runtime/vtouch-ui.log    面板日志
/data/local/tmp/vtouch-runtime/vtouchd.log      headless daemon 日志
/data/local/tmp/vtouch-runtime/regions.conf     区域表（唯一归属：面板读写）
/data/local/tmp/vtouch-runtime/*.pid            pid 书签（存活判断只认 pidof）
```

## 4. CI 产物（`.github/workflows/build.yml`，每次 push master 跑一次）

| 产物 | 大小 | 说明 |
|---|---|---|
| `vtouch_bundle-arm64.js` | ~754 KB | 与本地同款**带面板完整包**；workflow 里断言「内嵌面板 + 体积 >400 KB」，不达标直接 fail |
| `vtouch_bundle-x86_64-headless.js` | ~40 KB | AVD 测试用；面板 `.so` 只有 arm64 版，所以 x86_64 只出 headless（`uiStart()` 会明确报错） |
| `vtouchd` | 31 KB | arm64 纯 daemon |

## 5. 怎么判断手上的包是不是「完整可跑」

```sh
stat -c%s clients/vtouch_bundle.js                            # >400000 → 带面板；~40000 → headless
grep -c 'VTOUCH_UI_SO_GZ_B64 = "' clients/vtouch_bundle.js     # ≥1 → 内嵌了面板
node --check clients/vtouch_bundle.js                         # 语法门（等价 python scripts/build_bundle.py --check）
```

headless 包不是坏包：它只是没有面板，`vt.uiStart()` 会抛「这个 bundle 是 headless 构建…」，其余能力（连 daemon、区域事件、虚拟回注）照旧。

## 6. 重建（本机 Git Bash）

```sh
NDK=C:/Users/21102/android-ndk-r27d
A64=$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android24-clang.cmd
"$A64" -O2 -Wall -Wextra -Werror -D_GNU_SOURCE src/vtouchd.c -o build/vtouchd   # ① 核心
sh scripts/build_ui.sh                                                          # ② 面板 dex + so
python scripts/build_bundle.py                                                  # ③ 出 bundle
```

依赖：NDK r27d、JDK（`javac`）、Android SDK `platforms;android-24` + `build-tools;34.0.0`、`thirdparty/imgui` v1.91.8（自拉）。
路径都能用环境变量覆盖：`NDK_ROOT` / `ANDROID_SDK_ROOT` / `BUILD_TOOLS_VERSION` / `API_LEVEL`（CI 就是靠这个复用同一份脚本）。
只想要纯 daemon 包：`python scripts/build_bundle.py --headless`；默认不带开关时**必须有面板产物**，缺了直接报错退出，防止误发没有 UI 的包。
