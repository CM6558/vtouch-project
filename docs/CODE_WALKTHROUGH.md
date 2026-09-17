# vtouch 代码走读（现役）

> 行号以本文档写作时的现读为准（`grep -n` 取），源码一改就会漂。
>
> 本文只讲**盘上现役**代码。面板接入的定稿（共享区字段表 / 启动时序 / 旋转判定）见 `docs/UI_INTEGRATION.md`；
> 构建、部署、线协议表、退出码、已知边界见 `README.md`；`docs/VTOUCH_ARCH_PLAN.md` 是方案原文（历史存档），
> 描述的是当时的设计意图，与现码有出入时以现码为准。

## 0. 模块地图与工具链

核心是**多个 .c 一起链成一个可执行**（`scripts/build.sh:58`）。分工：

| 文件 | 职责 |
|---|---|
| `src/vtouchd.c` | 进程：参数 / init / poll 主循环 / 收尾 / main；`struct vt_state` 的唯一定义处 |
| `src/vt_input.c` | 认触摸屏（动态扫 event0..63）/ 建合并 uinput / 读物理流 |
| `src/vt_frame.c` | 组帧（一次 writev）/ 待抬补发 / 身份两段 / 物理转发 / 面板吞触摸 |
| `src/vt_ws.c` | 握手 / 帧解析 / 命令族分派 |
| `src/vt_region.c` | 区域表 / 五事件判定 / 区域线程 |
| `src/vt_queue.c` | 事件队列（SPSC 环）+ 出站队列（含 WS 成帧） |
| `src/vt_util.c` | 参数解析 / 坐标换算 / 时钟锚点 / 逻辑尺寸探测 |
| `src/vt_shm.{h,c}` | 共享内存契约（单 memfd 三区）+ 两侧访问器 |
| `src/vt_panel.c` | 拉起 / 看护面板子进程 + 内嵌面板三件套自解包 |
| `src-ui/ui_glue.c` | 面板侧胶水：附着共享内存 / 投编辑邮箱 / 发布面板矩形 |
| `src-ui/vtouch_ui.cpp` | ImGui 面板（渲染线程 + 触摸快照 + 落盘） |
| `src-ui/VTouchUI.java` | 图层壳：建图层、转屏原子翻转、轮询图层可见性 |

工具链：

- `scripts/build.sh:25` 的 `ui` 目标把面板三件套当**二进制对象**链进核心 `.rodata`（逐个 `objcopy`：`scripts/build.sh:43`，
  一次链接：`scripts/build.sh:51`），产物 `build/vtouchd_ui`；默认目标是零依赖的 `build/vtouchd`（`scripts/build.sh:58`）。
- `scripts/build_ui.sh` 编面板三件套（`classes.dex` + `libtestimgui.so` + `libc++_shared.so`）：
  `classes.dex` 走 `scripts/build_ui.sh:66`（javac）→ `scripts/build_ui.sh:69`（d8）；
  `libtestimgui.so` 走 `scripts/build_ui.sh:92`（`-shared` 链接）；`libc++_shared.so` 是
  `scripts/build_ui.sh:103` 从 NDK sysroot 拷来的。
- `scripts/ui-deploy.sh:53`、`scripts/ui-deploy.sh:54` 只推两个文件（核心 + 设备侧起停脚本），
  `scripts/ui-deploy.sh:70` 按 `<W> <H> <start|stop|status>` 调它（入参定义见 `scripts/ui_ondev.sh:11`）。
- `scripts/pack_client.py:36`-`41` 把核心 base64 塞进 SDK 的三个占位符；
  `scripts/ci_check.py:144` 是「SDK = 当前源码 + 这次的核心」的逐字节门，`scripts/ci_check.py:127` 是
  `--allow-core` 放行的那一支（本机 `build/vtouch_onefile.js` 的口径见 README「本机产物与 CI 门的口径」）。

## ① 启动定序

**执行者：主线程**（`main` → `vtouch_init`）。顺序的原则：能失败的先做，**抓着触摸的最后**。

1. `src/vtouchd.c:294` `main` 装信号：`src/vtouchd.c:301` SIGTERM、`src/vtouchd.c:303` 忽略 SIGPIPE。
2. `src/vtouchd.c:305` 调 `vtouch_init()`（定义在 `src/vtouchd.c:101`）；任何阶段失败都返回负数，`main` 直接拿它当退出码（`src/vtouchd.c:306`）。
3. `vtouch_init` 内部，逐条：
   - `src/vtouchd.c:107` stderr 无缓冲（日志实时落盘）。
   - `src/vtouchd.c:108` `wall_clock_anchor()`（`src/vt_util.c:158`）——先把「单调钟 ↔ 墙钟」的偏移钉住，事件才带得上能和 `Date.now()` 比的毫秒。
   - `src/vtouchd.c:109` `apply_args()`（`src/vtouchd.c:71`）解析 `-w/-h/-v/-p`。
   - `src/vtouchd.c:110` **尺寸门**：没给 `-w/-h` 就 `detect_logical_size()`（`src/vt_util.c:33`，跑 `wm size`、2s 超时、取最后一个 `WxH`，`src/vt_util.c:46`），拿不到就 `return -2`（`src/vtouchd.c:120`）。
   - `src/vtouchd.c:125` UI 构建才有：`vt_shm_create()`（`src/vt_shm.c:41`；`memfd_create` 在 `src/vt_shm.c:55`）——状态本体进共享内存，`g` 从此指向映射（`src/vt_shm.c:83`、`src/vt_shm.c:84`）。
   - `src/vtouchd.c:127` 清 `phys/virt/staged`。
   - `src/vtouchd.c:128` `discover()`（`src/vt_input.c:67`）扫 `/dev/input/event0..63` 找第一块 Type-B 多指屏 → 失败 `return -2`。
   - `src/vtouchd.c:132`-`src/vtouchd.c:139` 交叉自检：触摸屏 raw 量程比与逻辑尺寸比不一致只**告警**，不改值。
   - `src/vtouchd.c:143` 身份两段只是下标算术：`total_slots = phys_slots + vslots`；`src/vtouchd.c:144` 同步 `id_max`。
   - `src/vtouchd.c:145` `setup_uinput()`（`src/vt_input.c:85`）→ 失败 `return -3`。
   - `src/vtouchd.c:149` 打开触摸设备（`O_RDONLY|O_NONBLOCK|O_CLOEXEC`）→ 失败 `return -4`。
   - `src/vtouchd.c:152` `make_listen()`（`src/vt_ws.c:424`，只绑 `127.0.0.1`：`src/vt_ws.c:435`）→ 失败 `return -6`。
   - `src/vtouchd.c:154` `EVIOCGRAB` → 失败 `return -5`。
   - `src/vtouchd.c:156` 起区域线程（`region_thread_main`，`src/vt_region.c:336`）——**它一起来就吃队列**，所以排在所有能失败的步骤之后。
   - `src/vtouchd.c:161` 一行开机自述（dev / pool / virt_max / pressure / ws / size / 队列容量）。
   - `src/vtouchd.c:167` 最后一步才是 `vt_panel_start(shm_fd)`（`src/vt_panel.c:166`）——「以核心为准」：引擎全就绪才拉面板。

   原文（`src/vtouchd.c:151`-`src/vtouchd.c:154`）：

   ```c
       /* 先起监听、最后 grab：任何失败路径都不会留下「抓了却没人能控制」的状态 */
       g.listen_fd = make_listen();
       if (g.listen_fd < 0) { cleanup(); return -6; }
       if (ioctl(g.input_fd, EVIOCGRAB, 1) < 0) { cleanup(); return -5; }
   ```

4. 面板子进程怎么起（`src/vt_panel.c`）：`src/vt_panel.c:175` 过了 fd 检查才置 `S_shm_ok`（后面看门狗据此决定值不值得重试）；
   `src/vt_panel.c:180` 自解包三件套（`vt_embed_materialize`，`src/vt_panel.c:69`，每次启动无条件覆盖，日志 `src/vt_panel.c:94`）；
   `src/vt_panel.c:184` 缺 `classes.dex` 就 `return -1`（优雅降级成无 UI，不报错退出）；环境与 argv 在 `src/vt_panel.c:188`-`src/vt_panel.c:202`；
   `src/vt_panel.c:208` fork；子进程 `src/vt_panel.c:216` 把 shm fd `dup2` 到固定的 3 号、`src/vt_panel.c:220` 关掉其余 fd、`src/vt_panel.c:221` `execve`。

   原文（`src/vt_panel.c:217`-`src/vt_panel.c:219`）：

   ```c
           /* 必须显式清 CLOEXEC：memfd == VT_SHM_FD 时没走 dup2，CLOEXEC 会让它在 exec 时被关掉。
            * fcntl 在 async-signal-safe 列表里，fork 后可以用。 */
           if (fcntl(VT_SHM_FD, F_SETFD, 0) < 0) _exit(125);
   ```

5. 面板侧接到手：`src-ui/ui_glue.c:192` `vtouch_init()` 读 `VTOUCH_SHM_FD` → `vt_shm_attach()`（`src/vt_shm.c:182`），
   区 A 再映射成**只读**（`src/vt_shm.c:212`-`src/vt_shm.c:219`），区 B 单独 RW（`src/vt_shm.c:224`）——「面板改不了状态」由 MMU 保证。

## ② 数据面：一次物理触摸的一帧怎么走

**执行者：主线程**（poll → 读 → 组帧 → 入队）+ **区域线程**（判定 → 推送）。注入热路径上只有一次 `writev` 和一次入队。

1. `src/vtouchd.c:203` `poll()` 四路 fd（物理输入 / 监听 / 客户端 / 出站）；UI 构建下空闲也 8ms 一轮（`src/vtouchd.c:186`），
   有待重发的整帧时压到 5ms。物理 fd 可读 → `src/vtouchd.c:209` `physical_events()`。
2. **读物理流**（`src/vt_input.c:148`）：`src/vt_input.c:152` 循环 `read()`（一次 read 可能攒好几帧）；
   `src/vt_input.c:153` `ABS_MT_SLOT` 选槽、`src/vt_input.c:155` 越界槽 = 忽略；`src/vt_input.c:157` `ABS_MT_TRACKING_ID`
   （`src/vt_input.c:159` 负值 = 抬手只置 `pending_up`；`src/vt_input.c:161` 正值 = 按下，`src/vt_input.c:162` 记下按下时刻）；
   `src/vt_input.c:164`、`src/vt_input.c:165` 存 raw 坐标；`src/vt_input.c:167` `SYN_DROPPED` 保守全抬；
   `src/vt_input.c:173` `SYN_REPORT` 提交一帧并转发。
3. **组帧 + 提交**（`emit_frame`，`src/vt_frame.c:140`）——固定顺序，每步都有理由：
   - `src/vt_frame.c:145`-`src/vt_frame.c:152` 待抬的**物理**触点先发 `TRACKING_ID=-1`（身份 = 槽号下标）。
   - `src/vt_frame.c:153`-`src/vt_frame.c:156` 待抬的**虚拟**触点（身份 = `phys_slots + 下标`，`src/vt_frame.c:154`）。
   - `src/vt_frame.c:157`-`src/vt_frame.c:177` 物理触点本体：身份就是下标（`src/vt_frame.c:171`-`src/vt_frame.c:175`）；
     被面板吞掉的手在这里 `continue`（`src/vt_frame.c:169`）——判定只在**按下那一刻**问一次，命中就锁存整段手势（`src/vt_frame.c:161`-`src/vt_frame.c:167`）。
   - `src/vt_frame.c:178`-`src/vt_frame.c:186` 虚拟触点本体。
   - `src/vt_frame.c:187`、`src/vt_frame.c:188` `BTN_TOUCH` / `BTN_TOOL_FINGER` 取 `any_emitted()`（`src/vt_frame.c:113`）——判的是**来源状态**而不是 iovec，被吞的手也算不进去（`src/vt_frame.c:118`）。
   - `src/vt_frame.c:189` `SYN_REPORT` → `src/vt_frame.c:190` 整帧一次 `emit_iov_writev()`（`src/vt_frame.c:64`；短写补齐见 `src/vt_frame.c:73`-`src/vt_frame.c:99`，底层 `uinput_writev_retry` 在 `src/vt_frame.c:40` 对 EAGAIN 等 3×20ms）。
   - 写失败：置 `g_reemit`（`src/vt_frame.c:190`），主循环下一轮重发（`src/vtouchd.c:210`，连续 200 次失败才退出 `src/vtouchd.c:212`）。
   - 成功后清 `pending_up`（`src/vt_frame.c:191`、`src/vt_frame.c:192`）；手完全抬起才解除吞触摸锁存（`src/vt_frame.c:194`-`src/vt_frame.c:195`）。

   原文（`src/vt_frame.c:150`-`src/vt_frame.c:151`，身份两段的发射点）：

   ```c
           ev_add(EV_ABS, ABS_MT_SLOT, i);                    /* 物理身份 = 下标 */
           ev_add(EV_ABS, ABS_MT_TRACKING_ID, -1);
   ```

4. **判定变化并入队**（`enqueue_phys_changes`，`src/vt_frame.c:258`）：与上一帧快照比出 DOWN/UP/MOVE（`src/vt_frame.c:263`-`src/vt_frame.c:265`），
   静止不推；`src/vt_frame.c:267` `raw_to_logical()`（`src/vt_util.c:134`）换算成脚本坐标；
   `src/vt_frame.c:273` 被面板吞掉的手不进区域判定；`src/vt_frame.c:276` DOWN 用按下时刻、其余用 `now_ns()`；
   `src/vt_frame.c:277` `vtq_push()` 入 SPSC 环（`src/vt_queue.c:39`，容量 VTQ_CAP=64，`src/vt_internal.h:53`）。
   **虚拟触点不入这条队列**（`src/vt_frame.c:198`）——「回触不自激」在源头成立。
5. **区域线程**（`src/vt_region.c:336`）：`src/vt_region.c:341` `vtq_pop()`（`src/vt_queue.c:83`），空转 1ms（`src/vt_region.c:343`），
   然后 `region_apply()`（`src/vt_region.c:263`）：
   - `src/vt_region.c:288` 先发物理流 `phys_ev_send()`（`src/vt_region.c:219`，只发订了 `SUB_PHYS` 的：`src/vt_region.c:224`）。
   - `src/vt_region.c:289` 加 `region_lock`；`src/vt_region.c:297` 遍历区域表；`src/vt_region.c:303`、`src/vt_region.c:306`、`src/vt_region.c:307`、`src/vt_region.c:310`、`src/vt_region.c:313` 把事件**攒进 `pend[]`**（`src/vt_region.c:275`）而不是直接发。
   - `src/vt_region.c:318` 解锁；`src/vt_region.c:321`-`src/vt_region.c:322` 才逐条 `region_ev_send()`（`src/vt_region.c:193`）——锁里不再有 `fprintf` / 事件环 / 出站入队这些 I/O。
   - `region_ev_send` 内：`src/vt_region.c:202` 推面板事件环；`src/vt_region.c:205` 订了 `region` 通道才入出站队列。
     报文末的墙钟毫秒由 `wall_ms_from_mono()`（`src/vt_util.c:174`）从单调钟换算。
6. **刷出**：`outq_push_text()`（`src/vt_queue.c:149`）在入队时就补齐 WS 帧头（`src/vt_queue.c:154`-`src/vt_queue.c:156` 的短/长长度分支）；
   真正的 `send()` 只发生在主循环的 `outq_flush()`（`src/vt_queue.c:170`，调用点 `src/vtouchd.c:250`）——socket 慢不再卡注入。

## ③ 控制面：WS 命令族

**执行者：主线程**（accept → 取帧 → 分派 → 入队），绝不阻塞在 socket 上。

- 接入：`src/vtouchd.c:221` 监听可读 → `src/vtouchd.c:222` `accept4(..., SOCK_CLOEXEC)`；`src/vtouchd.c:227` 有新连接先踢旧的；
  `src/vtouchd.c:231` 握手 `websocket_handshake()`（`src/vt_ws.c:223`）；`src/vtouchd.c:235`-`src/vtouchd.c:236` 客户端 socket 设非阻塞；
  `src/vtouchd.c:238`、`src/vtouchd.c:239` 重置 WS 输入缓冲与出站队列（残包不许串给下一个客户端）。
- 取帧与分派：`src/vtouchd.c:246` → `client_frame()`（`src/vt_ws.c:384`）：`src/vt_ws.c:392` 取一帧（`ws_next_frame`，`src/vt_ws.c:346`）；
  `src/vt_ws.c:401` close 帧原样回；`src/vt_ws.c:402` ping → pong；`src/vt_ws.c:408` 把 CR/LF 换成空格（一行一条命令）；
  `src/vt_ws.c:409` 单行超 1024 字节直接断开；`src/vt_ws.c:411` `handle_line()`；`src/vt_ws.c:415` **有回包才入队**。
- `handle_line`（`src/vt_ws.c:673`）只做分派：`src/vt_ws.c:680`-`src/vt_ws.c:684` 依次 meta / point_once / frame / region / sub，都没认出就 `err unknown`（`src/vt_ws.c:685`）。
- 各命令族：
  - `ping` / `res` / `reset`：`cmd_meta`（`src/vt_ws.c:457`）；`res` 的组包在 `src/vt_ws.c:461`-`src/vt_ws.c:466`（末段 `phys <n>` 就是物理槽数）。
  - `down` / `move` / `up`：`cmd_point_once`（`src/vt_ws.c:486`），**每条命令自己提交一帧**（`src/vt_ws.c:493`、`src/vt_ws.c:505`）；
    逻辑坐标→raw 在 `src/vt_ws.c:504`（`logical_to_raw`，`src/vt_util.c:112`）。
  - `begin_frame` / `point` / `end_frame`：`cmd_frame`（`src/vt_ws.c:524`）；帧内先写 `g.staged`（`src/vt_ws.c:529`），
    `end_frame` 整体换进 `g.virt` 再提交一次（`src/vt_ws.c:548`-`src/vt_ws.c:549`）。
  - `region add|clear|list`：`cmd_region`（`src/vt_ws.c:567`）；`clear` 在 `src/vt_ws.c:572`；
    `list` 在 `src/vt_ws.c:576` → `src/vt_ws.c:601` 置 `resp[0]=0` 表示「本族自己发过了」、`src/vt_ws.c:604` 锁内只读表并格式化到本地 `rows[]`、`src/vt_ws.c:613`-`src/vt_ws.c:614` 解锁后逐条入队（走 `outq_push_text_keep`：**队列满就丢这一帧**，不挤旧数据）、`src/vt_ws.c:615`-`src/vt_ws.c:617` 末行 `end <n>` 单独一帧；
    `add` 在 `src/vt_ws.c:611`，id 合法性最终由核心判（`src/vt_region.c:61` 调 `id_ok`，字符集 `[A-Za-z0-9_-]`、长度 1..`REGION_ID_MAX`=`src/vt_internal.h:57`）。
  - `sub` / `unsub`：`cmd_sub`（`src/vt_ws.c:641`）；裸 `sub` = 只订区域通道（`src/vt_ws.c:645`）；
    `src/vt_ws.c:653` 是 `|=` 累加而不是赋值（分两次 `sub region` / `sub phys` 不互相覆盖）。
- 断连收尾：`src/vtouchd.c:244` HUP/ERR、`src/vtouchd.c:247` 解析失败 → `drop_client()`（`src/vt_ws.c:287`），
  内部 `src/vt_ws.c:297` 调 `owner_reset()`（`src/vt_frame.c:240`）——抬掉它的虚拟触点并立刻提交一帧，否则虚拟手指会永久粘在设备上。

原文（分帧口径，`src/vt_ws.c:601`、`src/vt_ws.c:613`-`src/vt_ws.c:614`、`src/vt_ws.c:615`-`src/vt_ws.c:617`）：

```c
        resp[0] = 0;
        ...
            outq_push_text(line, (size_t)w);
        ...
        w = snprintf(line, sizeof line, "end %d", n);
        if (w > 0 && (size_t)w < sizeof line) outq_push_text(line, (size_t)w);   /* 末行单独一帧 */
```

## ④ 面板侧：共享内存 + 图层

**执行者：核心**（建共享内存 / 吃编辑 / 判面板死活）+ **面板进程**（附着只读状态 / 渲染线程 / 图层翻转）。

- 契约：单 memfd 三区（头 / 状态只读 / 双向 / 事件环），布局写在 `src/vt_shm.h:11`-`src/vt_shm.h:14`；
  传给子进程的 fd 号固定 3（`src/vt_shm.h:26`）；事件环 64 槽 × 96 字节（`src/vt_shm.h:34`、`src/vt_shm.h:35`）。
- 核心侧每轮：`src/vtouchd.c:193` `vt_shm_tick()`（心跳，`src/vt_shm.c:91`）；`src/vtouchd.c:194` `vt_shm_edit_apply()`
  （`src/vt_shm.c:127`，`src/vt_shm.c:137`-`src/vt_shm.c:140` 把 CLEAR/ADD/DEL/RENAME 落到区域表）；`src/vtouchd.c:195` `vt_shm_stop_req()` → 退出。
- 吞触摸这条链：面板**每帧**发布矩形（`src-ui/ui_glue.c:160`，逆变换后 `src-ui/ui_glue.c:170` 发布；`src-ui/vtouch_ui.cpp:641` 是每帧调用点，
  稳定窗内不吞见 `src-ui/vtouch_ui.cpp:640`）→ 核心用 seqlock 读（`src/vt_shm.c:147`，发布中/不一致就保守不吞：`src/vt_shm.c:152`-`src/vt_shm.c:164`）
  → `src/vt_shm.c:167` 判点。面板**请求停引擎**的瞬间就不再吞（`src/vt_shm.c:171`），那只手立刻回系统。
- 面板侧附着：`src-ui/ui_glue.c:192` → `vt_shm_attach()`（`src/vt_shm.c:182`）；区 A 只读映射（`src/vt_shm.c:212`-`src/vt_shm.c:219`），区 B 单独 RW（`src/vt_shm.c:224`）。
- 面板主循环：`src-ui/ui_glue.c:210` `vtouch_poll_step()`——`src-ui/ui_glue.c:216` 面板心跳（`src/vt_shm.c:257`；核心心跳停滞 20 拍就自杀：`src/vt_shm.c:266`-`src/vt_shm.c:267`）、
  `src-ui/ui_glue.c:217` 事件环出队、`src-ui/ui_glue.c:219` 区域表代次变了就同步、`src-ui/ui_glue.c:220` `stop_req` → 返回 -1。
- 面板改表（**单条删/改名只有面板能发起**）：`src-ui/ui_glue.c:253`、`src-ui/ui_glue.c:258`、`src-ui/ui_glue.c:263`、`src-ui/ui_glue.c:268`
  → `glue_post()`（`src-ui/ui_glue.c:118`）；`src-ui/ui_glue.c:140` 是「等核心吃掉这一拍」（上限约 1s）——单槽邮箱不等就会互相覆盖。
- 渲染线程：`src-ui/vtouch_ui.cpp:1720`；每帧 `src-ui/vtouch_ui.cpp:627` 快照触摸 + `src-ui/vtouch_ui.cpp:641` 推矩形；
  `src-ui/vtouch_ui.cpp:1732` 清理引用了「已不存在 id」的面板状态；`src-ui/vtouch_ui.cpp:1736` 落盘（`save_regions` `src-ui/vtouch_ui.cpp:322`，写 `.tmp` 再 `rename`：`src-ui/vtouch_ui.cpp:340`）；
  载入 `load_regions`（`src-ui/vtouch_ui.cpp:348`）会**判返回值**（`src-ui/vtouch_ui.cpp:373`）——一条坏记录不带走整张表。
- 图层与转屏（双图层原子翻转）：`src-ui/vtouch_ui.cpp:1750` 为待命槽建 EGLSurface（可见槽继续出图）；`src-ui/vtouch_ui.cpp:1956`
  待命槽画满两帧 → `src-ui/vtouch_ui.cpp:1957` 置翻转请求；Java 在 `src-ui/VTouchUI.java:297`-`src-ui/VTouchUI.java:303`
  一个事务里旧槽 `alpha→0`、新槽 `alpha→1`、`apply()`、`nativeOnFlip(hid)`；native 收在 `src-ui/vtouch_ui.cpp:2078`。
  Surface 回调 `src-ui/vtouch_ui.cpp:2061`，显示变化 `src-ui/vtouch_ui.cpp:2100`。绘制尺寸取 `eglQuerySurface`（`src-ui/vtouch_ui.cpp:1888`）。

原文（`src-ui/ui_glue.c:216`-`src-ui/ui_glue.c:220`，面板每拍的固定工作）：

```c
        if (vt_shm_ui_tick() != 0) return -1;                /* 核心死了：别"看着正常其实全死" */
        while (vt_shm_ring_pop(line, sizeof line))
            if (HK_ok && HK.event) HK.event(line);           /* 事件环 → 面板的事件日志 */
        glue_watch_table();
        if (B && B->stop_req) return -1;                     /* 引擎要停 → 面板跟着收尾 */
```

原文（`src-ui/VTouchUI.java:297`-`src-ui/VTouchUI.java:303`，原子翻转）：

```java
                            /* **原子翻转**：一个事务里旧槽 alpha→0、新槽 alpha→1，SurfaceFlinger 一次提交，
                             * 中间不存在"两边都不可见"的帧 —— 这就是转屏零空白的来源。 */
                            int hid = 1 - cur;
                            txnCall(tt, "setAlpha", new Class<?>[]{SCC, float.class}, layers[cur], 0.0f);
                            txnCall(tt, "setAlpha", new Class<?>[]{SCC, float.class}, layers[hid], vis ? 1.0f : 0.0f);
                            TXN.getMethod("apply").invoke(tt);
                            nativeOnFlip(hid);
```

## ⑤ 收尾路径

**执行者：主线程**（`cleanup`，只跑一次）；面板进程的收尾由它自己的一拍循环负责。

- 信号：`src/vtouchd.c:286` `on_signal()` 只置 `g.stop_flag`（信号里不做清理）；主循环下一轮 `src/vtouchd.c:191` 返回 -1。
- `main`：`src/vtouchd.c:309` 置标志让区域线程从 1ms 空转里出来 → `src/vtouchd.c:310` `pthread_join` → `src/vtouchd.c:311` `cleanup()`。
- `cleanup()`（`src/vtouchd.c:264`）：`src/vtouchd.c:266`-`src/vtouchd.c:268` 只跑一次；
  `src/vtouchd.c:270` **先停面板**；`src/vtouchd.c:272`、`src/vtouchd.c:273` 关 client / listen；
  `src/vtouchd.c:274`-`src/vtouchd.c:277` `EVIOCGRAB=0` 再关输入 fd；`src/vtouchd.c:278` `UI_DEV_DESTROY`。
- 面板怎么停：`src/vt_panel.c:301` `vt_panel_stop()` 发 SIGTERM（`src/vt_panel.c:305`）→ 40×20ms 等待（`src/vt_panel.c:306`-`src/vt_panel.c:313`）→ SIGKILL 兜底（`src/vt_panel.c:315`-`src/vt_panel.c:316`）。
- 面板主动停引擎：`src-ui/ui_glue.c:228` `vtouch_cleanup()`——`src-ui/ui_glue.c:232` 先声明「我不吞了」，`src-ui/ui_glue.c:233` 置 `stop_req`，
  `src-ui/ui_glue.c:237` 只在 `core_pid == getppid()` 时发 SIGTERM（面板先死时父进程已被 reparent 到 init，不能盲杀）。
- 看门狗（核心侧，每轮 poll 调一次：`src/vtouchd.c:252`）：心跳停滞 300 拍（约 3s）杀面板（`src/vt_panel.c:260`-`src/vt_panel.c:263`）；
  面板**不在**时按**单调时间**判据重试：进「面板不在」态记 `S_absent_t0`，距上次尝试 ≥ `VT_PANEL_ABSENT_RETRY_MS`(3000ms) 就再拉一次
  （`src/vt_panel.c:276`-`src/vt_panel.c:288`；**不是**循环拍数 —— poll 会因可读事件提前返回，拍数口径在负载下会把预算瞬间烧完）；
  重启频率上限 3 次/60s（`src/vt_panel.c:236`-`src/vt_panel.c:241`，常量 `src/vt_panel.c:105`、`src/vt_panel.c:106`）；
  `shm` 不可用（`S_shm_ok` 假，`src/vt_panel.c:113`）就不重试。
  **不会最终放弃**：被拒的那次也 `++S_restarts`（`src/vt_panel.c:237`），而 60s 窗口只在下一次调用时才滚动（`src/vt_panel.c:236`）
  ⇒ 面板永远起不来时，稳定态是**每 ~60s 再来 3 次**（每次间隔 ~3s），一直循环下去（代价只是每轮两条 stderr 日志，注入不受影响）。
- 面板自杀路径：`src-ui/vtouch_ui.cpp:1964`-`src-ui/vtouch_ui.cpp:1967` 连续 100 帧提交失败 → `vtouch_cleanup()` + `_exit(0)`（不假装在跑）。

原文（`src/vtouchd.c:270`-`src/vtouchd.c:278`，逆序释放）：

```c
    vt_panel_stop();                     /* 先停面板，再放 grab（面板不该在抓着触摸时继续画） */
    if (g.client_fd >= 0) { close(g.client_fd); g.client_fd = -1; }
    if (g.listen_fd >= 0) { close(g.listen_fd); g.listen_fd = -1; }
    if (g.input_fd >= 0) {
        ioctl(g.input_fd, EVIOCGRAB, 0);
        close(g.input_fd); g.input_fd = -1;
    }
    if (g.u_fd >= 0) { ioctl(g.u_fd, UI_DEV_DESTROY); close(g.u_fd); g.u_fd = -1; }
```

## 读码发现

1. **WS 命令族里没有 `region del` / `region rename`，但注释还写着有。** `src/vt_ws.c:565` 的族注释是 `region add|del|clear|list`，
   实现只有 `clear`（`src/vt_ws.c:572`）、`list`（`src/vt_ws.c:576`）、`add`（`src/vt_ws.c:611`）；
   `region_del()`（`src/vt_region.c:104`）与 `region_rename()`（`src/vt_region.c:129`）只从共享内存编辑邮箱被调用（`src/vt_shm.c:139`、`src/vt_shm.c:140`），
   即对脚本不可达 —— 单条删/改名只能在面板上做。
2. **物理触点的压力值是常量。** `src/vt_frame.c:176` 发的是 `g.pressure_max`（不是当次读数）：`g.has_pressure` 只决定「发不发」，
   所以下游看到的压力恒为量程最大值。
3. **UP 事件的坐标不是抬手那一刻的坐标。** 抬手只置 `pending_up`（`src/vt_input.c:159`），`g.phys[slot].x/y` 不更新（`src/vt_input.c:164`、`src/vt_input.c:165` 只在收到位置事件时写）；
   转发时 UP 用 `g.phys[i].x/y`（`src/vt_frame.c:267`）——即抬手前最后一次坐标，`up` 的 `x,y` 是「最后位置」而不是「抬起位置」。
4. **超长命令行不是回 `err`，而是直接断连。** `src/vt_ws.c:409` 对超过 `MAX_LINE`（1024，`src/vt_internal.h:43`）的 payload 直接 `return -1`，
   调用方按断连处理（`src/vtouchd.c:247` → `drop_client()`）；只有「词数/数值范围」不对才回 `err point` / `err region`。
5. **`region list` 的「已自行发送」协议是 `resp[0] == 0` 这个哨兵。** `src/vt_ws.c:601` 置它，`src/vt_ws.c:415` 用它决定要不要再入队
   —— 也就是说任何命令族只要不写 `resp` 就会被静默当成「无回包」，改成别的返回约定时这两处必须一起动。
