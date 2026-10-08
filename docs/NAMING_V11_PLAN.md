# NAMING_V11_PLAN —— 命名放开中文（契约 v11「名字扩位」）

状态：**已拍板**（用户 2026-10-08：「做吧，注意确保正确性」）。
口径来源：用户「关于所有命名可以使用中文吗？」+ 评审提案①②③④⑤（本文件即提案落档）。
实施者：设备侧助手（核心/面板改码）+ 用户（Windows 构建验收）。

---

## 1. 目标与边界

**目标**：所有**用户可见命名**（区域 id / 操作名 / 方案名 / 模板名 / 点集名 / 自定义变量名）放开为
**UTF-8 可见字符、1..15 个字符**；现有 ASCII 名全部向后兼容。

**边界（不做）**：
- 不做 NFC/NFD 归一化 —— 名字按**字节精确**比较（同字形两种编码 = 两个名字；输入侧可提示，核心不归一）。
- 表达式文本长度上限保持 **63 字节**（中文变量名占 3B/字，≤21 字；不随 v11 扩）。
- 不做 Unicode 全量类别表 —— 可见性用**实务黑名单**（覆盖 C0/C1、各类空格、零宽/格式、变体选择符）。
- 面板「默认[N]」内部兜底名机制不动（继续承认非 ASCII 内部名）。

## 2. 口径定稿（唯一尺子）

**vt_id_ok（区域 id / 操作名 / 门控 / 步骤 ref〔条件区域·模板·点集〕共用）**
1. 长度：1..15 **字符**（Unicode 码点计）且字节数 ≤ 47（缓冲 48 含 NUL）。
2. ASCII：仅 `[0-9A-Za-z_-]`；**裸 `-`（整名 = "-"）拒收**（解除/无哨兵，v10 口径不变）。
3. 非 ASCII：必须**合法 UTF-8**（拒过长编码、孤立续字节、U+D800..DFFF 代理区、超 U+10FFFF、截断）；
   且码点不在**拒收黑名单**：
   - U+007F、U+0080..U+009F（C1 控制）
   - U+00A0、U+1680、U+2000..U+200A、U+2028、U+2029、U+202F、U+205F、U+3000（各类空格）
   - U+00AD、U+061C、U+200B..U+200F、U+2060..U+2064、U+2066..U+2069、U+FE00..U+FE0F、U+FEFF（零宽/格式/变体符）
4. 空名非法（门控/ref 语境由调用点单独允许空）。

**op_var_name_ok（自定义变量名）**
1. 同长度口径（1..15 字符、≤47B）。
2. 首字符：ASCII 字母 / `_` / 非 ASCII 可见码点；后续：ASCII 字母数字 / `_` / 非 ASCII 可见码点。
3. 拒 `-`（不进表达式名字空间，v10 口径不变）。

**表达式（vt_expr.c）**
- 词法标识符：ASCII 字母/`_` 开头 + 字母数字/`_` 续；**扩展：字节 ≥0x80 可作首字符与续字符**（字节级归类；
  UTF-8 合法性由名字表上游保证；未知名 → `未知名字` 兜底）。
- 名字表条宽 `[16]` → `[48]`（见 §4）。

**落盘 / 协议**
- regions.conf / ops.conf / WS 行都是空白分 token，中文名不含空白 → **格式零改动**；
- 唯一改动点：名字字段的**缓冲宽度**与 **scanf 格式宽度**（`%15s` → `%47s`）按 §5 审计全改。

## 3. 契约 v11（布局）

- `VT_SHM_VERSION 10 → 11`（src/vt_shm.h）。
- 新常量（src/vt_internal.h，置于旧三常量之前）：
  ```c
  #define VT_NAME_CHARS   15   /* 名字字符数上限（Unicode 码点；v11 起） */
  #define VT_NAME_BUF     48   /* 名字缓冲字节数（含 NUL）：15×3B+1=46 上取 4 对齐 */
  #define REGION_ID_MAX   VT_NAME_CHARS   /* 旧名保留：语义 = 字符数上限 */
  #define OP_NAME_MAX     VT_NAME_CHARS
  #define OP_VAR_NAME_MAX VT_NAME_CHARS
  ```
- 字段扩容（`[X_MAX+1]`→`[VT_NAME_BUF]`）：
  | 处 | 现状 | v11 |
  |---|---|---|
  | struct region.id / trig_op | 16 | 48 |
  | struct vt_step.ref | 16 | 48 |
  | struct vt_op.name / gate | 16 | 48 |
  | struct vt_op.vars[16][] | 16 | 48 |
  | struct vt_state.sub_region_id / op_trig_name | 16 | 48 |
  | vt_shm_edit.id / new_id | 16 | 48 |
  | vt_shm_frame_hdr.test_ref / test_region | 16 | 48 |
- 尺寸预算（估算，以最终编译断言为准）：
  - `struct vt_op` +1600B（name/gate +32×2、ref +32×32=1024、vars +512）⇒ **区 B 4004 → ≈5.7KB**，
    断言改 `sizeof(struct vt_shm_b) <= 8192`（两页；原一页纪律在 v11 破除，注释说明）；
  - 区 A（vt_state）+≈27.7KB（ops 16×1600 + regions 32×64 + 零头）—— 布局偏移由头部记录、`pg_up` 泛化，无需改映射代码；
  - 帧头 +64B，仍 ≤ 一页（断言不变）。
- 新增 `_Static_assert`（值对值 + 结构体字段尺寸），防止再漂。
- **md5**：`vt_id_ok` 属共享路径 ⇒ 默认核 md5 变更，属**有意**（提交信息按惯例记录）。

## 4. vt_expr 侧

- `vt_expr.h` 新增 `#define VT_EXPR_VAR_BUF 48`（= 核心 VT_NAME_BUF；vt_internal.h 里 `_Static_assert` 对拍）。
- `vt_expr_check/eval` 的 `names` 形参 `const char (*names)[16]` → `[VT_EXPR_VAR_BUF]`（.h/.c + 全部调用点：
  vt_ops.c、面板 `vtouch_expr_check` 转发、ui_glue.c）。
- 词法（vt_expr.c `ex_tok`）：标识符判定加 `(unsigned char)*p >= 0x80`（首字符与续字符两处）。
- `ex_name_kind` 里 `strnlen(c->names[i], 16)` → `strnlen(c->names[i], VT_EXPR_VAR_BUF)`。

## 5. 审计类别（防漏清单 —— 实施纪律，「注意确保正确性」的主绳）

**所有改动必须按类过一遍 grep（每类全仓库扫，改完复扫为零残留）：**

- **Ⅰ `strnlen(x, X_MAX+1)`** → `strnlen(x, VT_NAME_BUF)`（X_MAX 是字符限，不能当字节读上限）。
- **Ⅱ `strncmp(a,b,X_MAX)`** → 宽度 `VT_NAME_BUF`（防「前 15 字节相同」误判，如 微信头像模板A/B）。
- **Ⅲ 名字缓冲声明 `[X_MAX+1]`/`[16]`** → `[VT_NAME_BUF]`。
- **Ⅳ `n > X_MAX` 前置长度拒** → 删除该拒，统一走 `vt_id_ok`（防 CJK 被字节数误拒）。
- **Ⅴ scanf/格式串 `%15s` 类** → 名字字段 `%47s`（或手工切分）。
- **Ⅵ 面板/桩独立镜像**：`src-ui/ui_stubs.c`、`src-ui/vtouch_ui.cpp` 的 `#define REGION_ID_MAX 15` 旁新增
  `#define VT_NAME_BUF 48`；`(*refs)[REGION_ID_MAX+1]` 数组宽 → `[VT_NAME_BUF]`（含 g_ope_refs / g_ex_undo_refs / snap）。
- **Ⅶ `sizeof` 型写法（含 `sizeof h->test_ref`）自动生长，保持不动。**

## 6. 逐文件改动清单

### 核心（src/）
- `vt_internal.h`：§3 常量 + 8 个字段 + 断言。
- `vt_region.c`：`vt_id_ok` 重写（共享路径）；418/431/432 栈缓冲；注释。
- `vt_ops.c`：`op_var_name_ok` 重写；Ⅳ 类长度拒（114/121/216/236/258/307/312/347/370/458/…）；Ⅰ 类（388/457/600/601/1710/1711/1856/1857/1996/1997）；缓冲 387/524/527/599/738/1703/1850；提示语（`[A-Za-z0-9_-]` → 新口径）；门控校验升级为完整 `vt_id_ok`（原仅长度）。
- `vt_shm.h`：§3（版本/字段/断言）。
- `vt_shm.c`：223-225 nm 缓冲 + 邮箱应用处；布局代码**无需改**（pg_up 泛化，已核）。
- `vt_ws.c`：738/845（sid/子订阅 id 的字节拒 → 改 `vt_id_ok` 或字节上限）；region add 等解析处按 Ⅴ 扫。
- `vt_expr.h` / `vt_expr.c`：§4。
- 其余文件（vt_frame.c 等）：按 Ⅴ/Ⅱ 类全扫（事件行合成缓冲按 MAX_LINE，预计零改动）。

### 面板（src-ui/）
- `vtouch_ui.cpp`：常量镜像；`id_name_ok`（~2626）/`scheme_name_ok`（~1206）/`scheme_name_known`（~1220）改 UTF-8 口径；
  ~24 处 `char id[16]` 栈缓冲与解析格式（555/786/844/999/1006/1097/1134/1473/1527/1692/1779/1834/1849/1881/2112/2225/2599/2637/2978/3168/4133/4188/4502/5513）；
  `g_ope_refs`/`g_ex_undo_refs`（3197/3223）+ 签名（69/506/1070）；方案列表缓冲。
- `ui_glue.c`：`snap[32][16]`→48；strncmp 宽度（117/130/374）；534-551；`vtouch_op_put` 签名 refs 宽。
- `ui_stubs.c`：镜像常量 + stu_region/stu_op + 签名（185/208）。
- **字体（面板任意中文显示）**：`gen_ui_chars.py` 的前提「没有用户任意输入」被 v11 打破 ⇒
  `vtouch_ui.cpp` 字体段（~2172）改**按需重建**：初始 = 源码集（现状）；每帧前检测将要显示的
  name 串里缺字的码点（`ImFont::FindGlyphNoFallback`），有缺失 → 追加 ranges → 重建图集 +
  重传纹理（`Im_ImplOpenGL3` Destroy/CreateFontsTexture），下帧生效；重建发生在帧间。
  Plan-B（若重建不稳）：常集（GB2312 常用 6763 字）烘 30px 一档 + 名字统一走该档。
- 说明：本仓库面板**已不带核心头**的独立常量镜像必须手动同步（Ⅵ 类）。

### App（vtouch-editor，设备侧全量自验）
- `Assets.java`：`nameOk` UTF-8 版（字符≤15、字节≤47、黑名单）；注释/提示语。
- `Regions.java` `idOk` + `validate` 文案；`Schemes.nameOk`；其余校验器全列（OpsPage/StepEdit/Expr.java 词法）。
- 测试台：加中文命名断言组（合法/非法/边界）。
- 提示文案：`（[A-Za-z0-9_-]，1..15）` 类 → 新口径（保持短）。

### 文档
- `AGENTS.md`：「名字规则」「契约」两节更新为 v11 口径。
- 本文件（NAMING_V11_PLAN.md）实施完成后补「实施记录」小节（见 §9，已完成）。
- `AGENTS.md`「名字规则」「契约」等节已按 v11 更新（2026-10-08；用户关闭受保护文件门禁后写入）。

## 7. 验收门

**Windows（用户）**
1. 逐文件 `sh scripts/check_syntax.sh <file>`（所有改动文件绿）。
2. `sh scripts/build.sh && sh scripts/build.sh ui && VTOUCH_UI_CORE=real sh scripts/build_ui.sh`。
3. `python3 scripts/ci_check.py`；记录默认核 md5 变更（有意）。
4. 设备：`su -c 'sh /sdcard/vtouch.sh start'` → 面板起来（v11 版本对拍通过）。

**设备（助手）**
5. 独立单测（tcc harness）：`vt_id_ok`/`op_var_name_ok` 全边界用例（合法：5 汉字、15 汉字、混合、emoji 单点；
   非法：16 字符、超 47B、裸 `-`、空格、`/`、`.`、NBSP、C1、零宽、BOM、过长编码、孤立续字节、截断、代理区）。
6. 全链实测：编辑器 app 建「中文模板 + 中文变量计算步 + 中文区域/操作」→ 落盘 → 重启 → 回读 →
   触发运行 → 日志/方案切换/编辑器列表；模板/点集字节对拍。旧 ASCII 名不回归（既有测试全跑）。
7. 面板：含新汉字的区域/操作名**首次显示不豆腐**（字形重建路径）；面板重启后正常。

## 8. 风险与回滚

- 风险点：Ⅳ/Ⅴ 类漏改（CJK 被误拒）→ 靠「改完复扫为零残留」+ 单测兜底；面板字形重建稳定性 → Plan-B 兜底。
- 回滚：全部改动单主题提交（核心/面板/文档一次、app 一次），revert 即回；旧数据（ASCII 名）与旧版双向兼容
 （新版读旧文件零改动；旧版读含中文文件会拒收中文名 —— 属预期，不承诺旧版向前兼容）。

## 9. 实施记录（2026-10-08）

**核心（src/）— 完成；语法绿 + 命名单测 43/43**
- `vt_internal.h`（`VT_NAME_CHARS 15` / `VT_NAME_BUF 48` / `vt_name_scan` / `vt_name_cp_visible`）、`vt_shm.h`（版本 11u、
  名字段扩 48B、构建期断言）、`vt_region.c`（`vt_id_ok` 换 UTF-8 尺）、`vt_ops.c`（`op_var_name_ok` + casts 全改）、
  `vt_ws.c`（长度帽换 `VT_NAME_BUF-1`）、`vt_expr.h/.c`（`VT_EXPR_VAR_BUF 48`、词法非 ASCII 首字符）。
- 验证：tcc 全文件语法绿；单测 43/43（从 `vt_region.c` 逐字抽实现编译跑：合法 5/15 汉字、混合、emoji；
  非法 16 字符、超 47B、裸 `-`、空格、`/`、`.`、NBSP、C1、零宽、BOM、过长编码、孤立续字节、截断、代理区）。
- ⚠️ 本机无交叉工具链：真实构建走 §7 的 Windows 流程。

**面板（src-ui/）— 完成；`ui_stubs.c` tcc 绿；`vtouch_ui.cpp`/`ui_glue.c` 待 Windows 编译**
- 宽度全改：`[16]`→`VT_NAME_BUF`（含解析格式 `%15s`→`%47s`、`memmove` 步长、`snprintf` 尺寸、qsort 步长、`strncmp/strnlen` 帽）；
  签名单同改（`g_ope_refs` / `g_ex_undo_refs` / `(*names)[]` / `vtouch_op_put` refs 宽）。
- 校验器镜像：`ui_name_ok`（UTF-8 同核心；`id`/`scheme`/`ope`/`var`/`vis` 校验全换装）；提示文案统一新口径。
- 字形按需扩集（本仓库新增能力）：`ui_glyph_scan` / `ui_glyph_scan_all` / `ui_glyph_scan_edits` / `ui_font_rebuild`
  （帧间重建图集 + 重传纹理；防死循环 = 已尝试集；ImWchar 16 位 → 只覆盖 BMP）；`gen_ui_chars.py` 前提注释更新。

**App（vtouch-editor）— 完成；装机 + 测试台 53/53 全绿（20.8s）**
- `Regions.java`（`idOk`/`varOk`/`nameCheck`/`cpVisible` v11）、`Assets.nameOk`·`Ops.varNameOk` 委派、`RegionEdit` 校验换装、
  `Expr.java` 词法放开 ≥0x80、提示文案 12 处；测试台新增「命名 v11」检查 + 旧「素材命名规则」断言升为 v11 语义。

**待办（用户侧）**
1. Windows：`check_syntax.sh` → 默认/面板构建 → `ci_check.py`（默认核 md5 变更 = 有意，共享路径 `vt_id_ok`）；
2. 真机：核心+面板 v11 起 → 中文名不豆腐 / 编辑器全链中文命名实测；
3. ~~`AGENTS.md` v11 节更新~~ —— 已完成（门禁放行后写入，2026-10-08）。
