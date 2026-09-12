# 编译产物说明：哪个才是手机能跑的最小包

**一句话**：手机侧只要**一个文件** —— `clients/vtouch_bundle.js`（约 **750 KB**）。它是自包含的：里面按 base64/gzip 内嵌了 arm64 `vtouchd`、面板 `classes.dex` + `libtestimgui.so`，再加上整套 JS 库；运行时自己释放到 `/data/local/tmp/` 并拉起进程。**其它所有产物都是构建中间件或 CI 归档件，不用上手机。**

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
| `build/ui/classes.dex` | 4,384 B | 面板 Java 层 `VTouchUI`（d8 产物） | 内嵌 → 设备 `/data/local/tmp/vtouch-ui/classes.dex` |
| `build/ui/libtestimgui.so` | 936,360 B | 面板本体（ImGui + 核心 C 库，EGL/GLES2） | 内嵌（gzip）→ 设备 `/data/local/tmp/vtouch-ui/libtestimgui.so` |
| `build/ui/{obj,classes,dex}/`、`build/ui/md5.txt` | — | 编译中间态（`.o`/`.class`/md5 清单） | 可忽略 |
| `clients/vtouch_bundle.js` | **750,432 B** | **最终交付物**（自释放上面三样 + JS 库） | 推手机 |

### 当前这份 bundle 的内嵌对账

| 内嵌项 | 大小 | md5 |
|---|---|---|
| `vtouchd`（arm64） | 31,968 B | `55db7e6625a338eed470aebc9bf32dda` |
| 面板 `classes.dex` | 4,384 B | `3028b9f63f9fca53dd7e172462b5a34a` |
| 面板 `libtestimgui.so` | 936,360 B（gz 441,237 B） | `da1f09407856db4a373d88c84da8eda5` |
| `clients/vtouch_bundle.js` 本身 | 750,432 B | `287dbe15c79acfa889e5a725175b1a84` |

> `libtestimgui.so` 与 `classes.dex` 都是**字节可复现**的（同源码重编得到同一 md5）。
> 设备侧 `uiStart()` 按 md5 判断要不要重新释放：换了新 bundle 直接跑即可，不用手清 `/data/local/tmp/vtouch-ui/`。
> 历史提醒：早期一版 dex 是 6,712 B（`271cf78e…`），因为 `build/ui/classes` 里残留过已删除的 `SCProbe.class`；源码清理后重建就是干净的 4,384 B。

## 3. 怎么保证「包是最新的、不是旧产物」

用 `scripts/verify_bundle.py`——它把内嵌项解出来跟**本次编译产物**逐字节/md5 对账：

```sh
python scripts/verify_bundle.py --bin build/vtouchd --ui build/ui     # 完整包（真机用）
python scripts/verify_bundle.py --bin build/vtouchd --allow-headless  # headless 包（AVD 用）
```

检查项：内嵌 daemon 与 `build/vtouchd` 逐字节一致；内嵌 dex/so 的 md5 与 `build/ui/*` 一致；
内嵌 so 的 gz 解压后与声明 md5 一致；`VTOUCH_BIN_SIZE` 与实际字节数一致；daemon 是 ELF；`node --check` 通过；
没有面板时必须显式 `--allow-headless`（防止把 headless 包当真机包发出去）。任一项不符即退出码 1 —— 例如包内 dex 还是旧的 `271cf78e…` 而现编是 `3028b9f6…` 时，它会直接报 `包是旧的`。

CI（`.github/workflows/build.yml`）每次 push 都会跑同一份脚本：arm64 完整包与 x86_64 headless 包都在生成后立刻校验，
并把 commit、产物大小与 md5 写进 job summary，事后可对账「这个 artifact 对应这个 commit」。

## 4. 设备侧运行时会创建（不是随包分发的文件）

```
/data/local/tmp/vtouchd                         headless 模式的 daemon 二进制
/data/local/tmp/vtouch-ui/classes.dex           面板 Java 层（从 bundle 释放）
/data/local/tmp/vtouch-ui/libtestimgui.so       面板本体（从 bundle 释放）
/data/local/tmp/vtouch-runtime/vtouch-ui.log    面板日志
/data/local/tmp/vtouch-runtime/vtouchd.log      headless daemon 日志
/data/local/tmp/vtouch-runtime/regions.conf     区域表（唯一归属：面板读写）
/data/local/tmp/vtouch-runtime/*.pid            pid 书签（存活判断只认 pidof）
```

## 5. CI 产物（`.github/workflows/build.yml`，每次 push master 跑一次）

| 产物 | 大小 | 说明 |
|---|---|---|
| `vtouch_bundle-arm64.js` | ~750 KB | 与本地同款**带面板完整包**；断言「内嵌面板 + 体积 >400 KB + 内嵌项 = 本次刚编出的产物」，不达标直接 fail |
| `vtouch_bundle-x86_64-headless.js` | ~40 KB | AVD 测试用；面板 `.so` 只有 arm64 版，所以 x86_64 只出 headless（`uiStart()` 会明确报错） |
| `vtouchd` | 32 KB | arm64 纯 daemon |

## 6. 重建（本机 Git Bash）

```sh
NDK=C:/Users/21102/android-ndk-r27d
A64=$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android24-clang.cmd
"$A64" -O2 -Wall -Wextra -Werror -D_GNU_SOURCE src/vtouchd.c -o build/vtouchd   # ① 核心
sh scripts/build_ui.sh                                                          # ② 面板 dex + so
python scripts/build_bundle.py                                                  # ③ 出 bundle
python scripts/verify_bundle.py --bin build/vtouchd --ui build/ui               # ④ 对账（建议每次发前跑）
```

依赖：NDK r27d、JDK（`javac`）、Android SDK `platforms;android-24` + `build-tools;34.0.0`、`thirdparty/imgui` v1.91.8（自拉）。
路径都能用环境变量覆盖：`NDK_ROOT` / `ANDROID_SDK_ROOT` / `BUILD_TOOLS_VERSION` / `API_LEVEL`（CI 就是靠这个复用同一份脚本）。
只想要纯 daemon 包：`python scripts/build_bundle.py --headless`；默认不带开关时**必须有面板产物**，缺了直接报错退出，防止误发没有 UI 的包。
