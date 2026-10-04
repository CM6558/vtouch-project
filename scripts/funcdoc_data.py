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
         "步数 1..MAX_STEPS、类型 ∈ {点按,滑动,等待,按下,弹起,区域判断,开关判断,跳转,计算,找图,找色}、"
         "坐标字段（点按/滑动/按下/区域判断）0..逻辑尺寸-1 或负数编码引用（-9..-1：-1..-5 = 触发变量、-6..-9 = 结果槽）、"
         "时长字段（点按/滑动/等待）按类型分档（点按 0..60000 / 滑动 1..60000 / 等待 0..600000）"
         "或负数编码引用（同上）、计算步 a1 ∈ 1..4 且 expr 非空、过 vt_expr_check（其余字段/ref 必须空；不过拒 `表达式错: <原因>`）、"
         "其余类型的 expr 必须为空（防御：非空拒 `表达式错`；计算步与视觉步除外）、条件步两档位 a3/a4 ∈ 0..3（不成立侧/成立侧），"
         "档位 = 跳转时该侧目标（不成立侧 j2 / 成立侧 j1）∈ 0..步数（0 = 结束）、"
         "视觉步（找图/找色，v8）：ref = 模板名（找图，必填）/ 点集名（找色多点必填、单点必空）、"
         "expr = 区域名（空或 [A-Za-z0-9_-]、1..15；存在性不校验，运行时按 `区域不存在` 收场）、"
         "找图 a1 = 阈值 0..255、找色 a1 = 模式 0/1 且单点 a2 = (颜色<<8)|容差（按无符号解读、域 = 全部 32 位）"
         "/ 多点 a2 = 0、a3/a4 = 档位 0..3、ms = 0..60000（0 = 单次、>0 = 持续查找超时毫秒，T7.4）、跳转目标域同条件步、"
         "跳转步 a1 ∈ 0..步数、ref 长度 1..15 且过 vt_id_ok（存在性不校验，允许悬空）。"
         "拒绝打 `op 被拒 <名>: <原因>`、成功打 `op 编辑 put <名> 步数=N`；"
         "重名覆盖就地写（表位不变），要么整条生效、要么一点都不动。"),
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
"op_resolve": dict(brief="解析一个可变量字段：字面值原样出；负数编码查触发快照或结果槽。",
    params=[("v", "字段原值：字面值（≥0）或负数编码引用 -9..-1（-1..-5 = 触发变量、-6..-9 = 结果槽）"),
            ("lo", "结果槽取整后的夹取下界（坐标 0 / 时长按类型档；spec V5 §5.3）"),
            ("hi", "夹取上界（坐标 逻辑尺寸-1 / 时长按类型档）"),
            ("out", "成功时写入解析结果")],
    ret="0 成功；-1 失败（已按码中止：`变量无值` / `结果无值`）。",
    note="**静态**，只在执行器内用：v>=0 直接出；-1..-5 查 R.trig.mask 的对应位（OP_TRIGB_TDX << idx），"
         "未设即中止 `变量无值` —— **不静默当 0**（spec §1.3/D6）；-6..-9 查 R.slot_mask（未写即中止 `结果无值`），"
         "已写则 llround 取整后夹取 [lo,hi]（静默语义，spec V5 §5.3）；越界负值不会到达（op_valid 已拒，防御按无值中止）。"),
"op_release_held": dict(brief="收尾释放：还按着（held）就补一笔 up 并记 `op 收尾 松开`。",
    note="**静态**，只在执行器内用；正常完成（op_finish）与中止（vt_ops_abort）两条收尾路径共用 —— "
         "结束仍按着 → 自动松开（spec §2.2）。帧窗纪律：调用点都在帧关路径上（abort 撞帧窗走 "
         "stop_pending 推迟、帧关后才执行）—— 执行器任何路径不在帧窗内写 g.virt 的不变式不破；"
         "写失败忽略（槽已不在手里时无事可做，与 abort 抬指同款）。"),
"op_jump_apply": dict(brief="跳转收口（条件跳转 / 跳转步共用）：0 = 结束 → op_finish；否则过守卫后落位目标步骤。",
    params=[("target", "跳转目标：0 = 结束、1..步数 = 目标步骤")],
    note="**静态**，只在执行器内用。0 = 结束 → 正常完成（收尾释放 / 自动关照走），不占跳转计数、不判上限（spec §2.3）；"
         "否则跳转计数（R.jumps，起跑清零，条件跳转 + 跳转步共用一枚）+1，超过 VT_OPS_JUMP_MAX（200）"
         "→ 中止 `跳转超限`（防死循环）；通过 → R.step = 目标-2 → PH_WAIT（本动作单拍结束，"
         "下一拍 op_next_step 的正常推进 +1 精确落在目标步：R.step = 目标-1 —— 预置与「跳过下一步」同款）。"
         "VTOUCH_OPS_TRACE=1 时每跳一行 `op 跳转 <名> 第 A 步 → 第 B 步`（B=0 打 `→ 结束`；L9 默认零输出）。"),
"op_cond_apply": dict(brief="条件判定收口（两侧四档）：按侧记日志，再执行 继续 / 跳过 / 跳转 / 中止。",
    params=[("st", "当前条件 / 视觉步（读本侧档位与跳转目标）"), ("hit", "判定结果：1 = 成立、0 = 不成立"),
            ("word", "日志词：`区域判断` / `开关判断` / `找图` / `找色`"),
            ("arg", "日志第二词：条件步 / 找图 = 区域 / 模板名；找色 = NULL（不打印）"),
            ("no_word", "不成立侧中止词：条件步 = `条件不成立`；视觉步 = `未命中`")],
    note="**静态**，只在执行器内用（区域判断 / 开关判断 / 找图 / 找色共用：两侧四档一处实现防两处漂移）。"
         "档位与目标取本侧（成立侧 = a4/j1、不成立侧 = a3/j2）：继续（单拍结束，下一拍进下一步）；"
         "跳过（步序额外 +1：跳过的那一步不执行也不求值；越过末步 = 正常完成）；"
         "跳转（0 = 结束 → op_finish；其余交 op_jump_apply 过守卫后落位）；"
         "中止（不成立侧 = no_word 参数、成立侧 `条件中止`）。"
         "日志（spec §1.3 / VISION §8）：不成立侧恒打、成立侧档位 ≠ 继续才打；不成立行先于中止行。"
         "调用点都在 region_lock 之外（spec §3.2 锁纪律：持锁判定、解锁后记日志）。"),
# ---------------- 视觉步骤执行（vt_ops.c；spec VISION §6/§8） ----------------
"vis_exec_find": dict(brief="查找执行（找图 / 找色；op 视觉步与面板试查共用）：抓帧（或复用）→ 帧视图 → 区域换算 → 匹配；持续模式循环到命中或超时。",
    params=[("kind", "0 = 找图 / 1 = 找色单点 / 2 = 找色多点"),
            ("ref", "模板名（kind 0）/ 点集名（kind 2）；kind 1 忽略"),
            ("region", "区域名（空 = 全屏）"),
            ("a1", "找图 = 阈值 0..255；其余不用"),
            ("a2", "找色单点 = (颜色<<8)|容差；其余不用"),
            ("timeout_ms", "持续查找超时毫秒：0 = 单次（现状）；1..60000 = 循环抓帧查到命中或超时（T7.4）"),
            ("x,y", "输出：命中点**竖屏逻辑坐标**（仅命中有效）"),
            ("ms", "输出：单次 = 匹配耗时毫秒（只计匹配调用，不含抓帧等待 —— 保 op 路径 `耗时` 语义逐字不变）；持续 = 全程耗时（含抓帧等待；可 NULL）"),
            ("attempts", "输出：尝试次数（抓帧 + 匹配的轮数；单次 = 1；可 NULL）"),
            ("err", "输出：失败原因词（`无画面` / `视觉错` / `区域不存在` / `模板不存在`；命中 / 未命中置 NULL）")],
    ret="0 = 命中 / -1 = 未命中 / -2 = 失败（err 已置原因词，调用方中止或按码报错）。",
    note="**静态**，op 执行器与面板「试一下」共用（op 路径单次行为逐字不变：中止词 / 日志 / 结果槽口径照旧）。"
         "持续模式（T7.4）：循环 { 抓新帧（force=1，不复用缓存）→ 匹配 → 命中 break } 到 deadline；"
         "硬失败（无画面 / 视觉错 / 区域不存在 / 模板不存在）立即按 -2 收场，不等超时；超时按未命中（-1）返回。"
         "持续循环占着主循环：每轮手动 vt_shm_tick 喂核心心跳（面板心跳停滞 ≥3s 会自杀退出；单次 ≤1s 不越线）。"
         "链路（spec VISION §6.1）：抓帧失败 → `无画面`；内部错 → `视觉错`；区域名不存在 → `区域不存在`；"
         "模板 / 点集读不到 → `模板不存在`。命中点从帧坐标换算成竖屏逻辑坐标（vt_vis_frame_to_logic）；"
         "找图先读 .tmpl、方向不同先旋转模板；找色多点先读 .pts（单点 a2 = (颜色<<8)|容差）。"),
"op_vis_run": dict(brief="视觉步骤执行（找图 / 找色）：转调 vis_exec_find → 结果槽 + 四档分支。",
    params=[("st", "当前视觉步（type = OP_STEP_FINDIMAGE / OP_STEP_FINDCOLOR）")],
    note="**静态**，只在执行器内用；单拍完成（返回时 phase/deadline 已落，或已中止）。链路（spec §6.1）："
         "失败 → 按 vis_exec_find 的原因词中止（`无画面` / `视觉错` / `区域不存在` / `模板不存在`）；"
         "命中 → r1/r2 = 命中点**竖屏逻辑坐标** + 成立侧四档；未命中 → 不成立侧四档（中止词 `未命中`）。"
         "ms > 0 = 持续查找（T7.4）：vis_exec_find 循环抓帧查到命中或超时（超时 = 未命中）；"
         "日志（spec §8）：`vis 找图 <模板> 命中 x,y (耗时 <ms>)` / `未命中 (耗时 <ms>)`；`vis 找色 命中 x,y` / `未命中`；"
         "持续增量：`vis 找图 <模板> 持续 <ms> 命中 x,y（尝试 N 次 / 耗时 M ms）` / `… 未命中（超时 <ms>，尝试 N 次）`（找色同款去模板名）。"
         "按住期允许（纯读屏不碰手指，spec §6.2）。"),
"op_vis_capture": dict(brief="取帧：复用缓存（≤50ms / 帧未变 / 方向一致）或发抓帧请求并轮询等待完成。",
    params=[("fr", "输出：帧句柄（rgba 直指 shm 缓冲；调用方在使用期间保证不再发新请求）"),
            ("force", "1 = 强制抓新帧（持续查找用：复用缓存会原地空转同一画面）；0 = 允许复用（单次路径照旧）"),
            ("err", "输出：失败原因词（`无画面` / `视觉错`）")],
    ret="0 成功；-1 失败（err 已置原因词 —— 调用方中止或按码报错；本函数不再直接中止）。",
    note="**静态**，只在执行器内用。协议（spec §2.2/§3.1）：写 req_pending = ++请求序号 → 轮询等 "
         "req_seq == 该序号（usleep(1000)，总超时 VT_VIS_CAPTURE_TIMEOUT_MS）→ 校验 flags 无错 + 尺寸合法 → "
         "按 seqlock 读一次稳定帧（读 seq → 读字段 → 再读 seq；奇/变 → 重试至超时）。面板不在（ui_hb 冻结 ≥3s）→ "
         "立即 `无画面`；超时 / flags 报错 → `无画面`；头损坏 / 尺寸非法 → `视觉错`。"
         "TRACE（VTOUCH_OPS_TRACE=1）：`vis 抓帧 请求 → 完成 <ms>`。"),
"op_vis_region": dict(brief="区域换算：区域名 → 区域几何 → 逻辑矩形 → 帧矩形（空 = 全屏）。",
    params=[("name", "区域名（空 = 全屏；调用方保证 NUL 结尾）"), ("fr", "帧句柄（读 rot / w / h）"),
            ("rx,ry,rw,rh", "输出帧矩形（含端点；空 name = 全屏）")],
    ret="0 成功；-1 失败（区域名不存在 —— 调用方按 `区域不存在` 收场；本函数不再直接中止）。",
    note="**静态**，只在执行器内用。区域几何：矩形取两角归一（含端点）、圆取外接矩形（cx±r）；"
         "停用不影响（这里是「搜索范围」语义，不是判定）；锁纪律同条件步（持 region_lock 取几何、解锁后再换算）。"),
"op_vis_read_tmpl": dict(brief="读模板文件（.tmpl，小端）：\"VTM1\" + ver=1 + w/h + rot + res + 灰度。",
    params=[("name", "模板名（vt_id_ok 尺子；调用方已校验）"), ("gray", "输出：灰度缓冲（malloc；调用方 free）"),
            ("tw,th", "输出：模板尺寸"), ("trot", "输出：模板抓取方向（0..3）")],
    ret="0 成功；-1 文件问题（调用方中止 `模板不存在`）；-2 内存失败（调用方中止 `视觉错`）。",
    note="**静态**，只在执行器内用。目录 /data/local/vtouch-runtime/templates/（spec §5.1）；"
         "格式逐字照实施计划「模板/点集文件格式」（T2.1 读端）。"),
"op_vis_read_pts": dict(brief="读点集文件（.pts，小端）：\"VTP1\" + ver=1 + n + res + base_rgb + base_tol + n×{dx,dy,rgb,tol}。",
    params=[("name", "点集名"), ("base", "输出：基准色"), ("base_tol", "输出：基准容差（0..255）"),
            ("pts", "输出：参考点数组（容量 ≥ VT_VIS_PTS_MAX）"), ("n", "输出：参考点个数（1..16）")],
    ret="0 成功；-1 失败（缺文件 / 格式坏 —— 调用方中止 `模板不存在`）。",
    note="**静态**，只在执行器内用；n / 容差 / 偏移越界都按格式坏拒收（引擎的域校验兜底相同，"
         "这里先拒 = 明确的 `模板不存在`）。"),
"op_vis_rot_gray": dict(brief="灰度模板旋转（90° 数组变换；spec VISION §11-#7 模板方向对齐）。",
    params=[("src", "源灰度（sw×sh 紧凑）"), ("sw,sh", "源尺寸"),
            ("steps", "顺时针步数：1 = 90°、2 = 180°、3 = 270°（调用方保证 1..3）"),
            ("ow,oh", "输出：旋转后尺寸")],
    ret="新缓冲（malloc；调用方 free）；内存失败 NULL。",
    note="**静态**，只在执行器内用。旋转方向 = (模板 rot - 帧 rot) & 3 顺时针步（推导：帧→逻辑的映射是纯旋转；"
         "模板先回逻辑再进当前帧 —— 两段相消后净旋转 = 两方向差）。"),
"op_vis_panel_dead": dict(brief="「面板不在」判据：ui_hb 冻结 ≥3s（或从未有过心跳）= 面板不在。",
    ret="1 = 面板不在；0 = 心跳在动。",
    note="**静态**，只在执行器内用；判据同 vt_panel.c 看门狗（单调钟 3s，不是拍数；不扩 vt_panel 接口）。"
         "从未有过心跳（ui_hb == 0）直接算不在：面板没起来过，抓帧无从谈起 —— 视觉步立即 `无画面`，不空等。"),
"vis_test_err_code": dict(brief="试查失败原因词 → 结果错误码（词表同 op 中止词；面板按码显示）。",
    params=[("why", "失败原因词（vis_exec_find 输出；可 NULL = 防御）")],
    ret="结果错误码（VT_TEST_ERR_*；未知词按 `视觉错` 兜底）。"),
"vt_ops_test_poll": dict(brief="主循环每轮调：面板「试一下」请求 → 执行一次查找 → 写结果（x/y/err，release test_res_seq）。",
    note="**单请求在途**：面板写参数（参数先写、seq 最后 release），这里 acquire 读 test_req_seq 与本地 seen 比对；"
         "新请求 → 执行一次 vis_exec_find → 结果写回（命中 0 / 未命中 -1 / 其余 = 错误码，词表同 op 中止词）。"
         "**启动时 seen 对齐当前值**（首次调用）：跳过陈旧请求。防御（面板已预检）：kind 必须 0/1/2、"
         "找图 / 找色多点 ref 过 vt_id_ok 尺子 —— 真漏进来按词表收场，不硬撑。日志："
         "`vis 试查 找图 <模板> 命中 x,y (耗时 ms)` / `… 未命中` / `… 失败 <原因>`。"),
"vt_ops_run": dict(brief="起跑一条操作（忙时丢弃 + 日志）。",
    params=[("name", "操作名；表里查不到 / 没空闲槽 / 已有操作在跑 = 丢弃 + 对应日志"),
            ("td", "触发数据快照（mask/dx/dy/ux/uy/ms；spec §1.5）；NULL = 手动运行（全零 ⇒ 全部变量无值）")],
    note="起跑 = 整条快照进执行器私有内存（运行中改表 / 删表不影响本次，spec §7）+ 触发数据拷进 R.trig"
         "（起跑瞬间快照、运行中不回填，spec §1.2）+ 结果槽清零（值 + 未写标记，spec V5 §5.1）+ 挑第一个空闲虚拟槽"
         "（virt[]/staged[] 都空）全程占用，并写 g.op_run / op_run_step / op_run_state 供面板回显；"
         "日志 `op 启动 <名> 步数=N 槽=K 门控=<r1|无>`；VTOUCH_OPS_TRACE=1 时再一行 "
         "`op 变量 tdx=… tdy=… tux=… tuy=… tms=…`（未设字段打 `-`，值取快照）。"
         "门控 / 自动关（spec §4.3）：gate 非空先过门控检查 —— "
         "区域须存在、是开关型且开着，否则 `op 丢弃 门控拦截`；auto_off=1 在正常完成时把门控开关翻回关（中止不翻）。"),
"vt_ops_abort": dict(brief="中止运行中的操作（抬指 + 状态归位 + 日志原因）。",
    params=[("why", "中止原因（写进日志；如 停止按钮 / 引擎收尾 / reset/断连）")],
    note="**幂等**：没在跑（含没初始化、init 失败路径的 cleanup）就是空操作 —— cleanup() 无条件调它。"
         "抬指帧走 emit_frame：写失败置 g_reemit 等主循环重发；进程退出路径由随后 uinput 销毁兜底"
         "（触点随设备消失，物理触摸回系统）。按下步还按着（held）时收尾同样补一笔 up + `op 收尾 松开`"
         "（spec §2.2，走 op_release_held）。"),
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
"vtouch_get_op_step": dict(brief="取一条操作的某一步（类型 / 四个参数 / 时长 / 跳转目标 / 区域引用 / 表达式；只读区 A）。",
    params=[("i", "操作下标"), ("s", "步下标（0..步数-1）"),
            ("type", "输出步骤类型 OP_STEP_*（可 NULL）"), ("a1", "输出参数 1（可 NULL）"),
            ("a2", "输出参数 2（可 NULL）"), ("a3", "输出参数 3（可 NULL）"),
            ("a4", "输出参数 4（可 NULL）"), ("ms", "输出时长毫秒（可 NULL）"),
            ("ref", "输出区域引用缓冲（条件步的区域 id；可 NULL）"), ("refn", "区域引用缓冲容量"),
            ("j1", "输出成立侧跳转目标（条件步档位=跳转时有效；0 = 结束；可 NULL）"),
            ("j2", "输出不成立侧跳转目标（同 j1；可 NULL）"),
            ("expr", "输出表达式缓冲（计算步的表达式；可 NULL）"), ("exprn", "表达式缓冲容量")],
    ret="0 成功；-1 没接共享内存或下标越界。",
    note="点按：a1,a2 = 坐标、ms = 按住时长；滑动：a1,a2 → a3,a4 = 起终点、ms = 时长；等待：只用 ms；"
         "按下：a1,a2 = 坐标（按下并保持）；弹起：无字段；"
         "区域判断：a1,a2 = 判定点、a3 = 不成立档位、a4 = 成立档位（0=中止 1=跳过下一步 2=继续下一步 3=跳转）、"
         "j1 = 成立侧 / j2 = 不成立侧跳转目标（仅该侧档位=跳转时有意义；0 = 结束）、ref = 区域 id；"
         "开关判断：a3 = 不成立档位、a4 = 成立档位、j1/j2 同款、ref = 区域 id（须开关型）；"
         "跳转步：a1 = 目标步骤（0 = 结束）、其余字段忽略；计算步：a1 = 槽号 1..4、expr = 表达式。"
         "坐标 / 时长字段可为字面值或负数编码引用（-9..-1：-1..-5 = tdx/tdy/tux/tuy/tms、-6..-9 = r1..r4）。"
         "ref / expr 出参：写空串 = 无；空 / 未终止（防御）也写空串；n<=0 或指针 NULL 可省略；j1/j2 可 NULL。"),
"vtouch_op_put": dict(brief="新增或覆盖一条操作（整条投编辑邮箱 → 回读校验；面板侧入口）。",
    params=[("name", "操作名（核心再校验：1..15、[A-Za-z0-9_-]；裸 `-` 除外）"), ("gate", "门控开关区域 id；NULL 或空串 = 无"),
            ("autoff", "跑完自动关门控（非 0 视为 1）"),
            ("steps8", "扁平步表：每 8 个 int 一组，顺序 type,a1,a2,a3,a4,ms,j1,j2"),
            ("refs", "每步的区域引用表（条件步的 ref；可 NULL = 全空）；refs[i] 空串 = 第 i 步无引用"),
            ("exprs", "每步的表达式表（计算步的 expr；可 NULL = 全空）；exprs[i] 空串 = 第 i 步无表达式"),
            ("nsteps", "步数（1..32；越界当场拒，不投）"),
            ("out_err", "失败原因码（可 NULL）：0=成功；1=没接共享内存 / 载荷非法（未投递）；2=投递超时（未送达）；3=核心拒收（投递成功但回读不通过）")],
    ret="0 核心已吃掉且回读通过（同名 + 步数一致）；-1 失败（原因见 out_err）。",
    note="邮箱是单槽：投完等 edit_applied 到位才返回（正常 ~1ms），否则下一条编辑会把它盖掉；"
         "refs / exprs 逐步拷进 op.steps[i].ref / .expr（strnlen 防御照款：未终止按空串处理，同 vtouch_get_op_step 口径）；"
         "核心的校验是单点（名字 / 步数 / 类型 1..9 / 坐标 / 时长 / 变量编码 -9..-1 / 计算步 expr 过 vt_expr_check / 条件步 a3+a4+跳转目标 / 跳转步 a1），被拒时回读失败、面板走现有错误提示路径。"),
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
"vtouch_expr_check": dict(brief="校验计算步表达式（转发核心 vt_expr_check；面板 real 构建链核心源码，同一实现）。",
    params=[("s", "表达式文本（可 NULL / 空）"), ("why", "非法时写入短中文原因（可 NULL / 0 容）"), ("whycap", "why 缓冲长度")],
    ret="0 合法；-1 非法（why 已填原因）。",
    note="面板表达式子层的 [确定] 走它（spec V5 §4：不过 → 就地拒收、层不关）。"),
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
"vtouch_vis_test_post": dict(brief="发起一次试查（填参数 → release 写 test_req_seq → 写唤醒管道）。",
    params=[("kind", "0 = 找图 / 1 = 找色单点 / 2 = 找色多点"), ("ref", "模板名 / 点集名（找色单点不读）"),
            ("region", "区域名（空 = 全屏）"), ("a1", "找图 = 阈值 0..255 / 找色 = 模式"),
            ("a2", "找色单点 = (颜色<<8)|容差；其余 0")],
    ret="本次请求序号（≥1；与 take 的 seq 比对认领结果）；0 = 没接共享内存。",
    note="写唤醒管道 = 与编辑邮箱同款（glue_wake）：核心立刻醒，不等 poll 超时；没有唤醒 fd 时核心最坏 1s "
         "兜底轮询也会吃到。单请求在途：调用方（面板 UI）等待期间不重发。"),
"vtouch_vis_test_take": dict(brief="取一次试查结果（有新结果返回 1；结果序号给调用方认领）。",
    params=[("seq", "输出：结果序号（= 核心已应答的请求序号；可 NULL）"),
            ("x,y", "输出：命中点竖屏逻辑坐标（仅命中有效；可 NULL）"),
            ("err", "输出：0 = 命中 / -1 = 未命中 / 其余 = 错误码（VT_TEST_ERR_*；可 NULL）")],
    ret="1 有新结果（本次取走）；0 没有。",
    note="同一结果只回报一次；晚到的陈旧结果由调用方按 seq 比对丢弃。"),
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
# ---------------- 表达式引擎（vt_expr.c；spec OPS_PLAN_V5 §2） ----------------
"vt_expr_check": dict(brief="校验表达式是否合法（面板编辑期与核心拒收共用的唯一实现）。",
    params=[("s", "表达式文本（可 NULL / 空）"), ("why", "非法时写入短中文原因（可 NULL / 0 容）"), ("whycap", "why 缓冲长度")],
    ret="0 合法；-1 非法（why 已填原因；成功时 why 为空串）。",
    note="语法与语义逐字照 spec §2（递归下降；+ - * /；atan2/sin/cos/abs/min/max/sqrt（三角函数按度、atan2(0,0)=0）；"
         "小数；空白忽略；全小写精确匹配）。边界：长度 ≤ 63、括号嵌套 ≤ 8、token ≤ 128（词法口径：数字/标识符/运算符/括号/逗号各 1 个；"
         "token 上限先于长度检查，三条边界都可触发、各有 why）。静态常量折叠：变量/槽视为未知值并传播 —— "
         "「除零 / 负数开方 / 结果非有限」只在完全由常量决定时拦下（不会误拒 `1/tdx` 这类），运行期由 vt_expr_eval 同一套判定兜底。"),
"vt_expr_eval": dict(brief="求值表达式：触发数据 / 结果槽 / 字面量参与运算，返回 double。",
    params=[("s", "表达式文本"), ("trig_vals", "触发数据 [tdx,tdy,tux,tuy,tms]（可 NULL = 全无值）"),
            ("trig_mask", "位 0..4 同序，1=有值"), ("slots", "结果槽 [r1..r4]（可 NULL = 全未写）"),
            ("slot_mask", "位 0..3，1=已写"), ("out", "输出（仅返回 OK 时有意义；出错不写）")],
    ret="VT_EXPR_OK；VT_EXPR_NO_VAR（变量无值）/ VT_EXPR_NO_SLOT（结果无值）/ VT_EXPR_BAD（表达式错）。",
    note="与 check 同一套解析与判定：语法错、除零、负数开方、结果非有限（inf/NaN）、超限都返回 BAD；"
         "引用 mask 缺位的触发数据 / 未写的槽返回 NO_VAR / NO_SLOT（按求值顺序，先遇到先报）。"),

# ---------------- 视觉引擎（vt_vision.c；spec VISION_PLAN §3.3/§4） ----------------
# 灰度公式单一来源（vt_vision.h 的 static inline；引擎与面板写端共用；T3.2 提取）：
"vt_vis_gray_px": dict(brief="灰度公式（唯一来源）：(77r+150g+29b+128)>>8。",
    params=[("r,g,b", "像素通道值（0..255）")],
    ret="8bit 灰度（0..255）。",
    note="引擎（vt_vision.c 标量/NEON 路径）与面板写端（.tmpl 存灰度）共用这一处定义；"
         "NEON 路径复用同组权重常量（逐 lane 同算式）；改公式只改这里（实施计划「灰度公式单一来源」）。"),
"vt_vis_frame_prepare": dict(brief="准备帧视图（静态单例）：RGBA→灰度 + 1/2、1/4 金字塔。",
    params=[("rgba", "帧 RGBA8888（stride 字节/行；调用方保证在 release 前有效）"), ("w", "帧宽（像素）"),
            ("h", "帧高（像素）"), ("stride", "行跨距（字节；≥ w×4）")],
    ret="VT_VIS_OK；VT_VIS_BAD（参数非法 / 尺寸超上限）。",
    note="主循环单线程独占（不可并发调用）；rgba 缓冲在 release 前须保持有效（引擎只读、不拷贝）。"
         "灰度公式 (77r+150g+29b+128)>>8（对 Rec.601 加权真值误差 ≤1）；金字塔 = 四舍五入盒式平均。"
         "尺寸上限 4096×4096。失败会先把视图置为无效（不保留上一帧）。"),
"vt_vis_frame_release": dict(brief="释放帧视图（标记失效；静态缓冲不释放）。",
    note="release 后一切匹配返回 VT_VIS_BAD，直到下一次 prepare。"),
"vt_vis_find_color": dict(brief="找色：区域内行优先找首个 per-channel 色差 ≤ tol 的像素。",
    params=[("rx,ry,rw,rh", "搜索区域（帧坐标；与帧求交后使用）"), ("rgb", "目标色（低 24 位有效）"),
            ("tol", "容差（0..255）"), ("ox,oy", "命中输出（帧坐标；未命中不写）")],
    ret="VT_VIS_OK（命中）；VT_VIS_NO_MATCH；VT_VIS_BAD（未准备 / 参数非法）。",
    note="只比 R/G/B（忽略 alpha）；区域先与帧求交（越界部分不算、交集空 → 未命中）；命中即停。"),
"vt_vis_find_color_multi": dict(brief="多点找色：基准色命中后逐点校验偏移参考点，全对为命中（n ≤ 16）。",
    params=[("rx,ry,rw,rh", "基准搜索区域（帧坐标；与帧求交后使用）"), ("base", "基准色（低 24 位有效）"),
            ("base_tol", "基准容差（0..255）"), ("pts", "参考点数组（相对基准的偏移 + 目标色 + 容差）"),
            ("n", "参考点个数（1..16）"), ("ox,oy", "命中输出（基准坐标；未命中不写）")],
    ret="VT_VIS_OK（命中）；VT_VIS_NO_MATCH；VT_VIS_BAD（未准备 / 参数非法）。",
    note="参考点位置 = 基准 + 偏移，须落在帧内（越界 = 该候选不成立）；每点各自容差（0..255）；"
         "dx/dy 限 ±4096；区域只限定基准的搜索范围（参考点可越出区域）。"),
"vt_vis_find_image": dict(brief="找图：灰度模板匹配，命中判据 = 平均绝对差 ≤ thresh。",
    params=[("rx,ry,rw,rh", "搜索区域（帧坐标；与帧求交后使用）"), ("tmpl", "模板灰度（tw×th 紧凑）"),
            ("tw,th", "模板尺寸"), ("thresh", "阈值（0..255；平均绝对差上限）"),
            ("ox,oy", "命中输出（帧坐标；未命中不写）")],
    ret="VT_VIS_OK（命中）；VT_VIS_NO_MATCH；VT_VIS_BAD（未准备 / 参数非法）。",
    note="比较用 64 位累加：SAD ≤ thresh×tw×th（含等号）。模板两边都 ≥ 8 走金字塔（1/4 粗筛 → 1/2 定位 → "
         "全分辨率精修），任一边 < 8 走全分辨率直搜；SAD 行级早退；命中即停。金字塔为启发式（细小纹理 / 极限边缘"
         "允许漏检；不命中不回退直搜）。"),
"vt_vis_frame_to_logic": dict(brief="帧坐标（当前方向）→ 竖屏逻辑坐标（单点）。",
    params=[("rotation", "0..3（r 语义同 Android getRotation）"),
            ("fw,fh", "帧尺寸（当前方向）"), ("fx,fy", "帧坐标"),
            ("lx,ly", "输出逻辑坐标（可 NULL = 跳过）")],
    note="约定与面板 p2c/c2p 逐字同一套；rotation 越界按 r&3 归一。"),
"vt_vis_logic_rect_to_frame": dict(brief="竖屏逻辑矩形 → 帧坐标矩形（区域限定换算用）。",
    params=[("rotation", "0..3"), ("fw,fh", "帧尺寸（当前方向）"),
            ("lx,ly,lw,lh", "逻辑矩形（含端点，lw×lh 个像素）"),
            ("fx,fy,fw2,fh2", "输出帧矩形（含端点；可 NULL = 跳过）")],
    note="矩形含端点（lw×lh 个像素）；四角映射后取 min/max；输出矩形同样含端点。"),
"vis_gray_scalar": dict(brief="灰度转换（标量版）：逐像素 (77r+150g+29b+128)>>8。",
    params=[("dst", "输出灰度（w×h 紧凑）"), ("rgba", "输入帧（stride 字节/行，RGBA8888）"),
            ("w", "宽（像素）"), ("h", "高（像素）"), ("stride", "行跨距（字节）")],
    note="算式 = vt_vision.h 的 vt_vis_gray_px（唯一来源）；NEON 版共用同组权重常量（逐像素同算式）。"),
"vis_gray_neon": dict(brief="灰度转换（NEON 版）：vld4q_u8 解交织 + 16 位加权，16 像素/次。",
    params=[("dst", "输出灰度（w×h 紧凑）"), ("rgba", "输入帧（stride 字节/行，RGBA8888）"),
            ("w", "宽（像素）"), ("h", "高（像素）"), ("stride", "行跨距（字节）")],
    note="16 位通道和上限 65408 不溢出；尾部标量；逐 lane 与标量版同一算式（权重同 vt_vis_gray_px）。"),
"vis_down2": dict(brief="1/2 盒式下采样（四舍五入）：dst[Y][X] = (a+b+c+d+2)>>2。",
    params=[("dst", "输出（dw×dh）"), ("dw", "输出宽"), ("dh", "输出高"),
            ("src", "输入（行跨距 sw）"), ("sw", "输入行跨距（元素数）")],
    note="帧与模板金字塔共用；尺寸向下取整（奇数边最后一行/列不参与）。"),
"vis_px_match": dict(brief="单像素颜色匹配：per-channel max-diff ≤ tol（只比 R/G/B）。",
    params=[("p", "像素指针（RGBA8888）"), ("rgb", "目标色（低 24 位有效）"), ("tol", "容差（0..255）")],
    ret="1 匹配；0 不匹配。"),
"vis_color_row_scalar": dict(brief="在一行 [x0..x1] 内找首个匹配像素（标量版）。",
    params=[("row", "行首（RGBA8888）"), ("x0", "起始 x（含）"), ("x1", "结束 x（含）"),
            ("rgb", "目标色（低 24 位有效）"), ("tol", "容差（0..255）")],
    ret="命中 x；-1 = 无。"),
"vis_color_row_neon": dict(brief="在一行 [x0..x1] 内找首个匹配像素（NEON 版）：vabdq_u8 块筛 + 块内逐 lane 定位。",
    params=[("row", "行首（RGBA8888）"), ("x0", "起始 x（含）"), ("x1", "结束 x（含）"),
            ("rgb", "目标色（低 24 位有效）"), ("tol", "容差（0..255）")],
    ret="命中 x；-1 = 无。",
    note="整块无命中直接跳过（vminvq_u8）；有命中时块内定位首个；尾部标量；与标量版同序同结果。"),
"vis_pts_ok": dict(brief="多点参考点校验：全部点在帧内且各自色差 ≤ 自身容差 → 1。",
    params=[("rgba", "帧 RGBA 指针"), ("w", "帧宽"), ("h", "帧高"), ("stride", "帧行跨距（字节）"),
            ("bx", "基准命中 x"), ("by", "基准命中 y"), ("pts", "参考点数组"), ("n", "参考点个数")],
    ret="1 全对；0 有越界或不匹配。"),
"vis_clip_span": dict(brief="把 [v0, v0+n-1] 与 [0, lim-1] 求交（区域限定先与帧求交的统一入口）。",
    params=[("lo", "输出交集下界"), ("hi", "输出交集上界"), ("v0", "区间起点"),
            ("n", "区间长度（像素数）"), ("lim", "帧边长（像素数）")],
    ret="1 有交集（lo/hi 已写）；0 交集空。"),
"vis_row_absdiff_scalar": dict(brief="一行 |帧-模板| 绝对差之和（标量版，u32）。",
    params=[("f", "帧行首"), ("t", "模板行首"), ("n", "本行像素数")],
    ret="该行绝对差之和（u32；单行上限 255×4096 不溢出）。"),
"vis_row_absdiff_neon": dict(brief="一行 |帧-模板| 绝对差之和（NEON 版）：vabdq_u8 + vpaddl 两级升位累加。",
    params=[("f", "帧行首"), ("t", "模板行首"), ("n", "本行像素数")],
    ret="该行绝对差之和（u32；单行上限 255×4096 不溢出）。",
    note="16 像素/次、u32 通道防溢出、尾部标量；与标量版同值。"),
"vis_sad_ok": dict(brief="模板匹配判定：全模板 SAD ≤ limit → 1（64 位累加、行级早退 SSDA）。",
    params=[("f", "帧（灰度）基址"), ("fw", "帧行跨距（像素）"), ("x", "模板锚点 x"), ("y", "模板锚点 y"),
            ("t", "模板基址（中心裁剪视图可用）"), ("tstride", "模板行跨距（像素）"),
            ("tw", "模板宽"), ("th", "模板高"), ("limit", "判定上限（SAD ≤ limit 即命中）")],
    ret="1 命中（SAD ≤ limit）；0 超过。",
    note="早退只影响速度：提前返回必 > limit，判定与完整累加一致；模板行跨距由 tstride 给（中心裁剪视图用）。"),
"vis_image_direct": dict(brief="全分辨率直搜：行优先、步进 1、命中即停（模板 < 8 的回退路径）。",
    params=[("x0,x1", "有效锚点 x 范围（含）"), ("y0,y1", "有效锚点 y 范围（含）"),
            ("tmpl", "模板灰度（tw×th 紧凑）"), ("tw,th", "模板尺寸"), ("limit", "判定上限（SAD ≤ limit）"),
            ("ox,oy", "命中输出（帧坐标）")],
    ret="VT_VIS_OK（ox/oy 已写）/ VT_VIS_NO_MATCH。"),
"vis_image_pyramid": dict(brief="金字塔路径：1/4 粗筛 → 1/2 定位 → 全分辨率精修（精确 SAD 复核）。",
    params=[("x0,x1", "有效锚点 x 范围（含）"), ("y0,y1", "有效锚点 y 范围（含）"),
            ("tmpl", "模板灰度（tw×th 紧凑；调用方保证 tw、th ≥ 8）"), ("tw,th", "模板尺寸"),
            ("limit", "判定上限（SAD ≤ limit；精确复核用）"), ("thresh", "阈值（粗筛阈值 = thresh + 余量）"),
            ("ox,oy", "命中输出（帧坐标）")],
    ret="VT_VIS_OK（ox/oy 已写）/ VT_VIS_NO_MATCH。",
    note="粗筛用中心裁剪模板（去首行/首列粗像素）——对 ≥8×8 平坦图案，无论对齐与否真命中所在胞必过筛"
         "（推导见报告）；精修按全局行优先、命中即停；对细小纹理 / 部分重叠位允许漏检（启发式，报告披露）。"),

# ---------------- 共享内存帧区（vt_shm.c；spec VISION §3.1） ----------------
"vt_shm_frame": dict(brief="帧区头指针（核心读 / 面板写；区 D 布局见 vt_shm.h 与 spec VISION §3.1）。",
    ret="帧区头；未建 / 未附着时 NULL。"),
"vt_shm_frame_buf": dict(brief="第 idx 块帧缓冲基址（idx = 0/1；双缓冲，spec VISION §3.1）。",
    params=[("idx", "缓冲下标：0 / 1")],
    ret="缓冲基址；越界或未附着时 NULL。"),
"vt_shm_panel_rot": dict(brief="面板上报的当前显示方向（0..3；视觉帧复用/换算校验用）。",
    ret="当前方向；-1 = 拿不到（没建共享内存）。",
    note="面板每帧经 publish_rect 上报（g_rot）；与帧头里的 rotation（抓帧时方向）是两回事。"),
"vt_shm_ui_test_post": dict(brief="面板发起一次试查：填参数 → **最后** release 写 test_req_seq（序号 = 当前值 + 1）。",
    params=[("kind", "0 = 找图 / 1 = 找色单点 / 2 = 找色多点"),
            ("ref", "模板名 / 点集名（找色单点不读；按 test_ref 上限截断）"),
            ("region", "区域名（空 = 全屏；按 test_region 上限截断）"),
            ("a1", "找图 = 阈值 0..255 / 找色 = 模式 0/1"),
            ("a2", "找色单点 = (颜色<<8)|容差；其余 0")],
    ret="本次请求序号（≥1）；0 = 帧区没附着（没接核心）。",
    note="内存序照区 D 口径（写端）：参数先写、seq 最后 release 存 —— 核心侧 acquire 读 test_req_seq 后一定能"
         "看到全部参数。单请求在途由调用方（面板 UI）保证（等待期间不重发）。"),
"vt_shm_ui_test_take": dict(brief="面板取一次试查结果（acquire 读 test_res_seq vs 本地 seen；有新结果返回 1）。",
    params=[("seq", "输出：结果序号（= 核心已应答的请求序号；调用方与自己的在途序号比对认领；可 NULL）"),
            ("x,y", "输出：命中点**竖屏逻辑坐标**（仅命中有效；可 NULL）"),
            ("err", "输出：0 = 命中 / -1 = 未命中 / 其余 = 错误码（VT_TEST_ERR_*；可 NULL）")],
    ret="1 有新结果（本次取走）；0 没有。",
    note="同一次结果只回报一次（内部 seen）；面板重启后 seen 归零，会把当前结果当「新」报一次 —— 调用方"
         "（在途序号比对）自行丢弃陈旧结果。读侧以 acquire 读 res_seq（配核心侧 release 写：读到新 res_seq"
         "必能读到配对 x/y/err）。"),
}
