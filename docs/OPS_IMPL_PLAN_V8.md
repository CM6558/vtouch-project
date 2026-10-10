# OPS_IMPL_PLAN_V8 —— 操作语言化（真脚本+可视化）任务级实施计划

> 状态：**T0..T6 全部完成（2026-10-10 晚，V8 收口）**；记录 = `docs/OPS_V8_T4_ACCEPTANCE.md` / `OPS_V8_T52_ACCEPTANCE.md` / `OPS_V8_T53_MIGRATION_REPORT.md` + `OPS_V8_DOWNGRADE.md` / **`docs/OPS_V8_T6_ACCEPTANCE.md`**。剩 = 收尾评估项（v5 读路径退役 / 面板侧旗标退役——走 Windows 构建批次）+ 用户上机手测回执。拍板已齐：V8 方案 12 项**全按推荐采纳**（用户「按推荐来／先实现」）。
> 上位：`OPS_PLAN_V8.md`（方案）+ `VT_SCRIPT_V1_SPEC.md`（语言规范，语言唯一真相）。
> 执行方式：**Native 逐任务**（单设备串行；子代理会互抢设备，不用子代理制）。
> 纪律：每任务 = 实现 → 构建/离机验证 → 装机 → 测试台/真机验收 → 证据归档；每期结束跑既有 84+ 测试台与 zt 家族回归。
> 回滚点：T3 影子期内旧执行器与旧字段全保留，双跑对拍通过后才切主路径；核心/面板同构建发布。

## T0 语言规范（本批 ✅ 已完成）

- 交付：`docs/VT_SCRIPT_V1_SPEC.md` v1.0（关键字表 / 语句表 / 表达式 / 错误词表 / 文法 / §9 档位重写规则 / 示例）。
- 后续机制的**本批状态**：规范内 [对齐] 项（滑动默认时长等）在 T1.4 落地时与现编辑器 `newStep` 对齐回写。

## T1 编辑器前端（纯 Java，离机可测；~ /vtchk-host 扩展）

| 任务 | 内容 | 判据 |
|---|---|---|
| T1.1 | 词法器：token+位置+错误（保留字表、`{名}` 插值字符串、十进制/十六进制、转义集） | host 单测全绿 |
| T1.2 | 语法器 → AST（§7 文法全量；首错报 `第 N 行: <词>`） | host 单测全绿 |
| T1.3 | 静态检查：结构/名字/参数匹配/上限（§6 编译期词表全量） | 错误样例语料逐词命中 |
| T1.4 | 规范打印器（AST→文本；§8 规则；[对齐] 默认值回写规范） | parse→print→parse 等价率 100% |
| T1.5 | 语料库 ≥150 例（23 类型正向写法 + 错误例 + 往返例）＋ HostCheck 集成 | `~/vtchk-host` 全绿 |

## T2 编译器（AST → 字节码）+ 反汇编器

| 任务 | 内容 | 判据 |
|---|---|---|
| T2.1 | 字节码容器定稿：`VTBC` 魔数/版本/源哈希/常量表/名字表/行号表/指令编码（≤32KB/操作） | 格式文档 + 结构断言 |
| T2.2 | 编译：表达式栈机、语句、控制块→跳转地址、函数帧、表/串池引用、糖展开（for/插值/§9 档位重写） | 金样快照对拍 |
| T2.3 | 反汇编器（编辑器侧，人查）+ 结构自检 | 抽查 + 自检全过 |

## T3 核心 VM（C）+ 验证器 + 影子期

| 任务 | 内容 | 判据 |
|---|---|---|
| T3.1 | VM 骨架：取指/寄存器/栈/常量池/调用帧/行号；让出式 tick 整合主循环 | 宿主单测（无 Android 依赖部分） |
| T3.2 | 动作/视觉/展示指令：**复用现行函数**（从现执行器抽取） | 逐指令对拍 |
| T3.3 | 控制/调用/返回/表/字符串/短路 | 逐指令对拍 |
| T3.4 | 验证器（魔数/版本/跳转/栈深/引用界/指令数/表界）→ `程序非法` | 畸形构造 ≥30 例全拒 |
| T3.5 | 契约 v17 + 邮箱程序载荷 + `VT_EDIT_OP_PUT` 新路径；旧执行器并存开关 | 并存双跑 |

- [x] T3.6 影子期对拍（真机，核 `ebf81f93`；记录 `docs/OPS_V8_T36_ACCEPTANCE.md`）：双跑矩阵 9 场景 + 空载基线——**用时逐毫秒对齐**（点按 551=551、滑动 100≈102、按住 50=50、空转 1≈0、条件·否则 10=10、提示/框区域 0=0、嵌套循环 20=20）；找图中止词逐字一致（`无画面` ×2 复现）；空载 CPU 0.0%×3 平直（门② ≤10% ✓）；门④ 编译 0.107ms 典型 / 0.514ms 80 语句、`vt_vm_load` 载入+验证 4.6–5.1µs（目标 <10ms ✓）。局限（明示）：屏熄窗口下触摸实效不验（T3.5 屏亮已覆盖）+ 门③「找图真实抓帧 ≤1%」待屏亮窗口复测（配方已留 t36.sh）。血泪：拒收样板线复用（x>1439 竖屏逻辑域）/ pilot 命名协议（`ops-prog-<名>.vtbc`）/ 停起驱动须 pid 校验 / 证据循环须 flush+分段。

**T3 总判据（V8 §7 硬门）**：① 步骤版 vs 脚本版同场景双跑行为一致（zt 家族 + 代表性操作）→ **T3.5/T3.6 矩阵已过**；
② 空转基线波动 ≤10%；③ 找图场景端到端耗时差 ≤1%；④ 编译+验证耗时报告（典型 <10ms）。

## T4 存储 / 面板 / 热载

| 任务 | 内容 | 判据 |
|---|---|---|
| T4.1 | 编辑器：ops.conf v8（元数据 + `progs/`）+ 镜像不变量 + `.bak-v8` | 读写往返 + 镜像对账 |
| T4.2 | 面板：load/save 透传化（现 `vtouch_ui.cpp:936/:1047` 逐字段 → 不透明搬运） | 字节对拍 |
| T4.3 | 热载 T8：观察 ops.conf + progs/ → 防抖重放 | 改脚本 ~2s 生效实测 |

**T4 结论（2026-10-10 晚；记录 `docs/OPS_V8_T4_ACCEPTANCE.md`）**：
- **T4.1 编辑器存储层 ✅**：`OpsV8.java`（v8 元数据模型/解析/序列化/FNV 校验/对账比较器/`backupV8`）+ `OpsV8Check` 离机核 **38 例全绿**（`~/vtchk-host` 三连：HostCheck + VtScriptCheck + OpsV8Check）；`Ops.java` v8 记录一等公民（progHash/extra/往返保真/域校验）、`Schemes.java` progs 镜像链（mirror/switchTo/saveAs）；设备测试台 **85/0/0**（+「ops v8 存储」7 核：scratch 往返/镜像对账/篡改可见/修复/`.bak-v8` 备份/缺产物 null/真实数据只读对账）；APK v34 `108bd0e6`（装机逐字节一致；`src.bak-v34`）。首轮真雷 = `backupV8` 的 `{ [ -f ] && …; }` 组断链（修复 = 条件段 `;` 独立语句）。
- **T4.2 面板透传化 ✅**：`OPS_CONF_VER 8`（写端固定 v8、读端 v1..v5+v8）；`ops_load_prog` 不透明搬运 + FNV/大小校验（负样例按条跳过、不带走其余记录；gate `-` 归一空串真雷部署前修复）；`save_ops` 从核心原位写回 progs 字节 + 元数据行（`hr_files_note` 自写豁免）；`ui_glue` 补 `vtouch_prog_count/get/put`；scheme 层 progs 精确同步（sync_live/mirror/rollback/flush/switch 五路）+ 切换三 clear 不短路重放；T8 监视集扩 live 三文件 + 双侧 progs 目录签名 + current/ops.conf；`page_ops` 程序卡。**验收**：字节对拍过（往返后 progs `8349217d` 与 ops.conf `805e6503` 均不变）；负样例拒收词正确（`哈希不符`/`大小不符`）；镜像 live↔scheme 三对逐字节；v5 用户数据三次重启回归。部署核 `2cc6f9cb`（载荷对账 ✓；回滚备份 `ebf81f93`）。
- **T4.3 T8 热载换轨 ✅**：改脚本（双侧）→ 重载确认 **2055ms**；新脚本行为生效（`完成 851ms` vs 原 551ms 档）；**核零重启**；恢复后 552ms 回归。注意：恢复 = 元数据 + 产物成对（只改一侧 = 哈希不符假阳性）。
- **pilot 退役 ✅**：`ops-prog-*.vtbc` 扫描删除；**起跑旗标 `ops-prog.run` 保留**（真机起跑验证；供 T5 双跑）。收场：双侧还原 `666a45f9` + 试点文件清零 + 无 TRACE。
- **T4 血泪**：① 面板 ALOGI → logcat（tag `VTouchUI`）、核心 fprintf → `vt_ui_core.log`——验收取证必须**两源合并**（`t4.sh` logmark2）；② toybox sh 条件组假分支断整链；③ 元数据 gate `-` 归一空串；④ 空 progs 目录 vs 缺失（哨兵 0xFFFF0001；空↔空不触发、缺失↔空触发一次幂等）；⑤ 启动期既有静默 save_regions→mirror（T4 起多一行 progs 同步日志，非异常）。

## T5 迁移器

| 任务 | 内容 | 判据 |
|---|---|---|
| T5.1 | steps→script 翻译器（23 类型 + §9 档位重写 + 跳转兼容 + 编码解析） | 单条用例对拍 |
| T5.2 | 全方案 + zt 用例迁移，双跑对拍 | 逐条行为一致 |
| T5.3 | 全量 `.bak-v8` + 迁移报告 + 降级说明（旧端对 v8 = 整份丢弃，降级=还原备份） | 报告归档 |

**T5.1 结论（2026-10-10 晚；离机部分完成，设备双跑对拍留 T5.2）**：
- 交付：编辑器 `VtFromSteps.java`（steps→script 翻译器：23 类全量 + §9 四档重写 + 组合判定 DNF + 兼容跳转行号解析 + 模板/名字转换 + 旧未名槽按 translateOld 既有 `r+槽号` 约定自动命名〔notes 报告〕）；离机核 `VtFromStepsCheck.java` **38 例全绿**（逐字金样 + 每例译后 VtCheck/编译双自检）并接入 `~/vtchk-host/run.sh` **四连核**（HostCheck / VtScriptCheck / OpsV8Check / VtFromStepsCheck）。
- 真机语料冒烟（只读）：`VtMigSmoke`——**8/8 真实操作全译 + 编译过**（live==吕布 op1/保命装 + 干将莫邪 + 橘右京 + 默认 + 默认2；3 条含旧未名槽 → 自动命名 r2/r3）；历史 zt 家族 58 例 47 译 + 11 拒（8 = 故意的拒收测试数据正确拒收〔ztc2/4/7/11、ztd2/3、ztf2〕；3 = 「未写先读变量」语义项，口径待 T5.2 拍板）。
- 产出存档：`~/vtouch-shots/v8t5/{corpus,corpus-zt,out}`（.vts 产物）；核日志 `~/v8t5-check.log`。
- 判定口径（本轮敲定，已回核核心源码）：找色单点打包位 = 平铺步 a2 / 条件位（14·19·12m3）j1；12 模式3 = 成立即退 → `直到`；跳转 0 ≡ `终止`（不占跳数）；兼容跳转仅顶层语句起始行（预检 + VtCheck 复核）；行号解析在 VtPrint 规范归一化后仍精确（行数不变）。

**T5.2 结论（2026-10-10 晚；全链完成，记录 `docs/OPS_V8_T52_ACCEPTANCE.md`）**：
- 工具链：`VtMigrate`（纯逻辑迁移核）+ `VtMigrateRun`（树执行器：v8 表 + progs/ 产物 + 自检，面板同款 `OpsV8.checkArtifact`）+ `VtMigrateCheck` **18 例离机核**（run.sh **五连核**）+ `T52Round`（编辑器往返保真快核）。
- **逮到并修复 T3.2 起潜伏的核心真雷**：VM 视觉缝 NULL 空指针——`vm_cstr` 对「无」哨兵返回 NULL，`op_vis_region` 直解引用 → 亮屏脚本版找图必崩（SIGSEGV ×3 墓碑实锤）；修复 = `op_vis_eval` 入口 NULL→空串归一 + `op_vis_region` 容错；**新核 `3a6e723c`**（devbuild+pack_su 部署，回读一致）。
- 双跑对拍（R1 步骤 vs R2 迁移脚本，旗标+T8 热载）：9 场景 550/102/51/0/10/0/0/36/20 vs 551/101/50/7/10/0/0/55/21——**逐毫秒级一致**（zt8 find 本体 2ms=2ms = 门③ ✓ 并结清 T3 遗留小项）；迁移产 zt1.vtbc 与手写 ztp1 逐字节同（fnv=08fd5310）。
- 全方案迁移应用（R3）：5 方案 + live 逐字节审计 ✓、重启验净（op1/保命装 程序行上表、零跳过）、编辑器往返保真 ✓；回滚资产 6× `ops.conf.bak-v8`（原 v5 逐字节）+ 核副本存档。
- 开口两笔处置：①「未写先读」报告不译（真实语料 8/8 无一例，不造幽灵声明）；② 找色持续键缺口保持报告口径（零用例，未来再定 r8）。

**T5.3 结论（2026-10-10 晚；T5 收口，记录 `docs/OPS_V8_T53_MIGRATION_REPORT.md`）**：
- **全量 `.bak-v8` 核验 ✓**：6× ops + 6× regions（含 live；此前记「5× regions」系笔误），逐字节对上 T5.2 基线/现态；只读核验脚本 `~/vtouch-shots/v8t5/t53-verify.py`（可复跑）；核三件套（在跑核/载荷/本机副本）同一 md5 `3a6e723c`。
- **迁移报告**：6 位置 8 操作全部迁移、0 拒译（逐操作明细 + r2/r3 自动命名 ×3 + zt 语料 47 译 / 11 保留〔8 故意拒收 + 3「未写先读」〕+ 双跑矩阵 + 素材索引）。
- **降级说明** `docs/OPS_V8_DOWNGRADE.md`：一屏说明 + 照抄命令块（停核 → 还原 6 处 + 清 progs → md5 核对表 → 起核）+ 边界（无备份方案护栏 / 软件回退连 `vtouch.sh` / v5 存盘保持 v5〔`Ops.java:414-420`〕/ 再升回 v8 路径）；**还原序列已在副本上演练**（`t53-dryrun.txt`：md5 全对、progs 清扫、备份保留）。

## T6 双视图编辑器

| 任务 | 内容 | 判据 |
|---|---|---|
| T6.1 | AST → 块渲染（复用现卡片/块渲染链） | 渲染件 |
| T6.2 | 文本编辑器：高亮/缩进/错误行标红/行号 + 手机输入辅助条 | 上机 |
| T6.3 | 模式锁 + 转换（块→脚本恒可；脚本→块 = §8 判据） | 直驱核 |
| T6.4 | 测试台核 + 渲染件 + 手测 | 全绿 |

- **T6.1/T6.2 实现**：投影器 `VtToSteps`（固定点自检：再翻译逐字一致才放行；覆盖 = 步骤域全量〔SPEC §8 r8 覆盖清单〕；拒绝词含「仅脚本」类）/ 文本编辑器 `ScriptEdit`（高亮 = 轻扫扫描器〔保留字琥珀 / 字符串绿 / 注释灰〕；行号槽按实际 Layout 基线自绘；错误行 = 防抖 `VtCheck` → 行底标红 + 槽内红号 + 错误条〔点击跳行〕；辅助条 = 底部横滚文字键〔成对符号光标居中〕；滚动 = 外层 ScrollView 拖拽 + 光标跟随）；模式侧线 `#模式 块|脚本`（保存归一；缺省按可投影性推导）。
- **T6.3 存盘管线**：`OpsPage.commitEdit` 双路归一（脚本 = 文本原样 + 侧线；块 = 步骤 `VtFromSteps` 规范重生成）→ `VtCheck` + `VtCompiler` → `.vts` + `.vtbc` + `ops.conf` 元数据行 + 备份 + `Schemes.mirror`；删除连产物清扫；列表卡「脚本 N 行」。
- **T6 连带**：`CrossCopy` v8 补链（`Vts` 脚源口 / 脚本级依赖扫描 / 改名改写 + 重编译 + 记录哈希更新 / `Artifact` 产物清单〔原样复制‖重编译〕/ 执行期产物先落）。
- **验收（`docs/OPS_V8_T6_ACCEPTANCE.md`）**：离机 **六连核**（+`VtToStepsCheck` **66 例**；真实产物 out-mig 8 + out-mig-zt 42 件逐字往返全绿、负例 5 件仍拒）；设备测试台 **87/0/0**（APK `5ff29f71` 装机逐字节；轮1 85/0/2 = 测试代码自伤两处已修）；渲染件 `edit_script*.png` + 全量回归件；live 只读 2/2 件可投影；核心未动。
- **血泪**：`VtPrint` 打字符串优先 `raw`（AST 改写须同步 `raw`+`segs`）；EditText 拖拽 = 选字不滚动 → 外层 ScrollView + 触摸重允许 + `ensureCursorVisible`；`assertOverflow` 须豁免横滚容器子级；`raw()` 里 op 未初始化 NPE（投影器首跑即逮）。

## 里程碑与回归

- 期门映射 V8 §9（T1..T6 判据即验收门）；每期收尾 = 测试台全绿 + zt 家族回归 + 证据归档（`docs/OPS_V8_ACCEPTANCE.md` 逐期追加）。
- 风险 top（对策见 V8 §11）：双视图体验 / 验证器质量 / 迁移对拍量 / 面板改造 / 降级不可逆（靠备份+明示）。

## 当前进度

- [x] T0 语言规范 v1 定稿（修订 r1：`结束` 保留字 / 标识符不含 `-` / `插值非法`·`字符串未闭合` 词表）
- [x] T1.1 词法器（`VtTok`/`VtLexer`；离机核 `VtScriptCheck` 40 例全绿 + `--release 8` 编译门过；规范 r2 语境词/上限放开已同步）
- [x] T1.2 语法器（`VtAst`/`VtParser`；AST dump 核全绿；规范 r3：`点按 ms` 第 3 位置参 / `call·arg` 入文法 / `跳转 第N行` / 参数键支持集 / `类型错` 等）
- [x] T1.3 静态检查（`VtCheck`：名字/上下文/参数匹配/上限/静态类型；帧局部口径；规范 r4 预定义变量）
- [x] T1.4 规范打印器（`VtPrint`；规范形态逐字复现 + 幂等往返；[对齐] 默认值坐实 = 规范 r5）
- [x] T1.5 语料 161 例（A 正向全链 62 / B 错误例 45 / C 往返复现 54）+ `run.sh` 集成 —— **T1 完成**
- [x] T2.1 字节码容器定稿（`VTBC_V1.md` + `VtBc.java`：写/读/自检 + 拒收矩阵；超限词 `程序过大`；规范 r6）
- [x] T2.2 编译器（`VtOp`/`VtCompiler`：表达式栈机/语句/控制块→跳转/函数帧/糖展开；金样快照 3 + 行为断言 19 + 帧/容器/透传；**回跳标签落位修复**：`collectJumpTargets` 预扫描目标行——T3.1 夹具生成逮到「回跳编译期 跳转目标未落位」，修复后回跳/正跳均过 + 离机核 195）
- [x] T2.3 反汇编/结构自检（`VtDisasm`：render + 边界/引用界/行号自检；越界拒收矩阵）—— **T2 完成**
- [x] T3.1 核心 VM 骨架（`src/vt_vm.c`/`vt_vm.h`：载入+结构校验/指令解码走查/取指/值栈/全局槽+帧局部/表/常量池/调用帧 `CALL·RET·RETE`/行号表错误定位/短路·除零·atan2 度语义/内建/串池；动作·视觉占位按栈效应；`vt_vm_tick` 让出式 tick（WAIT 出参 ms）+ `vt_vm_run` 批模式）—— 宿主单测 **32 断言全绿**（夹具 `VtFixtureGen` 26 件：Java 编译器产物 → C VM 直跑对拍；`~/vtouch-vm-test.sh` = 夹具→`clang -Wextra -Werror`→跑）；随之并入 `~/vtouch-devbuild.sh` 全链（`vtouchd_ui` 重建 OK）。tick 主循环接线在 T3.5。
- [x] T3.2 动作/展示/视觉指令宿主化（**复用现行函数**）：`vt_vm_host` 钩子表（vt_vm.h）+ VM 多拍动作状态机（点按=down→等 ms→up；滑动=down→停1→N=max(2,ms/10) 采样 `k·ms/N−(k−1)·ms/N`→停2→up，与老执行器同时间表）；**vt_ops.c 抽取原语**：`op_toast_show`/`op_hl_region`/`op_hl_rect`（自提示/框区域/框矩形 case）、`op_region_pt_hit`/`op_toggle_state`（自区域判断/开关判断 case + `op_ctrl_eval` jt0/1，四处共用）、`op_finger_*`/`op_vis_eval` 直接复用；文件尾 `vt_ops_vm_host()` 钩子表（核心侧适配层，T3.5 接线）。宿主单测 **35 夹具 + 43 断言全绿**（逐指令 trace 契约：点按/滑动采样表/停1停2/按下弹起/提示插值文本/框区域框矩形/找图命中 fx·fy 回填/找色点色+容差/在区域/开关开/动作失败透传）—— **T3.2 完成**
- [x] T3.3 控制/调用/返回/表/字符串/短路（语义在 T3.1 落地；逐指令对拍 = 宿主夹具直跑，g_* 含短路/递归/表/插值/归位）—— **完成（宿主级）**；设备级对拍并入 T3.5 双跑
- [x] T3.4 验证器并入载入链（`vm_verify`：解码走查 + 指令数 ≤8192 + 代码 ≤32KB / 跳转与函数入口落指令边界 / 栈深数据流（种子 = main pc0 + 全部函数入口；合并点深度一致；下溢·超限拒）/ 引用界（常量种类·表·函数·实参数·槽按帧·视觉常量）/ CMP 子码·TERM 模式域 / RET·RETE 仅函数内；词 `程序非法: <由>`）—— **拒收矩阵 42 例全拒**（结构 9 + 夹具篡改 24 + 字节构造 9：含栈下溢/合并点不一致/指令数超限/代码超长/入口乱序）；宿主单测总 **85 断言 + 35 夹具全绿**
- [x] T3.5 契约 v17 + 邮箱程序载荷 + 新旧引擎并存 —— **完成（含真机双跑验收）**：契约 `VT_SHM_VERSION 17`（邮箱载荷联合化 `payload.op`/`payload.prog`；新命令 `VT_EDIT_PUT_PROG=12`/`VT_EDIT_PROG_CLEAR=13`；区 A 增程序表 `struct vt_prog progs[16]`）；核心 `vt_prog_put`（上表前 `vt_vm_load` 全量验证、按条拒收）/`vt_prog_clear`/`del` 兼删两表/单命名空间互斥；`vt_ops_run` 双表派发 → `op_vm_start`（私有快照 `g_vm_prog` + `vt_vm_set_host(vt_ops_vm_host())` + 触发数据同步进 TDX..TMS 槽）；`vt_ops_tick` VM 让出式分支（WAIT→deadline / DONE→`op_finish`（`term_mode`→中止）/ ERR→`vt_ops_abort(vm->err)`）；abort/finish 补 VM 口径（`R.vm`/`R.vm_touch` 抬指防御）；面板试点通道 `pilot_prog_check`（load_ops 收尾检查 `ops-prog.vtbc`+`ops-prog.run`〔内容=起跑名，空=ztp1；可指步骤操作 → 双跑自动化〕）。**真机验收（`docs/OPS_V8_T35_ACCEPTANCE.md`，核 `2e54d501`）**：步骤版 zt1 `完成 551ms` = 脚本版 ztp1 `启动 脚本=166B / 完成 551ms`（逐毫秒一致、连跑两轮）；坏载荷 `op 被拒 ztp1: 程序非法: 未知指令`；双侧还原 md5 逐字节。（本批修复血泪：**`vt_vm.h` 字段名 `g` 被核心宏 `#define g` 替换 → 结构体跨 TU 漂移 16KB → vt_vm.c `memset` 越界清零 `g_vm_prog`、err 偏移错位 → 运行期载入全灭；修=改名 `glob`+血泪注释，诊断链=nm 符号尺寸→objdump memset 尺寸→offsetof 双 TU 对拍。**）
- [x] T3.6 影子期双跑矩阵（见上 §T3 末条；核 `ebf81f93`；记录 `docs/OPS_V8_T36_ACCEPTANCE.md`）
- [x] T4.1 编辑器存储层（`OpsV8`/`OpsV8Check` 38 例/`Ops` 保真/`Schemes` 镜像链；设备测试台 85/0/0；APK v34 `108bd0e6`）
- [x] T4.2 面板 v8 链路（透传化 + 字节写回 + scheme 同步 + T8 监视集 + prog 接口；字节对拍/负样例/镜像对账全过；部署核 `2cc6f9cb`）
- [x] T4.3 T8 热载换轨（改脚本 → 重载 2055ms；新行为 851ms 生效；零重启）—— **T4 完成**（记录 `docs/OPS_V8_T4_ACCEPTANCE.md`）
- [x] T5.1 steps→script 翻译器（`VtFromSteps` + 38 例离机核〔run.sh 四连核〕+ `VtMigSmoke` 真实 8/8 全译；zt 家族 47 译 + 8 故意拒收 + 3 待口径）
- [x] T5.2 迁移执行 + 设备双跑对拍（`VtMigrate`/`VtMigrateRun`/`VtMigrateCheck`〔五连核〕；**修复核心 NULL 缝（VM 视觉可选串）→ 新核 `3a6e723c`**；R1/R2 矩阵 9 场景逐毫秒、门③ find 2ms=2ms；R3 全方案迁移落位 + 逐字节审计；记录 `OPS_V8_T52_ACCEPTANCE.md`）
- [x] T5.3 全量 `.bak-v8` 核验 + 迁移报告 + 降级说明（`OPS_V8_T53_MIGRATION_REPORT.md` + `OPS_V8_DOWNGRADE.md`；证据 `t53-verify.txt` / `t53-dryrun.txt`；核验脚本 `t53-verify.py`）
- [x] T6.1/T6.2 双视图落地（`VtToSteps` 投影器〔固定点自检 + 拒绝词表〕/ `ScriptEdit` 编辑器〔高亮/行号槽/错误行/辅助条/ScrollView 拖拽〕/ `ScriptStore` 侧线 + 管线；DEV 批次文档 `~/vtouch-editor/docs/UX_BATCH_v35.md`）
- [x] T6.3 模式锁 + 双路存盘（`#模式` 侧线；[转脚本] 恒可 / [转块] 子集内；打开路由 / 列表卡 / 删除清扫；CrossCopy v8 补链）
- [x] T6.4 测试台 + 渲染件 + 收口 —— 离机 **六连核**（`VtToStepsCheck` 66 例；真实产物 50 件往返全绿）；设备测试台 **87/0/0**（APK v35 `5ff29f71` 装机逐字节一致；渲染件 `edit_script*.png` 三件 + 全量回归件；live 只读 2/2 件可投影；证据 `~/vtouch-shots/v8t6/`）—— **T6 完成；V8 全部收口**。剩 = 收尾评估项（v5 读路径/旗标退役）+ 用户上机手测回执。
