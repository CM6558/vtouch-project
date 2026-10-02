# 方案（Profiles）实施计划 v4（区域+操作打包：切换 / 新建 / 另存 / 改名 / 删除）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 给面板加「方案」体系：`schemes/<名>/{regions.conf,ops.conf}` + `current` 指针 + live 镜像；切换 = 现有编辑邮箱 **clear + 重放**；CRUD（新建/另存/改名/删）+「方案」页；说明页/README/AGENTS 收尾。

**Architecture:** **全面板侧**（`src-ui/vtouch_ui.cpp`；预计零改动 `src/`、`ui_glue.c`、`ui_stubs.c`）。批 1 = 文件层（路径/迁移/镜像/切换执行器，宿主仿真）；批 2 = 方案页 UI + 文档 + 收尾。文件格式零改动（regions.conf / ops.conf 原样搬家）。

**Tech Stack:** C（NDK r27d，`-O2 -Wall -Wextra -Werror -D_GNU_SOURCE`）、C++/ImGui v1.91.8 面板、toybox sh、Python 3 工具脚本；设备 OnePlus PJZ110。

**Spec:** `docs/OPS_PLAN_V4.md`（执行时**两份一起读**；契约细节/日志词/文案以 spec 为准，本计划写"怎么做"）。

## Global Constraints

- 编译：`src/*.c` 一起链；单文件速查 `sh scripts/check_syntax.sh <文件>`；告警不超基线。
- **核心 / 契约零改动**：默认核心 md5 恒 **`984fa627`**（每任务核）；`VT_SHM_VERSION` 不动；WS 协议不动；只用现有编辑邮箱命令（`VT_EDIT_CLEAR` / `VT_EDIT_OP_CLEAR` / 区域 ADD / `VT_EDIT_OP_PUT` / `VT_EDIT_OP_STOP`）。
- 落盘只在 `/data/local/vtouch-runtime/`；schemes 目录在其下；**regions.conf / ops.conf 格式零改动**。
- 方案名规则：`vt_id_ok` 尺子（`[A-Za-z0-9_-]`、1..15、裸 `-` 除外）。
- **不变量**：`live == schemes/<current>`（启动强制同步 §2 + 存盘镜像 §3 + 切换直写 §4 共同保证）。
- 日志词逐字（spec §8）：`方案 切换 <旧> → <新>（区域 N / 操作 M）` / `方案 新建 <名>` / `方案 另存为 <名>` / `方案 改名 <旧> → <新>` / `方案 删除 <名>` / `方案 兜底迁移 → <名>（区域 N / 操作 M）` / `方案 镜像失败 <名>: <errno>`。
- 迁移（spec §2）：首启/兜底同一套；live 收进「默认」（名字冲突自增 `默认2/3…`）；空表文件 = 各表当前版本行。
- 切换（spec §4）：预检 → 静默边界 → flush 旧 → 写 live → 核心 clear+重放（**先区域后操作**）→ current → UI 刷新；失败面照 spec。
- 说明页：**v4 起唯一来源 = `docs/OPS_PLAN_V4.md` §12（全量 15 条）**，逐字节复核。
- md5 口径：核心恒 `984fa627`；real 面板基线 `b297833a`、UI 核心 `ca8e5a6f`（本波每任务会变，记录新值 ×2）；funcdoc `--check` 0/0。
- 提交：中文信息写 `build/_msg_v4_<x>.txt`（UTF-8）后 `git commit -F`；**每任务一提交、不自动 push**。
- 每批开工前全量备份 + 恢复点（`build/_backup_full_<TS>/`）。
- 测试口径：**实现阶段不接设备**；真机集中（首启迁移/两方案往返切/重启保持/镜像/CRUD 手感）并入阶段 4 窗口。
- 仿真照 v2/v3 口径：生成器**从源码逐字抽取** + 宿主 MinGW gcc + 断言（不入库）。
- 本计划所有函数名/常量名**逐字使用**（建议名标"建议"，定了就别改）。

---

## 文件结构（新增/修改地图）

| 文件 | 动作 | 职责 |
|---|---|---|
| `src-ui/vtouch_ui.cpp` | 修改 | 方案文件层；切换执行器；「方案」页；说明页增量 |
| `README.md` / `AGENTS.md`（两份） | 修改 | v4 口径（批 2 T2.2） |
| `src/`、`src-ui/ui_glue.c`、`src-ui/ui_stubs.c` | **不动** | 核心/胶水零改动（真需要动 → 停下报告） |

---

## 阶段 1 —— 文件层（批 1）

> 开工前：全量备份到 `build/_backup_full_<TS>/`，记恢复点。

### Task 1.1: 方案文件层（目录 / current / 迁移 / 镜像钩子）

**Files:**
- Modify: `src-ui/vtouch_ui.cpp`

**Interfaces（建议名，报告说明实际采用名）：**
- 路径常量：`SCHEME_DIR = REGION_CONF_DIR "/schemes"`、`SCHEME_CUR = REGION_CONF_DIR "/current"`。
- `scheme_name_ok(const char *n)`（vt_id_ok 尺子）；`scheme_list(char (*names)[16], int max)`（readdir 枚举，字母序；返回条数）。
- `scheme_cur_get(char *out)` / `scheme_cur_set(const char *name)`（`.tmp + rename` 口径，同 save_regions）。
- `scheme_migrate(void)`（spec §2 全套；返回当前方案名）。
- `scheme_sync_live(const char *name)`（`schemes/<name>/两文件 → live 两路径` 复制；缺失文件按"写空合法文件"补齐后再复制）。
- `scheme_mirror(void)`（live 两文件 → `schemes/<current>/` 复制；**save_regions / save_ops 落盘成功后各调一次**；失败打 `方案 镜像失败 <名>: <errno>` 不阻塞）。
- 启动接线：`scheme_migrate() → scheme_sync_live(cur) → 现有 load_regions(); load_ops();`。

- [ ] **Step 1: 定位接线点**：`save_regions` / `save_ops` 的成功出口（.tmp+rename 之后）与启动段（`load_regions(); load_ops();` 调用点）。
- [ ] **Step 2: 实现 helpers**：mkdir -p（`schemes/` 及 `schemes/<名>/`）、readdir 枚举（过滤 `.`/`..`）、两文件复制（读→写 .tmp→rename；源缺失→写空合法文件）、名字校验（复用/对齐 `vt_id_ok` 尺子）。
- [ ] **Step 3: 迁移**（spec §2）：读 current → 校验目录/两文件 → 缺则：live 任一存在→建「默认」收编（缺失份写空合法文件）；全新→建空「默认」；冲突→自增「默认2/3…」；`current` 写定；日志 `方案 兜底迁移 → <名>（区域 N / 操作 M）`。
- [ ] **Step 4: 镜像钩子**：两处 save 成功后调 `scheme_mirror()`；确认**不会**递归触发保存（镜像只写 schemes/，不碰 live）。
- [ ] **Step 5: 启动接线**：迁移+同步插在既有 `load_regions/load_ops` 之前（同步失败 → 告警并按现有文件继续，报告说明）。
- [ ] **Step 6: 门 + 宿主仿真**（生成器逐字抽取 + MinGW gcc；不入库）：迁移三情形（有 live / 全新 / 默认冲突）；current 往返（写读幂等）；名字校验边界（空/超长/非法字符/裸 `-`/重名）；镜像路径（live→scheme 复制字节一致；源缺失补空文件）。
- [ ] **Step 7: 提交**：`feat: 面板 —— 方案文件层（schemes/current/迁移/镜像）`。

### Task 1.2: 切换执行器

**Files:**
- Modify: `src-ui/vtouch_ui.cpp`

**Interfaces:**
- `int scheme_switch(const char *name)`（0=成功；非零=失败原因码，报告说明各码）；内部可拆 `scheme_precheck(name)`。
- load 逻辑**可重入**（切换期再跑一遍不依赖"只启动一次"的隐藏假设；报告说明怎么保证）。

- [ ] **Step 1: 预检**：只读解析 `schemes/<name>/` 两文件（dry-run：版本门/坏行/步数上限，照 load 口径但**不推核心**）；坏行 → 拒切（就地可提示的返回值）。`name == current` → 直接成功返回。
- [ ] **Step 2: 静默边界**：查现有入口——操作在跑 → 停（`VT_EDIT_OP_STOP` 的胶水函数）；取点态/编辑覆盖层开着 → 关（复用现有 close 路径）。报告列实际调用。
- [ ] **Step 3: flush 旧 + 写 live**：live 两文件直写回 `schemes/<old>/`（失败仅告警）；`schemes/<new>/` 两文件 → live（复用 1.1 复制 helper）。
- [ ] **Step 4: 核心替换**：`vtouch_region_clear()` → 重放区域（复用 load_regions 的"读文件→推核心"路径）→ `vtouch_op_clear()` → 重放操作（load_ops 路径）。**先区域后操作**；单条拒收照"坏记录单条跳过+警告"口径。
- [ ] **Step 5: current + 日志 + UI 刷新**：`scheme_cur_set(new)`；日志 `方案 切换 <旧> → <新>（区域 N / 操作 M）`；触发列表刷新（区域页/操作页/方案页用的重读口径）。
- [ ] **Step 6: 门 + 宿主仿真**（stub 化邮箱调用，**记录调用序列断言**）：清空先于重放（区域 clear→region adds→op clear→op puts 顺序）；A→B→A 往返后状态与初态等价（文件/调用序列层面）；预检坏行拒切且**零状态修改**；switch 到自身 = no-op。
- [ ] **Step 7: 提交**：`feat: 面板 —— 方案切换执行器（clear+重放；先区域后操作）`。

### Task 1.3: 批 1 收尾

- [ ] **Step 1: 全门**：双构建 ×2（real 两遍一致）+ funcdoc 0/0 + `git status` 清点。
- [ ] **Step 2: 报告** `task-1-report.md`（md5 集：默认恒 `984fa627`、UI/real 新值）。
- [ ] **Step 3**: 无文件改动则无提交。

---

## 阶段 2 —— 方案页 + 收尾（批 2）

> 开工前：全量备份，记恢复点。

### Task 2.1: 「方案」页（切换 / 新建 / 另存 / 改名 / 删）

**Files:**
- Modify: `src-ui/vtouch_ui.cpp`

- [ ] **Step 1: 页面注册**：左侧菜单加「方案」（与「操作」邻近；照现有 page 注册/分发口径）。
- [ ] **Step 2: 列表**：读 `scheme_list`；每行方案名 + [切换]（**当前行高亮、按钮禁用**）；可滚 + `SCR_LIST` 拖滚。
- [ ] **Step 3: 四操作键 + 子层**：`[新建(空)] [从当前另存为] [重命名] [删除]`；新建/另存/改名 → 名字键盘（复用 `draw_char_kb`，ASCII）；删除 → 新小确认层（`[取消][删除]`，文案「删除方案 <名>？不可恢复」）；名字冲突/非法就地拒收提示。
- [ ] **Step 4: 接线**：切换 → `scheme_switch`；新建 = 建空目录+空合法文件（**不自动切**）；另存 = live 两文件复制进 `schemes/<名>/` + `scheme_cur_set(名)`；改名 = 目录 rename + 若 current==旧名同步；删 = 拒当前（提示）、确认后删目录；各操作照 spec §8 打日志。
- [ ] **Step 5: 就地提示**（复用页面提示槽位口径）。
- [ ] **Step 6: 门**：real ×2、stub rc=0、默认不变、funcdoc 0/0；静态走查（五操作路径 + 拒收路径）。
- [ ] **Step 7: 提交**：`feat: 面板 —— 方案页（切换/新建/另存/改名/删除）`。

### Task 2.2: 说明页增量（15 条）+ README/AGENTS v4 口径

**Files:**
- Modify: `src-ui/vtouch_ui.cpp`（说明页数组）, `README.md`, `AGENTS.md`（**两份**：仓库根 + `C:/Users/21102/AGENTS.md`）

- [ ] **Step 1: 说明页重生成**：从 `docs/OPS_PLAN_V4.md` §12 提**全量 15 条**（逐字节双向复核）；数组注释"唯一来源"改 §12（v4）。
- [ ] **Step 2: README**：v4 节增量（方案存储布局 / 切换语义 / CRUD / 首启迁移 / 日志词）；`文件:行号` 引用现读。
- [ ] **Step 3: AGENTS（两份逐字节同步）**：口径增量（schemes/current/live 不变量 / 切换 = clear+重放 / 命名规则 / 说明页来源 §12）。
- [ ] **Step 4: 门**：real ×2、stub rc=0、默认不变、funcdoc 0/0；AGENTS 两份 md5 对齐。
- [ ] **Step 5: 提交**：`docs: 说明页 + 方案（v4）口径（README/AGENTS）`。

### Task 2.3: 收尾（全门 + 产物）

- [ ] **Step 1: 全门**：`check_syntax` 全量；双构建 ×2；funcdoc；`pack_client.py`（onefile 新值记录）；`ci_check.py`（10/0）；AGENTS 双份对齐。
- [ ] **Step 2: md5 全表**（默认/UI/real 三件/onefile/AGENTS）写报告 `task-2-report.md`。
- [ ] **Step 3**: 无文件改动则无提交。

---

## 阶段 3 —— 测试入口（真机，并入下次窗口）

> 范围：首启迁移（设备现有数据无感收进「默认」）；建两方案往返切（区域/操作/绑定/开关型全量正确）；重启保持 current；编辑即时镜像；方案页 CRUD 手感；与 v3 阶段 4 清单一并执行。
