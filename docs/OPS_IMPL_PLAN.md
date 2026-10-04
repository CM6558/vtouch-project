# 操作编辑器 + 核心执行器 · 实施计划

> 注（2026-10-05）：旧 JS 客户端通道已全退役；文中 `clients/*`、`pack_client.py`、`vtouch_onefile.js` 等引用均为历史记录（su 模板现位于 `scripts/vtouch.sh.in`）。

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 给 vtouch 加「面板内操作编辑器 + 核心主线程执行器 + 区域/开关触发侧 + su 脚本入口」——启动与操作用作不再依赖旧 JS 客户端（该通道后于 2026-10-05 全退役）。

**Architecture:** 操作表进共享内存区 A（`g.ops[]`，面板只读直读）；编辑走区 B 编辑邮箱（新码 5–11 + 操作载荷）；执行器在主循环（`vt_ops_tick()` + poll 第四档 deadline + 触发 eventfd）；面板加「操作」页 + 取点；su 脚本自包含单文件。

**Tech Stack:** C（NDK r27d 交叉编译，`-O2 -Wall -Wextra -Werror -D_GNU_SOURCE`）、C++/ImGui v1.91.8 面板（`src-ui/`）、toybox sh、Python 3 工具脚本。设备：OnePlus PJZ110（ColorOS / Android 16，KernelSU）。

**Spec:** `docs/OPS_PLAN.md`（设计定稿）。执行时**两份一起读**；本计划写"怎么做"，契约细节（字段/格式/日志行/测试清单）以 spec 为准。

## Global Constraints

- 编译：`src/*.c` 通配一起链（`scripts/build.sh:51` / `:58`），新文件 `src/vt_ops.c` 无需改构建脚本。
- **默认（无 UI）构建必须保持可编译**：所有新增核心代码放 `#ifdef VT_UI` 分支内（`vt_ops.c` 整文件同 `vt_shm.c` 的守卫写法）。
- 告警零容忍：`-Wall -Wextra -Werror`；单文件秒查 `sh scripts/check_syntax.sh <文件>`。
- 函数文档唯一来源 `scripts/funcdoc_data.py`：**每个新函数的文案先写进库**，再 `python scripts/apply_funcdoc.py`；`--check` 必须 **0 处**。
- Shell：`#!/system/bin/sh`（toybox），不用 bashism；**产物**（模板替换后的形态）必须 `sh -n` 通过——模板本身含占位符、按定义不可直接解析，不设模板解析门。产物必须 LF，且 LF 断言用**字节判据**（`python -c "…count(b'\x0d')"`），不用 `grep -c $'\r'`（在 MSYS 空转）。
- 契约：**WS 协议零改动**；不加线程、不加命令通道；坐标一律**竖屏逻辑坐标**；`VT_SHM_VERSION` 3→4 **在批 2 一次落全部布局**（spec §2.5），批 3 只加逻辑/UI。
- 提交：中文信息写到 `build/_msg_<x>.txt`（UTF-8）后 `git commit -F`；**每任务一提交、不自动 push**。
- 每批开工前：**全量备份 + 恢复点**（house 规矩：备份到 `build/_backup_full_<TS>/`，含 `git status` / `git diff --binary` 快照与未跟踪文件清单）。
- 测试口径（用户定）：**不前置真机探测、实现阶段不接设备**；日志按 spec §8 随实现埋好；真机测试集中到 spec §9（阶段 4 只是入口索引）。
- 本计划所有函数名/字段名/常量名**逐字使用**（后续任务按名字互认，别改拼写）。

---

## 文件结构（新增/修改地图）

| 文件 | 动作 | 职责 |
|---|---|---|
| `clients/vtouch.sh` | 新建 | su 入口模板（可读源码 + 占位符） |
| `scripts/pack_su.py` | 新建 | 打包器：模板 + `build/vtouchd_ui` → `build/vtouch.sh` |
| `src/vt_shm.h` | 修改 | ver 4；邮箱载荷 + 码 5–11；`vt_shm_b` pick 字段 |
| `src/vt_internal.h` | 修改 | `vt_step/vt_op`；`vt_state` 新字段；区域扩展；原型 |
| `src/vt_ops.c` | 新建 | 执行器 + 校验 + 触发槽 + 日志（整文件 `#ifdef VT_UI`） |
| `src/vt_shm.c` | 修改 | `edit_apply` 分派新码；pick 访问器（两侧） |
| `src/vt_region.c` | 修改 | `id_ok` 提取为 `vt_id_ok`；BIND/KIND 落地；触发判定；开关翻转 |
| `src/vt_frame.c` | 修改 | 取点吞（并入"面板吞掉"判定处） |
| `src/vtouchd.c` | 修改 | `vt_ops_tick()`；poll 第四档 + `p[5]`；`vt_ops_init()`；cleanup 先中止 |
| `src/vt_panel.c` | 修改 | 面板死亡分支清 `pick_mode` |
| `src-ui/ui_glue.c` | 修改 | 新面板 API（操作/绑定/取点/状态） |
| `src-ui/vtouch_ui.cpp` | 修改 | 操作页 + 编辑层 + 数字键盘 + 取点 UI + 区域卡片行 + 落盘 |
| `scripts/funcdoc_data.py` | 修改 | 新函数文案（先文案后代码） |
| `README.md` | 修改 | 操作编辑器 + su 脚本用法 |
| `AGENTS.md`（两份） | 修改 | 新功能口径（家目录 + 仓库根同步） |

---

## 阶段 1 —— su 脚本入口（批 1；只加两个新文件，**不碰核心**）

### Task 1.1: 模板 `clients/vtouch.sh`

**Files:**
- Create: `clients/vtouch.sh`

**Interfaces:**
- Produces: 占位符 `<<PAYLOAD_MD5>>`（32 位小写 hex）/ `<<PAYLOAD_SIZE>>`（十进制）/ `<<PAYLOAD>>`（base64）；载荷标记行 `__VTOUCH_PAYLOAD_BELOW__`。
- 子命令契约：`install | start | stop | status`；`start` 内含 install；无参数 = 用法 + 退出 2；非 root = 提示 + 退出 1。

- [ ] **Step 1: 写模板（完整内容，逐字）**

```sh
#!/system/bin/sh
# vtouch.sh —— 设备侧入口（自包含：内嵌核心二进制；面板三件套由核心启动时自解包）。
#
# 用法（都要 root）：
#   su -c 'sh /sdcard/vtouch.sh start'     # 装 + 起（幂等；已在跑则只报状态）
#   su -c 'sh /sdcard/vtouch.sh stop'      # 停：SIGTERM → 核心自己收尾（先停面板、再放 EVIOCGRAB）
#   su -c 'sh /sdcard/vtouch.sh status'    # 核心/面板 pid + 端口 + 二进制 md5 + 日志尾
#   su -c 'sh /sdcard/vtouch.sh install'   # 只装不启
#
# 生成：python scripts/pack_su.py（把 build/vtouchd_ui 内嵌进本模板 → build/vtouch.sh）
set -u

TARGET=/data/local/tmp/vtouchd_ui
RUNDIR=/data/local/tmp
LOG=$RUNDIR/vt_ui_core.log

PAYLOAD_MD5="<<PAYLOAD_MD5>>"
PAYLOAD_SIZE=<<PAYLOAD_SIZE>>
MARK=__VTOUCH_PAYLOAD_BELOW__

say() { echo "[vtouch] $*"; }
die() { echo "[vtouch] $*" >&2; exit 1; }

[ "$(id -u)" = "0" ] || die "需要 root：su -c 'sh $0 ${1:-start}'"

md5of() { md5sum "$1" 2>/dev/null | awk '{print $1}'; }

decode_payload() {   # $1 = 输出路径
    awk -v m="$MARK" 'x { print } $0 == m { x = 1 }' "$0" | base64 -d > "$1" 2>/dev/null \
        || die "载荷解码失败（toybox base64 可用？）"
}

do_install() {
    want="$PAYLOAD_MD5"
    have=""
    [ -f "$TARGET" ] && have=$(md5of "$TARGET")
    if [ "$have" = "$want" ]; then
        say "已是最新（md5 $want）"
        return 0
    fi
    say "安装核心（$PAYLOAD_SIZE 字节）→ $TARGET"
    tmp="$TARGET.tmp.$$"
    decode_payload "$tmp"
    sz=$(wc -c < "$tmp" | tr -d ' ')
    [ "$sz" = "$PAYLOAD_SIZE" ] || { rm -f "$tmp"; die "长度不符：$sz != $PAYLOAD_SIZE"; }
    got=$(md5of "$tmp")
    [ "$got" = "$want" ] || { rm -f "$tmp"; die "md5 不符：$got != $want"; }
    chmod 755 "$tmp" || { rm -f "$tmp"; die "chmod 失败"; }
    mv -f "$tmp" "$TARGET" || die "替换失败"
    got=$(md5of "$TARGET")
    [ "$got" = "$want" ] || die "回读 md5 不符：$got"
    say "安装完成（回读 md5 $got）"
}

do_start() {
    do_install || exit 1
    cur=$(pidof vtouchd_ui 2>/dev/null) && {
        say "核心已在跑 pid=$cur；面板 pid=$(pidof vtouch-ui 2>/dev/null || echo '(看门狗拉起中)')"
        return 0
    }
    : > "$LOG"
    cd "$RUNDIR" || die "进不去 $RUNDIR"
    nohup ./vtouchd_ui >>"$LOG" 2>&1 </dev/null &
    i=0
    while [ $i -lt 8 ]; do
        sleep 1
        if pidof vtouchd_ui >/dev/null 2>&1 && grep -q "engine=on" "$LOG" 2>/dev/null; then break; fi
        pidof vtouchd_ui >/dev/null 2>&1 || die "进程退出了，看 $LOG"
        i=$((i + 1))
    done
    cur=$(pidof vtouchd_ui 2>/dev/null) || die "启动超时（看 $LOG）"
    sleep 1
    say "核心 pid=$cur  面板 pid=$(pidof vtouch-ui 2>/dev/null || echo '(看门狗拉起中)')"
    tail -n 3 "$LOG"
}

do_stop() {
    cur=$(pidof vtouchd_ui 2>/dev/null) || { say "没在跑"; return 0; }
    say "停核心 pid=$cur（SIGTERM：先停面板、再放 EVIOCGRAB）"
    kill -TERM $cur 2>/dev/null
    i=0
    while [ $i -lt 10 ] && pidof vtouchd_ui >/dev/null 2>&1; do sleep 1; i=$((i + 1)); done
    if pidof vtouchd_ui >/dev/null 2>&1; then
        say "SIGTERM 10s 未退 → SIGKILL（grab 随进程退出释放）"
        kill -9 $(pidof vtouchd_ui) 2>/dev/null
        sleep 1
    fi
    pidof vtouchd_ui >/dev/null 2>&1 && die "没停掉？pid=$(pidof vtouchd_ui)"
    say "已停；面板 pid=$(pidof vtouch-ui 2>/dev/null || echo '-')  触摸已回系统"
}

do_status() {
    say "版本：$PAYLOAD_SIZE 字节 md5 $PAYLOAD_MD5"
    if [ -f "$TARGET" ]; then say "已安装 md5 $(md5of "$TARGET")"; else say "未安装"; fi
    say "核心 pid=$(pidof vtouchd_ui 2>/dev/null || echo '-')  面板 pid=$(pidof vtouch-ui 2>/dev/null || echo '-')"
    if grep -qi "6a2f" /proc/net/tcp 2>/dev/null; then say "端口 27183：在听/有连接"; else say "端口 27183：无"; fi
    say "日志尾："; tail -n 15 "$LOG" 2>/dev/null || say "(无日志)"
}

case "${1:-}" in
    install) do_install ;;
    start)   do_start ;;
    stop)    do_stop ;;
    status)  do_status ;;
    *) echo "用法: sh $0 install|start|stop|status"; exit 2 ;;
esac

exit 0

# ---- 以下为内嵌载荷（pack_su.py 填充；勿手改）----
__VTOUCH_PAYLOAD_BELOW__
<<PAYLOAD>>
```

- [ ] **Step 2: 提交**

```bash
cd /c/Users/21102/vtouch-project
git add clients/vtouch.sh
printf 'feat: clients/vtouch.sh 模板 —— su 入口（install/start/stop/status，自包含占位符）\n' > build/_msg_t11.txt
git commit -F build/_msg_t11.txt
```

### Task 1.2: 打包器 `scripts/pack_su.py`

**Files:**
- Create: `scripts/pack_su.py`

**Interfaces:**
- Consumes: `clients/vtouch.sh` 的三个占位符（Task 1.1）。
- Produces: `build/vtouch.sh`（自包含产物；`sub1` 找不到占位符则报错退出——防重复打包，同 `pack_client.py` 套路）。

- [ ] **Step 1: 写打包器（完整内容）**

```python
#!/usr/bin/env python3
"""把核心二进制 base64 内嵌进 clients/vtouch.sh，生成设备侧自包含入口 build/vtouch.sh。

用法：
    python scripts/pack_su.py                        # build/vtouchd_ui → build/vtouch.sh
    python scripts/pack_su.py <核心二进制> <输出路径>
"""
import base64, hashlib, pathlib, re, sys, textwrap

root = pathlib.Path(__file__).resolve().parent.parent
src = root / "clients" / "vtouch.sh"
bin_path = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else root / "build" / "vtouchd_ui"
out = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else root / "build" / "vtouch.sh"

if not src.is_file():
    sys.exit("pack_su: 找不到模板 %s" % src)
if not bin_path.is_file():
    sys.exit("pack_su: 找不到核心 %s（先 sh scripts/build.sh ui）" % bin_path)

data = bin_path.read_bytes()
md5 = hashlib.md5(data).hexdigest()
b64 = "\n".join(textwrap.wrap(base64.b64encode(data).decode("ascii"), 76))


def sub1(pattern, replacement, text, what):
    new, n = re.subn(pattern, lambda m: replacement, text, count=1)
    if n != 1:
        sys.exit("pack_su: 模板里找不到占位符（%s）—— 是不是已经被打包过？" % what)
    return new


sh = src.read_text(encoding="utf-8")
sh = sub1(r'<<PAYLOAD_MD5>>', md5, sh, "PAYLOAD_MD5")
sh = sub1(r'<<PAYLOAD_SIZE>>', str(len(data)), sh, "PAYLOAD_SIZE")
sh = sub1(r'<<PAYLOAD>>', b64, sh, "PAYLOAD")

# 自检：载荷解回来必须与源二进制逐字节一致（打包器自己的门）
back = base64.b64decode("".join(b64.split()))
if back != data:
    sys.exit("pack_su: 自检失败：载荷解码与源二进制不一致")

out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(sh, encoding="utf-8", newline="\n")   # 设备侧脚本必须 LF
print("已生成 %s" % out)
print("  内嵌 %s：%d 字节，md5=%s" % (bin_path.name, len(data), md5))
print("  产物大小：%.2f MB" % (out.stat().st_size / 1048576.0))
print("  推到设备：adb push %s /sdcard/vtouch.sh" % out)
```

- [ ] **Step 2: 跑一遍 + 语法门 + 确定性门**

```bash
cd /c/Users/21102/vtouch-project
python scripts/pack_su.py
sh -n build/vtouch.sh                       # 语法（git-bash sh；toybox 真跑留给测试阶段）
md5sum build/vtouch.sh                      # 记录
python scripts/pack_su.py && md5sum build/vtouch.sh   # 再打一遍：两次 md5 必须一致（确定性）
python -c "print(open('build/vtouch.sh','rb').read().count(b'\x0d'))"   # 必须为 0（LF 字节判据；grep 版在 MSYS 空转）
```

- [ ] **Step 3: 提交**

```bash
git add scripts/pack_su.py
printf 'feat: scripts/pack_su.py —— su 脚本打包器（内嵌核心 + md5/长度占位符 + 自检）\n' > build/_msg_t12.txt
git commit -F build/_msg_t12.txt
```

### Task 1.3: README 段落 + 批收尾

**Files:**
- Modify: `README.md`（新增一节「su 脚本入口（自包含单文件）」；文内引用 `scripts/vtouch.sh.in` / `scripts/pack_su.py` / `build/vtouch.sh`）

- [ ] **Step 1: 写 README 段落**（要点：设备上放 `build/vtouch.sh` → `/sdcard/vtouch.sh`；四个子命令与示例；"必定回读"的口径——pidof/端口/md5；旧 JS 客户端通道彼时仍在、不改）。规范同仓内其它节（`文件:行号` 引用、中文）。
- [ ] **Step 2: 批收尾门**：`git status --short` 只剩 README；`sh -n build/vtouch.sh` 再跑一次。
- [ ] **Step 3: 提交**

```bash
git add README.md
printf 'docs: README —— su 脚本入口用法（装/起/停/状态）\n' > build/_msg_t13.txt
git commit -F build/_msg_t13.txt
```

---

## 阶段 2 —— 操作模型 + 执行器 + 操作页（批 2；主体）

> 开工前：全量备份到 `build/_backup_full_<TS>/`（含 `git status`、`git diff --binary`、未跟踪清单），记为恢复点。

### Task 2.1: 共享内存契约 v4（一次落全部布局）

**Files:**
- Modify: `src/vt_internal.h`, `src/vt_shm.h`

**Interfaces:**
- Produces（逐字，后续任务全部按这些名字）：

```c
/* vt_internal.h 追加 */
#define MAX_OPS        16
#define MAX_STEPS      32
#define OP_NAME_MAX    15
#define OP_STEP_TAP    1
#define OP_STEP_SWIPE  2
#define OP_STEP_WAIT   3

struct vt_step { int type; int a1, a2, a3, a4; int ms; };
struct vt_op {
    char name[OP_NAME_MAX + 1];
    int  step_count;
    char gate[REGION_ID_MAX + 1];
    int  auto_off;
    struct vt_step steps[MAX_STEPS];
};

/* struct vt_state 追加字段 */
struct vt_op ops[MAX_OPS];
int op_count;
volatile int op_run;          /* -1 = 空闲 */
volatile int op_run_step;
volatile int op_run_state;    /* 0=空闲 1=运行 */
int ops_wake_fd;              /* -1 = 没有 */
volatile uint32_t op_trig_seq;
char op_trig_name[OP_NAME_MAX + 1];
int  op_trig_slot;

/* struct region 追加字段（mark 保留不动） */
char trig_op[OP_NAME_MAX + 1];   /* ""=无 */
int  trig_ev;                    /* 0=无 1=按下 2=完整按压 */
int  kind;                       /* 0=普通 1=开关型 */
volatile int toggle_on;

/* 原型（vt_ops.c / vt_region.c / vt_shm.c 新函数） */
int  vt_ops_init(void);
void vt_ops_tick(void);
int  vt_ops_next_deadline_ms(void);              /* -1 = 空闲 */
void vt_ops_run(const char *name);
void vt_ops_abort(const char *why);
int  vt_ops_put(const struct vt_op *op);
int  vt_ops_del(const char *name);
void vt_ops_clear(void);
void vt_ops_trigger_post(const char *name, int slot);   /* 区域线程调 */
int  vt_id_ok(const char *s, size_t n);                 /* 原 id_ok 提取 */
int  region_bind(const char *id, const char *opname, int ev);
int  region_kind_set(const char *id, int kind);
```

```c
/* vt_shm.h：ver 3→4；新码；载荷；pick 字段 */
#define VT_SHM_VERSION  4u
#define VT_EDIT_OP_PUT   5
#define VT_EDIT_OP_DEL   6
#define VT_EDIT_OP_CLEAR 7
#define VT_EDIT_OP_RUN   8
#define VT_EDIT_OP_STOP  9
#define VT_EDIT_BIND    10
#define VT_EDIT_KIND    11
struct vt_shm_edit { /*……现有字段……*/ struct vt_op payload; };
struct vt_shm_b    { /*……现有字段……*/
    volatile int pick_mode; volatile uint32_t pick_seq;
    volatile int32_t pick_x, pick_y; };
_Static_assert(sizeof(struct vt_shm_b) <= 4096, "区 B 必须装进一页");
```

- [ ] **Step 1: 改两个头文件**（按上面逐字；`struct vt_state` 新字段放 `region_started` 之后；`vt_shm_b` 新字段放 `ry2` 之后）。
- [ ] **Step 2: 门**：

```bash
sh scripts/check_syntax.sh src/vt_shm.c
sh scripts/build.sh          # 默认构建（新代码都在 VT_UI 分支外——本任务只动了结构定义，核对仍过）
sh scripts/build.sh ui
```

- [ ] **Step 3: 提交**：`feat: 共享内存契约 v4（操作表/邮箱载荷/区域绑定字段/取点字段）`

### Task 2.2: 核心·操作落地（put/del/clear + 校验 + 日志）

**Files:**
- Create: `src/vt_ops.c`
- Modify: `src/vt_shm.c`（`edit_apply` 只加 PUT/DEL/CLEAR 三个 case；RUN/STOP 在 2.3）
- Modify: `scripts/funcdoc_data.py`（新函数文案**先写库**）

**Interfaces:**
- Produces: `vt_ops_put/del/clear`（上述原型）；编辑邮箱新码可被核心消费。

- [ ] **Step 1: 写 `vt_ops.c` 前半**（文件头注释 + `#ifdef VT_UI` 守卫 + `vt_ops_put`/`vt_ops_del`/`vt_ops_clear` + 内部校验 `op_valid()`），要点（逐条实现，主语都是"校验不过 → 拒绝 + 日志 `op 被拒 <名>: <原因>`"）：
  - 名字：`vt_id_ok(name, strlen(name))`；
  - `step_count` 1..MAX_STEPS；`type` ∈ {TAP,SWIPE,WAIT}；
  - 坐标 `0..g.logical_width-1 / 0..logical_height-1`；ms：TAP 0..60000、SWIPE 1..60000、WAIT 0..600000；
  - PUT：同名覆盖（找现存下标就写字，否则表满拒绝 / 追加）；`del/clear` 顺手把 `op_run` 若指向被删项的处理留给 2.3（本任务只动表）。
  - 日志（spec §8）：`op 编辑 put <名> 步数=N` / `op 编辑 del <名>` / `op 编辑 clear`。
- [ ] **Step 2: `vt_shm.c` 的 `vt_shm_edit_apply` 加三个 case**（照现有 `regions_clear()` 的排法；`e.payload` 传指针）。
- [ ] **Step 3: funcdoc**：把 `vt_ops_put/del/clear` 文案写进 `scripts/funcdoc_data.py` → `python scripts/apply_funcdoc.py` → `--check` 0 处。
- [ ] **Step 4: 门**：`sh scripts/check_syntax.sh src/vt_ops.c`；`sh scripts/build.sh ui`；`sh scripts/build.sh`。
- [ ] **Step 5: 提交**：`feat: 核心操作落地 —— vt_ops_put/del/clear + 核心单点校验 + 邮箱三码`

### Task 2.3: 核心·执行器 + 主循环集成

**Files:**
- Modify: `src/vt_ops.c`（执行器主体）, `src/vtouchd.c`, `scripts/funcdoc_data.py`

**Interfaces:**
- Produces: `vt_ops_init/tick/next_deadline_ms/run/abort/trigger_post`；`VT_EDIT_OP_RUN/STOP` 两码在 `edit_apply` 里接线。
- Consumes: `set_virtual()` / `emit_frame()`（`src/vt_internal.h` 已有）、`g.g_reemit`、`g.ops[]`。

- [ ] **Step 1: 执行器主体**（`vt_ops.c`；私有静态 `R` 快照结构：`active/idx/name/nsteps/steps[]/step/t0/deadline/phase/slot/hold/样本进度`）。行为逐条对齐 spec §3.2：
  - 起跑（`vt_ops_run`）：门控字段本期先只抄（批 3 才生效）；挑空闲槽（`virt[]`/`staged[]` 都空）；快照；日志 `op 启动 <名> 步数=N 槽=K 门控=<r1|无>`；状态字段置位；推 `op_ev` 环行。
  - 推进（`vt_ops_tick`）：`if (g.g_reemit) return;`（写失败不推进）；按 deadline 依次做：点按（down → 等 hold → up）、滑动（down → 每 10ms 采样 move → 终点 up，`N=max(2,dur/10)`）、等待；每步开始打 `op 步 i/N …`；完成打 `op 完成 <名> 用时=NNNms`。
  - 中止（`vt_ops_abort`）：抬指 + `emit_frame` + 状态归位 + 日志 `op 中止 <名> 步 i/N 原因=<why>` + `op_ev`。
  - 触发槽（`vt_ops_trigger_post`）：写 name/slot → release 自增 `op_trig_seq` → 写 `ops_wake_fd`；`tick` 里 acquire 读、丢/起（busy → `op 丢弃 忙` / 覆盖 → `op 丢弃 覆盖`）。
  - `next_deadline_ms()`：运行中返回剩余毫秒（下限 0），空闲 -1。
- [ ] **Step 2: `vtouchd.c` 集成**（全部在 `VT_UI` 块内）：
  - init：`region_q_init()` 之后 `vt_ops_init()`（eventfd：`eventfd(0, EFD_NONBLOCK|EFD_CLOEXEC)`，失败记日志降级 -1）；
  - poll_step：`vt_shm_edit_apply()` 之后 `vt_ops_tick()`；
  - 超时：在 `to` 计算处加第四档 `int od = vt_ops_next_deadline_ms(); if (od >= 0 && od < to) to = od;`；
  - poll 集：`p[5]` 条件挂载 `g.ops_wake_fd`（`POLLIN`），`np` 逻辑同步（6）；唤醒时 drain；
  - `cleanup()` 开头：`vt_ops_abort("引擎收尾");`（在 `vt_panel_stop()` 之前、uinput 销毁之前）。
  - `edit_apply` 补 `case VT_EDIT_OP_RUN: vt_ops_run(e.id);` / `case VT_EDIT_OP_STOP: vt_ops_abort("停止按钮");`
- [ ] **Step 3: 日志清单核对**：对照 spec §8 的 1/2/3/4/5/6/8 行都在实现里（7 在 2.2 已做）。
- [ ] **Step 4: funcdoc**（新函数文案先写库 → apply → check 0）+ **门**（两个构建 + check_syntax 两个 .c）。
- [ ] **Step 5: 提交**：`feat: 核心执行器 —— 状态机/到点唤醒/触发槽/中止路径 + 主循环集成`

### Task 2.4: 胶水层（`src-ui/ui_glue.c` 新 API）

**Files:**
- Modify: `src-ui/ui_glue.c`, `scripts/funcdoc_data.py`

**Interfaces:**
- Produces（面板侧，逐字）：

```c
int  vtouch_op_count(void);
int  vtouch_get_op(int i, char *name, int n, int *steps, char *gate, int gn, int *autoff);
int  vtouch_get_op_step(int i, int s, int *type, int *a1, int *a2, int *a3, int *a4, int *ms);
int  vtouch_op_put(const char *name, const char *gate, int autoff, const int *steps6, int nsteps);
int  vtouch_op_del(const char *name);
void vtouch_op_clear(void);
int  vtouch_op_run(const char *name);
void vtouch_op_stop(void);
int  vtouch_op_status(int *run_i, int *run_step, int *run_state);
void vtouch_pick_request(void);
void vtouch_pick_cancel(void);
int  vtouch_pick_take(int *x, int *y);          /* 1 = 有新坐标 */
int  vtouch_region_kind_get(int i);
int  vtouch_region_toggle(int i);
int  vtouch_region_trig(int i, char *op, int n, int *ev);
```

- [ ] **Step 1: 实现**。`vtouch_op_put` 走新 `glue_post_op()`（照 `glue_post`：投邮箱 → `glue_wake()` → 等 `edit_applied`；载荷填 `struct vt_op`，`steps6` 每 6 个 int 一组）。取点：`pick_request → B->pick_mode = 1`；`pick_take` 记静态 last_seq，`pick_seq` 变了返回 1 并回传坐标；`cancel → pick_mode = 0`。
- [ ] **Step 2: funcdoc + 门**：`sh scripts/check_syntax.sh src-ui/ui_glue.c`；`--check` 0。
- [ ] **Step 3: 提交**：`feat: 面板胶水 —— 操作/取点/绑定只读 接口（~15 个）`

### Task 2.5: 面板·「操作」页（列表 + 运行/停止）

**Files:**
- Modify: `src-ui/vtouch_ui.cpp`, `scripts/funcdoc_data.py`（如有新函数）

**Interfaces:**
- Consumes: Task 2.4 的 API（在 `vtouch_ui.cpp` 的 `extern "C"` 声明区加逐字原型）。

- [ ] **Step 1: 导航第四页**：`g_nav` 重排为 `0=区域列表 1=操作 2=事件日志 3=设置`；改 `vtouch_ui.cpp:1586-1588` 三处 nav_btn 与 `:1923-1925` 分派（行号开工时现读；**三处一起改**，漏一处就是"点了没反应"）。
- [ ] **Step 2: `page_ops()`**：页头（`操作 · N 条` + 运行状态小字，读 `vtouch_op_status`）；卡片（名字/步数/门控/自动关 + `[运行]/[停止] [编辑] [删除]`）；`＋新建`（`gen_id` 同款生成 `op1..`）；空态提示。运行中卡片显示 `运行中 · 第 k/n 步`。
- [ ] **Step 3: 面板构建门 + 语法门**：

```bash
VTOUCH_UI_CORE=real sh scripts/build_ui.sh && sh scripts/build.sh ui
sh scripts/check_syntax.sh src-ui/vtouch_ui.cpp
```

- [ ] **Step 4: 提交**：`feat: 面板操作页 —— 列表/运行/停止/新建/删除（状态实时读核心）`

### Task 2.6: 面板·编辑覆盖层 + 数字键盘

**Files:**
- Modify: `src-ui/vtouch_ui.cpp`

- [ ] **Step 1: `draw_num_edit()` 数字弹层**：仿 `draw_name_edit`（函数名定位，行号现读）：0-9 / ⌫ / 取消 / 确定；带字段标签与当前值；范围校验就地提示。
- [ ] **Step 2: `draw_op_edit()` 编辑覆盖层**：整面盖住（同改名弹层），内容：名字行（复用改名弹层的字符键盘，改名对象换成操作）、步骤列表（每行 `i. <点按|滑动|等待> <参数文本> [参数][↑][↓][删]`，可滚动）、`[＋点按] [＋滑动] [＋等待]`、门控行（点击循环：`无` → 各 `kind==toggle` 区域，用 `vtouch_region_kind_get`）、`跑完自动关 [开/关]`、`[完成]`。步骤“参数”进 `draw_num_edit`；坐标字段带 `[取点]`（接线在 2.8）。
- [ ] **Step 3: 面板构建门 + 提交**：`feat: 面板编辑层 —— 步骤增删排序/数字键盘/门控行`

### Task 2.7: 面板·落盘 `ops.conf`

**Files:**
- Modify: `src-ui/vtouch_ui.cpp`

- [ ] **Step 1: `save_ops()` / `load_ops()`**：路径 `/data/local/vtouch-runtime/ops.conf`（复用 `REGION_CONF_DIR`）；格式照 spec §2.8（`#vtouch-ops v1` / `op <名> gate <g|-> autooff <0|1>` / `step <type> <a1> <a2> <a3> <a4> <ms>`）；`.tmp`+`rename`；失败重试走独立 pending 标志（照 `g_save_pending` 那套，加 `g_ops_save_pending`）；加载 = 版本门 + **只补缺**（核心已有同名跳过）+ 一条坏记录只警告。
- [ ] **Step 2: 接线**：所有操作编辑动作后置 `g_ops_save_pending=1`；启动时在 `load_regions()` 后调 `load_ops()`；渲染循环里与 `g_save_pending` 并排处理重试。
- [ ] **Step 3: 门 + 提交**：`feat: 面板落盘 —— ops.conf（保存/加载/只补缺）`

### Task 2.8: 取点链路（核心吞触摸 + 面板 UI）

**Files:**
- Modify: `src/vt_frame.c`, `src/vt_shm.c`, `src/vt_panel.c`, `src-ui/ui_glue.c`（两侧访问器）、`src-ui/vtouch_ui.cpp`

**Interfaces:**
- Consumes/Produces（逐字）：核心侧 `int vt_shm_pick_wanted(void); void vt_shm_pick_captured(int lx, int ly); void vt_shm_pick_panel_died(void);`；面板侧（2.4 已有）`vtouch_pick_*`。

- [ ] **Step 1: 核心**：`vt_shm.c` 加三函数（`pick_wanted`：`pick_mode` 且启动计时 ≤20s，超时清 mode + 日志 `取点 超时清除`；`pick_captured`：写 x/y、`pick_seq++`、清 mode、日志 `取点 捕获 x,y`；`pick_panel_died`：清 mode + 日志）；`vt_frame.c` 在"面板吞掉"判定（`:155-163`）后加 `else if`：未被面板矩形吞掉的**新按下** && `vt_shm_pick_wanted()` → 锁存 `ui_eaten[i]=1` + `vt_shm_pick_captured(elx, ely)`；`vt_panel.c` 面板死亡分支调 `vt_shm_pick_panel_died()`。
- [ ] **Step 2: 面板**：参数弹层里 `[取点]` → `vtouch_pick_request()` + 画提示条「点屏幕上目标位置（点面板里取消）」；面板内点击=取消（`vtouch_pick_cancel()`）；`vtouch_poll_step` 里 `vtouch_pick_take` 有结果 → 合成 `pick_ev <x> <y>` 事件行交 `HK.event`；`ui_ev_cb` 里处理 `pick_ev`（回填 + 退取点态 + 重画）。
- [ ] **Step 3: funcdoc + 门 + 提交**：`feat: 取点链路 —— 核心吞一次触摸回填坐标 + 面板取点 UI`

### Task 2.9: 批 2 收尾（全门 + README）

- [ ] **Step 1: 全量门**：

```bash
sh scripts/build.sh && sh scripts/build.sh ui
python scripts/apply_funcdoc.py --check        # 0 处
python scripts/pack_client.py && python scripts/ci_check.py --core build/vtouchd_ui
```

- [ ] **Step 2: README**：操作编辑器用法（页面、编辑、运行、取点、ops.conf 位置、日志口径指向 spec §8）。
- [ ] **Step 3: 提交**：`feat: 批 2 收尾 —— 门全绿 + README（操作编辑器）`

---

## 阶段 3 —— 触发侧（批 3；只加逻辑/UI，不动布局）

### Task 3.1: 核心·区域触发 + 开关

**Files:**
- Modify: `src/vt_region.c`, `src/vt_shm.c`, `scripts/funcdoc_data.py`

**Interfaces:**
- Produces: `region_bind/region_kind_set`（原型在 2.1 已定）；`edit_apply` 补 `VT_EDIT_BIND` / `VT_EDIT_KIND`；区域线程新增私有 `r_trig_latch[slot][rid]`。

- [ ] **Step 1: 落地两码**：`region_bind(id, op, ev)`（区域必须存在；`op` 允许悬空；ev 0/1/2）→ 写 `trig_op/trig_ev` + `region_gen++`（复用现有"结构变化才 bump"口径）+ 日志 `op 编辑 bind/kind`；`region_kind_set` 同理写 `kind`。
- [ ] **Step 2: 区域线程判定**（`region_apply` 内，锁内判定、锁外投递——现有 `pend[]` 模式）：down-hit 时：`r_trig_latch=1`；`trig_ev==1` → `vt_ops_trigger_post(trig_op, slot)` + 日志 `op 触发 <id> → <名>（按下）`。该 slot 的 up 事件时：若 latch：`kind==toggle` → 翻转 `toggle_on` + 推 `toggle_ev <id> <0|1>` + 日志 `区域 <id> 开关 → 开|关`；`trig_ev==2` → `vt_ops_trigger_post(…,（完整按压）)`；清 latch。
- [ ] **Step 3: funcdoc + 门 + 提交**：`feat: 触发侧（核心）—— 区域绑定/完整按压/开关翻转/触发投递`

### Task 3.2: 核心·门控与自动关

**Files:**
- Modify: `src/vt_ops.c`

- [ ] **Step 1: 起跑门控**：`vt_ops_run` 里 `gate` 非空 → 找区域；必须 `kind==toggle` 且 `toggle_on==1` → 放行；否则拒绝 + `op 丢弃 门控拦截`。
- [ ] **Step 2: 自动关**：正常结束时 `auto_off` → 翻转门控开关（同 §4.2 的翻转+日志口径）。
- [ ] **Step 3: 门 + 提交**：`feat: 触发侧（核心）—— 门控拦截 + 跑完自动关`

### Task 3.3: 面板·区域卡片行 + regions.conf 增量

**Files:**
- Modify: `src-ui/vtouch_ui.cpp`, `src-ui/ui_glue.c`（`vtouch_region_bind` / `vtouch_region_kind` 两个投递函数）, `scripts/funcdoc_data.py`

- [ ] **Step 1: 卡片行**（区域页 `region_card`）：`触发: <无|操作名>`（点击循环全部操作+无）、`时机: <按下|完整按压>`、`开关型: <关|开>`；开关型**显示开/关状态**（`vtouch_region_toggle`）。改完调 `save_regions()`（增量字段随之落盘）。
- [ ] **Step 2: regions.conf 增量**：`save_regions` 每条 region 行后补 `bind <id> <op|-> <down|press>` 与 `kind <id> <0|1>`；`load_regions` 解析这两类新行（旧文件缺 = 默认；坏行只警告）。
- [ ] **Step 3: 面板样式改读**：绿样式判定改 `mark || (kind==toggle && toggle_on)`（`mark` 兼容保留）；`toggle_ev` 行处理（记日志 + 重画，照 `mark_ev`）。
- [ ] **Step 4: funcdoc + 面板构建门 + 提交**：`feat: 触发侧（面板）—— 区域卡片行/regions.conf 增量/开关样式`

### Task 3.4: 批 3 收尾（全门 + README + AGENTS）

- [ ] **Step 1: 全量门**（同 2.9 Step 1）。
- [ ] **Step 2: README + AGENTS**：README 补触发侧用法与日志；AGENTS.md 补新模块口径；**两份 AGENTS.md 同步**（家目录 + 仓库根，`md5sum` 对齐）。
- [ ] **Step 3: 提交**：`feat: 批 3 收尾 —— 门全绿 + README/AGENTS（触发侧）`

---

## 阶段 4 —— 测试（后期；本阶段只登记入口，不接设备）

> 用户口径：不前置真机探测；实现阶段不接设备。真机执行照 spec §9 清单：
> 不需要手指（编辑/运行/停止/取点/开关/引擎起停 + su 四命令）→ 需要手指（区域触发手感/门控连招/取点手感/连招节奏）。
> 执行者在此阶段前先读 spec §8（日志清单）拿到全部 grep 判据；CI/构建门已在各批收尾跑过。

---

## Self-Review 记录

1. **Spec 覆盖**：§1 架构→任务 2.1-2.3；§2 模型/落盘→2.1/2.2/2.7；§3 执行器→2.2/2.3；§4 触发→3.1-3.3；§5 面板→2.5/2.6/2.8/3.3；§6 su 脚本→1.1-1.3；§7 失败面→各任务的中止/丢弃分支（2.3/3.1/3.2）；§8 日志→2.2/2.3/2.8/3.1 内嵌；§9 测试→阶段 4；§10 批次→阶段划分；§11 文件清单→文件地图；§12 拍板→无待办（事件环修复另开一轮，见 build 台账）。
2. **占位符扫描**：无 TODO/TBD；每个"实现"步都给了可核对的符号名/字段名/日志原文。
3. **类型一致性**：函数名/字段名/常量名在 2.1 的 Interfaces 块一次定义、后续任务逐字引用（`vt_ops_*`/`vtouch_*`/`vt_shm_pick_*`/`r_trig_latch`）。
