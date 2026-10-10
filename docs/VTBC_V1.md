# VTBC_V1 —— vt-script 字节码容器格式 v1（T2.1 定稿）

> 状态：v1 定稿（2026-10-10，T2.1）。上位：`VT_SCRIPT_V1_SPEC.md`（语言规范）。
> 一个操作 = 一个 VTBC blob（≤32KB）；编辑器（Java）编译产出，核心 VM（C）执行；字节序一律 **LE**。
> 实现：编辑器侧 `VtBc.java`（写/读/结构自检）；核心侧验证器在 T3.4（指令级）。两者对本文件负责。

## 1. 总体布局（固定顺序）

| 偏移 | 节 | 说明 |
|---|---|---|
| 0 | magic | `VTBC`（4B，ASCII） |
| 4 | version | u16 = 1 |
| 6 | flags | u16 = 0（保留） |
| 8 | srcHash | u32（源码 UTF-8 的 FNV-1a；源↔字节码对账：热载/迁移/缓存键） |
| 12 | totalLen | u32（= blob 实际字节数；末尾自检） |
| 16 | codeLen | u32；紧随其后 code[codeLen] |
| — | constCount | u32；constCount × const |
| — | slotCount | u32（全局槽数，**含预定义 7 个**；≥7） |
| — | mainLocals | u32（主程序帧局部槽数，含计数/控制槽） |
| — | tableCount | u32；tableCount × u32 len（1..4096；表索引 = 定义顺序） |
| — | fnCount | u32；fnCount × { entry u32, locals u32, nargs u8 } |
| — | lineCount | u32；lineCount × { off u32, line u32 }（off 升序） |
| — | nameCount | u32；nameCount × { kind u8, index u32, nameConst u32 } |

- const := `kind u8`（0 = num: f64 8B；1 = str: u32 字节长 + UTF-8）+ 载荷。池索引 = 出现顺序。
- name：kind 0=变量（index=槽号）/ 1=表（index=表序号）/ 2=函数（index=函数序号）；nameConst = 池内**字符串**常量索引（调试/反汇编/双视图用，运行期可不读）。
- 无字面量程序也须含数值常量 0 与 1（短路/真假编译结果用）。

## 2. 槽位与帧模型

- **全局槽** 0..slotCount-1。0..6 固定为预定义变量：`0 tdx 1 tdy 2 tux 3 tuy 4 tms 5 fx 6 fy`；用户全局从 7 起（编译期分配）。
- **帧**：主程序 = 帧 0；每次 CALL 开一帧。帧局部槽 = `slotCount + localIdx`：
  参数（前 nargs 个）→ for 计数变量 → 循环控制槽（计数器/轮/时长起点）→ 其余临时（编译器分配，main 用 mainLocals，函数用各自 locals）。
- 变量值 ∈ {数值 double | 字符串 | **空**（未写）}；读空槽 → 运行期中止 `变量无值`；空值参与运算/条件 → `结果无值`。
- **表** = 独立槽空间；起跑全 0；读写越界 → `下标越界`（带行号）。起跑：全局槽全空、表全 0、局部槽随帧建立时清零。
- 函数调用深度 ≤32（超出 → `调用超深`）；递归允许。

## 3. 指令编码

- 每条 = `u8 opcode` + 定长操作数（u8/u32，LE；跳转目标 = 代码**字节偏移**，绝对）。
- 表达式 = 栈机；所有语句/表达式按后序发出；语句位调用的结果用 `POP` 弃掉。
- 代码末尾自然结束 = 主程序正常完成（同 `终止` 完成收工，收尾释放/自动关照走）。

### 3.1 指令表（v1）

| op | 助记 | 操作数 | 栈效应 / 语义 |
|---|---|---|---|
| 0x01 | KN | u32 n | push 数值常量[n] |
| 0x02 | KS | u32 n | push 字符串常量[n] |
| 0x03 | LV | u32 slot | push 变量（空 → `变量无值`） |
| 0x04 | SV | u32 slot | pop → 变量（pop 空 = 槽变空） |
| 0x05 | LT | u32 t | pop 下标 → push 表项（越界 → `下标越界`） |
| 0x06 | ST | u32 t | pop 值、pop 下标 → 写表 |
| 0x10 | ADD | — | 任一侧为串 → 拼接（数转串：≤6 位小数）；否则数值加 |
| 0x11 | SUB | — | 数值减（串 → `类型错`） |
| 0x12 | MUL | — | 数值乘 |
| 0x13 | DIV | — | 数值除（除零 → `类型错`? 见 §4 对齐注） |
| 0x14 | MOD | — | 数值取模 |
| 0x15 | NEG | — | 一元负 |
| 0x16 | NOT | — | 非零→0，否则→1（串 → `类型错`） |
| 0x17 | CMP | u8 o | pop b、pop a → push (a o b ? 1 : 0)；o: 0== 1!= 2< 3> 4<= 5>=；`==`/`!=` 任一侧为串 → 串比较，其余遇串 → `类型错` |
| 0x21 | POP | — | 弃栈顶 |
| 0x30 | JMP | u32 off | 无条件跳 |
| 0x31 | JF | u32 off | pop；假（0）→ 跳；串 → `类型错` |
| 0x32 | JT | u32 off | pop；真（非 0）→ 跳；串 → `类型错` |
| 0x33 | CJMP | u32 off | 兼容跳转：守卫计数（全程 >200 → 中止 `跳转超限`）后跳 |
| 0x40 | CALL | u32 fn, u8 nargs | 弹 nargs 开帧 → 跳 fn.entry；返回后 push 返回值（无值 = 空） |
| 0x41 | RET | — | pop 值 → 返回调用方 |
| 0x42 | RETE | — | 无值返回（调用方得空） |
| 0x50 | TAP | — | pop ms、y、x → 点按 |
| 0x51 | SWIPE | — | pop 停2、停1、时长、y2、x2、y1、x1 → 滑动 |
| 0x52 | WAIT | — | pop ms → 等待 |
| 0x53 | DOWN | — | pop y、x → 按下并保持 |
| 0x54 | UP | — | 弹起（无按住 → 中止 `未按下`） |
| 0x55 | TOAST | — | pop 时长、msg → 提示 |
| 0x56 | HL | u32 regionStr | pop 时长 → 框区域 |
| 0x57 | BOX | — | pop 时长、y2、x2、y1、x1 → 框矩形 |
| 0x58 | FINDIMG | u32 tplStr, u32 regionStr | pop 持续、色校、阈值 → push 1/0；命中写 fx/fy（槽 5/6） |
| 0x59 | FINDCOLOR | u32 setStr, u32 regionStr | pop …… → push 1/0（多点找色；命中写 fx/fy） |
| 0x5A | FINDDOT | u32 regionStr | pop 容差、色值 → push 1/0 |
| 0x5B | INREGION | u32 regionStr | pop y、x → push 1/0 |
| 0x5C | SWON | u32 nameStr | push 1/0（非开关型 → 中止 `非开关型`） |
| 0x5D | TERM | u8 mode | mode 0 = 完成收工 / 1 = 中止 |
| 0x60 | NOW | — | push 当前时刻（ms；时长循环用） |
| 0x70 | LEN | — | pop → push 串长 |
| 0x71 | STRN | — | pop → push 串（数值转串，≤6 位小数） |
| 0x72 | NUM | — | pop → push 数值（不可转 → `类型错`） |
| 0x73..0x7D | RAND FLOOR CEIL ROUND ABS MIN MAX SQRT SIN COS ATAN2 | — | 内建（三角按度；rand 含端点整数；min/max 各弹 2） |

`regionStr = 0xFFFFFFFF` 表示"无区域/全屏"。预定义变量、`真/假`（= KN 1/0）、`轮`（= LV 帧内轮槽）无需专门指令。

## 4. 编译映射要点（规范 → 字节码）

- `&&`/`||`：短路用跳转编译（左侧不求值的短路侧不执行）；结果恒 1/0；`!` = NOT。
- 字符串插值：编译为 ADD 串接链（`str()` 对数值段自动生效）。
- for（`循环 i 从 a 到 b`）：先评 a、b 入 i/限位槽；顶判 `i ≤ 限位`；体力行后 i+1；**正常跑完 i 归位 = 限位值**（示例 2 `{i}` 口径）；跳出 → 跳出时值；轮 同帧递增（0 起）。
- 循环·计数：`n = floor(n)`，≤0 不跑；循环·时长：顶判 `now - 起点 < ms`；循环·无限 / `当` / `直到`（首判；直到 = 取反）。
- `跳出/继续 层数`：编译期解析到目标循环的 end/continue 标签；continue = 增量段前。
- 兼容 `跳转 第N行` → CJMP（目标 = 该行首条语句的偏移）；跳转目标不许进/跨块（编译期已查）。
- 找图 命中写 fx/fy；**未命中行为（保留旧值/清空）与核心对齐 → T3 对拍时定稿**。找色点 色值为数值；找色/找图 的 区域/模板 必须是常量（编译期入池）。
- 默认值（缺省编译为常量入栈）：点按 ms 50、滑动 时长 300/停1 0/停2 0、提示 2000、框区域 3000、框矩形 3000、找图 阈值 8/色校 0/持续 0、找色点 容差 8。
- 按下/弹起/收尾释放语义 = 复用现核心持有态逻辑（T3.2 抽取）。

## 5. 校验分层

- **写端（编译期）**：totalLen ≤32768（超限 `程序过大`）；nameConst 必指字符串常量；fn.entry ≤ codeLen；表长 1..4096；各表（line 升序等）自洽。
- **读端（结构）**：magic/version/flags；totalLen =实际长；各节长度不越界；const kind ∈ {0,1}；UTF-8 合法；slotCount ≥7；跳转/entry 界（粗查）。
- **核心验证器（T3.4，指令级）**：逐指令 跳转落点合法 / 栈深有界配对 / 常量·槽·表引用界 / 指令数上限 → 违者整条操作拒收 `程序非法`。

## 6. 版本历史

- v1（2026-10-10）：首版（随 T2.1 定稿）。
