# OPS_IMPL_PLAN_V6F —— 框矩形步骤实施计划（Task 1–5）

依据 `OPS_PLAN_V6F.md`（§3 八项已拍板：全按推荐）。Native 逐任务。

## Task 1 — 核心常量与契约 v16

- `src/vt_internal.h`：`#define OP_STEP_BOX 23`（a1..a4 = x1,y1,x2,y2 四角坐标〔同点按编码尺子〕；ms=时长 0..60000〔0=3000〕；ref/expr 空、j1/j2=0）。
- `src/vt_internal.h` 状态块：hl 通道加 `hl_kind`（0=区域名/1=矩形）+ `hl_x1/hl_y1/hl_x2/hl_y2`。
- `src/vt_shm.h`：`VT_SHM_VERSION` 15 → 16 + 历史注（v16：hl 通道泛化 = 23 框矩形。旧面板 15 会拒启 → 两侧同发布）。
- 门：devbuild 过（语法）。

## Task 2 — 核心校验 + 执行

- `src/vt_ops.c` `op_valid` 加 `case OP_STEP_BOX`：a1..a4 过坐标尺子（字面 0..逻辑尺寸-1 / -25..-1 编码；拒词沿用该类型族 `坐标非法`）；ms 0..60000 `时长越界`；ref/expr 空 + j1/j2=0 `字段必须为 0`。
- `src/vt_ops.c` `op_begin_step` 加 `case OP_STEP_BOX`：`op_resolve` 逐解析四坐标（缺值中止同点按）→ 自动归一（min/max）→ 写通道（`hl_kind=1` + 四坐标 + ms〔0→3000〕、seq release 最后）→ 恒打 `op 框矩形 x1,y1–x2,y2（ms）` → PH_WAIT。
- `case OP_STEP_HL` 补 `hl_kind=0`（区域模式显式标 kind）。
- `scripts/funcdoc_data.py`：op_valid 词条补 23；`hl_kind` 说明。
- 门：funcdoc 幂等 + devbuild 绿（记核心 md5）。

## Task 3 — 面板

- `src-ui/ui_glue.c`：`vtouch_hl_take` 扩签名 `(char *id, int cap, int *kind, int *x1, int *y1, int *x2, int *y2, int *ms)`；读期重读逻辑不变。
- `src-ui/ui_stubs.c`：同款。
- `src-ui/vtouch_ui.cpp`：`draw_region_hl` 两模式——kind 0 走现区域查找链（含 id 标签）；kind 1 直接用四坐标（同一 p2c 换算、min/max、**无标签**）；样式同 V6E（琥珀脉动 + 末 300ms 淡出、不经显隐闸）。
- funcdoc 词条（vtouch_hl_take 参数表）。
- 门：`~/vtouch-ui-check.sh` 全绿 + devbuild 绿。

## Task 4 — 编辑器（v30 目标）

- `Ops.java`：`T_BOX = 23`；**两条名字链都加**（`Ops.typeName` case 23 + StepEdit.TYPE_NAMES/TYPE_IDS —— V6E 血泪）；解析闸 `>22→>23`；validate 闸 `>22→>23` + `case T_BOX`（坐标尺子镜像 + ms + 余字段）；summary「框矩形 · (x1,y1)–(x2,y2)（N 秒）」。
- `StepEdit.java`：build case 23 = 四行坐标输入（x1/y1/x2/y2，**复用点按坐标控件含变量按钮**）+ 时长 + 说明；read case 四 `ivCoord`。
- `OpsPage.java`：newStep 默认 ms=3000。
- `Forms.java`：typeSheet 基础组加「框矩形」。
- `TestBench.java`：⑥ 框矩形核（往返 / 镜像正负例 / 摘要 / 渲染件 `edit_step_boxcoords.png`）→ **79 项**；**未知类型夹具 23→24**（老雷）。
- 门：javac + 构建装机 + 测试台 79/0/0 + 渲染件目检（**含头部类型名检查**——V6E 血泪）。

## Task 5 — 真机验收（TRACE + 双侧载荷 + 用户目视）

- P0：ztf1 合法 put；ztf2 坐标越界（-26）拒；ztf3 ms 越界拒；ztf4 残留字段拒。
- P1：ztf5 字面矩形（如 200,300,900,800，3000ms）目视；**ztf6 变量链**（计算 e9..e12 → 框矩形 e9,e10,e11,e12）目视（若要「标找图位置」完整示范：找图命中 fx/fy → 计算四角 → 框）；ztf7 引用未写变量 → 中止 `变量无值`。
- 收场：双侧还原（`.bak-v6f-pre`）+ 无 TRACE 干净核 + 验收记录 `docs/OPS_V6F_ACCEPTANCE.md` + 提交 + 技能/账本。
