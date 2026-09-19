# UI 接入方案（核心为准 · 共享内存 · 单执行文件）

> 状态：方案定稿待实现。旋转/横屏部分已**真机判定完毕**（见 §8），核心侧改动清单见 §6。
> 唯一可信源：盘上代码。本文所有结论都带 `文件:行号`。

## 1. 目标与不可协商项

| # | 要求 | 落实方式 |
|---|---|---|
| 1 | **以核心为准** | 核心先把自己完整拉起来（`vtouch_init()` 全部成功）后才起面板；核心 `cleanup()` 关面板；面板崩不带走注入 |
| 2 | 核心启动面板（不是面板启动核心） | `vtouch_init()` 最后一步 `vt_panel_start()`；面板是核心的**子进程** |
| 3 | 面板独立进程、崩溃隔离 | fork/exec；面板只对状态区有**只读**映射（MMU 强制，实测写只读区→SIGSEGV 且父进程照跑） |
| 4 | 延迟尽可能低 | 状态**本身**放在共享内存（零拷贝、零系统调用、零命令往返）；面板编辑 = 一次内存写 + `region_gen++` |
| 5 | 不引入命令通道 | 面板的"操作"本质全是数据写入（改区域表/面板矩形/停止标志），不需要请求-应答 |
| 6 | 核心改动最小 | 默认构建（`VT_UI` 关）`.text` 逐字节不变；新增文件为主，改动集中在 6 处 |
| 7 | 跨设备/跨 ROM 兼容 | 只用公开 API + 稳定隐藏 API（`Surface(SurfaceControl)`）；不硬编码 ROM 私有行为，能力探测 + 优雅降级 |
| 8 | 单可执行零依赖 | 面板复用核心同一份代码 + 单文件 JS 客户端，不引入额外载体 |

## 2. 交付物

| 产物 | 状态 |
|---|---|
| `src-ui/vtouch_ui.cpp`（ImGui 面板；行数以现读为准，约 2131 行） | ✅ 已从备份还原 |
| `src-ui/VTouchUI.java`（图层壳） | ✅ 已还原 |
| `thirdparty/imgui` v1.91.8 | ✅ 已就位 |
| `src-ui/ui_stubs.c`（桩：11 个 API） | ✅ 已写，桩模式单跑在真机跑通（`first frame t=+315ms`） |
| `scripts/build_ui.sh`（`VTOUCH_UI_CORE=stub\|real`） | ✅ 双模式 |
| 旋转方案 | ✅ 真机判定完成 = **策略 C**（§8） |
| 共享内存架构 | ✅ 可行性探针真机验证通过（memfd + fork/exec + 只读映射 + CLOEXEC + 心跳） |
| 核心侧改动 | ⬜ 待做（§6） |
| `src-ui/ui_glue.c` + `src/vt_panel.c` | ⬜ 待做 |
| 启动时序图 / 机制图 | ⬜ 待做（与代码同版交付） |

## 3. 架构总览

```
                ┌──────────────────────── 核心进程（app_process，由 service.sh 起）────────────────────────┐
service.sh ──▶  │ Java 壳：建全屏图层（先不可见）→ 交给 native                                        │
   (root)       │                                                                                     │
                │ native 主流程：                                                                     │
                │   ① vtouch_init()：开触摸屏 → EVIOCGRAB → 建 uinput 合并设备 → 监听 27183 → 区域线程  │
                │   ② 建共享内存（单 memfd，三区）→ g 搬迁进区 A（只读给面板）                          │
                │   ③ vt_panel_start()：fork/exec 面板子进程，把「状态区 fd + 事件环 fd」按固定 fd 传下去 │
                │   ④ 图层变可见 → while (vtouch_poll_step() == 0);                                    │
                │   ⑤ 退出：cleanup() → 停区域线程 → 释放 EVIOCGRAB → vt_panel_stop()                    │
                └─────────────────────────────────────────────────────────────────────────────────────┘
                                              │  memfd（按固定 fd 继承，FD_CLOEXEC 卫生）
                                              ▼
                ┌──────────────── 面板进程（app_process --nice-name=vtouch-ui）──────────────────────┐
                │ 自己的全屏图层（SurfaceControl + EGL + ImGui 渲染线程）                              │
                │ 区A 只读映射（读状态）｜区B 读写（区域表 / 面板矩形 / stop_req）｜区C 只读（事件环）  │
                │ 无 socket、无 eventfd、无 opcode、无解析器、无回复通道                               │
                └────────────────────────────────────────────────────────────────────────────────────┘
```

**数据流**：核心写 `g.phys[]/g.virt[]` + 区域表 → 面板直接读（零拷贝）；面板写区域表 + 面板矩形 + `stop_req` → 核心立刻醒（面板持**唤醒 pipe** 的写端，投编辑时写 1 字节；核心在 poll 上等它，不再靠 8ms 轮询）。

**唯一"请求"**：面板要停引擎 → 写 `stop_req` + 给父进程发 `SIGTERM`（核心已有 `on_signal` → `stop_flag`，打断 `poll()` 立即生效）。

## 4. 共享内存布局（单 memfd，三区，页对齐）

| 区 | 大小 | 权限 | 内容 | 同步方式 |
|---|---|---|---|---|
| **A** | 3 页 12KB | 核心 RW / 面板 RO | `header{magic,ver,hb,ui_hb}` + `struct vt_state`（就是现在那个 `g`） | 单字段原子；`hb`/`ui_hb` 各有写方，互看 |
| **B** | 1 页 4KB | 双方 RW | 区域表 `regions[32]` + `region_gen` + 自旋锁 + 面板矩形 `{x1,y1,x2,y2,visible}` + `stop_req` | 自旋锁 + `gen` 双检；矩形用 seq 奇偶校验 + 一次重试（拿不准保守不吞） |
| **C** | 2 页 8KB | 核心 W / 面板 RO | 事件环：64 × 96B 文本行 + head/tail | SPSC 无锁，环满丢最旧 |

**契约纪律**：
- 面板对区 A **无写权限**（MMU 强制，实测越界写 → SIGSEGV，只死面板）；
- 无锁读者会看到"半更新"，所以面板读复合状态（如矩形整体）必须走 seq 校验；
- 布局是**双侧契约**，改一处必须同时改另一侧 → 用 `magic` + `ver` 校验，不匹配直接拒绝启动。

**为什么 `g` 能整体搬进映射**（已验证）：全库只有一处定义（`src/vtouchd.c:57-60`）；结构体纯 POD、**无指针成员**（可整块 `memcpy`）；没有别的标识符叫 `g` → 用

```c
extern struct vt_state *g_ptr;   /* vt_internal.h */
#define g (*g_ptr)
```

全库 224 处调用点**一行不改**。初始化时把 `static const struct vt_state G_INIT` 拷进映射即可。

## 5. 启动时序

```
service.sh(root)
  └─ app_process --nice-name=vtouch-ui VTouchUI 1440 3168       ← 进程入口（必须带 ART 才能建系统图层）
       ├─ Java 壳：SurfaceControl.Builder → 全屏 trusted overlay（先 alpha=0/不可见）→ 交给 native
       └─ native：
            vtouch_init()  ── 成功 ─┐
            （开触摸屏/EVIOCGRAB/uinput/27183/区域线程；任一步失败 → 直接退出，图层从未可见）
                                    ├─ 建 memfd + 三区映射 → g 搬入区 A
                                    ├─ vt_panel_start() → fork/exec 面板子进程（传状态 fd + 事件环 fd）
                                    └─ 图层变可见 → while (vtouch_poll_step() == 0);

面板子进程：attach 映射 → 校验 magic/ver → 起 EGL/ImGui 渲染线程 → 首帧
```

**硬规则**：UI 由核心启动（`vtouch_init()` 的最后一步）；UI 由核心关闭（`cleanup()`）；Java 只是建层的壳。
**fork 到 exec 之间只能调 async-signal-safe 函数**（`fork` 后不 `malloc`，`exec` 前只 `dup2/setenv/execve`）。
**`fork/exec app_process` 必须传完整 `environ`**——实测只传精选变量时 ART 起不来、子进程连 `main` 都进不去、静默退 0。

## 6. 核心侧最小改动清单

| 文件 | 改动 | 估算 | 为什么必须 |
|---|---|---|---|
| `src/vt_internal.h` | `extern struct vt_state *g_ptr; #define g (*g_ptr)` | 3 行 | 让 224 处调用点不动 |
| `src/vtouchd.c` | `struct vt_state g = {…}` → `static const struct vt_state G_INIT`；init 开头建 memfd + 映射 + `memcpy` | ~10 行 | 状态进共享内存 |
| `src/vt_panel.c`（新） | fork/exec 面板、fd 传递（`dup2` 到固定 fd）、心跳监控、`vt_panel_stop()` | ~90 行 | 核心启动 UI |
| `src/vt_shm.h` | 区 B 的布局（`struct vt_shm_edit` 编辑邮箱 + `struct vt_shm_b`：自旋锁 `lock`、`edit_applied`、矩形 seqlock `rect_seq`）+ 双侧访问器原型 | ~60 行 | 双侧契约唯一出处 |
| `src/vt_region.c` | `region_del(id)` / `region_rename(old,new)`；变更后 `gen++`；区域线程改读共享表 | ~25 行 | **绕不开**：`region_gen` 是文件内静态，面板直接改表碰不到它，区域线程会拿过期状态（`src/vt_region.c:290` 的 `r_seen_gen != region_gen` 重置私有状态） |
| `src/vt_frame.c` + `src/vt_input.c` | 吞触摸：`eaten[]` 锁存 + `emit_frame()` 物理段跳过（`src/vt_frame.c:140-169`）+ 可选 `enqueue_phys_changes()` 跳过 | ~12 行 | 只有核心能做（`EVIOCGRAB` 后 App 看不到任何事件） |
| `src/vt_queue.c` | `outq_push_text()` 开头把文本行也写进事件环 | 2 行 | 面板免 socket 收事件 |
| `src/vtouchd.c` + 各 fd | `input_fd`/`u_fd`/`listen_fd`/`client_fd` 设 `FD_CLOEXEC`；**子进程里再显式清掉共享内存目标 fd 的 CLOEXEC** | ~8 行 | **硬性检查项**：不做 → 面板继承带 `EVIOCGRAB` 的 fd，核心退出后 grab 不释放、**物理触摸回不来**。⚠️ 反向坑：父进程给 memfd 设了 CLOEXEC 后，若它恰好就是 `VT_SHM_FD`，子进程不会走 `dup2`（而 `dup2` 是唯一会清 CLOEXEC 的操作）→ fd 在 exec 时被关掉、随后被 ART 复用成别的 fd（实测被复用成 socket，面板拿不到共享内存）→ 必须 `fcntl(VT_SHM_FD, F_SETFD, 0)`。验证：`ls -l /proc/$(pidof vtouch-ui)/fd` 里 `memfd:vtouch-shm` 有、`/dev/input/event*` 无（验收脚本已从仓库移除） |

**不再需要**（相对早期方案删掉）：`vtouch_poll_step(ms)` 加 timeout 参数、命令通道/socketpair/eventfd/opcode/解析器、每帧 memcpy 镜像、`outq_push_text` 的"面板当 WS 客户端"版本、`region_del/rename` 做成 C 接口给面板直接调。

## 7. 面板侧改动（胶水接口不变，只换实现）

面板代码里 `extern "C" { … }` 块里的 C 入口声明（`src-ui/vtouch_ui.cpp:27` 起）**一行不改**，`src-ui/ui_glue.c` 换实现：

| 老接口 | 新实现 |
|---|---|
| `vtouch_init/cleanup` | 无（核心管的）→ 空实现 |
| `vtouch_poll_step(ms)` | 改成"等事件环有新数据"（阻塞在事件环 head/tail 上） |
| `vtouch_phys_slots/phys_get` | 读区 A 的 `g.phys_slots` / `g.phys[]` + `raw_to_logical` |
| `vtouch_region_count/get` | 读区 B 区域表（自旋锁 + 一次重试） |
| `vtouch_region_add` | 写区 B（同 id 原地更新，语义同 `vt_region.c`）+ `gen++` |
| `vtouch_region_del/rename` | 新增（核心侧同步提供 `region_del/region_rename`） |
| `vtouch_region_clear` | 写区 B：`count=0` + `gen++` |
| `vtouch_set_hooks()` | **按值拷贝**（`src-ui/vtouch_ui.cpp:2034-2032` 传的是栈上临时量；存指针会悬空 → 曾导致 SIGBUS）。`consume` 语义改成"面板往区 B 推矩形，核心自判" |

## 8. 旋转 / 横屏适配（本次重点）

### 8.1 结论：核心**完全不需要**旋转感知

核心/脚本/区域表/事件坐标统一是**竖屏逻辑坐标**（`-w/-h` 指定，`src/vtouchd.c:75-80`）：

| 环节 | 位置 | 说明 |
|---|---|---|
| raw → 逻辑 | `src/vt_util.c:134`（`raw_to_logical`），`size = axis ? logical_height : logical_width` | 区域判定用 |
| 逻辑 → raw | `src/vt_util.c:112`（`logical_to_raw`） | 注入用（WS 命令 `src/vt_ws.c:498-536`） |
| 区域合法性 | `src/vt_region.c:64` | 以 `logical_width/height` 为界 |

而触摸控制器的 raw 轴是**面板物理轴**（本机 0..23040 × 0..50688，物理锚定、不随显示方向变），注入也写回同一个合并设备 → **区域 = 物理锚定的矩形**，随设备一起"转"，框架负责把逻辑坐标映到当前显示方向。所以：

> **核心、区域表、脚本、注入：零旋转感知。** 唯一需要旋转感知的是面板的"图层/绘制/命中"这三件事。

### 8.2 面板的双坐标空间（老面板已经做对，照搬即可）

`src-ui/vtouch_ui.cpp:207-223` 的模型：

```
外空间（永不变）＝ 竖屏逻辑坐标 g_w×g_h
                ＝ daemon 坐标系 ＝ 区域表 ＝ 事件坐标 ＝ 脚本看到的坐标
内空间（随方向变）＝ 当前屏坐标 g_scr_w×g_scr_h
                ＝ ImGui / 面板布局 / 命中判定所在空间 ＝ 图层 buffer 尺寸
换算：p2c()（外→内，四方向纯旋转无缩放）  src-ui/vtouch_ui.cpp:215-223
```

### 8.3 横屏适配清单（逐条已实现/待实现）

| # | 环节 | 代码锚点 | 处理 |
|---|---|---|---|
| 1 | **图层 surface** | `VTouchUI.java` 显示变化处理 | 唯一真正"重建"的地方 → 策略 C（§8.4） |
| 2 | **绘制尺寸来源** | 面板渲染线程 | 取 `eglQuerySurface` 的真实 buffer 尺寸；**不要**用 `ANativeWindow_getWidth/Height`（那是图层 default 几何，换绑后会陈旧 → 实测把场景画成错位椭圆） |
| 3 | **ImGui 画布** | `src-ui/vtouch_ui.cpp:1640` | `io.DisplaySize = 当前屏尺寸`；旋转后强制全量重绘（`g_force_frames`） |
| 4 | **ImGui 输入** | `src-ui/vtouch_ui.cpp:1653` `AddMousePosEvent(mx,my)` | 触点先 `p2c` 再喂 ImGui → 面板交互在横屏同样准确 |
| 5 | **命中判定** | `src-ui/vtouch_ui.cpp:409 in_panel()` | 一律在内空间算；谓词 `ui_consume_cb` 先把外部坐标 `p2c` 进来（`src-ui/vtouch_ui.cpp:424-428`） |
| 6 | **面板自身落位** | `src-ui/vtouch_ui.cpp:226-248 on_display()` | 未拖动过则回右上角（公式与 `nativeInit` 一致）；拖过则夹回屏内 |
| 7 | **几何唯一来源** | `src-ui/vtouch_ui.cpp:94` 注释 | 面板几何有**三处同公式**（`#define` + `panel_w()`/`in_panel()` + `build_panel()`）→ 改一处必须三处同改，否则"看着对、点不着" |
| 8 | **稳定窗** | `src-ui/vtouch_ui.cpp:213`、`:232`、`:426` | 换向后 500ms 内吞触摸谓词**一律返回 0**（宁放不吞）。原因见 `src-ui/vtouch_ui.cpp:230-231` 注释：谓词是"按下问一次、锁存整段手势"，一次错判会吞掉/漏掉整段 |
| 9 | **区域绘制/轨迹** | `src-ui/vtouch_ui.cpp:853/899/906/923/951/1647` | 区域表与触点是**外空间**数据 → 画之前统一 `p2c` |
| 10 | **面板矩形推给核心**（新架构新增） | `src-ui/ui_glue.c` 新代码（`vt_shm_publish_rect()`） | 必须推**竖屏逻辑坐标**（内→外的逆变换）→ 这是搬迁后唯一的新增旋转适配点，且是**静默 bug 型**（写错只是吞错位置，不报错） |
| 11 | **稳定窗推给核心**（新架构新增） | 同上 | 核心看不到 Java 的稳定窗 → 窗内**推空矩形**（等同"不吞"），把策略 8 搬到新架构 |

### 8.4 旋转策略定型：C（重建 surface）

四条策略真机判定完毕（`probe/` 目录及其中的 `probe/README.md` 已按要求从仓库移除，删除提交 `be3e9c3` —— 原文用 `git show be3e9c3^:probe/README.md` 取）：

| 策略 | 判定 |
|---|---|
| A 恒定 buffer + 合成器旋转（`setGeometry`/`setMatrix`） | ✗ 本 ROM 不可用（`setGeometry` 把图层摆到可视区外；`setMatrix(Matrix,float[])` 抛 AIOOBE；`setMatrix(sc,float[])` 不存在） |
| B 只改 buffer 几何（不重建） | ✗ EGL 窗口 surface 尺寸创建时固定 → 必拉伸 |
| E 固定竖屏图层 + 绘制旋转 | ✗ 合成器把全屏图层**按显示尺寸拉伸**（实测红圆宽高比 0.847），声明尺寸拦不住 |
| D 双图层 + 原子翻转 | 机制可行（事件→翻转 3~28ms），但旋转后合成结果与 native 账本不一致 → 未采用 |
| **C 重建 surface** | ✓ **采用**：零拉伸、跨 ROM 兼容 |

**统一的物理解释**（A/B/E 为什么都失败）：合成器把图层映射到**当前显示的逻辑矩形**，且**没有人替你旋转图层** —— 所以只要"声明尺寸 ≠ 显示逻辑尺寸"就必然各向异性拉伸；而 EGL 窗口 surface 尺寸不可变 → 尺寸一变只能换 surface。

**C 的四条落地要求**：

1. 事件驱动检测：公开 `DisplayManager.registerDisplayListener` + 500ms 稳定观察窗（回调**早于**状态更新，实测开窗后 6ms 抓到真值）；窗外每 2s 一次漏事件保险；
2. 顺序：新 EGLSurface **先建好并提交首帧**，**再**销毁旧的；建 surface 要**重试**（`0x3003 EGL_BAD_ALLOC` 是 resize 后瞬态）；
3. 遮挡：准备期间可见层 `alpha=0`（挡住拉伸帧），首帧真上屏后恢复 —— 用 native→Java 回调而不是定时猜；
4. 尺寸：图层按**当前显示逻辑尺寸**建；绘制尺寸取 `eglQuerySurface`。

**量化基线**（真机三次旋转）：事件→读到正确状态 6~7ms；换绑完成 19~26ms；首帧上屏 +6ms；**不可见总时长 40~80ms**（裸图层的物理下限，系统自己的旋转动画会盖掉大半）。

**可继续压榨的点**（未做，留作 P6）：窗内改成 2~4ms 紧凑复查；按"预测尺寸"先动手再确认；准备帧 `eglSwapInterval(0)`；180° 旋转（0↔2）长宽比不变 → **可不遮挡、零空白**；试 `registerComponentCallbacks`（公开 API，回调里带 `Configuration` = 带数据的事件源）→ 成立则真正零复查。

## 9. 生命周期与故障

| 情况 | 行为 |
|---|---|
| 核心正常退出 | `cleanup()` → 停区域线程 → 释放 `EVIOCGRAB` → 停面板 → 退出 |
| 面板崩溃 | 核心检测心跳停滞（`hb` 不再更新）→ 记录日志 → **注入继续**（面板崩不带走注入）；默认最多重启 3 次/分钟（待拍板） |
| 核心崩溃/被杀 | 面板发现 `hb` 停滞 → 自杀退出（实测：核心停 2s → 面板自行退出）；`EVIOCGRAB` 随核心进程死释放 → 物理触摸恢复 |
| 面板要停引擎 | 写区 B `stop_req` + `SIGTERM` 父进程 → 核心走正常收尾 |
| 面板写只读区 | SIGSEGV（只死面板），核心照跑（实测 `status=0xb`，父进程继续） |
| 面板越界写区 B | 自旋锁 + seq 校验挡住撕裂；矩形不可信时保守**不吞** |

## 10. 验收门（分阶段）

| 阶段 | 内容 | 验收门 |
|---|---|---|
| P0 | 本方案 + 启动时序图入库 | 文档事实与代码一致（逐条 `文件:行号` 抽查） |
| P1 | 核心侧改动（`g` 搬迁 + memfd + CLOEXEC + 吞触摸 + `region_del/rename`） | ① 默认构建 **`.text`/`.data`/`.rodata` 逐字节不变**（实测通过；整文件 md5 差异只来自新增两个空 TU 的文件名符号）；② 关掉 UI 跑主机侧 smoke（握手 + 注入链路）全绿；③ `/proc/<ui_pid>/fd` 回读：`event/uinput` **0 条**、`memfd:vtouch-shm` **≥1 条**（实测通过）；④ 触摸注入功能与改动前一致 |
| P2 | `vt_panel_start()` + 面板子进程拉起 | ✅ 已实测：核心起 → 面板起（父进程=核心）→ 面板 fd 表 `memfd:vtouch-shm` 1 条、`event/uinput` **0 条**；杀面板 → 核心继续注入并按策略重启 |
| P3 | `ui_glue.c` + 面板 12 接口换实现 | ✅ 已实测：面板 `已接核心（逻辑 1440x3168 core_pid=…）`；`regions.conf` 3 条 → 面板投邮箱 → 核心 `region add` 三条全落地（total 3）→ 面板卡片 `监听中 · 3/32`、`ui_wide 已停用` 与核心状态逐项一致（截图复核）；`fd 3 -> /memfd:vtouch-shm` 且触摸设备 fd 0 条 |
| P4 | 面板编辑 → 核心生效 | 手指点面板不穿透（`consume` 生效）；新建/移动/删除/改名区域后核心区域线程 1~2ms 内吃到 |
| P5 | 旋转 | 策略 C 四条到位：零拉伸、不可见 ≤80ms、稳定窗内不吞触摸、矩形换算正确（横屏下点面板命中准确） |
| P6 | 性能与打点（可选） | 延迟直方图（P50/P99）出现在面板上；`eglSwapInterval(0)`、紧凑复查等收益可测 |

## 10.1 P1 已实测（真机 PJZ110）

| 项 | 结果 |
|---|---|
| 默认构建不变 | `.text` 22856 B / `.data` 8272 B / `.rodata` 2052 B，与改动前**逐字节相同**（基线由 `git archive HEAD` 另建一棵树编出，比较前先断言文件存在——写松了会把"文件缺失"读成"变了"） |
| 共享内存 | `vtouchd: 共享内存就绪 fd=3 total=28672 state@4096(12288) b@16384 c@20480` |
| 核心拉起面板 | `面板已启动 pid=2522（dir=/data/local/tmp/vtouch-ui shm_fd=3）`，面板父进程 = 核心 pid ✓ |
| fd 卫生 | 面板 `fd 3 -> /memfd:vtouch-shm`；指向 `/dev/input/event*` 或 `/dev/uinput` 的 fd **0 条** ✓ |
| 面板崩/被杀 | `面板已退出 status=0x9（核心继续跑，注入不受影响）` + 按策略重启、1 分钟上限 3 次 ✓ |
| 停核心 | SIGTERM → 面板一起停 ✓ → `/dev/input/event8` 持有者回到系统自身 3 个 ✓（grab 正确释放） |
| 无 UI 降级 | 面板目录缺失时 `面板未就绪（缺 …/classes.dex）→ 以无 UI 模式继续`，引擎照常起来 ✓ |
| 面板产物 | 必须连同 **`libc++_shared.so`** 一起交付（NDK 默认动态链 libc++；缺它 `System.load` 抛 `UnsatisfiedLinkError`，被 `src-ui/VTouchUI.java:223-224` 的 catch 吞掉后 `return` → **进程退 0、什么都不干**，极难查）。已由 `scripts/build_ui.sh` 自动带上 |

验证脚本：现役是 `scripts/ui_ondev.sh`（设备侧 `su -c 'sh …'`：默认 = 起、`stop` = 停并验证 grab 已释放、`status` = 只看现状），主机侧一键走 `scripts/ui-deploy.sh all|stop|status`（会把它推上设备再调）。

## 10.2 P3 联调实测（真机）

链路：面板启动读 `regions.conf` → `vtouch_region_add` 投**编辑邮箱** → 核心 `vt_shm_edit_apply()` 按 `region_add` 语义生效 → 面板每帧从区 A 回读并绘制。

```
vtouch-ui: 已接核心（逻辑 1440x3168 core_pid=14811 面板 pid=14815）
vtouchd: region add ui_rect   type0 120,700,1320,1500 en1 (total 1)
vtouchd: region add ui_circle type1 720,2300,260,0   en1 (total 2)
vtouchd: region add ui_wide   type0 200,2500,1240,2900 en0 (total 3)
面板: panel init 1440x3168 regions=3 / first frame t=+291ms draw=19ms
面板标题栏: 监听中 · 3/32 · 2.4ms      ← 区域数与引擎延迟都是核心的真实数据
```

**过程中修掉的三个真 bug（都由"验收判据"抓出，不是猜的）**：

| 现象 | 根因 | 修法 |
|---|---|---|
| 面板上线即 `SIGSEGV (SEGV_ACCERR)`，backtrace `vt_shm_ui_tick+32`、fault addr = 头部页 `+0x28`（`ui_hb` 偏移） | 我把**头部**也映射成只读，而 `ui_hb`/`panel_pid` 本来就是面板写的 | 头部 + 区 B 面板可写；**区 A 单独再映射一段 `PROT_READ`** 盖住 —— 状态只读由 MMU 强制，头部只读属误伤 |
| `regions.conf` 3 条只落地 1 条 | 编辑邮箱是**单槽**的，连续投会被覆盖 | `glue_post()` 投完等核心吃掉（`edit_applied == seq`，上限 1s）；顺带把"调用返回即已生效"的同步语义还给面板 |
| 冒烟脚本 fd 判据误报"不合格" | 用的是日志里最后一个 pid（可能已退出/被重启过），且 `grep event` 会命中 ART 自己的 `anon_inode:[eventfd]` | 判据改成"当前活着的面板 pid" + 精确匹配 `/dev/input/event\|/dev/uinput` |

**待人工确认（需要手指，机器造不出来）**：① 手指点面板 → 面板响应（ImGui 输入来自核心物理槽表）；② 点面板**不穿透**到后面 App（核心日志应出现 `面板吞掉 slotN @x,y`）；③ 面板上改区域 → 核心日志出现 `region add/upd/del`。

## 11. 风险与对策

| 风险 | 对策 |
|---|---|
| 共享内存 bug 难查 | `magic`+`ver` 校验；`--dump-shm` 调试开关；把区布局收在一个头文件（`src/vt_shm.h`） |
| `g` 搬迁侵入核心 | 只加 3 行宏；默认构建 `.text` 不变作为回归门 |
| 双写区并发纪律 | 自旋锁 + `gen` 双检；读者拿不准时保守（不吞触摸、旧数据可接受处才读） |
| fork 到 exec 只能 async-signal-safe | 面板启动代码整体审查；预先算好 argv/envp 字符串 |
| 面板继承带 grab 的 fd | **硬性检查项**：`FD_CLOEXEC` + 装机后用 `/proc/<ui_pid>/fd` 回读断言 |
| 面板尺寸/坐标换算错位（静默） | 单一换算函数 + 横屏真机点测（P5）；几何三处同公式的纪律 |
| 旋转仍有一次视觉中断 | 已量化 40~80ms；若要零瑕疵只能窗口化（需 window token 隐藏 API，与兼容性冲突）→ 列为可选升级 |

## 12. 待拍板项

| # | 问题 | 默认取值 |
|---|---|---|
| 1 | 区域表挪到区 B（~30 行机械改动，换来 MMU 保护）还是区 A 也对面板开 RW（0 行，丢保护） | 挪到区 B |
| 2 | 面板崩了核心要不要自动重启面板 | 重启，上限 3 次/分钟 |
| 3 | 被吞的触点要不要也从区域事件里排除 | 排除（已实现：`src/vt_frame.c:273` 的 `enqueue_phys_changes` 跳过 `ui_eaten`，被面板吞掉的手整段不进区域判定） |
| 4 | 面板要不要画虚拟触点轨迹 | 画（约 10 行，读区 A `g.virt[]`） |
| 5 | P6 的旋转优化（180° 零空白、`eglSwapInterval(0)`、紧凑复查）现在做还是接完主线再做 | 接完主线再做 |
