# .vscode —— 本项目的 C/C++ 环境（NDK 交叉编译，非本机编译）

## 一个前提

这份代码**不是**在 Windows 上编译的主机程序，而是交叉编译给 Android arm64 的（`-D_GNU_SOURCE` + NDK sysroot）。
所以 IntelliSense 不能按「本机 clang/MSVC」配，否则 `<linux/input.h>`、`<sys/uio.h>`、`<pthread.h>` 全都报红。
四个配置文件都用**严格 JSON**（不写注释），改动后用 `python -m json.tool` 校验即可。

## 用的工具链

| 项 | 路径 |
|---|---|
| NDK | `C:/Users/21102/android-ndk-r27d` |
| 目标编译器（构建用） | `toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android24-clang` |
| IntelliSense 用的编译器 | 同一个的 `.cmd` 包装（`…-clang.cmd`，Windows 上由 cmd 转调） |
| bash（任务与集成终端） | `C:/Users/21102/AppData/Local/hermes/git/bin/bash.exe` |
| 真机调试用 lldb | `…/bin/lldb.exe` + 设备侧 `…/lib/clang/18/lib/linux/aarch64/lldb-server` |

`c_cpp_properties.json` 里的 `includePath` **不是抄来的**，是对着编译器的实际搜索列表取的：

```sh
echo "" | .../bin/aarch64-linux-android24-clang -E -v -x c -     # 打印 #include <...> search starts here
```

它给出三条（`lib/clang/18/include`、`sysroot/usr/include/aarch64-linux-android`、`sysroot/usr/include`），
配置里就是这三条 + `${workspaceFolder}/src`。想验证配置够不够，用同一个开关直接问编译器：

```sh
for f in src/*.c; do .../aarch64-linux-android24-clang -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -fsyntax-only "$f" || echo FAIL $f; done
```

这段能全过，说明 `defines` + `includePath` 与真实构建一致（`sh scripts/build.sh` 用的是同一套开关）。

## 任务（Ctrl+Shift+P → Run Task，或 Ctrl+Shift+B 默认跑第一条）

| 任务 | 等价命令 |
|---|---|
| `build: vtouchd (arm64 · 全部模块)` | `sh scripts/build.sh` |
| `check: 语法检查当前文件（NDK clang）` | `…-clang -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -fsyntax-only <当前文件>` |
| `device: deploy / start / stop / status` | `sh scripts/deploy.sh <模式>`（需 adb + 设备 root） |
| `test: ws_smoke` | `python tests/ws_smoke.py`（需 daemon 在跑 + `adb forward tcp:27183 tcp:27183`） |
| `test: id_split_check` | `python tests/id_split_check.py --require-both`（需设备上抓过 `merged.log`） |
| `diagrams: 重渲四张工程图` | `sh build/_render_planb.sh`（脚本在 `build/` 下，未入库） |

## 换机器要改的地方（只有两处，其它都是相对的）

1. `c_cpp_properties.json` 里那四条绝对路径（NDK 位置）；
2. `tasks.json` 里的 `bash.exe` 路径。

`scripts/build.sh` 的 NDK 默认值也是 `C:/Users/21102/android-ndk-r27d`，可用环境变量覆盖：`NDK_ROOT=<路径> sh scripts/build.sh`。

## 真机调试（还没配，需要的话可以加）

NDK 自带 `lldb.exe` 与 aarch64 的 `lldb-server`，所以可以做「设备侧跑 daemon + PC 侧 lldb 附着」。
还没配的原因：要先把 `lldb-server` 推到设备、用 `adb forward` 打通端口，再写 `launch.json` 里的 `cppdbg` + `MIMode: lldb`。
需要的话说一声，我按这台设备（PJZ110 / KernelSU）的实际情况把这套跑通再落进 `launch.json`。
