# OPS_V8 交接单（剩余任务 T5..T6 + T3 遗留门③；2026-10-10 T4 完成时状态）

> 给下一个会话：V8「操作语言化」已做完 T0..T4 + T5.1（翻译器·离机），**从这里续接 T5.2（迁移执行 + 设备双跑对拍）**。
> 开工先读：`skill vtouch-project`（V8 全链 + 血泪）→ `docs/OPS_PLAN_V8.md`（方案：§5 载体 / §7 硬门 / §8 双视图判据 / §9 期门 / §11 风险）→ `docs/OPS_IMPL_PLAN_V8.md`（进度板）→ `docs/VT_SCRIPT_V1_SPEC.md`（语言唯一规范 r1..r7）→ `docs/VTBC_V1.md` → `docs/VTSTORE_V8.md`（存储 v8 唯一规范）→ 验收档 `docs/OPS_V8_T35_ACCEPTANCE.md` / `docs/OPS_V8_T36_ACCEPTANCE.md` / `docs/OPS_V8_T4_ACCEPTANCE.md`。

## 一句话现状

T0..T4 完成：语言规范定稿、编辑器前端（词法/语法/检查/打印/编译器/反汇编，离机核 195 全绿）、核心 VM+验证器（宿主 85 断言+35 夹具全绿）、契约 v17（邮箱程序载荷 + 程序表 + 双引擎并存）、影子期真机双跑矩阵（9 场景逐毫秒对齐 + 空载 0% + 门④ 基准）、**T4 存储 v8 全链**（编辑器元数据+progs 旁路、面板透传/字节写回/FNV 校验、T8 热载换轨实测、pilot 扫描退役）。
**T5.1 翻译器完成**（steps→script 23 类全量；离机核 38 例；真实 8/8 全译；工具/产物 `~/vtouch-shots/v8t5/`）。**设备正跑 v17+T4 核 `2cc6f9cb`；用户数据双侧 md5 `666a45f9`（未动）；编辑器 v34 `108bd0e6`；`progs/` 两侧为空目录（=无产物）；起跑旗标 `ops-prog.run` 机制保留（当前无文件）；无 TRACE。**

## 剩余任务清单

### T3 遗留（一件小测，可随缘补）

- [ ] **门③屏亮复测**：亮屏 + 前台可动作窗口时，跑一对找图（zt8/ztp8 或等效），取**真实抓帧+扫描**的端到端耗时差（判据 ≤1%）。
  - ⚠ 配方需按 T4 起新通道调整：**脚本侧投件 = 写 `progs/ztp8.vtbc`（双侧）+ ops.conf 元数据行（FNV/大小现算）+ 起跑旗标**；`t36.sh` 的 `ops-prog-*.vtbc` 扫描协议已退役（T4.2）。载荷仍取 `~/vtouch-shots/v8pilot/`；写法参考 `~/vtouch-shots/v8t4/t4.sh` 的 `prep`/`variant`。
  - 息屏下只能拿到 `无画面` 路径（T3.6 已记）。

### T4（已完成，供回归对照）

- 记录 `docs/OPS_V8_T4_ACCEPTANCE.md`；驱动/载荷 `~/vtouch-shots/v8t4/`（t4.sh + `ztp1/ztp1v` + t4-run.log / core.log / panel-logcat.txt）。
- **过渡口径**：v5 步骤读路径保留（T5 迁移完成前）；`VTOUCH_UI_OPS_SAVE_TEST` 验收钩与起跑旗标退役点 = T5 验收后评估。

### T5 迁移器（下一批从这里开码）

- [x] **T5.1 steps→script 翻译器**（`VtFromSteps.java` + `VtFromStepsCheck` 38 例〔run.sh 四连核〕 + `VtMigSmoke`；真实 8/8 全译+编译过；旧未名槽按 `r+槽号` 自动命名并出 notes；产物 `~/vtouch-shots/v8t5/`）。
- [ ] **T5.2 全方案 + zt 用例迁移，双跑对拍**：迁移后脚本版 vs 迁移前步骤版逐条行为一致（复用 T3.5/T3.6 矩阵法；脚本版起跑走**起跑旗标**现成自动化）。**开码前先拍板两笔开口**：①「未写先读变量」口径（旧=运行期 `结果无值` 中止；脚本编译期无未定义名 → 幽灵声明保语义 or 从迁移集合排除）；② 找色/找色点**持续查找**（ms>0）无参数键——遇此类 op 先报告拒绝，动语言需规范 r8。
- [ ] **T5.3 全量 `.bak-v8` + 迁移报告 + 降级说明**（`VtMigSmoke` 的 notes/problems 即报告素材；旧端对 v8 = 整份丢弃；**降级 = 备份还原**，须向用户明示）。

### T6 双视图编辑器

- [ ] **T6.1 AST → 块渲染**（复用现卡片/块渲染链）。
- [ ] **T6.2 文本编辑器**：高亮 / 缩进 / 错误行标红 / 行号 + 手机输入辅助条。
- [ ] **T6.3 模式锁 + 转换**：块→脚本恒可；脚本→块 = V8 §8 判据。
- [ ] **T6.4 测试台核 + 渲染件 + 手测**。**UI 类改动必须走 `impeccable` 技能流程；功能类先加载 `brainstorming`。**

## 设备与部署口径（T4 收尾时点）

- 设备核 = v17+T4 `2cc6f9cb`；回滚备份 `/data/local/tmp/vtouchd_ui.bak-ebf81f93` + `/sdcard/vtouch.sh.bak-t4-pre`。
- 部署链：`sh ~/vtouch-devbuild.sh` → `python3 scripts/pack_su.py` → `su -c 'sh /sdcard/vtouch.sh stop'` → `su cp build/vtouchd_ui /data/local/tmp/` + `su cp build/vtouch.sh /sdcard/vtouch.sh && chmod 755` → `start`（需 TRACE 时 `su -c 'VTOUCH_OPS_TRACE=1 sh /sdcard/vtouch.sh start'`）。
- **`/sdcard/vtouch.sh` 是用户真实入口**：start 先按 md5 对账，不符会用**内嵌载荷反向覆盖**——只 cp 新核、不更新 sdcard 脚本 = 修改静默不生效（血泪，必防）。
- 编辑器侧（`~/vtouch-editor`，非 git）改动经 APK 构建/测试台验证；改动前备份 `src.bak-*`（当前 v34）。

## 工具索引

| 工具 | 用途 |
|---|---|
| `~/vtchk-host/run.sh` | 离机核三连（HostCheck + VtScriptCheck + **OpsV8Check**）；改 `~/vtouch-editor` 的 java 后先 cp 过去再跑 |
| `sh ~/vtouch-vm-test.sh` | VM 宿主单测（夹具→clang→test_vt_vm；EXTRA 模式 `./build/test_vt_vm build/vmfix <名>` 看任意夹具轨迹） |
| `build/bench_load <blob>` / `java -cp ~/vtchk-host com.vtouch.editor.VtPerf` | 门④ 基准（载入验证 µs / 编译 ms） |
| `sh ~/vtouch-devbuild.sh` / `sh ~/vtouch-ui-check.sh` | 设备侧全链构建 / 面板语法门（动面板必跑） |
| `~/vtouch-shots/v8t4/t4.sh` | **T4 验收驱动**（gate/deploy/start/prep/run/variant/neg/negsz/fix/savert/read/clean；日志两源合并 logmark2） |
| `~/vtouch-shots/v8t4/{ztp1,ztp1v}.{vts,vtbc}` | v8 验收载荷（原版 551ms / 变体 851ms） |
| `~/vtouch-shots/v8pilot/{pilot.sh,t36.sh}` | 双跑矩阵驱动（**扫描协议已退役**——t36 需改用 progs/ 通道，见门③条） |
| `python3 scripts/apply_funcdoc.py --check` | 核心函数文档门（0 调整才算过） |
| `bash ~/vtouch-editor/build.sh` + `~/vtouch-editor-run-v34.sh` | 编辑器 APK 构建/测试台（保活一条龙脚本照抄改档） |

## 纪律与坑（沿用 + T4 新增）

1. **前台闸门**：动作前 `dumpsys window | grep mCurrentFocus`——`launcher/vtouch/hermescontrol` 可动；视频/阅读/**通知栏** = 停；检查只报告、与动作不串一条链。
2. **屏幕**：`Dozing` = 不唤醒、不注入（注入类验收要亮屏窗口且用户在场/离机静止）。
3. **T8 渲染拍依赖触摸**：外改盘后需一次触摸（或重启核）才重放；息屏下无渲染拍 → 用**重启式驱动**（改文件 → stop/start，启动导入即重放）。
4. `su -c pidof vtouchd_ui` 才有真值（Termux 裸 pidof 假阴性）；停起驱动必须校验**新 pid ≠ 旧 pid**（否则静默没重启、日志累积假象）。
5. **抄历史载荷先核对其「原预期结果」**——曾把拒收样板线当正例（条件点坐标域 = 竖屏逻辑 0..1439）。
6. `/tmp` 不可写（重定向进 `$HOME`）；`su` 下 toybox `grep` 无 `\|`（用 `-E`）；execute_code 长循环**每轮 flush + 分段跑**（300s 上限，超时证据会丢）。
7. **跨 TU 结构体字段名避开核心宏**（`vt_vm.h` 的 `g` 被 `#define g (*g_ptr)` 替换 → 结构体漂移 16KB 的大事故；新增字段照此纪律 + 血泪注释在头文件里）。
8. 试点文件命名协议：`ops-prog-<名>.vtbc`（前缀）——**仅存于 t36 时代**（T4 起改走 `progs/` 通道）；起跑旗标 `ops-prog.run` 内容 = 起跑名。
9. **验收收场三件套**：双侧还原（备份 md5 对账）+ 试点文件清零 + 无 TRACE 净重启；屏幕超时若有改动一并还原。
10. 用户会随时上机改数据；动手前对现状（mtime/存档序列/技能新条目），发现陌生改动按现状续接。
11. **日志双源（T4 血泪）**：面板 ALOGI → **logcat（tag `VTouchUI`）**；核心/胶水 fprintf → `/data/local/tmp/vt_ui_core.log`。面板侧证据（重载/上表/跳过/ops-test）只看核日志会全丢——验收脚本两源合并（`t4.sh` 的 `logmark2`/`plog`）。
12. **toybox sh 条件组（T4 血泪）**：`{ [ -f x ] && …; }` 假分支会**中断整条 `&&` 链**（不是只跳过该组）→ 条件段用 `;` 独立语句（backupV8 首轮被测试台逮到）。
13. **v8 元数据 gate `-` 归一空串**（核心按「门控非法」拒收）；**恢复类操作要元数据 + 产物成对**（只改一侧 = 哈希不符假阳性，`t4.sh fix` 首版即栽）。
14. **空 progs 目录 vs 缺失**：目录签名用哨兵 `0xFFFF0001` 区分；空↔空两侧相等不触发重载、缺失↔空触发一次（幂等无害）。
15. **启动期静默 save_regions→mirror** 是既有行为（无成功日志）；T4 起多一行「progs 同步 拷 1」——不是 T4 引入的假象。
