# vtouch 代码全程走读（现读行号版）

> 本文所有行号都是**本次从磁盘源码逐行读出来的**（不是抄文档、也不是历史会话记忆）。
> 工作区状态（`git status` 本次回读）：`src/vtouchd.c`、`src-ui/vtouch_ui.cpp`、`src-ui/VTouchUI.java`、
> `scripts/build_bundle.py`、`scripts/build_ui.sh`、`scripts/verify_bundle.py` 相对 `HEAD`(5000318, 2026-09-12 19:19)
> 都**有未提交改动**（core +1106 行、面板 +664 行、Java +175 行）⇒ 盘上代码比仓库 HEAD 新；
> 而 `README.md`、`AGENTS.md`、`docs/**` 在**工作区已被删除**（HEAD 里还在，`git checkout -- docs` 可恢复）。
> 读码结论一律以盘上文件为准。

---

## §0 怎么读这份文档

系统分四层、跨两个进程边界，一次触摸会穿过全部四层：

| 层 | 文件 | 跑在哪个进程 | 职责 |
|---|---|---|---|
| L1 脚本库 | `scripts/build_bundle.py` 里的 JS 文本（产物 `clients/vtouch_bundle.js`） | AutoJs6 进程 | 部署/启动面板、握手、订阅、解析事件、注入命令、坐标换算、退出收尾 |
| L2 面板 Java | `src-ui/VTouchUI.java` | `app_process` 面板进程 | `load .so`、反射建 composer 图层、把 Surface 递给 native、40ms 轮询可见性与方向 |
| L3 面板 native | `src-ui/vtouch_ui.cpp` | 同上 | ImGui 渲染 + 面板交互 + overlay、注册 4 个回调、起 poll/render 线程 |
| L4 核心 | `src/vtouchd.c` | 同上（编进同一个 `.so`；headless 形态下自己就是一个进程） | `EVIOCGRAB` 抓物理屏、解析 Type-B 帧、合并物理+虚拟、写 uinput、区域匹配、WS 服务端 |

headless 兜底形态：`src/vtouchd.c:1777 main()` 独立进程（`/data/local/tmp/vtouchd`），**没有** L2/L3。

三条最容易记混的边界，先钉死：

1. **坐标有三个域**：raw（内核轴，量程即 `axmin/axmax`；「该机 = 逻辑 ×16」是既有真机读数，本轮未复测）、竖屏逻辑（`g_w×g_h` = 区域表 = 事件坐标 = 脚本坐标，**转向不变**）、当前屏（ImGui/面板命中，随方向变）。换算只有两处：core 的 `logical_to_raw`/`raw_to_logical`（`vtouchd.c:257/270`）和面板的 `p2c`（`vtouch_ui.cpp:193`），脚本侧对应 `vtC2P/vtP2C`（`build_bundle.py:280/287`）。
2. **区域只有一份存储**：`/data/local/vtouch-runtime/regions.conf`（`vtouch_ui.cpp:283-285`），面板独占，脚本只能回读(`rgList`)/下发(`rgPush`)。
3. **WS 只有一条连接、一条 FIFO**：daemon 单 client，新连接踢旧的（`vtouchd.c:1724-1730`）；事件行与命令回包共用同一出站队列（`vtouchd.c:150-244`）。

---

### §0.1 来源分级（这份文档的自我约束：**只有代码算证据**）

| 级别 | 来源 | 本文的处理 |
|---|---|---|
| **A** | 盘上源码 + 本次工具输出（`grep`/`wc`/`verify_bundle.py`/渲染与版面校验） | 唯一可作为结论的依据；每条都能照行号或命令复现 |
| **B** | 源码里的注释与字符串字面量 | 可信度 = 作者当时的认知，行为仍以代码为准（例：`build_ui.sh:48` 说「历史上 SCProbe.class 混进过 dex」——那是注释的说法，不是本轮事实） |
| **C** | `docs/**`、`README.md`、`AGENTS.md`、skill 笔记、历史会话 | **只作线索，必须回代码核**；凡引用 C 级的一律显式标注「据文档/据记录、未复现」 |

另外：凡涉及 **AutoJs6 运行时行为**的说法（Looper 泵事件、JS 引擎共享锁、`threads.start` 与主线程的连带退出、`shell(cmd,true)` 的能力边界），本文只按**代码注释**（B 级）与既有记录（C 级）转述，**本轮没有真机复测**；这类说法要当结论用，得先在设备上做 A/B。

**缺席类断言（「代码里没有 X」）必须给 grep 计数**——本文涉及的全部逐条核过：

| 断言 | 核验 | 结果 |
|---|---|---|
| 没有 `recvBlocking` | `grep -rn recvBlocking src src-ui scripts/build_bundle.py` | **0**（只在旧代备份 `build/_wt_backup_20260913_150905/scripts__build_bundle.py:226`） |
| 没有 `LinkedBlockingQueue` / pump 线程 / `reserved` 占位 | 同上 | **各 0** |
| 没有 `vtouch_set_consume_cb` | 同上 | **0**（吞触摸走合并后的 `hooks.consume`，`vtouchd.c:97/574`） |
| 没有 `vtouch_set_event_cb` / `vtouch_ui_sync` | 同上 | **各 0** |
| 线程未命名 | `grep -rn pthread_setname_np src src-ui` | **0**；`pthread_create` 共 4 处：`outq :1670`、`render :1937`、`poll :1941`、测试线程 `:1930` |
| `touch_cb` 全量镜像已废 | `grep -rn touch_cb src src-ui` | 2 处**都在注释里**（`vtouchd.c:906/1767`），无实现 |
| `vtouch_init_ui` 无人调用 | `grep -rn vtouch_init_ui src src-ui` | 只有定义 `vtouchd.c:1768`，**零调用者** |

**已知的文档错值（C 级，别信）**：

| 出处 | 它说的 | 盘上代码事实 |
|---|---|---|
| 项目上下文里的 `AGENTS.md`（内容 = `c22415b`(2026-09-09) 前后那一代） | `src/vtouchmerge.c`/`vtouchws.c`/`vtouchsupervise.c`、`ksu-module/`、`sdcard/vtouch-merge/`、`scripts/install_from_sdcard.sh`、`tests/ws_smoke.py` | 全都**不存在**：盘上是「面板 = daemon」单进程（`src/vtouchd.c` + `src-ui/*`），`tests/` 只有 `onregion_harness.js`；HEAD 里的 AGENTS.md 已是重写版（「面板（ImGui）本身就是 daemon」） |
| skill 笔记 `vtouch-autojs6` 的若干条 | `recvBlocking`（标 `build_bundle.py:226`）、FIFO 256（`LinkedBlockingQueue`）+ pump 线程、`reserved` 占位、`vtouch_set_consume_cb`、桩测 **98** 断言 | 现码：`available()` 非阻塞读 + 2s ping/6s pong（:617–627）、每事件 `threads.start`（:562）、无 reserved、`hooks.consume`、桩测 **46** 断言 |
| 归档图 `docs/diagrams/vtouch-full-flow` / `vtouch-click-journey`（HEAD 里、工作区已删）与草稿 `build/_gen_diagrams.py` | 同一代说法（recvBlocking / LinkedBlockingQueue / pump / `vtouch_init:1495`） | 同上；现况请看 `docs/diagrams/vtouch-current-journey.*` |

## §1 全景：进程 / 线程 / 数据流

```
AutoJs6 进程                          面板进程 (app_process, nice-name=vtouch-ui)
┌───────────────────────────┐         ┌───────────────────────────────────────────────┐
│ 主线程(脚本)              │         │ Java 主线程  VTouchUI.main :139                │
│  · require bundle         │         │   40ms 轮询：图层可见性 + 每 8 tick 查方向     │
│  · vt.onRegion(...)       │  shell  │   (VTouchUI.java:170-227)                      │
│  · setInterval 保活 :647  │────────▶│                                               │
│  · exit 钩子 :237         │ 起进程  │ render 线程  render_thread_fn :1674            │
│                           │         │   EGL/字形/首帧、快照→ImGui→swap               │
│ 读线程 (threads.start)    │         │ poll 线程    poll_thread_fn :707               │
│  · conn.recv() 轮询 :615  │         │   vtouch_poll_step :1680                       │
│  · rgParseEv :776         │         │    ├ 物理读 physical_events :1196               │
│                           │   WS    │    ├ 合帧 emit_frame :559  → /dev/uinput        │
│ 回调线程 (每次事件一个)   │◀───────▶│    ├ 区域匹配 region_match :961                 │
│  · fn(h) :562 threads.start│ 27183  │    └ WS 解帧 client_frame :1544                 │
└───────────────────────────┘         │ outq 线程    outq_thread :221                  │
                                      │   唯一写 socket / stderr 的线程                 │
                                      └───────────────────────────────────────────────┘
                                                    │                     ▲
                    内核 Type-B 帧 /dev/input/eventN │  EVIOCGRAB(1) :1666 │ uinput vtouch-merged
                                                    ▼                     │ (setup_uinput :446)
                                          物理触摸屏 ─────────────────────┘
```

线程清单（可直接对着 `top -H` 或 `/proc/<pid>/task/*/syscall` 核）：面板进程 4 条自有线程（Java 主、render、poll、outq），三个 native 线程**未命名**（`--nice-name` 之后 `/proc/<pid>/task/*/comm` 全是 `main`，只能按阻塞 syscall 区分：`ppoll`=poll、`nanosleep`=render、`futex`=outq/Java 睡眠）；脚本进程 = 主线程 + 读线程 + 每事件一个回调线程。

---

## §2 代码地图（文件 → 区块 → 行号）

### 2.1 `src/vtouchd.c`（1784 行，核心）

| 行 | 区块 | 一句话 |
|---|---|---|
| 1–15 | 文件头注释 | 单二进制 = 合并器 + WS 桥；坏客户端只关它自己；协议命令表 |
| 39–45 | `mono_ms()` | 单调毫秒，只为握手预算与日志限频（不分配、不加锁） |
| 47–59 | 常量/全局 | `MAX_PHYS 64`/`MAX_VIRT 32`/`MAX_LINE 2048`/`MAX_PAYLOAD 4096`；fd、`ws_port=27183`、`vslots=10`、`phys_slots`、`total_slots`、轴量程、逻辑尺寸、pressure |
| 61–74 | 能力镜像缓冲 | `CAP_LONGS` 位图长度宏 + EV/KEY/ABS/PROP 位图 + 每轴 `absinfo` + 名字 + `input_id` + `oid_mod` |
| 75–89 | `struct contact` | 一根触点：来源字段（id/x/y/down/pending_up/fresh_*）+ 下游身份（oslot/oid/tool/pressure/seq） |
| 92–112 | hooks | 四个回调打包注册（event/region_changed/consume/ui），NULL 字段 = 无该功能 |
| 113–137 | 帧暂存与订阅 | `frame_open`、`ws_in[]` 半包缓冲、`staged[]`、`sub_mask`、`ps_*`、`phys_eaten*`、`g_reemit`、`g_emit_fail` |
| 144–244 | 出站队列 | `outq_push` / `outq_push_block` / `vt_log` / `outq_clear` / `outq_thread` |
| 248–286 | 解析与坐标 | `parse_long`、`logical_to_raw`、`raw_to_logical`、`bit()` |
| 288–349 | 设备发现 | `validate_device`（Type-B 关卡 + 能力快照）、`discover`（扫 event0..63） |
| 351–444 | uinput 写帧 | `ev_add`、`uinput_writev_retry`、`emit_iov_writev`（含短写补全） |
| 446–508 | `setup_uinput` | 镜像声明 → 四轴兜底 → props → 名字/bus |
| 510–531 | `cleanup` | 只跑一次；顺序：发送线程 → client → listen → 解 grab → 销毁 uinput |
| 533–557 | 帧辅助 | `any_emitted`、`evict_newest_virtual`（池满顶最新虚拟并通知客户端） |
| 559–654 | `emit_frame` / `owner_reset` | 吞触摸 latch → 待抬 → 物理 → 虚拟 → BTN → SYN → 一次 writev → 释放身份 |
| 656–716 | 身份池 | `slot_taken`/`id_taken`/`alloc_oslot`/`alloc_oid`/`set_virtual` |
| 718–904 | 区域表 | 固定数组 + 一把锁；`region_add`/`region_hit`/`region_del`/`region_rename` + 导出接口 |
| 906–1008 | 面板直读与匹配 | `vtouch_phys_*`（快照接口）、`region_ev_send`、`region_match` |
| 1010–1170 | `handle_line` | 全命令分发（行协议的唯一入口） |
| 1172–1282 | 物理事件泵 | `broadcast_phys`（pev）、`physical_events`（解析 → SYN → 合帧 → 匹配） |
| 1284–1449 | WS 协议层 | 自带 SHA-1/Base64、握手、`write_full`、`ws_send` |
| 1451–1578 | 客户端生命周期与解帧 | `drop_client`、`ws_peek_frame`、`ws_next_frame`、`client_frame` |
| 1581–1627 | 监听与参数 | `make_listen`（只回环）、`apply_args` |
| 1629–1677 | `vtouch_init` | **socket first, grab last** 的全套定序 |
| 1679–1762 | `vtouch_poll_step` | 单轮 poll：物理 + 踢人 + 重发 + accept + 客户端帧 |
| 1764–1784 | 导出与入口 | `vtouch_cleanup`、`vtouch_init_ui`、`main` |

### 2.2 `src-ui/vtouch_ui.cpp`（1985 行，面板 native）

| 行 | 区块 | 一句话 |
|---|---|---|
| 27–47 | extern "C" 声明 | 复述核心 API + `struct vtouch_hooks`（与 core 同布局） |
| 58–98 | 全局与几何 | 逻辑尺寸、EGL 句柄、面板位置/宽度公式（`panel_w` 97 / `panel_h` 98）、配色与尺寸宏 |
| 100–164 | 交互/重画状态 | 触点快照环、鼠标状态、四个重画标志、滚动/命中遥测、落盘请求 |
| 166–183 | `ui_show_cb` | 「关闭 UI」只改标志，日志交渲染线程 |
| 185–225 | 旋转 | `g_rot/g_scr_*`、`p2c` 换算、`on_display` 重新落位 |
| 227–278 | 瞬态视觉与编辑态 | 闪框/圈/进出提示/事件环 + 框选、选中、隐藏表 |
| 279–369 | regions.conf | 版本门 v2、`.tmp`+rename 落盘、旧路径迁移、`gen_id` |
| 371–442 | 与核心的桥 | `now_ms`/`t_since_start`、`in_panel`、`ui_consume_cb`、`ui_ev_cb`、`ev_note` |
| 444–594 | 手势 | `sel_geom`、`cap_commit`（框选落库）、`edit_apply_live`（拖改节流直播）、`panel_press/drag_move/release` |
| 596–719 | 快照与 poll 线程 | `snapshot_touches`（每帧直读 phys 表）、`poll_thread_fn` |
| 721–780 | EGL 与字体 | `egl_init_locked`、字形区间、`font_probe`（两档字号 + 缺字兜底） |
| 782–924 | `build_overlay` | 全屏无形窗：区域描边/填充闪/进出脉冲/轨迹/蓝点/扩散圈 |
| 926–1072 | 样式与控件 | `apply_bento_style`、四个按钮 helper、图标按钮、小字 helper、导航项 |
| 1073–1175 | 容器与区域操作 | `pub_zone`、`drag_scroll_for`、`page_header`、`del_region`、`id_name_ok`、`rename_region`、`name_open` |
| 1177–1433 | 面板骨架与页面 | 标题栏、侧栏、区域卡片、区域/日志/设置三页、改名弹层 |
| 1535–1642 | 组装与提交 | `build_panel`（位置推送 + 快照式 push/pop）、`draw_frame`（喂鼠标 → NewFrame → 双内容 → swap） |
| 1644–1672 | 悬空引用清理 | `region_id_exists`、`prune_panel_refs` |
| 1674–1878 | `render_thread_fn` | 渲染循环：落盘 → 换 Surface → 快照 → 重画门 → 限频 → 首帧保证 |
| 1880–1899 | 测试线程 | `-DVT_TEST_EV` 合成事件（生产构建里不存在） |
| 1901–1985 | JNI | `JNI_OnLoad`（启动锚点）、`nativeInit`、`nativeOnSurface`、`nativeOnDisplay`、`nativeDestroy`、`nativeWantLayerVisible` |

### 2.3 `src-ui/VTouchUI.java`（229 行）

| 行 | 函数 | 一句话 |
|---|---|---|
| 13–17 | native 声明 | 5 个 `native` 方法 = Java 与 native 的全部接口 |
| 23–32 | 反射缓存 | 候选类名、`mDmg`/三个 Method、`reflectReady/Failed/readWarned` |
| 34–65 | `initReflect` | `DisplayManagerGlobal` → `getInstance` → 隐藏方法 `getDeclaredMethod`+`setAccessible`；全失败退启动参数 |
| 68–93 | `readDisplay` | 主路线 `getDisplay(0)`+`getRotation`+`getRealSize`；退路 `getDisplayInfo(0)` 读字段 |
| 96–103 | `queryDisplay` | 读不到就返回传入值（保证起得来） |
| 105–110 | `newSurface` | 反射 `Surface(SurfaceControl)` 构造 |
| 112–119 | `txnNew`/`txnCall` | 反射构造/调用 `SurfaceControl.Transaction` |
| 120–137 | `makeLayer` | 建 composer 层：名字/buffer 尺寸/TRANSLUCENT/layerStack/setLayer/z 序/alpha/**setTrustedOverlay(true)**/show/apply |
| 139–159 | `main` | 解析尺寸 → `System.load` → `nativeInit`(=0 才继续) → 建层 → `nativeOnDisplay` → `nativeOnSurface` |
| 161–227 | 主循环 | 40ms：① 图层可见性（失败不提交，下轮重试）② 每 8 tick 查方向/尺寸，全成功才提交并换新 Surface |

### 2.4 `scripts/build_bundle.py`（882 行，JS 库的唯一来源）

| 行 | 内容 | 说明 |
|---|---|---|
| 20–25 | 路径常量 | `BIN=build/vtouchd`、`OUT=clients/vtouch_bundle.js`、`UI_DIR=build/ui` |
| 27–377 | `CORE` 段 | 自释放、最小 WS 客户端、连接、事件解析、退出钩子、旋转换算、`vt.run`、`Finger`、`frame` |
| 379–414 | `DEMO` 段 | 纯 `module.exports` 表（唯一的对外 API 清单） |
| 417–519 | `UI_BOOT` 段 | pidof/kill 工具、md5 幂等释放、`uiStart/uiStop/uiRestart` |
| 522–668 | `ON_REGION` 段 | `vt.onRegion` 入口 + 事件集过滤 + 分发 + 停监听 + `ui show/hide` |
| 679–813 | `ONE_LIB` 段 | 区域本地引擎/命中、`rgPush`/`rgList`/探针、`rgParseEv`、`createEngine` |
| 670–676 | `write_out` | 写盘 + `node --check` 语法门（不过就返回 False） |
| 815–817 | `_b64_lines` | base64 按 76 列分行（单行超长会被中间设备拦 403） |
| 820–878 | `main` | 参数：默认面板构建 / `--headless`（`--no-panel` 同义）；缺产物直接报错退出；末尾按 `CORE + blob + uiblob + ONE_LIB + UI_BOOT + ON_REGION + DEMO` 顺序拼接（:868） |

`main` 的关键分支（`:826-867`）：`--headless` 时必须已有 `build/vtouchd`，内嵌它并把 `VTOUCH_BIN_SIZE` 写成真实长度；默认（面板）构建**不内嵌 vtouchd**（`VTOUCH_BIN_SIZE=0`，核心已在 `.so` 里），并要求 `build/ui/libtestimgui.so` + `classes.dex` 存在，否则打印「先跑 `sh scripts/build_ui.sh`」并返回 1。so 走 `gz+b64`（`:855/864`），dex 原样 b64（`:861`），两者都带 md5 供设备侧幂等门用（`:865-866`）。

---

## §3 核心逐函数走读（`src/vtouchd.c`）

### 3.1 启动定序：`vtouch_init`（:1629–1677）

| 行 | 步骤 | 失败出口 |
|---|---|---|
| 1634–1636 | 装 SIGTERM/SIGINT 处理器、忽略 SIGPIPE、`stderr` 设 `_IONBF` | — |
| 1637 | `apply_args` 解析 `-w -h -v -p`（坏值只出声、保留默认；`-ui`/`-s` 兼容空位） | 不退出 |
| 1638–1641 | 尺寸门：`logical_width/height < 2` | **-2** |
| 1642–1647 | 清 `phys[]`/`virt[]`，并**显式**把 `oslot/oid` 置 -1（0 是合法槽，不能靠 memset） | — |
| 1648–1651 | `discover(dev)`：`validate_device` 扫 `/dev/input/event0..63`，第一个 Type-B 设备胜出 | **-2** |
| 1654–1655 | `total_slots = phys_slots`（**声明给系统的槽数 = 面板槽数**，不再 phys+偏移） | — |
| 1656–1659 | `setup_uinput()` 镜像声明 + `UI_DEV_CREATE` | **-3** |
| 1660–1661 | 打开物理设备 `O_RDONLY|O_NONBLOCK|O_CLOEXEC` | **-4** |
| 1663–1664 | `make_listen()`（只绑 127.0.0.1） | **-6** |
| 1666–1667 | **`EVIOCGRAB(1)`** —— grab 放最后 | **-5**（唯一在 grab 之后的失败） |
| 1668 | `TCP_NODELAY`（服务端侧） | — |
| 1669–1673 | 起 `outq_thread`；创建失败只告警（触摸仍工作，事件发不出去） | 降级 |
| 1674–1675 | 打一次 `dev=… pool=… ws=… size=…` 汇总行 | — |

配套读法（`validate_device` :288–339）：Type-B 四轴齐全（:300）→ `ABS_MT_SLOT` 量程合规（:302）→ X/Y 量程有效（:304–307）→ pressure 可选（:311–315）→ **能力快照**（EV/KEY/ABS/PROP + 每轴 absinfo + 名字 + input_id，:317–325）→ id 池取小（:326–331）→ 打镜像日志（:332–337）。所以「镜像」不是一个函数，而是 **`validate_device` 存 → `setup_uinput` 放** 的一对。

### 3.2 能力镜像与 uinput（:446–508）

- `setup_uinput` 逐位搬：EV（:454）、KEY（:457）→ ABS + absinfo（:460–477）→ props（:493）。
- 只有 4 处冲突用相似值覆盖：① `ABS_MT_TOOL_TYPE` 抬到 ≥ `MT_TOOL_PALM`（:468）② `ABS_MT_SLOT` 抬到 `total_slots-1`（:471）③ `ABS_MT_TRACKING_ID` 抬到 `oid_mod-1`（:474）④ 名字加 `_vtouch` + `BUS_VIRTUAL`（:497–502）。
- `INPUT_PROP_DIRECT` **无条件声明**（:496）：缺了系统按触控板处理，会画出鼠标指针。
- 物理屏没声明四根 MT 轴时补齐（:478–492），否则合并设备发不出 MT 事件。
- 任何一步失败走 `fail:`（:506）销毁并关 fd。

### 3.3 一帧的生成：`emit_frame`（:559–637）

固定顺序，**每一步都有存在的理由**：

1. **吞触摸判定**（:565–578）：每根按下的手指问一次 `vtouch_consume_cb`（逻辑坐标），结果 **latch** 到抬手；被吞的手指在抬手帧直接把 `pending_up` 清掉（:568）——下层不会看到「没有 down 的 up」。
2. ① 待抬触点（:582–589）：用**当前**下游身份发 `ABS_MT_TRACKING_ID = -1`（身份要到这一帧写成功才释放）。
3. ② 物理触点（:592–610）：**在这里才分配下游身份**（`alloc_oslot`/`alloc_oid`，:595–602）；池满 → 顶掉最新虚拟触点（`evict_newest_virtual`）并置 `g_reemit`，这一帧先不发它。
4. ③ 虚拟触点（:612–620）：身份在 WS 命令里就分好了，这里只发。
5. `BTN_TOUCH`/`BTN_TOOL_FINGER` 用 `any_emitted()`（:621–622）——只认真的进了帧的触点。
6. `SYN_REPORT`（:623）→ 一次 `emit_iov_writev`（:627）。
7. 写失败**绝不清 `pending_up`、绝不释放身份**，置 `g_reemit` 交给 poll 循环重发（:624–630）。
8. 写成功才清 `pending_up`（:631–632）、才把「已抬起且已发出」的身份交还池子（:633–635）。

### 3.4 身份池（:656–716）

- 池大小：槽 = `phys_slots`（本机 10），id = `oid_mod`（≤32）。
- `slot_taken`/`id_taken` 把 `staged[]`（帧内已分配）也算占用（:667/:674）——这就是 `owner_reset` 必须把 staged 身份还回去（:649）的原因。
- 分配时机不同：虚拟触点在 **WS 命令**里分配（`set_virtual` :698–702，池满可如实回 `err point`）；物理触点在 **发帧**时分配（因为要不要进系统取决于吞触摸判定）。
- 释放时机统一：那一帧**写成功之后**。

### 3.5 区域表与匹配（:718–1008）

- 表：固定 `regions[32]`（:719–727），**一把锁** `region_mu`（:731），三张槽状态表 `slot_in/slot_hit/slot_last_x/y`（:732–735）。
- `regions_clear`（:738）：连槽状态一起清。
- `id_charset_ok`（:754）：core 也校验 `[A-Za-z0-9_-]`——因为面板字形表是按源码字符生成的（`scripts/gen_ui_chars.py`），字符集必须封闭，否则面板只会画方框（安静且难查）。
- `region_add`（:766）：校验（长度/id 字符集/type/非负/起点在屏内，:771–776）→ **同 id 原地更新**（:779–790，面板开关/显隐只改属性）→ 容量上限（:791）→ 新建并清该 rid 的历史槽状态（:797–803）→ 锁外调 `region_changed()`（:814，避免面板回头查表自锁）。
- `vtouch_region_del`（:857）/`vtouch_region_rename`（:883）：都是**单条**操作；删是整块搬移 + 清全部槽状态，改名是原地改 + **不碰槽状态**（按住手指改名不丢 up）。
- `region_ev_send`（:919）：先喂面板回调（无 WS 客户端也通知，:927）→ 判订阅门（:928）→ move 只在 socket 真可写时入队（:940–948）→ 全部入 `outq`（触摸线程不写 socket）→ 日志限频 1s（:925/950）。
- `region_match`（:961）：**每 SYN 调一次**；锁内拷 32×40B 快照、锁外匹配（:967–971）→ 逐物理槽（:973）→ `region_hit`（:818，圆用 long 防溢出）→ 按 `fresh_down`/`fresh_up` 报 down/up/enter/exit/move（:979–998）→ 更新 `slot_in`（:999）→ **抬手帧立刻清 `slot_hit`**（:1005，否则残留标志会让下一根手指替它补报假 up）。
- 边界语义：`enabled=0` 的区域直接被跳过（:820/:977）；虚拟触点永远不产生 `region_ev`（只看 `phys[]`）。

### 3.6 控制面：`handle_line`（:1010–1170）命令表

| 命令 | 行 | 语义/判据 |
|---|---|---|
| `ping` | 1017 | 回 `pong` |
| `res` | 1018–1022 | 回 `res <lw> <lh> raw <xmin> <xmax> <ymin> <ymax>` |
| `reset` | 1023–1028 | **帧中途拒绝**（`err frame`）；否则 `owner_reset()` |
| `region clear/list/del/rename/add` | 1029–1089 | `list` 每行一条 + `end N`（:1049）；`add` 的 `a3` 上限 100000（圆半径复用该位，见 §10-F4） |
| `ui show\|hide\|toggle` | 1090–1102 | 没人注册钩子 → `err no-ui`（不静默成功） |
| `sub [region\|phys\|all]` | 1103–1119 | 裸 `sub` = 两个通道都订（老客户端语义） |
| `unsub` | 1120–1122 | 清掩码 |
| `up <slot>` | 1123–1131 | 帧内拒绝；`set_virtual(...,"up")` + 单独成帧 |
| `down/move <slot> <lx> <ly>` | 1132–1143 | 逻辑→raw（:1138）→ `set_virtual` → **各自成帧**（一次 SYN） |
| `begin_frame` | 1144–1149 | 拷 `virt→staged`、`frame_open=1` |
| `point <slot> <state> <lx> <ly>` | 1150–1162 | 只能帧内用；同一 slot 一帧只能一次（`frame_seen`） |
| `end_frame` | 1163–1168 | `staged→virt`，提交一帧 |
| 其它 | 1169 | `err unknown` |

### 3.7 数据面：`physical_events`（:1196–1282）

逐条 `read()` 解析（一次 read 可能攒 N 帧，所以边沿标志**每帧清**）：

| 行 | 事件 | 处理 |
|---|---|---|
| 1211–1215 | `ABS_MT_SLOT` | 选槽；越界置 `-1` = 忽略后续槽事件（不夹到 0，避免污染第 0 槽） |
| 1217–1224 | `TRACKING_ID < 0` | 抬手：`down=0`、`pending_up=1`、`fresh_up=1` |
| 1225–1235 | `TRACKING_ID ≥ 0` | 按下：`fresh_down=1`、`down=1`、tool=FINGER、pressure=最大值 |
| 1236–1247 | X/Y/TOOL/PRESSURE + `ABS_X/ABS_Y` 兜底 | 存原始值 |
| 1250–1258 | `SYN_DROPPED` | 内核环形缓冲溢出：所有槽当抬起，等下一帧重建（宁可一次假 up，不要粘指） |
| 1259–1271 | `SYN_REPORT` | `emit_frame()`（失败只计数，不 return）→ `region_match()` → **清 fresh_*** |
| 1273–1275 | 整轮结束 | 若见过 SYN：`broadcast_phys(订阅了 phys)` 推 `pev` 行 |
| 1276–1281 | 读错误 | `ENODEV/EIO` → 打日志 + `stop_flag=1` |

`broadcast_phys`（:1175–1194）自己维护 `ps_down/ps_x/ps_y`，**每 SYN 都走**（与是否订阅解耦），否则无订阅时状态冻结会每帧重报 down。

### 3.8 协议层与客户端生命周期（:1284–1578）

- 自带 SHA-1（:1289–1337）+ base64（:1338–1354）+ `header_value`（:1356）+ `has_token`（:1373）。
- `write_full`（:1384）：**先 `poll(POLLOUT,0)`**，当刻不可写立即失败 ⇒ 调用方踢客户端。这条是「触摸不被慢客户端拖住」的根。
- `websocket_handshake`（:1404）：1s 总预算（:1410），逐字节收（:1413），`CRLFCRLF` 用**数值比较**避免转义坑（:1416），校验 5 个头（:1419–1425），回 101 + `Sec-WebSocket-Accept`（:1431–1438）。
- `ws_send`（:1441）：文本/控制帧头 + payload（控制帧 ≤125 字节）。
- `drop_client`（:1454）：关 fd → `g_out_gen++`（旧消息作废）→ 清队列 → 清 `sub_mask` → 清半包缓冲 → `owner_reset()`（抬掉虚拟触点 + 归还 staged 身份）。
- 解帧三件套：`ws_peek_frame`（:1473，只看不消费；非法/未掩码返回 -1，过大 -2）、`ws_next_frame`（:1504，拼不齐返回 1 并留着缓冲；拼齐后去掩码 + memmove）、`client_frame`（:1544，单轮最多 32 帧；1002/1009 关闭码；ping→pong；丢给 `handle_line`；回包 `outq_push_block`）。
- `client_frame` 的注释即设计依据：**半包不能消费缓冲**，且 `poll_step` 必须 `(POLLIN || ws_in_len > 0)` 才进客户端路径（:1755）。

### 3.9 单轮循环：`vtouch_poll_step`（:1680–1762）

| 行 | 动作 |
|---|---|
| 1686–1691 | 三个 pollfd（物理 / listen / client）；`g_reemit` 时超时压到 5ms（尽快把手抬起来） |
| 1698–1703 | 物理可读 → `physical_events()` + **看门狗**：整段 ≥3ms 就打 `[stall]` |
| 1704–1708 | `g_client_wedged`（发送线程判定写不动）→ 由触摸线程统一 `drop_client` |
| 1709–1715 | `g_reemit` → 重发整帧；连续失败 ≥200 次（≈1s）→ 返回 -1 收摊（物理触摸回系统） |
| 1716–1719 | 输入设备挂断 → 停止 |
| 1720–1747 | `accept4` → 换代次/清队列 → **踢旧客户端**（`drop_client` + `frame_open=0`）→ 设 `SO_RCVTIMEO 500ms`/`SO_SNDTIMEO 20ms`/`TCP_NODELAY` → 握手 → 失败关掉、成功记 `client_fd` |
| 1748–1753 | 客户端挂断 → `drop_client`（不 return，避免跳过重发） |
| 1754–1760 | 客户端可读或缓冲还有 → `client_frame()`；失败 → `drop_client` |

### 3.10 收尾：`cleanup`（:510–531）

只跑一次（:515–520，poll 线程与渲染线程都会调）→ 停/join 发送线程（:521）→ 关 client（:522）→ 关 listen（:523）→ **`EVIOCGRAB(0)`** + 关物理 fd（:524–529）→ `UI_DEV_DESTROY` + 关 uinput（:530）。顺序即语义：先断控制面、最后放手物理触摸。

---

## §4 面板逐区块走读（`src-ui/vtouch_ui.cpp`）

### 4.1 与核心的桥（回调是怎么挂上的）

`nativeInit`（:1911）里一次注册四个回调（:1923–1926）：

| 回调 | 实参 | 行为 |
|---|---|---|
| `event` | `ui_ev_cb` :408 | poll 线程调用；写事件环（:412–413）、按事件类型置闪框/扩散圈/进出提示（:415–424）、置 `g_ov_need`（:426）——**无锁、单写者单读者** |
| `region_changed` | `ui_region_changed` :164 | 只置 `g_prune_req`/`g_need`/`g_force_frames`/`g_save_pending`，真正清理与落盘都在渲染线程 |
| `consume` | `ui_consume_cb` :396 | 谓词：关 UI / 图层不可见 → 0（穿透）；转屏 500ms 稳窗内 → 0；否则 `p2c` → `in_panel` |
| `ui` | `ui_show_cb` :169 | 只改 `g_ui_off`/`g_want_layer`/`g_off_cleared`/强推帧；日志交渲染线程（:181） |

### 4.2 坐标与旋转

- `p2c`（:193）：竖屏逻辑 → 当前屏，唯一换算点；`rot 90/270` 时矩形两角互换，所以 overlay 里画矩形要先 `p2c` 两角再取 min/max（:812–821），圆只换圆心（:815）。
- `on_display`（:204）：写 `g_scr_w/h/rot` → 开 500ms 不吞触摸的稳窗（:210）→ 没被拖过就回右上角（:213–214），拖过就只做钳制（:216–220）→ 强推 4 帧。
- `ui_consume_cb`（:396）与 `snapshot_touches`（:604）都读 `g_rot`/`g_ui_off`——所以这两个量是 `volatile`（:126/:189）。

### 4.3 regions.conf（唯一持久状态）

- 路径：`/data/local/vtouch-runtime/regions.conf`（:283–285），`mkdir` 容 EEXIST（:286–290）。
- `save_regions`（:300）：先写 `.tmp` 再 `rename`（:305–318）；**只有真成功才清 `g_save_pending`**（:322）；任何失败走 `save_failed()`（:294）把请求重挂 + 1s 后重试。
- `load_regions`（:326）：新路径没有就读老 tmpfs 路径并标记迁移（:331–336）→ **版本门**（首行必须是 `#vtouch-regions v2`，否则整份丢弃并立刻写回空表，:337–343）→ 逐行 `region`/`hide`（:344–351）→ 迁移则写回（:353）。
- 写者唯一 = 渲染线程（:1689–1691）；改动来自按钮/框选/拖改/WS 钩子，全部只置 `g_save_pending`。

### 4.4 交互面：面板自己怎么被点

- 快照（:597–705）：每帧 `vtouch_phys_get` 读每根手指（**level 轮询，不是回调**）→ 算 down/up 边沿（:612–613）→ 记轨迹环（:619–621）→ 面板外按下：起框选或选中拖改（:623–661）→ 面板内按下：`panel_press`（:664–665）→ 拖动：`panel_drag_move` → 抬手：`panel_release` / `cap_commit` / 拖改提交（:672–695）。
- 指针路由（:541–560）：标题栏实区→拖窗；否则按实区列表/内容页/侧栏锁定滚动容器；落空隙既不拖也不滚。
- `ui_live`（:604）是总闸：关 UI 后快照仍看得见手指，但**一切面板交互与框选/拖改都门掉**（否则会在看不见的面板上改表并落盘）。
- 面板鼠标喂 ImGui 一律用当前屏坐标（:1610–1613），且只在边沿时发按键事件（:1616–1619）。

### 4.5 渲染循环的重画门（:1674–1878）——性能与"看起来没反应"的分界

顺序（每轮）：

1. `g_prune_req` → `prune_panel_refs()`（:1685）清理引用了已删 id 的面板状态。
2. `g_save_pending` → `save_regions()`（:1689）——**放在这里而不是 `draw_frame` 里**，因为关 UI 时下面会 `continue`。
3. `g_ui_log_pending` → 打 `ui show/hide` 日志（:1694–1699）。
4. 换 Surface（:1701–1713）：销毁旧 EGL surface、接手 `g_win_new`、强推 4 帧。
5. `snapshot_touches()`（:1714）。
6. 重画决策：`need_draw = g_need`（显式请求不被静止门吞，:1715）→ `go = g_need||g_ov_need||force_frames`（:1716–1717）→ 叠加层可见时有手指就画（:1721–1723）→ 鼠标/闪框/扩散圈（:1724–1731）→ 清零 `g_need/g_ov_need`（:1732–1733）→ **叠加层由有到无补一帧**（:1734–1748，免残影）→ **首帧保证**（:1751）→ 静止签名门（:1756–1760）→ **叠加层限频 `VT_OV_FPS_MS`（默认 33ms）**（:1765–1772，只推迟不丢，丢标记 `g_ov_need`）。
7. 未就绪则初始化 EGL/ImGui/字体/样式（:1774–1804）；`eglCreateWindowSurface` 单独重试（:1805–1822）。
8. `g_ui_off`：清在途交互状态 + 画一帧全透明 + 之后彻底不画（:1823–1843）。
9. 画帧（:1844–1869）：`draw_frame` → 首帧日志（`first frame t=+Nms`）→ 帧耗时滑均 `g_frame_ms` → 连续 100 帧提交失败则收摊 `_exit(0)` → 还有"活动"就继续请求下一帧 → 补睡到 16ms。
10. 都不满足：解锁 + 睡 10ms（:1870–1873）。

`draw_frame`（:1597–1642）：`eglMakeCurrent` → 喂鼠标（:1604–1619）→ `NewFrame` → overlay（可选）+ 面板 → `Render` → `glClear` → **`GetDrawData` 空指针保护**（:1632–1634）→ `eglSwapBuffers` 失败计数（:1637–1641）。

### 4.6 面板布局的三处同公式

`panel_w()`（:97）与 `in_panel()`（:381–385）、`build_panel`（:1538–1542 的钳制）用同一组 `#define`（`PAD_X/SIDE_W/SHEET_W/COL_GAP/WIN_H/MINI_*`，:79–89）——换形态只改数，不改逻辑。标题栏是唯一拖动区（:1177–1238），其右边界在按钮左侧就收（:1233），所以按按钮不会顺手拖窗。

### 4.7 JNI 五个入口

`JNI_OnLoad`（:1905）记启动锚点 `g_t0_ms`；`nativeInit`（:1911）存尺寸 → 落位 → 兜底拖动区 → 挂钩子 → 组 argv → `vtouch_init(7,argv)` → `load_regions` → 起 render（失败 `-3`）→ 起 poll（失败先 `g_running=0` 再 join render，仍 `-3`）；`nativeOnSurface`（:1949）只存 `g_win_new` + 置 `g_swap_win`（EGL 归渲染线程）；`nativeOnDisplay`（:1964）加锁调 `on_display`；`nativeDestroy`（:1971）停线程 + `vtouch_cleanup`；`nativeWantLayerVisible`（:1980）给 Java 轮询。

---

## §5 Java 层逐函数（`src-ui/VTouchUI.java`）

- **`initReflect`（:34–65）**：候选 `android.hardware.display.DisplayManagerGlobal` / `android.view.DisplayManagerGlobal`（:23–26）→ `getInstance()`（:39）→ 隐藏方法必须 `getDeclaredMethod`+`setAccessible`（:45/:50）→ `getRotation`/`getRealSize` 是公开方法用 `getMethod`（:54–57）→ 成功后置 `reflectReady` 并**只报一次**（:58–59）；全失败置 `reflectFailed`（:63），永不再试（避免每 300ms 一条堆栈刷屏）。
- **`readDisplay`（:68–93）**：主路线 `getDisplay(0)`+`getRotation`+`getRealSize`（:70–77）；退路 `getDisplayInfo(0)` 读 `rotation/logicalWidth/logicalHeight` 字段（:79–88）；异常只报一次（:89–91）。
- **`queryDisplay`（:96–103）**：查不到就回传入值 ⇒ **永远起得来**。
- **`makeLayer`（:120–137）**：`new SurfaceControl.Builder()` → `setName`/`setBufferSize`/`setFormat(1 TRANSLUCENT)`（:123–125）→ 一个 Transaction 里 `setLayerStack(sc,0)`（:128）、`setLayer(sc, 2099990000)`（:129）、`setPosition(0,0)`、`setAlpha(1)`、**`setTrustedOverlay(sc,true)`（:133，tapjacking 拦截的钥匙）**、`show(sc)`（:134）→ `apply()`（:135）。
- **`main`（:139–159）**：尺寸参数（默认 1440×3168，:140–145）→ `Log.i("start …")`（:146）→ `System.load("/data/local/tmp/vtouch-ui/libtestimgui.so")` 失败**直接 return**（:147–149）→ `nativeInit != 0 → exit(3)`（:150）→ 建层 + `nativeOnDisplay` + `nativeOnSurface`，任何异常 `exit(2)`（:151–160，此时 grab 已在 native 手里，进程退出即释放）。
- **主循环（:161–227）**：`Thread.sleep(40)`（:172）→ ① 图层可见性：`nativeWantLayerVisible()` 与本地 `vis` 不同才提交 `setVisibility`（失败退 `setAlpha`）+ `setAlpha`（:174–201），**失败不提交 `vis`**（下轮重试）+ 日志限频 3s（:188–195）；② 每 8 tick（≈320ms）查方向/尺寸，**全部成功才提交 `disp`**（:204–225）。

---

## §6 JS 库逐函数（`scripts/build_bundle.py` 的六段）

### 6.1 `CORE`（:27–377）

| 行 | 函数 | 作用与关键分支 |
|---|---|---|
| 33–37 | 顶部常量 | `VTOUCH_BIN=/data/local/tmp/vtouchd`、host、port 27183 |
| 41–45 | `CURR`/`vtouchCur` | 当前连接；没有就抛错（提示用 `onRegion` 或 `uiStart+connect`） |
| 49–64 | `vtouchEnsure` | **headless 兜底**：面板活着直接 return（:51）；否则一条 root shell：pid 存活→`exit 0`（:53）、缺二进制→`exit 11`（:54）、否则 `pidof` 清残留 + `wm size` 归一化竖屏 + nohup 起（:55–58）；`code 11` → `vtouchInstall()`（:59–62） |
| 67–83 | `vtouchInstall` | 面板构建包（`VTOUCH_BIN_SIZE==0`）直接抛人话（:69）；否则 b64 解码 → 写 `getFilesDir()` 暂存 → root `cp+chmod`（:74）→ 再起一次 |
| 86–95 | WS key/accept | 16 随机字节 base64；`SHA-1(key+GUID)` 再 base64 |
| 96–99 | `vtouchWriteAll` | `write+flush` |
| 101 | `SEND_LOCK` | 发包互斥（读线程与业务线程共用同一 socket） |
| 102–121 | 读工具 | `vtouchReadFull`（读满或在 EOF 抛「连接断开」）、`vtouchReadLine`（丢 CR 收 LF） |
| 123–124 | `jb` | Java byte 有符号：128–255 折成负数 |
| 128–139 | `vtouchConnect` | 轮询重试直到超时（默认 10s，每次失败 `sleep(300)`）——覆盖「daemon 刚起、端口滞后」 |
| 140–196 | `vtouchConnectOnce` | `Socket.connect(...,3000)`（:142）→ `SoTimeout`（:143）→ 手写握手请求（:146–149）→ 校验 101（:151）→ **校验 `Sec-WebSocket-Accept`**（:152–158）→ `conn.send`（:160–176，掩码帧，锁内一次 write）→ `conn.recv`（:178–189，`available()<2` 返回 null = 非阻塞）→ `drain`（:191）→ `close`（:192）→ 置 `CURR` + 注册退出钩子（:193–195） |
| 198–210 | 发送/订阅 | `vtouchSend`（未订阅时先 drain）、`vtouchReset`、`vtouchSub(mode)`（**订阅后 `c.watch=true`，此后发包不再 drain**）、`vtouchUnsub` |
| 212–220 | `vtouchParseEv` | `pev <slot> <down\|move\|up> <lx> <ly>` → `{slot,action,x,y}`；非 pev 返回 null |
| 221–248 | 自动收尾 | `AUTOSTOP=true` / `HANDOVER=false` / `EXIT_HOOK`；`vtouchAutoStop(on)`；`vtouchExitHook()` 注册**唯一一个** `events.on("exit")`：`HANDOVER||!AUTOSTOP` → 只 `vtouchListenStop()`+关连接；否则 `vtouchStop()` |
| 249–264 | `vtouchStop` | 先 `vtouchListenStop`（否则读线程会把正常收尾报成「通道断开」）→ 面板活着就 `vtouchUiStop()`，**「起来不到 2s 就被收」会 toast 喊话**（:257–258）→ `vtouchKillAll` + 删 pid 文件 |
| 266–293 | 旋转 | `vtouchRot`（`Display.getRotation`）、`vtouchPortrait`（min/max）、`vtC2P`/`vtP2C`（与面板 `p2c` 同一套公式） |
| 295–318 | `vtouchRun` | `wakeUpIfNeeded` → `threads.start`：`keepScreenOn` → ensure → connect → `fn()` → close → finally `vtouchStop()` + `exit()` |
| 320–366 | `Finger` | `down/move/up`（自动 c2p + 取整）、`tap`（默认 60ms）、`swipe`（按 16.7ms 步进）、`frame`（转 `vtouchFrame`）、`state`；`vtouchFinger(slot)`：省略 slot 时扫 0..9 取第一个非 down 的（:357–359），显式 slot 校验 0..9（:361–362） |
| 367–376 | `vtouchFrame` | `begin_frame` → 逐点 `point` → `end_frame`，同步 `c.fingers[slot].downState` |

### 6.2 `DEMO`（:379–414）

`module.exports` 全表：`run/ensure/install/connect/send/finger/Finger/frame/reset/stop/uiShow/uiHide/rot/c2p/p2c/rgPush/rgList/createEngine/rgParseEv/onRegion/autoStop/sub/unsub/parseEv/uiStart/uiStop/uiRestart/uiAlive/uiPid/uiDeploy/uiTail`。**这就是脚本能用的全部 API**——库里没有 `on(...)` 派发器，消费者要自己写读循环（或用 `onRegion`）。

### 6.3 `UI_BOOT`（:417–519）

`vtouchPids`（:437，`pidof` 单条短命令）→ `vtouchKillAll`（:442，最多 8 轮 kill+150ms 复读）→ `vtouchUiAlive`/`vtouchUiPid`/`vtouchUiTail`（:451–456）→ `vtouchUiDeployed`（:458，`md5sum` 两个文件与内嵌 md5 比对）→ `vtouchUiStage`（:463，写 app 私有目录暂存）→ `vtouchUiGunzip`（:469，b64→GZIPInputStream→byte[]）→ `vtouchUiDeploy`（:478）：headless 包抛人话（:479）→ 已部署则 `false`（:481）→ 否则释放 + **回读 md5 自证**（:490）→ `vtouchUiStart`（:496）：幂等复用（:497）→ 清残留（:498–499）→ 部署（:500）→ 组 launcher（:501–504，`CLASSPATH=dex app_process --nice-name=vtouch-ui VTouchUI W H`，**不套 `sh -c`**）→ `shell(launch)`（:505）→ 轮询 24×150ms（:507）→ 起不来 tail 日志抛错（:508）→ 记 `UI_T0` + 挂钩子（:509–510）→ `vtouchUiStop`（:513）/`vtouchUiRestart`（:518）。

### 6.4 `ON_REGION`（:522–668）—— 用户唯一要写的入口

- `VT_DEF_EV = "down,up,enter,exit"`（:539，**默认不含 move**）；`vtouchEvSet`（:540）把 `"down,move"`/数组/`"*"` 统一成集合或 `"*"`。
- `vtouchRegMatch`（:550）：事件集 + id（`null/""/"*"` = 不限）。
- `vtouchDispatch`（:554）：快照 regs（:557）→ 匹配 → **每个回调各起一条线程**（:562–566，回调里的 `sleep` 不该堵读线程）→ 回调抛错只 toast（:564）。
- `vtouchListenStop`（:570）：置 `closing` → 清保活定时器 → `unsub` → 关连接。
- `vtouchUiCmd`（:586）：当前连接发 `ui show|hide`，没连接就抛错。
- `vtouchOnRegion(a,b,c)`（:594）：三态签名解析（:596–598）→ 事件集合（:599）→ 回调必需（:600）→ **首次才做整段初始化**（:601–654：`uiStart` → `connect` → `sub region` → `ui show` → 建 `live` → `probe` → 读线程 → 保活定时器 → 退出钩子）→ `regs.push` + `vtouchCheckId`（:655–657）→ 返回 `{stop}`（:658–666，最后一个摘掉才停监听）。

---

## §7 线协议与事件（`docs/VTOUCH_PROTOCOL.md` 已不在工作区，以下按代码写）

命令 → 回包（`vtouchd.c:1011-1169`）：`ping→pong`、`res→res …`、`reset→ok|err frame`、`region clear|list|del|rename|add`、`ui show|hide|toggle→ok 0|1|err no-ui`、`sub [region|phys|all]→ok|err sub`、`unsub→ok`、`down|move <slot> <lx> <ly>→ok|err point`、`up <slot>→ok|err point`、`begin_frame|point|end_frame→ok|err frame|err point`，其它 → `err unknown`。

事件行（订阅后与回包同一条流）：

| 行 | 产生处 | 含义 |
|---|---|---|
| `pev <slot> <down\|move\|up> <lx> <ly>` | `broadcast_phys` :1175 | 物理手指状态（逻辑坐标，slot = `phys[]` 下标） |
| `region_ev <id> <down\|up\|enter\|exit\|move> <slot> <lx> <ly>` | `region_ev_send` :919 | 区域命中事件（**只报物理手指**；id 永远是 `rg->id` 的最新值） |
| `vdrop <客户端槽>` | `evict_newest_virtual` :552 | 池满时被顶掉的虚拟触点（客户端据此清 downState） |
| `region <id> <type> <a1..a4> <en>` / `end N` | `handle_line` :1041/:1049 | `region list` 的应答体 |

---

## §8 示例串联：一条 `vt.onRegion("s3","down",fn)` 的完整旅程

脚本（`clients/vtouch_region_min.js` 正文，仅 2 行）：

```js
var vt = require("/sdcard/vtouch_bundle.js");
vt.onRegion("s3", "down", function (h) { vt.finger().tap(h.x, h.y); });
```

下表把这条两行代码**穿过全部四层**，每一步都给「谁在做 / 文件:行 / 判据」：

| # | 谁 | 位置 | 做什么 / 判据 |
|---|---|---|---|
| 1 | AutoJs6 主线程 | `build_bundle.py:868` 产物 | `require` 只定义函数、返回 API 表（`DEMO` :380）；内嵌 b64 是**数组 + `join("")`**（:`815` 的 `_b64_lines`），Rhino 编译代价只与字面量个数有关、不建深表达式树 |
| 2 | 主线程 | `build_bundle.py:594` | `vtouchOnRegion` 解析签名：`id="s3"`、`ev="down"`(→集合)、`fn` 必需（:596–600） |
| 3 | 主线程 | `build_bundle.py:497` | `vtouchUiStart`：`pidof vtouch-ui`（:437/451）命中即复用；否则清残留（:498）→ 释放二进制（:478，md5 门 :458/481） |
| 4 | 面板进程 Java | `VTouchUI.java:139` | `main`：`System.load`（:148）→ `nativeInit`（:150，非 0 就 `exit(3)`） |
| 5 | 面板 native | `vtouch_ui.cpp:1911` | `nativeInit`：存尺寸/落位/兜底拖动区（:1913–1920）→ **挂 4 个回调**（:1924）→ `vtouch_init(7,argv)`（:1932）→ `load_regions`（:1933）→ `panel init` 日志（:1934）→ 起 render（:1937）→ 起 poll（:1941） |
| 6 | 核心 | `vtouchd.c:1629` | `vtouch_init`：信号/无缓冲（:1634–1636）→ 尺寸门（:1638）→ 清表 + 身份置 -1（:1642–1647）→ `discover`（:1648）→ `setup_uinput`（:1656）→ 开物理设备（:1660）→ `make_listen`（:1663）→ **`EVIOCGRAB`（:1666）** → `TCP_NODELAY`（:1668）→ 起 outq（:1669）→ `dev=… pool=… ws=… size=…`（:1674） |
| 7 | Java | `VTouchUI.java:154-158` | 建层：`makeLayer`（:120，含 `setTrustedOverlay(true)` :133）→ `nativeOnDisplay`（:214 分支外的首调用 :157）→ `newSurface`+`nativeOnSurface`（:158）→ `layer up`（:159）→ 进 40ms 主循环（:170） |
| 8 | native | `vtouch_ui.cpp:1949/1964` | `nativeOnSurface` 只存 `g_win_new`+置 `g_swap_win`；`nativeOnDisplay` 加锁调 `on_display`（:1967） |
| 9 | 脚本主线程 | `build_bundle.py:603` | `vtouchConnect()`（:128 → :140）：TCP + 手写握手 + 校验 accept；成功后置 `CURR` 并挂退出钩子（:193–195） |
| 10 | 脚本主线程 | `build_bundle.py:604-605` | `sub region`（:204，**只订区域通道**）→ `ui show`（:605，叫回上一轮被关掉的面板） |
| 11 | 脚本主线程 | `build_bundle.py:610` | `vtouchRegionProbe`（:737）在**读线程之前**发 `region list`，拿到面板当前 id/enabled 表 |
| 12 | 脚本主线程 | `build_bundle.py:611-645` | 起**读线程**：`conn.recv()` 轮询（:617）→ `rgParseEv`（:620/776）→ `vtouchDispatch`（:554）；每 2s `ping`、6s 无 `pong` 判开（:623–627）；真断开时用 `vtouchUiAlive()` 区分「被接管」（`HANDOVER`+自退，:636–640）与「面板死了」（:642） |
| 13 | 脚本主线程 | `build_bundle.py:647` | 主线程 `setInterval(…,1000)` 保活（子线程 `while` 保不住脚本） |
| 14 | 主线程/回调 | `build_bundle.py:657` | `vtouchCheckId`（:757）：id 写错/被禁用/面板没画 → 立刻 toast；探针没答复则静默 |
| 15 | 内核 → poll 线程 | `vtouchd.c:1199` | 手指按下：Type-B 帧到 `/dev/input/eventN` → `read()` 循环解析：SLOT（:1211）→ `TRACKING_ID≥0` → `fresh_down=1/down=1`（:1230–1231）→ X/Y（:1236–1239） |
| 16 | poll 线程 | `vtouchd.c:1259` | `SYN_REPORT` → `emit_frame()`（:1264）→ `region_match()`（:1266）→ 清 `fresh_*`（:1270） |
| 17 | poll 线程 | `vtouchd.c:559` | `emit_frame`：吞触摸 latch（:565–578；面板矩形内的手指在这里被判成"不进系统"）→ 待抬（:582）→ 物理补身份（:592–610）→ 虚拟（:612）→ BTN（:621）→ `SYN_REPORT`（:623）→ **一次 `writev`**（:627） |
| 18 | 内核/框架 | `vtouchd.c:446` 建的设备 | `vtouch-merged` 收到物理触点 ⇒ `EVIOCGRAB` 下系统只看这条转发流（能力镜像保证分类与原屏一致） |
| 19 | poll 线程 | `vtouchd.c:961` | `region_match`：锁内拷快照（:967–971）→ 逐槽 `region_hit`（:978）→ `fresh_down` + 命中 ⇒ `region_ev_send("s3","down",0,x,y)`（:981） |
| 20 | poll 线程 | `vtouchd.c:919` | `region_ev_send`：喂面板回调（:927，闪框）→ 订阅门（:928；没订就只打 `(NO-CLIENT/UNSUB)` 日志）→ 非阻塞入队（:940–945） |
| 21 | outq 线程 | `vtouchd.c:238-241` | 出队 → `ws_send`（:1441）→ `write_full`（:1384，`poll(POLLOUT,0)` 不可写即失败 → 标记 `g_client_wedged` 由 poll 线程踢人） |
| 22 | 脚本读线程 | `build_bundle.py:617` | `conn.recv()` 收到 `region_ev s3 down 0 720 1584` → `rgParseEv`（:776）→ `{id:"s3",ev:"down",slot:0,x:720,y:1584}` |
| 23 | 脚本回调线程 | `build_bundle.py:562` | `vtouchDispatch` 按 id/事件集过滤后 `threads.start` 调 `fn(h)`（回调抛错只 toast） |
| 24 | 回调线程 | `build_bundle.py:353/340` | `vt.finger()`：扫 0..9 找空闲 slot（:357）；`tap`：`down` → `sleep(60)` → `up`；每条命令经 `vtC2P`（:280）换算后由 `conn.send` 打成掩码帧（:160） |
| 25 | poll 线程（同一进程，另一条路） | `vtouchd.c:1132` | `handle_line` 收到 `down 0 x y`：逻辑→raw（:1138）→ `set_virtual`（:693，:698 分身份）→ `emit_frame()`（:1139）→ 回 `ok` |
| 26 | 系统 | — | 虚拟触点与物理手指在**同一 uinput 设备**、同一帧序（先物理后虚拟，`vtouchd.c:592/612`）⇒ 应用看到的是一次真实点击 |
| 27 | poll 线程 | `vtouchd.c:1219` | `tap` 的 `up` 后 60ms：手指抬起 → `fresh_up=1` → `region_match` 因 `slot_hit` 报 `up`（:993–998）→ 身份归还池（:634） |
| 28 | 脚本主线程 | `build_bundle.py:237` | 脚本运行结束 → `events.on("exit")` 钩子 → `vtouchStop()`（:249）→ `uiStop`（:514，`kill -9 $(pidof vtouch-ui)` 语义） |
| 29 | 核心 | `vtouchd.c:510` | 进程退出路径 `cleanup()`：停发送线程（:521）→ 关 client/listen（:522–523）→ **`EVIOCGRAB(0)` + 关物理 fd（:524–529）** → `UI_DEV_DESTROY`（:530）⇒ 物理触摸回到系统直读 |

对照图（工程图归档在 `docs/diagrams/`）：
- **`vtouch-current-journey`（本次新出，25 节点）**：就是上表的图形版，每个节点标「谁在做 + 文件:行」，两侧红色虚框是并发原语（锁 / 出站 FIFO / 身份池）。四件套 `.json/.svg/.png/.report.json`，重渲命令见 §11.4。
- `vtouch-full-flow` / `vtouch-click-journey`：**HEAD 里还在、工作区已被删**，且内容描述的是另一代实现（见 §10-F2）——要恢复得 `git checkout -- docs/diagrams` 后再按现码改 JSON 重渲，别直接当现况用。

---

## §9 失败出口总表

| 出口 | 触发条件 | 位置 | 现象 |
|---|---|---|---|
| `-2` | 尺寸缺失 / 扫不到 Type-B 设备 | `vtouchd.c:1640/1650` | `nativeInit` 返回 -2 → Java `exit(3)` |
| `-3` | `setup_uinput` 失败 / render、poll 线程创建失败 | `:1658 / vtouch_ui.cpp:1939/1944` | 同上 |
| `-4` | 打不开物理设备 | `:1661` | 同上 |
| `-5` | **`EVIOCGRAB` 失败**（唯一在 grab 之后的失败） | `:1666` | 同上（此时 grab 未生效，触摸未被抓） |
| `-6` | 27183 被占 | `:1664` | 同上 |
| `exit(2)` | 建层 / `newSurface` / `nativeOnDisplay` 抛错 | `VTouchUI.java:160` | 层没起来；grab 已生效但随进程退出释放 |
| `return`（不退出） | `System.load` 失败 | `VTouchUI.java:149` | 进程安静结束（logcat `load so`） |
| 脚本 throw | 旧面板清不掉 / 面板 3.6s 没起 / 10s 连不上 | `build_bundle.py:499/508/135` | 脚本报错；设备未被抓 |
| `_exit(0)` | poll 循环退出 / 连续 100 帧提交失败 | `vtouch_ui.cpp:715/1857` | 面板收摊、触摸回系统（**让脚本看得见**） |
| 降级 | `outq_thread` 创建失败 / EGL 起不来 / `eglCreateWindowSurface` 失败 | `vtouchd.c:1670 / vtouch_ui.cpp:1786/1808` | 触摸、注入、事件照跑；只是事件发不出去 / 面板画不出来（日志升级但不杀后端） |
| 重发兜底 | uinput 写失败 | `vtouchd.c:627 / 1709-1714` | `g_reemit` 5ms 重发；连续 200 次（≈1s）放弃 → 物理触摸回系统 |
| 踢客户端 | 队列满 / 写不动 / 协议错 / 新连接 | `:945 / 240 / 1558 / 1728` | 只关这条 WS，grab 与 uinput 不动 |

---

## §10 读码发现的点（与实际行为相关，按影响排序）

- **F1 交付物过期（实测）**：`python scripts/verify_bundle.py --bin build/vtouchd --ui build/ui` 本次实跑 = **11 ok / 1 FAIL**，退出码 1——`clients/vtouch_bundle.js` 里内嵌的 `libtestimgui.so` md5 = `5b0b8b09…`，而现编 `build/ui/libtestimgui.so` = `7686bb60…`；dex 一致（`48248d46…`）。也就是说**盘上那个 bundle 里的面板是旧 .so**，按它推设备会跑到旧面板（症状通常是「改了没效果 / 新命令 err unknown」）。修法：重跑 `sh scripts/build_ui.sh` 后 `python scripts/build_bundle.py`，再跑一次对账（脚本自己会钉住这一条）。
- **F2 工程图与文档是另一代实现**：`build/_gen_diagrams.py` 生成的图（以及备份里的 `docs/VTOUCH_BUNDLE.md`）标着 `recvBlocking`、`FIFO 队列 256（LinkedBlockingQueue）`、`pump 线程`、`reserved 占位`、`vtouch_set_consume_cb`、`vtouch_init :1495` 等——**当前代码里都不存在**：`recvBlocking` 只出现在 `build/_wt_backup_20260913_150905/scripts__build_bundle.py:226`；当前库是 `available()` 非阻塞读 + 2s ping/6s pong 看门狗（`build_bundle.py:617-627`），回调是每事件 `threads.start`（:562），吞触摸走合并后的 `hooks.consume`（`vtouchd.c:97/574`）。**重渲这些图之前必须按现码改 JSON**，否则图会系统地误导。
- **F3 读线程的 2s 周期 ping 仍在**：`build_bundle.py:623-627` 每 2 秒从读线程发 `ping`。**代码事实**：读线程每 2s 发一个包。**机制推断（C 级，本轮未真机复测）**：AutoJs6 的 JS 引擎是共享锁、脚本主线程即应用 UI 线程，周期性抢锁会造成周期性卡顿——这条来自既有记录，要定性得按「同一负载开/关这个 ping」做 A/B，别拿它当已验证结论。要消除它只需改成阻塞读（对端 `close()` 时 `read` 返回 EOF 当场发现，不必等 6s 门限）。
- **F4 `region add` 的坐标校验不对称**：`handle_line` 里 `a3` 用 `parse_long(sa3, 0, 100000, …)`（:1080）是为了兼容圆的半径，但同一个参数位也是矩形的 `x2`；core 的 `region_add` 只拒 `a1 >= logical_width`（:776），`a3/a4` 只查非负（:775）。**按代码推导**（未真机实测）：`region add rX 0 100 100 100000 100 1` 会被接受，从而建出 `x2` 远超声宽的区域；面板框选自己 clamp（`cap_commit` :466-468），所以只有「脚本经 WS 下发」这条路会漏。
- **F5 帧没收尾会让注入整体卡住**：`vtouchFrame`（:367–376）没有 try/finally。若 `point` 之后抛错（例如 socket 断），`begin_frame` 已置 `frame_open=1`，此后 `down/move/up/point` 全回 `err point`/`err frame`，连 `reset` 也被拒（:1026）——**只有重连**（`drop_client → owner_reset` :652 清 `frame_open`）才能恢复。多指/`frame` 路线要格外小心。
- **F6 `Finger.frame()` 不归还 slot 占位**：`vtouchFinger` 靠 `c.fingers[i].downState` 判空闲（:357），而 `downState` 只在正常路径被改（:373）。抛错路径下会留 `downState=true`，自动分配永远跳过那个 slot（最多丢 10 个槽后报「无空闲 slot」）。手动 `slot` 不受影响。
- **F7 `vtouchEnsure` 的 headless 路径没有 md5 门**：只在文件缺失时释放（:54 `[ -x $D ] || exit 11`）。改 `src/vtouchd.c` 后重建 bundle，设备上仍是旧二进制且**完全静默**。面板路径有 md5 门（:458/481）兜着，不受影响。
- **F8 `apply_args` 遇到未知参数只打 usage、不退出**（:1622–1625）：`vtouchd -x` 会继续按默认值跑。
- **F9 客户端未设 `TCP_NODELAY`**：服务端设了（:1668/1737），脚本侧 `vtouchConnectOnce` 只设了 `SO_TIMEOUT`（:143）。突发小包（`frame` 多点、`swipe` 步进）在 Nagle 下可能被推迟几十毫秒；改法一行 `sock.setTcpNoDelay(true)`，改完要重建并重推 bundle。
- **F10 面板「退出」按钮与 `cleanup` 的语义**：`build_sidebar` 里 `btn_red("退出")` → `vtouch_cleanup(); _exit(0)`（:1277）——顺序即 `cleanup`（:510）的定序；`_exit` 不删 pid 文件（pid 文件只是书签，真相看 `pidof`）。
- **F11 `region_ev_send` 的 move 是「可丢」设计**：只有 socket 当刻可写才入队（:940–948），丢的是 move（位置类），down/up/enter/exit 必发（失败则标记踢人）。这是刻意的取舍，不是 bug——但意味着**move 不保证一条不漏**。
- **F12 转屏 500ms 稳窗**：`ui_consume_cb` 在 `now_ms() < g_rot_settle_t` 时一律返回 0（:400）⇒ 换向后半秒内面板不吞触摸（宁放不吞），这段时间点面板会点到下层。

---

## §11 附录

### 11.1 一命令重新取行号（源码一动就重取）

```sh
cd /c/Users/21102/vtouch-project
# 函数定义全表（核心 / 面板）
grep -nE '^(static|int|void|long|float|double|char|unsigned|struct|const|JNIEXPORT|jint|jboolean|extern|BOOL)[ ]' src/vtouchd.c
grep -nE '^(static|int|void|long|float|double|char|unsigned|struct|const|JNIEXPORT|jint|jboolean|extern|BOOL)[ ]' src-ui/vtouch_ui.cpp
# JS 六段的边界（build_bundle.py 里都是内嵌字符串）
grep -nE '^(CORE|DEMO|UI_BOOT|ON_REGION|ONE_LIB) = ' scripts/build_bundle.py
# 单点确认：函数名 + 定义行
grep -nE '^(static |int |void )?(region_match|emit_frame|handle_line)\(' src/vtouchd.c
```

### 11.2 构建与产物链（谁产出谁消费）

```
src/vtouchd.c ──(NDK aarch64-linux-android24-clang)──▶ build/vtouchd        ┐
src-ui/vtouch_ui.cpp + VTouchUI.java + thirdparty/imgui                        │
        └──(scripts/build_ui.sh: javac -encoding UTF-8 → d8 → clang -shared)──▶│ build/ui/{classes.dex,libtestimgui.so}
                                                                               ▼
scripts/gen_ui_chars.py ─▶ build/ui/ui_chars.h（字形表：按源码实际字符生成，编译前自动重生成）
                                                                               │
                                    scripts/build_bundle.py（默认要面板产物；--headless 只内嵌 vtouchd）
                                                                               ▼
                                  clients/vtouch_bundle.js ──▶ 推 /sdcard/ ──▶ AutoJs6 require
                                                                               │
        scripts/verify_bundle.py（逐字节/md5 对账，不匹配退 1）◀───────────────────┘
        scripts/package_dist.py（bundle + example.js + README + md5 → build/dist/…zip）
        .github/workflows/build.yml（arm64 全套 + x86_64 headless，两个 artifact）
```

### 11.3 术语表

| 词 | 含义（以代码为准） |
|---|---|
| `phys_slots` | 物理屏声明的槽数（本机 10）；也是我们声明给系统的槽数 |
| `vslots` | 客户端可用槽号上限（`-v`，默认 10），编进 argv 的只有 `-w/-h/-p` |
| `oslot` / `oid` | **下游身份**：我们写进 uinput 的槽位与 tracking id，物理与虚拟共用一个池 |
| `slot`（`Finger.slot` / `h.slot`） | 两套不同编号：`Finger.slot` 是客户端句柄号（0..9）；`h.slot` 是 daemon `phys[]` 下标 |
| `phys_eaten` | 被面板矩形吞掉的物理手指（整段不进系统，连 up 也不发） |
| `g_reemit` | 整帧写失败后的重发标志（poll 循环压到 5ms） |
| `sub_mask` | 订阅掩码：`SUB_REGION=1` / `SUB_PHYS=2`，0 = 未订阅 |
| `frame_open` | `begin_frame`…`end_frame` 之间为真；期间拒绝一切单点命令与 `reset` |
| `g_force_frames` | 渲染侧强制连画 N 帧（切换显隐/开关/换 Surface 后兜底） |

### 11.4 工程图怎么重渲（本次实际用的命令）

现况图共 **10 张**（9 张走读全套 + 早期 1 张旅程概览），归档在 `docs/diagrams/`，索引见 `docs/diagrams/README.md`：

| 图 | 讲什么 | 对应本文 |
|---|---|---|
| `vtouch-map-overview` | 四层 / 四线程 / 三条边界 + 坐标三域注记 | §1、§2 |
| `vtouch-core-data` | core 数据面：定序 → 采集 → 合帧 → writev → 匹配 → 出站 | §3.1–3.5、§3.7 |
| `vtouch-core-control` | core 控制面：连接/踢旧/握手/解帧/命令表/回包/挂断 | §3.6、§3.8、§3.9 |
| `vtouch-failure-exits` | 失败出口与降级（启动码 / 退出码 / 兜底） | §9 |
| `vtouch-panel-java` | 面板侧四层：Java 建层 → native 桥 → 交互与渲染 → 落盘 | §4、§5 |
| `vtouch-js-lib` | L1 脚本库六段逐函数 | §6 |
| `vtouch-example-29-steps` | 示例贯通（24 个执行步骤 = §8 的 29 行明细，同层相邻行合并） | §8 |
| `vtouch-region-system` | 区域系统：表在 core / 存储归面板 / 脚本只读下发 | §3.5、§10 |
| `vtouch-build-chain` | 交付链：构建五阶段 → 装配 → 对账 → 打包 → CI → 设备侧 | §11.2、§11.5、§11.6 |
| `vtouch-current-journey` | 早期单张：一次区域触发的 21 步概览 | §8 |

```sh
cd /c/Users/21102/vtouch-project
python build/_gen_walkthrough_diagrams.py   # 生成全套 JSON（权威源；数据驱动，改内容只改这里）
bash   build/_render_all.sh                 # 逐张：渲染 + 五道校验 + 版面自检 + PNG
```

本轮结果（10 张全绿）：五道校验（xml / markers / collisions / geometry / composition）全 ok、
composition score=**100**（`bridged_crossings` 0、`total_bends` 0~6、`max_route_stretch` ≤4.4）；
版面自检三项 PASS（节点无重叠 / 文字无超宽 / PNG 无贴边）；PNG 均为画布 2×（例：`vtouch-js-lib` 2240×4744）；
视觉复核逐张读回通过——**并据此抓到一处内容错**：示例图标题曾写「29 步」而图上只有 24 个节点，已改成「24 步」。
**改图只改生成器或 JSON 再重渲，不要手改 SVG。**

### 11.5 构建 / 测试 / CI 脚本（逐行级，行号均本次现读）

**`scripts/build_ui.sh`（86 行）** —— 面板构建，本机 Git Bash 与 CI Linux 共用一份：

| 行 | 内容 |
|---|---|
| 1–15 | 头注释：产物、用法、可覆盖环境变量（`NDK_ROOT`/`ANDROID_SDK_ROOT`/`BUILD_TOOLS_VERSION`/`API_LEVEL`）、imgui 依赖来源 |
| 16–17 | `set -e`；`cd "$(dirname "$0")/.."`（**不依赖调用者的 cwd**） |
| 19–23 | 变量默认值；`IMG=thirdparty/imgui` |
| 26–29 | `uname -s` 分支：Windows 用 `.cmd/.exe/.bat` 后缀，Linux 裸可执行 |
| 31–36 | 工具链路径：`CC`/`CXX`/`STRIP`/`android.jar`/`d8` |
| 39–44 | **依赖预检**（imgui.h、android.jar、d8、javac、编译器各一条，缺谁就给可执行装法）；编译器用 `-f` 而非 `-x`（Windows 的 `clang.cmd` 不是可执行位） |
| 46–55 | `mkdir` 产物目录；默认**清掉 classes/obj**（陈旧 .class/.o 会被打进 dex/so —— 踩过的坑，注释里点了 `SCProbe.class`）；`VTOUCH_UI_KEEP=1` 只跳过 imgui 的 .o |
| 56–64 | `[0/5]` 字形表：探测可用的 `python3`/`python`（Windows 上 `python3` 可能是商店占位符，要真跑一次 `-c "print(1)"` 才算），再跑 `gen_ui_chars.py --out build/ui/ui_chars.h` |
| 65–66 | `[1/5]` `javac -encoding UTF-8 -cp <android.jar> -d build/ui/classes src-ui/VTouchUI.java` |
| 67–70 | `[2/5]` `d8 --lib <android.jar> --min-api <API> --output build/ui/dex build/ui/classes/*.class`，再把 dex 挪到 `build/ui/classes.dex`（**喂全部 .class**，别写 `VTouchUI*`：多一个顶层类就静默漏编） |
| 71–78 | `[3/5]` 编译：`vtouchd.c`（`-O2 -Wall -fPIC -D_GNU_SOURCE`）、imgui 四个 .cpp、`imgui_impl_opengl3.cpp`（`-DIMGUI_IMPL_OPENGL_ES2`）、`vtouch_ui.cpp`（`-Isrc-ui -Ibuild/ui`，为拿生成的 `ui_chars.h`） |
| 79–81 | `[4/5]` `-shared -o build/ui/libtestimgui.so *.o -lEGL -lGLESv2 -landroid -llog -lm` |
| 82–84 | `[5/5]` `llvm-strip --strip-unneeded` 后覆盖原文件 |
| 85–86 | `ls -l` + `md5sum … | tee build/ui/md5.txt`（产出对账用的 md5 清单） |

**`scripts/gen_ui_chars.py`（86 行）** —— 面板字形表生成：

- 扫 `SOURCES = ["src-ui/vtouch_ui.cpp", "src-ui/VTouchUI.java"]`（:28），收集**所有 `ord(ch) > 0x7F`** 的字符（:32–43），排序成串。
- `render()`（:46–56）转义 `\`/`"` 后写成 `static const char k_ui_chars[] = "…";`，文件头写明「字符集封闭」的前提（面板 id 限 `[A-Za-z0-9_-]`、日志行纯 ASCII、无任意用户输入）。
- `main()`（:59–82）：`--out` 默认 `build/ui/ui_chars.h`；`--check` 模式把现文件与重新生成的结果逐字符比，不一致打印「字形表过期」并退出 1；生成模式打印字数（**不写死数字**，会随文案漂）。
- 反面前提由 core 兜底：`region_add`/`region_rename` 拒非 `[A-Za-z0-9_-]` 的 id（`vtouchd.c:754/773/888`），否则脚本经 WS 推怪 id，面板只会画方框。

**`tests/onregion_harness.js`（429 行）** —— 主机侧（Node）跑 `vt.onRegion` 全路径的桩测：

- 桩清单（:`122-179`）：`java`（含 `Socket`/`MessageDigest`/`Base64`/`Array.newInstance`）、`android.util.Base64`、`threads`、`sleep`、`setInterval`/`clearInterval`、`shell`、`toastLog`/`log`/`toast`、`exit`、`device`、`context`、`events`。
- 可控时钟（:18–20）：`Date.now()` 每次 +1ms（`CLOCK_STEP`），**默认非 0** 否则探针的 600ms 窗口不收敛。
- 驱动模型（:9–12 注释 + `drain()` :185 / `drainTimes()` :181）：`threads.start` 只入队，`drain()` 驱动一轮；读线程取空后 `sleep()` 抛 `YIELD` 让出（:161）。
- 断言：`ok(cond,label,extra)`（:24）+ `checks/fails` 计数，末行 `ALL PASS checks/checks` 或 `FAILED n/checks`（:428），退出码 0/1（:429）。**本次 grep 计数 = 46 个断言调用点**（case 编号 T1…T13，:205–388）。
- 覆盖面就是当前实现的契约：默认事件集不含 move（T1）、追加注册共用连接（T2）、句柄 `stop()` 语义（T3/T4）、`id="*"` 与回调抛错（T5）、`vt.stop()` 先停监听再收面板（T6）、显式 move（T7）、被顶掉让位自退（T8）、**空闲 2s 自动 ping / 收不到 pong 判断开（T9）**、`sub` 三种模式（T10）、启动校验 id（T11）、底层写法也自动收尾（T12）、`autoStop(false)`（T13）。
  → **T9 的存在即证据**：当前这一代实现就是「ping 看门狗」版，与 §10-F3 一致（不是阻塞读那代）。

**`scripts/package_dist.py`（149 行）与 `scripts/templates/dist-README.md`（104 行）**：

- 产 `build/dist/vtouch-bundle-<version>-arm64/{vtouch_bundle.js, example.js, README.md, md5.txt}` + 同名 zip；版本默认取 bundle md5 前 8 位（:71），`--verify` 时以子进程先跑 `verify_bundle.py`（:82），`git_commit()` 取短 HEAD（:42–44）。
- `example.js` = `clients/vtouch_region_min.js` + **自动生成的横幅**（含示例自身 md5、bundle md5 与大小，:97–113）——示例不手抄第二份。
- README 由模板填 5 个占位符：`{{VERSION}}/{{BUNDLE_MD5}}/{{BUNDLE_SIZE}}/{{DATE}}/{{COMMIT_LINE}}`（:119–123），随后 `assert "{{" not in readme`（:124）挡住「模板加了字段而脚本没跟上」。
- md5.txt 逐文件回读写入（:127–131），最后 `zipfile` 打包（:133–137）。

**`.github/workflows/build.yml`（186 行）** 步骤链：Checkout → 缓存/安装 NDK → 设 PATH → 缓存/拉 imgui v1.91.8 → JDK 17 → 装 SDK（platform + build-tools）→ 编 `vtouchd`(arm64) → `sh scripts/build_ui.sh` → 生成 arm64 bundle（带面板）→ **`verify_bundle.py` 对账（`arm64 bundle matches THIS build`）** → `package_dist.py` 打包 → 编 x86_64 + `--headless` bundle → `Verify binaries` → 上传两个 artifact（`vtouch-bundle-arm64` / `vtouch-bundle-x86_64-headless`）→ release 时附 zip。

### 11.6 `scripts/verify_bundle.py`（182 行）——产物新鲜度门（本次逐行亲读）

为什么它是这份走读里最该背下来的脚本：bundle 把 `.so`/`.dex`（headless 还含 daemon）以 base64
缝进 JS 里，**肉眼看不出包里是哪一版**；发设备前的唯一硬门就是它。

| 行 | 校验项 | 失败条件 |
|---|---|---|
| 59–75 | 参数：`--bundle/--bin/--ui/--allow-headless` | 未知参数 → 打印并 `return 2` |
| 78–79 | bundle 存在性 | 不存在 → `FAIL`，`return 1` |
| 85–90 | **结构**：6 个必备变量存在（`VTOUCH_BIN_SIZE`/`VTOUCH_BIN_B64`/`VTOUCH_UI_DEX_SIZE`/`VTOUCH_UI_SO_SIZE`/`VTOUCH_UI_SO_GZ_B64`/`vtouchOnRegion`） | 缺任一 → fail |
| 96–99 | 后端归属：daemon 与面板都没有 → fail；只有面板 → ok「核心在面板 .so 里」 | — |
| 101–112 | daemon 段：b64 可解码、长度 = 声明、**ELF 魔数** | 非法 b64 / 长度不符 / 非 ELF |
| 113–122 | `--bin` 逐字节对账（md5） | 不等 → 「包是旧的」 |
| 125–131 | 面板段：读出 `VTOUCH_UI_DEX_MD5`/`VTOUCH_UI_SO_MD5` | — |
| 132–141 | **so 自洽**：`gzip.decompress(b64)` 后 md5 = 声明值 | 不等 / 解不开 |
| 142–145 | 无面板时必须显式 `--allow-headless` | 否则 fail（挡「误发没 UI 的包」） |
| 147–160 | `--ui` 对账：包内声明 md5 vs 现编 `classes.dex`/`libtestimgui.so` | 不等 → 「包是旧的」 |
| 162–171 | 语法门：`node --check`（没 node 则 skip） | 非 0 → fail |
| 173–177 | 汇总 `N ok / M fail`，有 fail 打印「别发这个包」并 `return 1` | — |

- **本次实跑结论（就是 §10-F1）**：`--bin build/vtouchd --ui build/ui` = `11 ok / 1 fail`
  → `包内 libtestimgui.so md5 5b0b8b09… ≠ 现编 7686bb60…`。**这一条是「包里带了旧面板」的机器判据**，
  不是猜的；重建顺序 = `sh scripts/build_ui.sh` → `python scripts/build_bundle.py` → 再跑本脚本。
- 分片 b64 的取法（:46–51）值得记住：`js_string_value` 把该变量下**所有**字符串字面量按序拼接
  （`_b64_lines` 用 `"…"` + `"…"` 形式分行），只取第一段会短解码、看着像「包是旧的」。
