# -*- coding: utf-8 -*-
"""函数文档数据（Doxygen 风格）：由 build/_apply_funcdoc.py 注入到 src/*.c 的定义处与 vt_internal.h 的原型处。
字段：brief 一句话；params [(名字, 说明)]；ret 返回值语义；note 线程/调用时机/陷阱。"""

DOCS = {
# ---------------- §2 小工具 ----------------
"parse_long": dict(brief="把字符串解析成 [lo, hi] 区间内的整数（命令参数解析用）。",
    params=[("s", "待解析文本"), ("lo", "允许下界（含）"), ("hi", "允许上界（含）"), ("out", "成功时写入结果")],
    ret="0 成功；-1 非数字、越界或带多余字符。",
    note="范围检查就是协议的一部分：越界一律回 err，不静默截断。"),
"bit": dict(brief="取位图（cap_* 那几张能力位图）里的第 n 位。",
    params=[("b", "位图数组"), ("n", "位号")], ret="非 0 表示该位置位。"),
"logical_to_raw": dict(brief="逻辑坐标（设备像素）→ 触摸屏 raw 坐标。",
    params=[("logical", "逻辑值"), ("axis", "0=X 1=Y"), ("raw", "输出 raw 值")],
    ret="0 成功；-1 轴非法或该轴量程为 0。"),
"raw_to_logical": dict(brief="raw 坐标 → 逻辑坐标（转发区域事件时用）。",
    params=[("raw", "raw 值"), ("axis", "0=X 1=Y"), ("logical", "输出逻辑值")], ret="0 成功；-1 轴非法或量程非法。"),
"now_ns": dict(brief="单调时钟（纳秒），事件时间戳用。", ret="单调递增的纳秒数（CLOCK_MONOTONIC）。"),

# ---------------- §3+§4 队列 ----------------
"queue_drop_log": dict(brief="队列丢弃的诊断日志：前 3 次每次都打，之后每 100 次打一行。",
    params=[("what", "队列名/动作标签（如 \"事件(丢新)\" / \"事件(合并旧move)\" / \"出站\"）"), ("n", "该队列累计丢弃数")],
    note="诊断不占热路径（代价只是一次取模比较）。"),
"vtq_push": dict(brief="事件入队（单生产者 = 主线程，消费者 = 区域线程）。",
    params=[("q", "队列"), ("ev", "事件（按值拷入）")],
    note="永不阻塞、永不失败：队满先合并（队尾同槽 move，再退 8 格找同槽旧 move 原地覆盖），"
         "都不行就丢**这一条新的**。生产者**绝不推进 head**（那是消费者一个人的）—— "
         "老实现满态 CAS 推 head「丢最旧」会与消费者抢 head（评审 C13）。丢的只影响事件条数，注入路径不受影响。"),
"vtq_pop": dict(brief="事件出队（单消费者 = 区域线程）。", params=[("q", "队列"), ("ev", "输出事件")], ret="1 取到；0 队空。"),
"outq_reset": dict(brief="清空出站队列（新客户端接入 / 断连时）。", note="残包不许串给下一个客户端（§4.6）。"),
"outq_pending": dict(brief="出站队列是否非空（主循环据此决定要不要挂 POLLOUT）。", ret="1 有；0 无。"),
"outq_push": dict(brief="把一段**已经成帧的字节**放入出站队列（生产者 = 主线程 / 区域线程，一把短锁只包住一次 memcpy）。",
    params=[("p", "数据"), ("n", "长度")],
    note="队满丢最旧；**唯一的例外是正在续写的那条**（sent>0，它的前半截已经进了客户端 socket，"
         "扔掉会让后续字节接到错的帧头 ⇒ 解帧错位）：这时改为丢这一条新的。文本请用 outq_push_text，不要直接调这个。"),
"outq_push_text": dict(brief="把一行文本按 WS 文本帧（未加掩码）补齐帧头后入队。", params=[("s", "文本"), ("n", "长度")],
    note="成帧必须在这一层做 —— 队列里存的就是完整帧，刷出端只负责写字节。漏了这步的症状：客户端收到裸文本、"
         "一帧都解不出来，而注入照常生效（所以最难发现）。"),
"outq_flush": dict(brief="主循环唯一的刷出点：socket 可写才写，写不完留到下次；真错（EPIPE/ECONNRESET）才踢客户端。",
    note="客户端 socket 是非阻塞的，这里绝不阻塞主线程。"),

# ---------------- §5+§6 区域 ----------------
"region_q_init": dict(brief="建区域队列的唤醒 fd（eventfd）：区域线程靠它阻塞等待，不再 1ms 空转。",
    ret="0 成功（或已建过）；-1 eventfd 创建失败（不致命：区域线程退回 1ms 空转，功能一个不少）。"),
"region_q_wake": dict(brief="唤醒区域线程（入队方在推完一批事件后调一次）。",
    note="写 eventfd 计数；没有 eventfd 时是空操作。绝不阻塞（非阻塞写，写满也只是丢一次唤醒、不丢事件）。"),
"regions_clear": dict(brief="清空区域表，并把代次 +1（让区域线程重置它私有的状态表）。"),
"region_add": dict(brief="新增或覆盖一个区域（主线程持 region_lock 写表）。",
    params=[("id", "区域名（≤ REGION_ID_MAX 字符）"), ("type", "0=矩形 1=圆"), ("a1", "矩形 x1 / 圆 cx"),
            ("a2", "矩形 y1 / 圆 cy"), ("a3", "矩形 x2 / 圆 r"), ("a4", "矩形 y2"), ("enabled", "1 启用 0 禁用")],
    ret="0 成功；-1 参数非法、id 去重失败或表满。", note="几何只做宽松量程检查（±4096，防判定里 dx*dx 溢出）：区域跟着屏幕方向走时可以落在屏外，此时竖屏坐标允许负数/超界（面板「视口坐标不变」语义）；越界区域在核心侧天然不可命中（手指原生坐标恒在框内），除非半径探进可见区。补充：type=1（圆）时 a3 是半径，必须非负，负数直接拒。"),
"region_hit": dict(brief="点是否落在区域内（矩形含边界；圆按半径平方比较）。",
    params=[("rg", "区域"), ("lx", "逻辑 x"), ("ly", "逻辑 y")], ret="1 命中；0 未命中。",
    note="圆的比较用 64 位算：lx 的上界由 -w 决定（可到 100000），dx*dx 在 32 位里有符号溢出是 UB。",
    ),
"region_ev_send": dict(brief="发一条区域事件：订了 region 通道才入出站队列，没订就只打 (UNSUB) 日志。",
    params=[("id", "区域名"), ("ev", "down/enter/move/exit/up"), ("slot", "物理槽号"),
            ("lx", "逻辑 x"), ("ly", "逻辑 y"),
            ("ts_mono", "事件时间戳（单调钟纳秒；发出去时换算成墙钟毫秒）")],
    note="低频事件；只报物理手指。报文末尾带 <ms>：事件发生的墙钟毫秒（与脚本 Date.now() 同基准），由 ts_mono 换算而来 —— 脚本算按压时长/防抖/看延迟用它。"),
"region_apply": dict(brief="处理一个物理事件：先按 slot 报物理触摸流（sub phys），再做区域五事件判定。",
    params=[("ev", "来自 region_q 的事件")],
    note="状态表（含 T3.1 的触发锁存 r_trig_latch）是线程私有的，只在 region_lock 里读区域表；"
         "触发/开关判定同在锁内，投递（vt_ops_trigger_post / toggle_ev 环行 / 日志）与 region_ev_send 一样在解锁之后。"),
"phys_ev_send": dict(brief="物理触摸流（sub phys）：按 slot 报 down/move/up，不按区域过滤。",
    params=[("ev", "来自 region_q 的事件（带逻辑坐标与时间戳）")],
    note="「按下之后一路跟到抬起」的底座：区域事件出了区域就断了，这条流不断。只订 SUB_PHYS 才发；只报物理手指，虚拟触点不进（防自激）。"),
"region_thread_main": dict(brief="区域线程主循环：pop region_q → region_apply；区域表代次变了就重置私有状态。",
    params=[("arg", "未使用")], ret="NULL（线程不主动退出）。",
    note="只消费队列、只写自己的状态表（含开关位）与触发槽、只往出站队列/事件环塞 region_ev 与 toggle_ev；"
         "绝不注入、绝不直写 socket、绝不碰 phys[]/virt[]。空闲时阻塞在唤醒 fd（eventfd）上 —— 事件入队即醒，不再 1ms 空转。"),

# ---------------- §4 触发侧（T3.1；面板编辑入口，定义在 vt_region.c） ----------------
"region_bind": dict(brief="把区域绑定到操作（opname 允许悬空：触发时解析失败则丢弃）；ev：0=无 1=按下 2=完整按压。",
    params=[("id", "区域名（必须已存在）"), ("opname", "操作名；空串或 \"-\" = 解除绑定（区 B 邮箱契约）"),
            ("ev", "触发时机：0=无 1=按下 2=完整按压")],
    ret="0 成功；-1 区域不存在或参数非法。",
    note="面板编辑入口（区 B 邮箱 VT_EDIT_BIND → vt_shm_edit_apply 调）。名字过 vt_id_ok 同一把尺子；"
         "引用不存在的操作**允许悬空**（触发时解析失败由执行器丢弃）。原地更新（同 id、同表位）"
         "**不动 region_gen** —— 与 region_add 原地分支同款口径：代次一变区域线程会整表清零私有状态，"
         "编辑绑定/面板重启重放配置时进行中的按压会丢 up/锁存。"
         "解除绑定（空串 / \"-\"）时**同时清 trig_ev=0** —— 防面板回读/落盘出现「无绑定却有时机」残留。"),
"region_kind_set": dict(brief="设置区域类型（0=普通 1=开关型）。",
    params=[("id", "区域名（必须已存在）"), ("kind", "0=普通 1=开关型")],
    ret="0 成功；-1 区域不存在或参数非法。",
    note="面板编辑入口（区 B 邮箱 VT_EDIT_KIND → vt_shm_edit_apply 调）。原地更新（同 id、同表位）"
         "**不动 region_gen**（同 region_bind 口径）；只改 kind，toggle_on 原样保留（切回开关型时沿用上次开关态）。"),

# ---------------- §7 物理输入 ----------------
"phys_event_one": dict(brief="分发单条 input_event（槽选择 / 按下抬起 / 位置 / SYN_DROPPED 兜底 / SYN_REPORT 结帧）。",
    params=[("e", "一条 input_event（来自批量读的缓冲）")],
    note="从 physical_events 里抽出来的同一段逻辑（批量读之后一次要处理一批）；边沿语义与逐条 read 的旧版逐字一致。"
         "两处相对旧版的修正：SYN_DROPPED 除了把各槽标抬起，还会**立刻结帧**（不再等后面的 SYN_REPORT，"
         "评审 C14）；g_emit_fail 不在这一层自增（唯一所有者是重发路径，评审 C16）。"),
"validate_device": dict(brief="认一块设备是不是 Type-B 触摸屏（槽 + tracking id + XY 四轴 + 量程），"
                             "并把它的能力声明整份抄进 cap_*（供 setup_uinput 镜像）。",
    params=[("p", "设备节点路径"), ("slots", "输出物理槽数"), ("xmin", "输出 X 下界"), ("xmax", "输出 X 上界"),
            ("ymin", "输出 Y 下界"), ("ymax", "输出 Y 上界")], ret="0 是；-1 不是或打不开。",
    note="不写死 eventN：由 discover 扫 event0..63 逐个问。"),
"discover": dict(brief="扫 /dev/input/event0..63，找第一块 Type-B 触摸屏。",
    params=[("out", "输出设备节点路径"), ("n", "缓冲长度")], ret="0 找到；-1 没找到。"),
"setup_uinput": dict(brief="建合并 uinput 设备：照抄物理屏的能力（EV / KEY / ABS+absinfo / props），"
                          "只改 4 处真冲突（TOOL_TYPE 量程、槽数、tracking id 量程、名字与 bus），并强制 INPUT_PROP_DIRECT。",
    ret="0 成功；-1 失败（调用方以退出码 3 退出）。",
    note="槽数 = phys_slots + vslots；tracking id 上限 = total_slots - 1。名字加 _vtouch 后缀，避免与物理设备同名。"),
"physical_events": dict(brief="读物理流：解析 Type-B 事件进 phys[]（按槽），在 SYN_REPORT 处提交一帧并转发。",
    note="一次 read 取一批（最多 64 条 input_event）再循环解析，不是每条事件一次 read()（syscall 降一个量级）；"
         "一次读可能攒好几帧，边沿在每帧处理完就清；SYN_DROPPED 保守地把所有槽当抬起。"),

# ---------------- §8+§9 组帧 / 合帧 / 转发 ----------------
"ev_add": dict(brief="往本帧的 iovec 里追加一条 input_event（纯内存，不做系统调用）。",
    params=[("t", "事件类型"), ("c", "事件码"), ("v", "值")],
    note="上限 MAX_IOV（1024 ≥ 最坏整帧 771，由 vt_internal.h 的 static_assert 钉住）：满了就丢事件，"
         "所以这个上限必须**大于**最坏帧 —— 否则帧尾的 SYN_REPORT 可能被丢掉，系统里成了半帧。"),
"uinput_writev_retry": dict(brief="把当前帧一次 writev 写进 uinput（只对 EINTR 重试；EAGAIN 立刻返回 -1，交给重发通道）。",
    ret="实际写出的字节数；-1 失败（含 EAGAIN）。",
    note="uinput 以 O_NONBLOCK 打开：EAGAIN 不在热路径里等（老写法 poll 3×20ms 会把触摸线程卡住 60ms）；"
         "调用方据此置 g_reemit，主循环 5ms 后重发同一帧。"),
"emit_iov_writev": dict(brief="提交本帧；**短写要把剩下的 iovec 补完**（只补一条会丢帧尾的 SYN_REPORT，系统里就成了半帧）。",
    ret="0 成功；-1 失败。"),
"any_emitted": dict(brief="本帧是否真的发了触点（决定 BTN_TOUCH / BTN_TOOL_FINGER 的值）。", ret="1 有触点；0 没有。",
    note="判的是来源状态里有触点，不是 iovec 里有没有 —— 虚拟触点抬起时不能把真手指的 BTN_TOUCH 带下去。"),
"emit_frame": dict(brief="把 phys[]/virt[] 合成一帧并提交：待抬 → 物理 → 虚拟 → BTN → SYN，整帧一次 writev。",
    ret="0 提交成功；-1 提交失败（置 g_reemit，由主循环重发）。",
    note="身份按下标算（物理 = i，虚拟 = phys_slots + i）；写失败绝不清 pending_up、绝不释放身份。"),
"set_virtual": dict(brief="改一个虚拟触点的状态（down/move/up）—— 单点命令与帧内命令共用这一段。",
    params=[("state", "virt[] 或 staged[]"), ("slot", "客户端槽号"), ("name", "\"down\"/\"move\"/\"up\""),
            ("x", "raw x"), ("y", "raw y")], ret="0 成功；-1 状态非法（重复 down、没 down 就 move/up）。",
    note="只改来源状态，不写身份字段（身份发射时按下标算）。"),
"owner_reset": dict(brief="客户端断连/被踢：抬掉它所有虚拟触点并立即提交一帧。",
    note="少了这段，客户端在 begin_frame..end_frame 中间断开会把虚拟手指永久粘在设备上。"),
"enqueue_phys_changes": dict(brief="物理帧边界之后：每槽比快照判 down/up/move，把变化入 region_q 喂区域线程（不推客户端）。",
    note="推的是「完整帧状态的快照」；静止不刷屏；必须在 emit_frame 之后调用（§4.1）。有事件才唤醒区域线程一次"
         "（region_q_wake）——静止的手指不产生任何唤醒。"),

# ---------------- §10 WebSocket ----------------
"rol32": dict(brief="32 位循环左移（SHA-1 内部用）。", params=[("x", "值"), ("n", "位数")], ret="左移结果。"),
"be32": dict(brief="读 4 字节大端整数（SHA-1 内部用）。", params=[("p", "字节指针")], ret="大端解读结果。"),
"sha1_block": dict(brief="处理一个 64 字节块（SHA-1 内部）。", params=[("s", "上下文"), ("p", "块起始")]),
"sha1_init": dict(brief="SHA-1 初始化。", params=[("s", "上下文")]),
"sha1_update": dict(brief="SHA-1 追加数据。", params=[("s", "上下文"), ("p", "数据"), ("n", "长度")]),
"sha1_final": dict(brief="SHA-1 收尾，输出 20 字节摘要（WS 握手用）。", params=[("s", "上下文"), ("out", "20 字节输出")]),
"base64": dict(brief="标准 Base64 编码。",
    params=[("in", "输入"), ("n", "输入长度"), ("out", "输出缓冲"), ("cap", "缓冲容量")],
    ret="写入的字节数（含结尾 \\0）；-1 缓冲不够。"),
"header_value": dict(brief="从 HTTP 请求头里取某个头的值（头名大小写不敏感）。",
    params=[("req", "请求原文"), ("name", "头名"), ("out", "输出"), ("cap", "缓冲容量")], ret="0 找到；-1 没有或缓冲不够。"),
"has_token": dict(brief="在请求头值里按逗号分词找 token（大小写不敏感，用于 Connection: Upgrade）。",
    params=[("s", "头值"), ("token", "要找的 token")], ret="1 有；0 没有。"),
"write_full": dict(brief="把 len 字节写满（EINTR、短写自动续写）。",
    params=[("fd", "目标 fd"), ("buf", "数据"), ("len", "长度")], ret="0 成功；-1 出错。",
    note="握手与上行同步写用它；下行的响应/事件走出站队列，不走这里。"),
"websocket_handshake": dict(brief="读 HTTP 请求、校验 Upgrade 与 Sec-WebSocket-Key，回 101。",
    params=[("fd", "已 accept 的连接")], ret="0 成功；-1 不是合法 WS 请求。", note="握手期用带超时的阻塞读（最多被拖 300ms）。"),
"ws_send": dict(brief="直接发一个 WS 帧（控制帧：close / pong 用）。",
    params=[("fd", "连接"), ("opcode", "操作码"), ("p", "正文"), ("n", "正文长度")], ret="0 成功；-1 失败。",
    note="只给控制帧用；文本帧请走 outq_push_text。"),
"drop_client": dict(brief="丢弃当前客户端：关连接 + 抬掉它的虚拟触点 + 清订阅位 + 清出站队列。",
    note="残留的订阅与残包不许串给下一个客户端（§4.6）。"),
"ws_peek_frame": dict(brief="试着从接收缓冲里解析出一个完整帧的表头。",
    params=[("frame_len", "输出整帧长度"), ("opcode", "输出操作码"), ("payload_off", "输出正文偏移")],
    ret="1 解析到；0 数据不够；-1 协议错。"),
"ws_next_frame": dict(brief="取出一个完整帧的正文（必要时继续收）。",
    params=[("payload", "输出正文"), ("plen", "输出长度"), ("opcode", "输出操作码")],
    ret="0 取到；1 暂时没有数据（EAGAIN）；-1 连接关闭或协议错。", note="半包不消费，留到下一轮 poll 继续拼。"),
"client_frame": dict(brief="处理客户端可读事件：一轮最多 32 个帧，解帧 → handle_line → 响应入出站队列。",
    ret="0 保持连接；-1 断开（协议错或连接关闭）。"),
"make_listen": dict(brief="建监听 socket，只绑 127.0.0.1（回环），不对外暴露。", ret="fd；-1 失败（调用方以退出码 6 退出）。"),
"cmd_meta": dict(brief="命令族：ping / res / reset（不碰触点的元命令）。",
    params=[("t", "命令词"), ("stp", "strtok_r 状态"), ("resp", "响应缓冲"), ("cap", "缓冲容量")],
    ret="1 不是本族命令（交给下一族）；0 / -1 = 已处理（-1 时 resp 是错误响应）。"),
"cmd_point_once": dict(brief="命令族：up / down / move —— 单点命令，每个命令提交一帧。",
    params=[("t", "命令词"), ("stp", "strtok_r 状态"), ("resp", "响应缓冲"), ("cap", "缓冲容量")],
    ret="1 不是本族命令；0 / -1 = 已处理（-1 时 resp 是错误响应）。"),
"cmd_frame": dict(brief="命令族：points（一条命令一帧多点）/ begin_frame / point / end_frame —— 帧内多点，一次 SYN 提交。",
    params=[("t", "命令词"), ("stp", "strtok_r 状态"), ("resp", "响应缓冲"), ("cap", "缓冲容量")],
    ret="1 不是本族命令；0 / -1 = 已处理（-1 时 resp 是错误响应）。",
    note="points <n> <slot> <state> <lx> <ly> …：语义与 begin_frame + N×point + end_frame 逐字等价，"
         "但先全部解析校验、再一次性提交（任何一组不合法 ⇒ 整条不生效、不留半帧）。"),
"cmd_region": dict(brief="命令族：region add | clear | list。",
    params=[("t", "命令词"), ("stp", "strtok_r 状态"), ("resp", "响应缓冲"), ("cap", "缓冲容量")],
    ret="1 不是本族命令；0 / -1 = 已处理（-1 时 resp 是错误响应）。", note="主线程只写表（短锁），判定全在区域线程。"),
"cmd_sub": dict(brief="命令族：sub [phys|region|all] [<选择> [<事件>]] / unsub [phys|region]（不带选择 = 老语义全订；带选择 = 精确订阅过滤器）。",
    params=[("t", "命令词"), ("stp", "strtok_r 状态"), ("resp", "响应缓冲"), ("cap", "缓冲容量")],
    ret="1 不是本族命令；0 / -1 = 已处理（-1 时 resp 是错误响应）。",
    note="<选择>：phys 是槽号列表（0 / 0,3 / * / -1 = 全部槽），region 是区域 id（* = 全部）。"
         "<事件>：逗号列表或 *（down,enter,move,exit,up）。给了 <选择> 而不给 <事件> 时默认「简报」——"
         "phys: down,up；region: down,up,enter,exit，**默认不含 move**；要位置流必须显式写 move"
         "（实测物理行占 97% 的量，默认开它等于白烧 CPU）。过滤器只作用于推送，不影响 stderr 日志。"),
"vt_subev_bit": dict(brief="事件名 → SUBEV_* 位（订阅过滤器共用；未知名字返回 0）。",
    params=[("ev", "事件名：down/enter/move/exit/up")], ret="对应位；未知名字 0。",
    note="vt_region.c 推送时判、vt_ws.c 解析 sub 命令时用。"),
"parse_slot_mask": dict(brief="逗号分隔的槽号列表 → 槽位掩码（sub phys 的 <选择>）。",
    params=[("s", "槽号列表，如 \"0\" / \"0,3\"（原地切分）"), ("out", "结果掩码")],
    ret="0 成功；-1 语法错（非数字 / 越界 / 空）。",
    note="用独立的 strtok_r saveptr —— 复用外层状态会把命令参数切坏。"),
"parse_ev_bits": dict(brief="逗号分隔的事件名列表（或 *）→ SUBEV_* 位（sub 的 <事件>）。",
    params=[("s", "事件名列表，如 \"down,up\"；\"*\" = 全部"), ("out", "结果位；* 存 0（= 未设 = 全通）")],
    ret="0 成功；-1 语法错（含未知事件名）。",
    note="* 与「<选择> 缺省」共用「0 = 全通」这一约定。"),
"handle_line": dict(brief="一行命令 → 一行回包：按命令族分派（每族一个 cmd_* 函数）。",
    params=[("line", "命令文本（原地改）"), ("resp", "响应缓冲"), ("cap", "缓冲容量")], ret="0 有响应；-1 错误响应。",
    note="响应文本拼进 resp，由调用方（client_frame）入出站队列。"),

# ---------------- §11 进程 ----------------
"apply_args": dict(brief="解析命令行：-w 宽 -h 高（可选，不给就自动探测）、-p 端口、-v 虚拟槽数。",
    params=[("argc", "参数个数"), ("argv", "参数数组")], note="取值越界会打日志并保留默认值。"),
"vtouch_init": dict(brief="初始化：锚墙钟 → 尺寸门 → 清表 → 认设备 → 建 uinput → 先起监听 → 最后 EVIOCGRAB → 起区域线程。",
    params=[("argc", "参数个数"), ("argv", "参数数组")], ret="0 成功；负数 = 失败阶段（-2..-7），main 直接拿它当退出码。",
    note="这个顺序是有意的：任何失败路径都不会留下「抓着触摸却没人能控制」的状态。"),
"vtouch_poll_step": dict(brief="主循环一轮：poll 五路 fd（物理 / 监听 / 客户端 / 出站 / 面板唤醒）→ 各自处理 → 唯一刷出点。",
    ret="0 继续；-1 该退出。",
    note="poll 超时取最紧的一档：待重发的整帧 5ms ｜ 输入缓冲里已有完整帧 1ms ｜ 默认 1000ms（VT_UI 若无唤醒 fd 则 8ms）；"
         "面板的编辑/停引擎请求/面板死亡都由唤醒 fd 立刻打断长睡眠。"),
"cleanup": dict(brief="释放资源：关客户端 → 关监听 → 放 EVIOCGRAB → 关设备。",
    note="逆序释放：先放触摸（物理触摸立刻回系统），再拆设备。"),
"ws_input_reset": dict(brief="复位 WS 输入缓冲（新客户端接入前清掉上一个客户端的残包）。"),
"ws_has_complete_frame": dict(brief="WS 输入缓冲里是否**已经有一个完整帧**（主循环据此把 poll 超时压到 ~1ms）。",
    ret="1 有完整帧；0 没有（含「只有半包」）。",
    note="与 ws_has_pending 的区别：那个问「有没有半包」（半包压超时就是空转），这个问「有没有整帧」——"
         "整帧已经在我们手里了，不会再有 POLLIN 来敲门，所以必须尽快处理掉。"),
"ws_has_pending": dict(brief="WS 输入缓冲里是否还有没解析完的半包数据（主循环据此继续挂 POLLIN）。", ret="1 有；0 没有。",
    note="半包不消费：解析不出完整帧就留着，等下一轮 poll 再拼。"),
"on_signal": dict(brief="信号处理器：置退出标志，让主循环下一轮自己收尾（不在信号里做清理）。",
    params=[("s", "信号编号")], note="只置标志，不打印、不关 fd —— 信号处理函数里能做的事越少越安全。"),
"main": dict(brief="进程入口：装信号 → init → 主循环 → 置 stop_flag 并 join 区域线程 → cleanup。",
    params=[("argc", "参数个数"), ("argv", "参数数组")], ret="0；init 失败时返回对应的错误码。"),

# ---------------- 操作（vt_ops.c；spec OPS_PLAN §3） ----------------
"vt_ops_put": dict(brief="新增或覆盖一条操作（重名覆盖；核心单点校验，不过拒绝）。",
    params=[("op", "整条操作载荷（名字 + 步数 + 步表）")],
    ret="0 成功；-1 参数为空、校验不过或表满（表满只发生在新增）。",
    note="校验全在核心这一处（与区域 id 同一把尺子）：名字 vt_id_ok（[A-Za-z0-9_-]、1..15；裸 `-` 除外）、"
         "步数 1..MAX_STEPS、类型 ∈ {点按,滑动,等待,按下,弹起,区域判断,开关判断}、"
         "坐标字段（点按/滑动/按下/区域判断）0..逻辑尺寸-1 或变量引用（负数编码 -1..-5）、"
         "时长字段（点按/滑动/等待）按类型分档（点按 0..60000 / 滑动 1..60000 / 等待 0..600000）"
         "或变量引用（负数编码 -1..-5）、条件步不成立行为 a3 ∈ {0,1}、ref 长度 1..15 且过 vt_id_ok"
         "（存在性不校验，允许悬空）。拒绝打 `op 被拒 <名>: <原因>`、"
         "成功打 `op 编辑 put <名> 步数=N`；重名覆盖就地写（表位不变），要么整条生效、要么一点都不动。"),
"vt_ops_del": dict(brief="删除一条操作。",
    params=[("name", "操作名")],
    ret="0 成功；-1 名字非法或表里没有同名条目。",
    note="后面的条目整体前移一格（与 region_del 同套路，表尾清零）；只动表 —— "
         "运行中的操作按自己的快照跑完，怎么收场是执行器侧的事。"),
"vt_ops_clear": dict(brief="清空操作表。",
    note="整表归零（不只是 op_count=0），不留\"幽灵操作\"；只动表 —— 运行中的操作按自己的快照跑完。"),
"vt_ops_init": dict(brief="初始化操作执行器（建触发唤醒 eventfd；失败降级 -1）。",
    ret="0 成功（或已建过）；-1 eventfd 创建失败（不致命：触发最坏退回下一轮 poll 超时唤醒）。",
    note="唤醒 fd 存进区 A 的 g.ops_wake_fd（面板只读得到、不参与）：区域线程投触发时写它一下，主循环立刻醒；"
         "没有它时触发晚 ≤1s 被 poll 兜底捡起，功能不丢。"),
"vt_ops_tick": dict(brief="主循环每轮调：消费触发槽 → 推进运行中的操作 → 刷新到点 deadline。",
    note="按序三件事：① acquire 读触发槽（seq 变了 → 起跑最新一发，被盖掉的记 `op 丢弃 覆盖`；"
         "忙 / 操作不存在 / 门控拦截 / 没空闲槽由 vt_ops_run 丢弃 + 日志）；② 撞槽自检（手指被外力抬掉 → `op 槽冲突 k` + 顺带中止）；"
         "③ 到点做**一个**动作（步入 / 抬指 / 采样点）—— 下一拍做下一个，靠 poll 第四档的 0ms 超时追拍。"
         "上一帧没写出去（g_reemit）时整拍不动：不丢帧、不跳步。"),
"vt_ops_next_deadline_ms": dict(brief="下一步到点的剩余毫秒数（下限 0）；-1 = 空闲。",
    ret="剩余毫秒数（已到点 = 0）；-1 = 没有运行中的操作。",
    note="主循环拿它当 poll 超时的**第四档**（与 5/1/1000ms 取最紧的那一档）：到点就醒，不早不晚；空闲不参与。"),
"vt_ops_run": dict(brief="起跑一条操作（忙时丢弃 + 日志）。",
    params=[("name", "操作名；表里查不到 / 没空闲槽 / 已有操作在跑 = 丢弃 + 对应日志")],
    note="起跑 = 整条快照进执行器私有内存（运行中改表 / 删表不影响本次，spec §7）+ 挑第一个空闲虚拟槽"
         "（virt[]/staged[] 都空）全程占用，并写 g.op_run / op_run_step / op_run_state 供面板回显；"
         "日志 `op 启动 <名> 步数=N 槽=K 门控=<r1|无>`。门控 / 自动关（spec §4.3）：gate 非空先过门控检查 —— "
         "区域须存在、是开关型且开着，否则 `op 丢弃 门控拦截`；auto_off=1 在正常完成时把门控开关翻回关（中止不翻）。"),
"vt_ops_abort": dict(brief="中止运行中的操作（抬指 + 状态归位 + 日志原因）。",
    params=[("why", "中止原因（写进日志；如 停止按钮 / 引擎收尾 / reset/断连）")],
    note="**幂等**：没在跑（含没初始化、init 失败路径的 cleanup）就是空操作 —— cleanup() 无条件调它。"
         "抬指帧走 emit_frame：写失败置 g_reemit 等主循环重发；进程退出路径由随后 uinput 销毁兜底"
         "（触点随设备消失，物理触摸回系统）。"),
"vt_ops_trigger_post": dict(brief="区域线程投一次触发（写触发槽 → release 自增 seq → 写唤醒 fd）。",
    params=[("name", "要起跑的操作名"), ("slot", "触发来源手指的物理槽号（日志用）"),
            ("td", "触发数据（mask/dx/dy/ux/uy/ms；spec §1.5）：先写各字段、最后 release 自增 seq 发布")],
    note="触发槽是**单槽覆盖**：主线程还没消费就被下一发盖掉时，tick 按 seq 差值记 `op 丢弃 覆盖`。"
         "写出次序：name/slot/触发数据全部先写、seq 最后 release 自增（消费端 acquire 读全）；"
         "名字按上限截断写；唤醒 fd 没建成（-1）时只丢这次唤醒 —— seq 还在，≤1s 的兜底 poll 会捡起。"),

# ---------------- §2.4 面板胶水（src-ui/ui_glue.c；该文件不在 C_FILES ⇒ 工具不注入，文案与手写文档块逐字同步） ----------------
"vtouch_op_count": dict(brief="操作表当前条数（面板列表用；只读区 A）。",
    ret="操作条数；没接共享内存时 0。"),
"vtouch_get_op": dict(brief="取一条操作的元信息（名字 / 步数 / 门控 / 自动关；只读区 A）。",
    params=[("i", "操作下标（0..count-1）"), ("name", "输出名字缓冲"), ("n", "名字缓冲容量"),
            ("steps", "输出步数（可为 NULL）"), ("gate", "输出门控区域 id 缓冲（可为 NULL）"),
            ("gn", "门控缓冲容量"), ("autoff", "输出跑完自动关（可为 NULL）")],
    ret="0 成功；-1 没接共享内存或下标越界。",
    note="任一出参可为 NULL（跳过不写）；字符串一律 snprintf 截断、保证 NUL 结尾。"),
"vtouch_get_op_step": dict(brief="取一条操作的某一步（类型 / 四个参数 / 时长；只读区 A）。",
    params=[("i", "操作下标"), ("s", "步下标（0..步数-1）"),
            ("type", "输出步骤类型 OP_STEP_*（可 NULL）"), ("a1", "输出参数 1（可 NULL）"),
            ("a2", "输出参数 2（可 NULL）"), ("a3", "输出参数 3（可 NULL）"),
            ("a4", "输出参数 4（可 NULL）"), ("ms", "输出时长毫秒（可 NULL）")],
    ret="0 成功；-1 没接共享内存或下标越界。",
    note="点按：a1,a2 = 坐标、ms = 按住时长；滑动：a1,a2 → a3,a4 = 起终点、ms = 时长；等待：只用 ms。"),
"vtouch_op_put": dict(brief="新增或覆盖一条操作（整条投编辑邮箱 → 回读校验；面板侧入口）。",
    params=[("name", "操作名（核心再校验：1..15、[A-Za-z0-9_-]；裸 `-` 除外）"), ("gate", "门控开关区域 id；NULL 或空串 = 无"),
            ("autoff", "跑完自动关门控（非 0 视为 1）"),
            ("steps6", "扁平步表：每 6 个 int 一组，顺序 type,a1,a2,a3,a4,ms"),
            ("nsteps", "步数（1..32；越界当场拒，不投）")],
    ret="0 核心已吃掉且回读通过（同名 + 步数一致）；-1 没接共享内存 / 超时 / 被核心拒（回读不通过）。",
    note="邮箱是单槽：投完等 edit_applied 到位才返回（正常 ~1ms），否则下一条编辑会把它盖掉；"
         "核心的校验是单点（名字 / 步数 / 坐标 / 时长），被拒时回读失败、面板走现有错误提示路径。"),
"vtouch_op_del": dict(brief="删除一条操作（面板侧入口）。",
    params=[("name", "操作名")],
    ret="0 表里已无同名条目（含本来就不存在）；-1 没接共享内存 / 超时。",
    note="回读校验 = 找不到同名条目（spec §2.6）。"),
"vtouch_op_clear": dict(brief="清空操作表（面板侧入口）。",
    note="回读校验 = op_count==0；未生效只打一行日志（void 返回，不阻塞面板）。"),
"vtouch_op_run": dict(brief="起跑一条操作（投编辑邮箱；核心忙 / 没空闲槽 / 帧内时按核心口径丢弃 + 日志）。",
    params=[("name", "操作名")],
    ret="0 核心已吃掉本次请求；-1 没接共享内存 / 超时。",
    note="**不做回读校验**（spec §2.6）：运行可能瞬间结束、状态已归位，回读判不了；要显示运行态请读 vtouch_op_status。"),
"vtouch_op_stop": dict(brief="中止运行中的操作（投编辑邮箱；没在跑时是空操作）。",
    note="与 run 同口径：投递成功即返回；核心侧幂等、收尾也走它。"),
"vtouch_op_status": dict(brief="读执行器运行状态（只读区 A）。",
    params=[("run_i", "输出运行中的操作下标（-1 = 空闲；可 NULL）"), ("run_step", "输出当前步（0 起；可 NULL）"),
            ("run_state", "输出 0=空闲 1=运行（可 NULL）")],
    ret="0 成功；-1 没接共享内存。",
    note="运行中删表可能短暂显示错名（预裁决接受）：显示层、下轮运行自愈。"),
"vtouch_pick_request": dict(brief="请求取点：置区 B pick_mode=1，核心吞一次触摸后回填坐标并自清。",
    note="核心侧两重防呆：20s 超时自清 + 面板死亡清理；本函数不叫醒核心（触摸按下本身会唤醒它）。"
         "同时置面板侧「取点态」（pick_armed）并把 take 基线推进到当前 pick_seq（T2.8；T2.4 递延①"
         "的等价防护：面板重启接旧核心时 pick_seq 可能非 0，不推基线的话点 [取点] 会在用户 tap 之前"
         "把**旧捕获**吐成 pick_ev —— 凭空回填旧坐标）。"),
"vtouch_pick_cancel": dict(brief="取消取点：清区 B pick_mode（面板内点击 = 取消）。",
    note="同时清面板侧「取点态」（pick_armed）：取消后不再 take。"),
"vtouch_pick_take": dict(brief="取走一次取点结果（对比 pick_seq 变化；1 = 有新坐标）。",
    params=[("x", "输出竖屏逻辑坐标 x（可 NULL）"), ("y", "输出竖屏逻辑坐标 y（可 NULL）")],
    ret="1 有新坐标（本次取走）；0 没有新结果。",
    note="基线 pick_last_seq 在 vtouch_pick_request 里推进到当时的 pick_seq（T2.8）——只回报**本次取点态"
         "之后**的捕获；同一次捕获只回报一次。读侧以 ACQUIRE 读 pick_seq（配写侧屏障：读到新 seq 必能"
         "读到配对坐标）。面板在 vtouch_poll_step 里轮询它，读到就合成 pick_ev。"),
"vtouch_region_kind_get": dict(brief="区域的开关型标记（0=普通 1=开关型；只读区 A）。",
    params=[("i", "区域下标")], ret="kind 值；-1 没接共享内存或下标越界。"),
"vtouch_region_toggle": dict(brief="开关型区域的当前开/关状态（核心写、面板只读）。",
    params=[("i", "区域下标")], ret="1 开 0 关；-1 没接共享内存或下标越界。",
    note="与 mark 是两套来源：面板样式画 mark || (kind==toggle && toggle_on)。"),
"vtouch_region_trig": dict(brief="区域的触发绑定（绑定的操作名 + 触发时机；只读区 A）。",
    params=[("i", "区域下标"), ("op", "输出操作名缓冲（可 NULL；未绑定 = 空串）"), ("n", "缓冲容量"),
            ("ev", "输出触发时机 0=无 1=按下 2=完整按压（可 NULL）")],
    ret="0 成功；-1 没接共享内存或下标越界。",
    note="绑定 / 开关型写入见 vtouch_region_bind / vtouch_region_kind（T3.3 已接线）；这里读的是核心区 A 里的现值（悬空引用照读）。"),
"vtouch_region_bind": dict(brief="把区域绑定到操作（投编辑邮箱 → 等 applied → 回读校验；面板侧入口）。",
    params=[("id", "区域名"), ("opname", "操作名；NULL / 空串 / \"-\" = 解除绑定（照区 B 邮箱契约）"),
            ("ev", "触发时机：1=按下 2=完整按压（解除时忽略 —— 核心会清 0）")],
    ret="0 核心已吃掉且回读通过；-1 没接共享内存 / 超时 / 被核心拒（回读不通过）。",
    note="回读口径：解除 = trig_op 空串且 trig_ev==0；绑定 = trig_op==opname 且 trig_ev==ev"
         "（悬空操作名照过 —— 核心允许悬空，触发时再解析）。邮箱单槽：投完等 edit_applied"
         "到位才返回（正常 ~1ms）；核心不给逐条回执，成没成以回读为准。"),
"vtouch_region_kind": dict(brief="设置区域开关型（投编辑邮箱 → 等 applied → 回读校验；面板侧入口）。",
    params=[("id", "区域名"), ("kind", "0=普通 1=开关型")],
    ret="0 核心已吃掉且回读通过；-1 没接共享内存 / 超时 / 被核心拒（回读不通过）。",
    note="回读口径 = 区域 kind 与请求一致；toggle_on 不动（核心口径：切回开关型沿用上次开关态）。"),
}
