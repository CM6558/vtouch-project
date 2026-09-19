/* vtouchd —— 最小核心 + 转发引擎（Plan B：合并/转发 与 判断/推送 分离）。
 *
 * 它做什么（两块）：
 *   ① 合并方案（进）：EVIOCGRAB 抓物理触摸屏 → 解析进 phys[]；和 WS 注入的 virt[] 在同一帧里
 *      一起写进一个 uinput 设备 → Android 看到一块统一的触摸屏，不区分真手与注入。
 *   ② 转发方案（出）：物理帧边界（SYN）之后，把「状态变化」入队，判断与推送各自离开热路径：
 *        事件队列 region_q  → 区域线程（五事件判定）→ region_ev
 *        出站队列 outq      → 主循环 POLLOUT → 客户端 socket
 *      注入热路径只剩 writev + push（微秒级、永不阻塞）；客户端再慢也只堆积在它的出站队列里。
 *
 * 模块地图（每个 .c 一个功能块；链成一个可执行，见 src/vt_internal.h）：
 *   vt_util.c §2 小工具 · vt_queue.c §3+§4 队列 · vt_region.c §5+§6 区域/区域线程
 *   vt_input.c §7 物理输入+uinput 设备 · vt_frame.c §8+§9 组帧/合帧/转发 · vt_ws.c §10 WebSocket
 *   vtouchd.c §1 共享状态定义 + §11 进程（参数/init/主循环/退出）
 *
 * 引擎部件（方案 §4/§5）：
 *   事件队列  region_q   SPSC 无锁环，容量 64，主线程 push / 区域线程 pop（§4.3）
 *   区域线程  region_apply()：五事件判定，slot_in/slot_hit/slot_last 线程私有（§4.4）
 *   出站队列  outq       容量 64 帧，生产者 = 主线程(响应) + 区域线程(region_ev)，
 *                        消费者 = 主线程 POLLOUT 刷出（§4.5）
 *   身份分段  静态两段、不做避让，而且**不落字段**：发给系统的槽位与 tracking id 都是下标的纯函数 ——
 *             物理触点 = 物理槽号（0..phys_slots-1，虚拟跳过这一段），虚拟触点 = phys_slots + 客户端槽号。
 *             （README「身份两段」；发射点见 emit_frame）
 *
 * 有意不做（别在这里找）：面板(ImGui) / 区域持久化 / UI 回调 / 旋转换算 / 落盘 —— 完整版在 build/_backup_full_*。
 *
 * 用法: vtouchd -w <竖屏宽> -h <竖屏高> [-p 端口] [-v 虚拟槽数]
 * 协议: 一行一条命令，回一行（loopback WS，单客户端，新连接踢旧连接；命令表见 vt_ws.c 的命令族）
 *   ping                     -> pong
 *   quiet [0|1]              -> ok | err quiet          （1 = 注入族不回 ok，脚本侧少白收白解析）
 *   res                      -> res <lw> <lh> raw <xmin> <xmax> <ymin> <ymax> phys <n>
 *   reset                    -> ok | err frame
 *   down <slot> <lx> <ly>    -> ok | err point      （各自成一帧）
 *   move <slot> <lx> <ly>    -> ok | err point      （各自成一帧）
 *   up   <slot>              -> ok | err point      （各自成一帧）
 *   begin_frame              -> ok | err frame
 *   point <slot> <down|move|up> <lx> <ly> -> ok | err point   （多指合并进同一帧）
 *   end_frame                -> ok | err frame
 *   points <n> <slot> <down|move|up> <lx> <ly> … -> ok | err point | err frame
 *                              （一条命令一帧多点：n 组，语义同 begin_frame+N×point+end_frame，
 *                                但先全解析校验再一次性提交 —— SDK 的 frame() 用它，N+2 条变 1 条）
 *   region clear             -> ok <n>
 *   region list              -> region <id> <type> <a1> <a2> <a3> <a4> <en>… / end <n>
 *   region add <id> <type> <a1> <a2> <a3> <a4> <en> -> ok <n> | err region
 *   region mark <id> <0|1>   -> ok <n> | err region    （脚本「开关样式」，面板直接读共享内存照着高亮）
 *   sub [phys|region|all] [<选择> [<事件>]] -> ok       （**两条**推送通道，各有自己的过滤器）
 *   unsub [phys|region]      -> ok
 *
 * 出站事件（订阅后推给客户端，走发送队列）：
 *   phys_ev <down|move|up> <slot> <lx> <ly>                    物理触摸流（sub phys；只报物理手指）
 *   region_ev <id> <down|enter|move|exit|up> <slot> <lx> <ly>  区域五事件（sub region；只报物理手指）
 *   两者行末都可带 <墙钟毫秒>（时间戳按需，见 sub 的 <事件> 里的 ts/nots）
 *   面板侧还有第三条「通道」：共享内存事件环（与脚本订不订无关，见 vt_shm.c）。
 *   其余命令（ping / reset / region clear / sub all）现役 SDK 不发，保留作**调试口** ——
 *   改这几条没有"客户端兼容"压力（评审 §3 的口径）。
 *
 * 失败语义：坏客户端只影响它自己（关连接 + 抬掉它的虚拟触点 + 清它的出站队列）；grab 与 uinput 不受影响。
 * 构建: sh scripts/build.sh
 */

#include "vt_internal.h"

/* ===== §1 共享状态（唯一定义在这里）===== */
#ifdef VT_UI
/* VT_UI 构建：状态本体放进共享内存（面板只读映射同一份）。启动早期 g 指向引导副本
 * ——apply_args 要往里写；vt_shm_create() 之后 g 指向映射，全库调用点一行不改。 */
static struct vt_state G_BOOT = { VT_STATE_DEFAULTS };
struct vt_state *g_ptr = &G_BOOT;
#else
struct vt_state g = { VT_STATE_DEFAULTS };
#endif
/* ===== §11 进程（参数 / 初始化 / 主循环 / 退出）===== */

/**
 * (vtouch-doc: apply_args)
 * @brief 解析命令行：-w 宽 -h 高（可选，不给就自动探测）、-p 端口、-v 虚拟槽数。
 * @param   argc     参数个数
 * @param   argv     参数数组
 * @note    取值越界会打日志并保留默认值。
 */
void apply_args(int argc, char **argv)
{
    int i, n;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-w")) {
            if (i + 1 >= argc || parse_long(argv[++i], 2, 100000, &g.logical_width) != 0)
                fprintf(stderr, "vtouchd: -w 取值无效，保留默认 %d\n", g.logical_width);
        } else if (!strcmp(argv[i], "-h")) {
            if (i + 1 >= argc || parse_long(argv[++i], 2, 100000, &g.logical_height) != 0)
                fprintf(stderr, "vtouchd: -h 取值无效，保留默认 %d\n", g.logical_height);
        } else if (!strcmp(argv[i], "-v")) {
            if (i + 1 >= argc || parse_long(argv[++i], 1, MAX_VIRT, &n) != 0)
                fprintf(stderr, "vtouchd: -v 取值无效（1~%d），保留默认 %d\n", MAX_VIRT, g.vslots);
            else g.vslots = n;
        } else if (!strcmp(argv[i], "-p")) {
            if (i + 1 >= argc || parse_long(argv[++i], 1, 65535, &g.ws_port) != 0)
                fprintf(stderr, "vtouchd: -p 取值无效，保留默认 %d\n", g.ws_port);
        } else if (strcmp(argv[i], "-w") && strcmp(argv[i], "-h") && strcmp(argv[i], "-v") && strcmp(argv[i], "-p")) {
            fprintf(stderr, "用法: %s [-w 宽 -h 高] [-v 虚拟槽数] [-p 端口]（尺寸缺省自动探测）\n", argv[0]);
        }
    }
}
/**
 * (vtouch-doc: vtouch_init)
 * @brief 初始化：锚墙钟 → 尺寸门 → 清表 → 认设备 → 建 uinput → 先起监听 → 最后 EVIOCGRAB → 起区域线程。
 * @param   argc     参数个数
 * @param   argv     参数数组
 * @return  0 成功；负数 = 失败阶段（-2..-7），main 直接拿它当退出码。
 * @note    这个顺序是有意的：任何失败路径都不会留下「抓着触摸却没人能控制」的状态。
 */
int vtouch_init(int argc, char **argv)
{
    char dev[PATH_MAX];
#ifdef VT_UI
    int shm_fd = -1;
#endif
    setvbuf(stderr, NULL, _IONBF, 0);   /* 日志实时落盘，别被全缓冲吞掉 */
    wall_clock_anchor();               /* 事件要带墙钟时间给脚本看，先把两套钟的偏移钉住 */
    apply_args(argc, argv);
    if (g.logical_width < 2 || g.logical_height < 2) {
        int dw = 0, dh = 0;
        const char *src = "?";
        /* 没给 -w/-h 就自己探测：先问框架（wm size，和脚本 device.width/height 同源），
         * 拿不到再读内核的屏幕模式。两者都归一化成竖屏逻辑空间（不随旋转变）。 */
        if (detect_logical_size(&dw, &dh, &src) == 0) {
            g.logical_width = dw; g.logical_height = dh;
            fprintf(stderr, "vtouchd: 未指定逻辑尺寸 → 自动探测 %dx%d（来源：%s）\n", dw, dh, src);
        } else {
            fprintf(stderr, "vtouchd: 需要逻辑尺寸：自动探测失败，请显式给 -w 宽 -h 高\n");
            return -2;
        }
    }
#ifdef VT_UI
    /* 状态进共享内存（在拿设备之前：之后就都在映射里写了）。失败不致命 → 按无 UI 模式继续。 */
    shm_fd = vt_shm_create();
#endif
    memset(g.phys, 0, sizeof g.phys); memset(g.virt, 0, sizeof g.virt); memset(g.staged, 0, sizeof g.staged);
    if (discover(dev, sizeof dev) < 0) {
        fprintf(stderr, "vtouchd: 没找到 Type-B 触摸屏（扫了 /dev/input/event0..63）\n");
        return -2;
    }
    /* 一致性自检：触摸屏 raw 量程的比例必须与逻辑尺寸的比例一致，否则整套坐标是歪的
     *（这台机器 raw 0..23040 × 0..50688 ↔ 1440×3168，正好 16×）。只告警，不改值。 */
    {
        long rw = (long)g.axmax[0] - g.axmin[0] + 1, rh = (long)g.axmax[1] - g.axmin[1] + 1;
        double dr = (double)rw / (double)rh;
        double dl = (double)g.logical_width / (double)g.logical_height;
        if (dr > dl * 1.02 || dr < dl * 0.98)
            fprintf(stderr, "vtouchd: 警告：raw 量程比 %.4f 与逻辑尺寸比 %.4f 不一致，坐标可能错位\n", dr, dl);
    }
    /* 身份两段（物理段 0..phys_slots-1，虚拟段紧接其后）只是下标算术，没有要初始化的状态；
     * 这里只算两个要给系统声明的数：槽数与 tracking id 上界 —— 都盖住两段之和。 */
    g.total_slots = g.phys_slots + g.vslots;
    g.id_max = g.total_slots - 1;
    if (setup_uinput() < 0) {
        fprintf(stderr, "vtouchd: uinput 建设备失败: %s\n", strerror(errno));
        return -3;
    }
    g.input_fd = open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (g.input_fd < 0) { cleanup(); return -4; }
    /* 先起监听、最后 grab：任何失败路径都不会留下「抓了却没人能控制」的状态 */
    g.listen_fd = make_listen();
    if (g.listen_fd < 0) { cleanup(); return -6; }
    if (ioctl(g.input_fd, EVIOCGRAB, 1) < 0) { cleanup(); return -5; }
    /* §4.3：区域线程最后起 —— 它一起来就吃队列，所以要等「所有能失败的步骤」都过了再拉它。
     * 唤醒 fd 先建：没有它线程只是退回 1ms 空转，功能不变（见 region_thread_main）。 */
    region_q_init();
    if (pthread_create(&g.region_tid, NULL, region_thread_main, NULL) != 0) {
        fprintf(stderr, "vtouchd: 区域线程创建失败: %s\n", strerror(errno));
        cleanup(); return -7;
    }
    g.region_started = 1;
    fprintf(stderr, "vtouchd: dev=%s pool=%d virt_max=%d pressure=%s ws=127.0.0.1:%d size=%dx%d engine=on(evq=%d outq=%d)\n",
            dev, g.total_slots, g.vslots, g.has_pressure ? "on" : "off", g.ws_port, g.logical_width, g.logical_height,
            VTQ_CAP, OUTQ_CAP);
#ifdef VT_UI
    /* 以核心为准：引擎（设备 / grab / uinput / 监听 / 区域线程）全部就绪，最后才拉面板；
     * 面板没起来也不影响注入 —— 它只是观察者 + 输入面板。 */
    if (shm_fd >= 0 && vt_panel_start(shm_fd) == 0) vt_shm_tick();
#endif
    return 0;
}
/**
 * (vtouch-doc: vtouch_poll_step)
 * @brief 主循环一轮：poll 五路 fd（物理 / 监听 / 客户端 / 出站 / 面板唤醒）→ 各自处理 → 唯一刷出点。
 * @return  0 继续；-1 该退出。
 * @note    poll 超时取最紧的一档：待重发的整帧 5ms ｜ 输入缓冲里已有完整帧 1ms ｜ 默认 1000ms（VT_UI 若无唤醒 fd 则 8ms）；面板的编辑/停引擎请求/面板死亡都由唤醒 fd 立刻打断长睡眠。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   单轮 poll：返回 0 = 继续，-1 = 停止
 */
int vtouch_poll_step(void)
{
    struct pollfd p[5];
    nfds_t np;
    int want_out, r;
    int to;
    if (g.stop_flag) return -1;
#ifdef VT_UI
    vt_shm_tick();                       /* 核心心跳 */
    vt_shm_edit_apply();                 /* 面板投的区域编辑：这一轮就吃掉 */
    if (vt_shm_stop_req()) { fprintf(stderr, "vtouchd: 面板请求停引擎 → 退出\n"); return -1; }
#endif
    /* poll 超时（三档，取最紧的那个）：
     *   ① 有待重发的整帧 → 5ms：尽快把手抬起来（老行为，逐字保留）；
     *   ② 输入缓冲里**已经有完整帧** → 1ms：它已经在我们手里了，不会再有一次 POLLIN 来敲门
     *      （只看 ws_has_pending 的「有没有半包」是不够的 —— 半包不该压超时，整帧才该）；
     *   ③ VT_UI：有面板唤醒 fd（pipe）就敢长睡 1000ms —— 面板投编辑 / 请求停引擎 / 面板自己死了
     *      都会立刻把它叫醒；没有这个 fd 时退回 8ms 轮询（否则面板编辑要等满一个 poll 超时）。 */
    to = g.g_reemit ? 5 : 1000;
    if (!g.g_reemit && ws_has_complete_frame()) to = 1;
#ifdef VT_UI
    if (to > 8 && vt_panel_wake_fd() < 0) to = 8;
#endif
    want_out = (g.client_fd >= 0 && outq_pending());
    p[0] = (struct pollfd){ g.input_fd, POLLIN | POLLHUP | POLLERR, 0 };
    p[1] = (struct pollfd){ g.listen_fd, POLLIN, 0 };
    p[2] = (struct pollfd){ g.client_fd, g.client_fd >= 0 ? (POLLIN | POLLHUP | POLLERR) : 0, 0 };
    /* §4.5：队列非空就把客户端 fd 也挂上 POLLOUT（可写的 socket 总是报 POLLOUT → poll 立刻返回） */
    p[3] = (struct pollfd){ g.client_fd, want_out ? POLLOUT : 0, 0 };
    np = g.client_fd >= 0 ? 4 : 2;
#ifdef VT_UI
    if (vt_panel_wake_fd() >= 0) {
        p[4] = (struct pollfd){ vt_panel_wake_fd(), POLLIN | POLLHUP | POLLERR, 0 };
        np = 5;
    }
#endif
    r = poll(p, np, to);
    if (r < 0) {
        if (errno == EINTR) return 0;
        fprintf(stderr, "vtouchd: poll 失败 errno=%d (%s) → 停止\n", errno, strerror(errno));
        return -1;
    }
    if (p[0].revents & POLLIN) physical_events();
    if (g.g_reemit && g.u_fd >= 0) {
        /* g_emit_fail 的**唯一所有者**（评审 C16）：数的是「重发连续失败次数」，成功即归零；
         * 5ms 一拍 ⇒ 200 拍 ≈ 1s 连续失败才停机（真到这一步说明 uinput 已经不收事了）。
         * 物理路径那边只提交、不计数 —— 它失败时置的就是这个 g_reemit，下一拍必然在这里被计到。 */
        if (emit_frame() == 0) { g.g_reemit = 0; g.g_emit_fail = 0; }
        else if (++g.g_emit_fail >= 200) {
            fprintf(stderr, "vtouchd: uinput 连续 %d 次写失败（约 1s）→ 停止（物理触摸回系统）\n", g.g_emit_fail);
            return -1;
        }
    }
    if (p[0].revents & (POLLHUP | POLLERR)) {
        fprintf(stderr, "vtouchd: 输入设备挂断 (revents=0x%x) → 停止\n", p[0].revents);
        return -1;
    }
    if (p[1].revents & POLLIN) {
        int ncf = accept4(g.listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (ncf >= 0) {
            struct timeval rtv = { .tv_sec = 0, .tv_usec = 300000 };   /* 握手最多被拖 300ms */
            struct timeval stv = { .tv_sec = 0, .tv_usec = 20000 };    /* 写超时（第二道保险） */
            int one = 1;
            if (g.client_fd >= 0) { fprintf(stderr, "vtouchd: 新连接，踢掉旧客户端\n"); drop_client(); }
            setsockopt(ncf, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof rtv);
            setsockopt(ncf, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof stv);
            setsockopt(ncf, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            if (websocket_handshake(ncf) != 0) {
                fprintf(stderr, "vtouchd: ws 握手失败\n");
                close(ncf);
            } else {
                int fl = fcntl(ncf, F_GETFL, 0);
                if (fl >= 0) fcntl(ncf, F_SETFL, fl | O_NONBLOCK);   /* §4.5：出站写永不阻塞主线程 */
                g.client_fd = ncf;
                ws_input_reset();
                outq_reset();
                fprintf(stderr, "vtouchd: ws client connected\n");
            }
        }
    }
    if (g.client_fd >= 0 && (p[2].revents & (POLLHUP | POLLERR))) drop_client();
    /* ws_in_len > 0 也要进来：已经读进缓冲、还没处理完的帧不能让 POLLIN 决定生死 */
    if (g.client_fd >= 0 && ((p[2].revents & POLLIN) || ws_has_pending())) {
        if (client_frame() < 0) drop_client();
    }
    /* §4.5：唯一的刷出点 —— 队列里是刚入队的响应，或区域线程塞进来的 region_ev */
    if (g.client_fd >= 0 && ((p[3].revents & POLLOUT) || outq_pending())) outq_flush();
#ifdef VT_UI
    /* p[4] 面板唤醒 fd：POLLIN = 面板投了编辑 / 请求停引擎（具体动作由**下一轮开头**的
     * vt_shm_edit_apply / vt_shm_stop_req 做）→ 这里只把管道读空，别让它一直可读；
     * POLLHUP/POLLERR = 写端全关 = 面板没了 → 立刻收尸重启，不等看门狗下一拍。
     * 先读后判：数据与 EOF 可能同一拍到达（POLLIN|POLLHUP）。 */
    if (np == 5 && p[4].revents) {
        char b[64];
        if (p[4].revents & POLLIN) { while (read(p[4].fd, b, sizeof b) > 0) ; }
        if (p[4].revents & (POLLHUP | POLLERR)) vt_panel_wake_drop();
    }
    vt_panel_watchdog();                 /* 回收子进程 / 判心跳 / 按策略重启 */
#endif
    return 0;
}
/**
 * (vtouch-doc: cleanup)
 * @brief 释放资源：关客户端 → 关监听 → 放 EVIOCGRAB → 关设备。
 * @note    逆序释放：先放触摸（物理触摸立刻回系统），再拆设备。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   收尾顺序固定：client → listen → 放 grab → 销毁 uinput。只跑一次。
 */
void cleanup(void)
{
    static int cleaned;
    if (cleaned) return;
    cleaned = 1;
#ifdef VT_UI
    vt_panel_stop();                     /* 先停面板，再放 grab（面板不该在抓着触摸时继续画） */
#endif
    if (g.client_fd >= 0) { close(g.client_fd); g.client_fd = -1; }
    if (g.listen_fd >= 0) { close(g.listen_fd); g.listen_fd = -1; }
    if (g.input_fd >= 0) {
        ioctl(g.input_fd, EVIOCGRAB, 0);
        close(g.input_fd); g.input_fd = -1;
    }
    if (g.u_fd >= 0) { ioctl(g.u_fd, UI_DEV_DESTROY); close(g.u_fd); g.u_fd = -1; }
}
/**
 * (vtouch-doc: on_signal)
 * @brief 信号处理器：置退出标志，让主循环下一轮自己收尾（不在信号里做清理）。
 * @param   s        信号编号
 * @note    只置标志，不打印、不关 fd —— 信号处理函数里能做的事越少越安全。
 */
void on_signal(int s) { (void)s; g.stop_flag = 1; }
/**
 * (vtouch-doc: main)
 * @brief 进程入口：装信号 → init → 主循环 → 置 stop_flag 并 join 区域线程 → cleanup。
 * @param   argc     参数个数
 * @param   argv     参数数组
 * @return  0；init 失败时返回对应的错误码。
 */
int main(int argc, char **argv)
{
    struct sigaction sa;
    int rc;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, 0);
    sigaction(SIGINT, &sa, 0);
    signal(SIGPIPE, SIG_IGN);

    rc = vtouch_init(argc, argv);
    if (rc != 0) return -rc;            /* 退出码 = 2..7（见 README 的失败出口表） */
    while (vtouch_poll_step() == 0)
        ;
    g.stop_flag = 1;                      /* 让区域线程从 poll 里出来 */
    region_q_wake();                      /* 它可能正阻塞在 eventfd 上（1s 兜底超时外）：显式叫醒一次 */
    if (g.region_started) pthread_join(g.region_tid, NULL);
    cleanup();
    return 0;
}
