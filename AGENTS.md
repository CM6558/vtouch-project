# vtouch-project

Android 触摸模拟/合并系统：用户态 `EVIOCGRAB` + `uinput`，把真实触摸与虚拟触摸合成**一条**统一触摸流，供 AutoJs6 通过本地 WebSocket 调用。**面板（ImGui）本身就是 daemon**——采集、注入、区域匹配、WS、UI 全在一个进程里。

## 结构

```
src/vtouchd.c             核心：EVIOCGRAB 采集 + uinput 注入 + region 匹配 + WS(127.0.0.1:27183)
                          库化接口 vtouch_init/poll_step/cleanup、region_*、set_callbacks；命令行入口保留
src-ui/vtouch_ui.cpp      面板实现（EGL GLES2 + Dear ImGui + 触摸路由 + regions.conf 读写）
src-ui/VTouchUI.java      ~80 行：反射拿 SurfaceControl 图层 → JNI 进 C++
scripts/build_bundle.py   唯一来源：装配 clients/vtouch_bundle.js（内嵌 build/vtouchd + 面板 dex/so + JS 库）
scripts/build_ui.sh       编面板：javac → d8 → ndk cc → link → strip（产出 build/ui/）
clients/vtouch_bundle.js  生成物（大文件，gitignore；设备侧唯一交付物）
clients/vtouch_region_min.js  最小示例；vtouch_touchback.js / vtouch_orient_demo.js 回触、转屏示例
tests/onregion_harness.js 主机侧 Node 桩测（假 AutoJs6 + 假 daemon，46 项断言）
docs/VTOUCH_BUNDLE.md     使用手册（客户端 API / 区域监听 / 生命周期 / FAQ）
docs/VTOUCH_PROTOCOL.md   WS 线协议
docs/diagrams/            工程图：JSON 源 + SVG/PNG（README 里有重渲命令与自检清单）
.github/workflows/        CI：NDK r27d 编 arm64 + x86_64 bundle
thirdparty/imgui          面板依赖，不入库（自备；build_ui.sh 需要）
```

## 构建（Windows Git Bash，产物都在被 gitignore 的 `build/`）

```sh
NDK=C:/Users/21102/android-ndk-r27d
A64=$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android24-clang.cmd
"$A64" -O2 -Wall -Wextra -Werror -D_GNU_SOURCE src/vtouchd.c -o build/vtouchd   # ① 核心
sh scripts/build_ui.sh                                                          # ② 面板 dex + so
python scripts/build_bundle.py                                                  # ③ 出 bundle
python scripts/build_bundle.py --check                                          # 可选：node --check 语法门
```

C 代码统一 `-O2 -Wall -Wextra -Werror -D_GNU_SOURCE`；`src-ui/vtouch_ui.cpp` 另需 `-DIMGUI_IMPL_OPENGL_ES2`。
`build/` 整个删掉也能重建，重跑上面三步即可。

## 测试

```sh
node tests/onregion_harness.js     # 期望 ALL PASS 46/46，退出码 0
```

覆盖：WS 握手与分片帧拼接、`sub` 选择性订阅、`region list` 探针校验、ping/pong 探活、被新实例接管让位、退出自动收尾、逃生门 `autoStop(false)`。

## 部署与验证（真机 PJZ110 / ColorOS / KernelSU）

```sh
adb push clients/vtouch_bundle.js /sdcard/
adb shell md5sum /sdcard/vtouch_bundle.js          # 回读对账，别信 push 回执
adb shell am start -a android.intent.action.VIEW \
  -d file:///sdcard/vtouch_region_min.js -t application/x-javascript \
  -n org.autojs.autojs6/org.autojs.autojs.external.open.RunIntentActivity
adb shell su -c "pidof vtouch-ui"                  # 有 pid = 面板在跑 = 抓着 EVIOCGRAB
adb shell su -c "tail -30 /data/local/tmp/vtouch-runtime/vtouch-ui.log"
```

设备侧路径：部署 `/data/local/tmp/vtouch-ui/`（`classes.dex` + `libtestimgui.so`）；运行 `/data/local/tmp/vtouch-runtime/`（`vtouch-ui.log`、`regions.conf`、`vtouch-ui.pid`、`vtouchd.pid`）；headless 二进制 `/data/local/tmp/vtouchd`。
面板由 `CLASSPATH=…/classes.dex app_process /system/bin --nice-name=vtouch-ui VTouchUI <w> <h>` 起；**存活只认 `pidof`**（`app_process` 的 `/proc/<pid>/comm` 是 `main`，pid 文件只是书签）。

## 约定

- 脚本只写业务：`vt.onRegion(id, [ev], fn)` 是唯一推荐入口，起面板/连接/订阅/校验/读线程/收尾全在库内。
- `ev` 省略 = `down/up/enter/exit`（**不含 move**，按需显式写 `"move"`/`"down,move"`/数组/`"*"`）；`id` 省略或 `"*"` = 所有区域；回调入参 `h = {id, ev, slot, x, y}`。
- 回调统一丢子线程执行，脚本里可直接 `sleep()` 做长按/拖拽；分发按 FIFO。
- 区域表唯一归属是面板（`/data/local/tmp/vtouch-runtime/regions.conf`），脚本侧不要另存一份。
- 客户端 API 刻意用短名（`onRegion`/`finger`/`uiStart`/`rgList`/`sub`），避免与脚本里常见通用名撞车。
- 改 `clients/*.js`、`scripts/*.py` 保持 CRLF；`\r\n` 字面量按字节比对（见坑）。

## 坑（都踩过）

- **`/sdcard` 是 noexec**：ELF 必须落到 `/data/local/tmp/` 才能执行（bundle 自释放就是干这个的）。
- **应用进程看不见 `/data/local/tmp`**（挂载命名空间不同）：判文件存在/进程存活一律走 root 回读，别用应用侧 `File`。
- **强杀（强行停止）跳过 `exit` 事件** → 收尾钩子不执行，面板留在后台抓着触摸。手动清：`kill -9 $(pidof vtouch-ui)`。
- **WS 单客户端**：新连接踢旧连接；旧实例自退且**不收面板**（面板归新实例），避免「先退的把共用面板收走」。
- **区域 id 写错/被禁用不会触发**：库内启动时用 `region list` 探针校验并 toast 报错，别退回「静默没反应」。
- **面板只监听不代点**：命中区域只发 `region_ev`，要动作必须走虚拟回注（`finger()`）。
- **EVIOCGRAB 是独占的**：面板崩了就没人转发物理触摸，必须重启；fd 关闭才释放 grab。
- **ColorOS 日志**：AutoJs6 的 `console`/`toastLog` 走 `GlobalConsole`，是 **D 级**——`logcat *:E` 抓不到，按 tag 或级别筛。
- **别让编辑器/补丁工具改写 JS 里的 `\r\n` 转义**（WS 帧分隔），会静默破坏握手；这类文件整份重写、改完跑桩测。
- **改坐标转换/事件语义后**，真机回归看 daemon 日志 `vtouchd: ev <id> <ev> slot<n> x,y` 与脚本侧输出是否逐字一致。
