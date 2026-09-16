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
