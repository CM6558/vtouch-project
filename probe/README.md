# UI 接入架构 · 可行性探针（probe/）

**目的**：在动任何核心代码之前，先把新 UI 架构里风险最高的四个机制在真机上验掉。
探针**不碰触摸注入、不改核心、不抢 `EVIOCGRAB`**（触摸屏只以只读方式打开一次，用于验 fd 卫生）。

## 一、验什么、结论

| # | 机制 | 结论 | 证据 |
|---|---|---|---|
| ① | 核心侧 `memfd` + `fork/exec app_process`，子进程继承共享内存 fd | **成立**：子进程在固定 fd 3 上拿到 memfd，无需路径/权限/挂载 | `P: memfd=4 … cloexec=0` / `C: start fd=3` / `C: attach ok: magic=0x56545042` |
| ② | 两进程双向共享（核心写自己的字段、UI 写自己的字段，互不阻塞） | **成立**：父写 `core_hb/core_cnt`、子写 `ui_hb/ui_cnt/ui_pid`，两侧都读到；连续 60s 稳定 | `P: t=59123ms core_hb=592 … ui_hb=589` |
| ③ | `FD_CLOEXEC` 卫生（防"UI 继承带 grab 的 fd → 核心退出后触摸回不来"） | **成立**：子进程 fd 表里 `event/uinput` 命中数 **0**（父进程 3 号是 `/dev/input/event8`，带 CLOEXEC，子进程没有它） | `子进程命中 event/uinput 数量 = 0`；`父进程 … = 1` |
| ④ | 只读映射 = MMU 强制 + 崩溃隔离 + 心跳自杀 | **全部成立**：子进程写只读映射 → SIGSEGV（`status=0xb`）；父进程不受影响继续跑 20s；父进程退出后子进程在 2s 停滞时自杀 | `P: 子进程已退出 status=0xb（父继续跑）` / `C: core_hb 停滞 2 秒 → 核心已死，自杀退出` |
| ⑤ | "数据式请求"（不需要命令通道） | **成立**：子进程写 `stop_req=1` → 父进程立刻收到 | `C: 已写 stop_req=1` / `P: 收到 stop_req（子进程要求停）` |

## 二、探针逼出来的两个真坑（已写进代码注释，最终方案必须照做）

1. **`fork/exec app_process` 必须带完整 `environ`**。只传 `CLASSPATH/ANDROID_ROOT/PATH` 时，子进程**连 `main` 都进不去、静默退 0**（logcat 里没有 `Calling main entry`）。
   证据：`probe_native.c` 里 `execve(..., environ)` 前后对比。
2. **`memfd` 本体 fd 要单独设 CLOEXEC**。`dup2(fd,3)` 之后原 fd 仍是非 CLOEXEC，会一起继承给子进程（本探针里子进程同时出现 fd 3 和 fd 4 指向同一 memfd）。功能上无害，但卫生上应 `fcntl(3, F_SETFD, FD_CLOEXEC)` 之后再 fork/exec。

## 三、复现

```sh
# 父进程（"核心侧"）
sh scripts/build_probe.sh                       # 或手工按下面两条编
adb push build/probe/* /sdcard/probe/
adb shell "su -c 'mkdir -p /data/local/tmp/vtouch-probe && cp -f /sdcard/probe/* /data/local/tmp/vtouch-probe/ && chmod 755 /data/local/tmp/vtouch-probe/probe_native'"
adb shell "su -c 'sh /data/local/tmp/vtouch-probe/fdcheck.sh'"      # ③ fd 卫生
adb shell "su -c 'cd /data/local/tmp/vtouch-probe && ./probe_native /data/local/tmp/vtouch-probe --try-ro-write --pump 20'"   # ④ 只读保护
```

| 文件 | 作用 |
|---|---|
| `probe_shm.h` | 共享区布局（magic/ver/size + 心跳/计数 + 只读窗口），父与子 JNI 两侧编同一份 |
| `probe_native.c` | 父进程：`memfd` + 分区 `mmap` + `fork/exec app_process` + 心跳/计数 |
| `probe_jni.c` | 子进程 JNI：映射 fd、双向读写、只读写入测试（期望 SIGSEGV） |
| `ProbeMain.java` | 子进程 Java 壳：attach → 每 100ms tick → 核心停滞 2s 自杀 |
| `fdcheck.sh` | 设备侧一键检查 fd 卫生（查完自动清理） |

## 四、还没验的（需要手持真机配合）

- **旋转时 Surface 要不要重建**：策略 A（图层恒竖屏尺寸 + 合成器 transform）、B（同 Surface 只改 buffer 几何）、C（现状：重建）——只能真人转动手机看。
- **`DisplayListener` 回调**（去掉 320ms 轮询）在 `app_process` 下的可用性（反射注册、Looper 线程）。

## 五、旋转策略实测结论（真机 PJZ110 / ColorOS / Android 16）

面板是"全屏 `SurfaceControl` 图层 + ImGui"，旋转时必须决定 surface 怎么处理。三种策略都真机试过：

| 策略 | 做法 | 结果 |
|---|---|---|
| **B** 只改 buffer 几何 | `Transaction.setBufferSize` + `ANativeWindow_setBuffersGeometry`，surface 与 EGLSurface 都不重建 | ✗ 必失败：**EGL 窗口 surface 的尺寸在创建时固定**，驱动仍按旧尺寸出帧 → 合成器把旧尺寸帧铺满新显示尺寸 = **拉伸** |
| **A** 恒定 buffer + 合成器旋转 | buffer 永远竖屏尺寸，旋转交给 `setGeometry`/`setMatrix` | ✗ 本 ROM 上不可用：`setGeometry(sc, src, dst, orient)` 把图层摆到可视区外（实测整层不可见）；`setMatrix(sc, Matrix, float[])` 抛 `ArrayIndexOutOfBoundsException`（该重载底层 `Matrix.getValues` 要求 9 个元素数组）。靠逐 ROM 试隐藏 API，兼容性差 |
| **C** 重建 surface（**采用**） | Java：`setBufferSize(new)` + 同图层 `new Surface`；native：帧边界销毁旧 EGLSurface → 换 window → 重建 EGLSurface（G 上下文/字体/ImGui 全保留） | ✓ 跨 ROM 稳、零拉伸、只用 `SurfaceControl.Builder`(公开) + `Surface(SurfaceControl)` + 标准 NDK EGL |
| **E** 固定竖屏图层 + 绘制旋转 | 图层按竖屏逻辑尺寸建一次（永不 resize/重建），旋转只在 `px2ndc` 里把内容坐标旋转后画进同一块 buffer | ✗ 不成立：合成器会把**全屏图层按显示尺寸拉伸**，图层声明的 buffer 尺寸拦不住它 → 横屏实测红圆 1219×1440（宽高比 0.847，横向 2.2×/纵向 0.45× 拉伸）、角标被裁出屏外。**根因同时解释了为什么 A/B 也必失败**：只要不允许拉伸，图层声明尺寸就必须等于当前显示的逻辑尺寸 → 每次旋转必须改尺寸 → EGL 窗口 surface 尺寸不可变 → **surface 必须重建 = C** |
| D 双图层 + 原子翻转 | 准备隐藏层 → 单事务 `alpha` 对切 | 机制可行（事件→翻转 3~28ms），但旋转后**合成结果与 native 账本不一致**（日志报 `绘制用=显示尺寸`，屏幕实际只有下半屏 1440×1440 区域、横向拉伸 2.13 倍）→ 未采用，记录备查 |

### C 的最终形态（要搬进面板的四条）

1. **检测**：公开 API `DisplayManager.registerDisplayListener`（`ActivityThread.systemMain()` 拿 system Context）+ **500ms 稳定观察窗**（回调常早于显示状态更新，实测开窗能把"读到过期值→漏掉旋转"变成 6ms 内抓到真值）+ 窗外 2 秒一次保险检查；
2. **顺序**：新 EGLSurface **先建好并提交首帧**，**再**销毁旧的（重叠，不留空档）；建 surface 要**重试**（`0x3003 EGL_BAD_ALLOC` 是 resize 后的瞬态）；
3. **遮挡**：旋转是非等比缩放（2.2×），准备期间可见层仍显示旧尺寸 = 必然拉伸 → 先把可见层 `alpha=0`，新首帧上屏后恢复；
4. **尺寸来源**：图层按**当前显示尺寸**建；绘制尺寸取 **`eglQuerySurface`**，不要用 `ANativeWindow_getWidth/Height`（那是图层 default 几何，换绑后会陈旧 → 实测把场景画成错位椭圆）。

量化（真机三次旋转）：事件→读到正确状态 6~7ms；换绑完成 19~26ms；首帧上屏 +6ms；**可见层不可见总时长 40~80ms**（这是裸图层的物理下限）。

### 过程中踩到并已写进代码注释的坑

| 坑 | 症状 | 规避 |
|---|---|---|
| `ActivityThread.systemMain()` 放非主线程 | 进程**静默消失**、无异常日志 | 只能主线程调；且主线程要先 `Looper.prepareMainLooper()`，否则 `RuntimeException: Can't create handler …` |
| `fork/exec app_process` 环境不全 | 子进程**连 main 都进不去**、静默退 0 | 必须带完整 `environ`（不是精选几个变量） |
| `memfd` 本体 fd 未设 CLOEXEC | 子进程多继承一份 fd（无害但不洁） | `dup2` 后对新 fd 补 `FD_CLOEXEC` |
| `-O2` 把成对 `sinf/cosf` 融成 `sincosf/sincos` | `dlopen: cannot locate symbol` → `.so` 加载失败 | 探针直接改用预计算单位圆表，不碰 libm |
| `glUseProgram`/`glBindBuffer` 漏写 | 只出清屏色、没有图元，且不报错 | 补齐；`glGetError` 在初始化后与首帧各查一次 |
