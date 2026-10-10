# OPS_V8 交接单（剩余任务 T4..T6；2026-10-10 会话收尾时状态）

> 给下一个会话：V8「操作语言化」已做完 T0..T3，**从这里续接 T4**。
> 开工先读：`skill vtouch-project`（V8 全链 + 血泪）→ `docs/OPS_PLAN_V8.md`（方案：§5 载体 / §7 硬门 / §8 双视图判据 / §9 期门 / §11 风险）→ `docs/OPS_IMPL_PLAN_V8.md`（进度板）→ `docs/VT_SCRIPT_V1_SPEC.md`（语言唯一规范 r1..r7）→ `docs/VTBC_V1.md` → 验收档 `docs/OPS_V8_T35_ACCEPTANCE.md`、`docs/OPS_V8_T36_ACCEPTANCE.md`。

## 一句话现状

T0..T3 完成：语言规范定稿、编辑器前端（词法/语法/检查/打印/编译器/反汇编，离机核 195 全绿）、核心 VM+验证器（宿主 85 断言+35 夹具全绿）、契约 v17（邮箱程序载荷 + 程序表 + 双引擎并存）、影子期真机双跑矩阵（9 场景逐毫秒对齐 + 空载 0% + 门④ 基准）。**设备正跑 v17 核 `ebf81f93`，用户数据双侧 md5 `666a45f9`（未动），pilot 试点件已清、无 TRACE。**

## 剩余任务清单

### T3 遗留（一件小测，可随缘补）

- [ ] **门③屏亮复测**：亮屏 + 前台可动作窗口时，跑一对找图（zt8/ztp8 或等效），取**真实抓帧+扫描**的端到端耗时差（判据 ≤1%）。配方/载荷全部就位：`~/vtouch-shots/v8pilot/`（t36-steps.txt、ztp8.vtbc、`ops-prog-<名>.vtbc` 命名协议、t36.sh 驱动）。息屏下只能拿到 `无画面` 路径（T3.6 已记）。

### T4 存储 / 面板 / 热载（下一批从这里开码）

- [ ] **T4.1 编辑器：ops.conf v8**（元数据 + `progs/` 旁路：`<op>.vts` 源 + `<op>.vtbc` 编译产物）+ 镜像不变量（`live == schemes/<current>`）+ 迁移/改动前 `.bak-v8`。判据：读写往返 + 镜像对账（`~/vtchk-host` 离机核先行 + 设备实测）。
- [ ] **T4.2 面板透传化**：`src-ui/vtouch_ui.cpp` 的 load_ops/save_ops 由「逐字段解析步骤」改为**不透明搬运**（读 v8：`progs/*.vtbc` 原样投 `VT_EDIT_PUT_PROG`；不再认识步骤字段语义）。判据：字节对拍（同一操作 vtbc md5 经面板往返不变）。**动面板必跑 `sh ~/vtouch-ui-check.sh`。**
- [ ] **T4.3 T8 热载换轨**：观察 `ops.conf + progs/`（mtime 轮询 1s + 防抖 600ms）→ 清空重放（PUT_PROG）。判据：改脚本 ~2s 生效实测（注意 T8 渲染拍依赖触摸，见坑 3）。
- T4 完成后：**pilot 通道（`ops-prog-*.vtbc` 扫描）退役**（正式链路已由 T4.2 提供）。

### T5 迁移器

- [ ] **T5.1 steps→script 翻译器**：23 类步骤 + V8 §9 档位重写规则 + 跳转/块配对 + 负编码/字段解析 → vt-script 源（产 `.vts`，经 `VtCompiler` 编译校验；不可译项出报告）。
- [ ] **T5.2 全方案 + zt 用例迁移，双跑对拍**：迁移后脚本版 vs 迁移前步骤版逐条行为一致（复用 T3.5/T3.6 矩阵法与工具）。
- [ ] **T5.3 全量 `.bak-v8` + 迁移报告 + 降级说明**（旧端对 v8 = 整份丢弃；**降级 = 备份还原**，须向用户明示）。

### T6 双视图编辑器

- [ ] **T6.1 AST → 块渲染**（复用现卡片/块渲染链）。
- [ ] **T6.2 文本编辑器**：高亮 / 缩进 / 错误行标红 / 行号 + 手机输入辅助条。
- [ ] **T6.3 模式锁 + 转换**：块→脚本恒可；脚本→块 = V8 §8 判据。
- [ ] **T6.4 测试台核 + 渲染件 + 手测**。**UI 类改动必须走 `impeccable` 技能流程；功能类先加载 `brainstorming`。**

## 设备与部署口径（交接时点）

- 设备核 = v17 `ebf81f93`（含 pilot v3 多脚本扫描）；回滚备份 `/sdcard/vtouch.sh.bak-v8pilot`。
- 部署链：`sh ~/vtouch-devbuild.sh` → `python3 scripts/pack_su.py` → `su -c 'sh /sdcard/vtouch.sh stop'` → `su cp build/vtouchd_ui /data/local/tmp/` + `su cp build/vtouch.sh /sdcard/vtouch.sh && chmod 755` → `start`（需 TRACE 时 `su -c 'VTOUCH_OPS_TRACE=1 sh /sdcard/vtouch.sh start'`）。
- **`/sdcard/vtouch.sh` 是用户真实入口**：start 先按 md5 对账，不符会用**内嵌载荷反向覆盖**——只 cp 新核、不更新 sdcard 脚本 = 修改静默不生效（血泪，必防）。
- 编辑器侧（`~/vtouch-editor`，非 git）改动经 APK 构建/测试台验证；改动前备份 `src.bak-*`。

## 工具索引

| 工具 | 用途 |
|---|---|
| `~/vtchk-host/run.sh` | 语言核离机跑（HostCheck + VtScriptCheck）；**改 `~/vtouch-editor` 的 Vt*.java 后先 cp 过去再跑** |
| `sh ~/vtouch-vm-test.sh` | VM 宿主单测（夹具→clang→test_vt_vm；EXTRA 模式 `./build/test_vt_vm build/vmfix <名>` 看任意夹具轨迹） |
| `build/bench_load <blob>` / `java -cp ~/vtchk-host com.vtouch.editor.VtPerf` | 门④ 基准（载入验证 µs / 编译 ms） |
| `sh ~/vtouch-devbuild.sh` / `sh ~/vtouch-ui-check.sh` | 设备侧全链构建 / 面板语法门 |
| `~/vtouch-shots/v8pilot/{pilot.sh,t36.sh}` | 真机验收驱动（gate/prep/start/run/bad/read/clean；载荷+脚本同目录，命名协议 `ops-prog-<名>.vtbc`） |
| `python3 scripts/apply_funcdoc.py --check` | 核心函数文档门（0 调整才算过） |
| `bash ~/vtouch-editor/build.sh` + `~/vtouch-editor-run-v33.sh` 模式 | 编辑器 APK 构建/测试台（保活一条龙脚本照抄改档） |

## 纪律与坑（沿用 + 本会话新增）

1. **前台闸门**：动作前 `dumpsys window | grep mCurrentFocus`——`launcher/vtouch/hermescontrol` 可动；视频/阅读/**通知栏** = 停；检查只报告、与动作不串一条链。
2. **屏幕**：`Dozing` = 不唤醒、不注入（注入类验收要亮屏窗口且用户在场/离机静止）。
3. **T8 渲染拍依赖触摸**：外改盘后需一次触摸（或重启核）才重放；息屏下无渲染拍 → 用**重启式驱动**（改文件 → stop/start，启动导入即重放）。
4. `su -c pidof vtouchd_ui` 才有真值（Termux 裸 pidof 假阴性）；停起驱动必须校验**新 pid ≠ 旧 pid**（否则静默没重启、日志累积假象）。
5. **抄历史载荷先核对其「原预期结果」**——曾把拒收样板线当正例（条件点坐标域 = 竖屏逻辑 0..1439）。
6. `/tmp` 不可写（重定向进 `$HOME`）；`su` 下 toybox `grep` 无 `\|`（用 `-E`）；execute_code 长循环**每轮 flush + 分段跑**（300s 上限，超时证据会丢）。
7. **跨 TU 结构体字段名避开核心宏**（`vt_vm.h` 的 `g` 被 `#define g (*g_ptr)` 替换 → 结构体漂移 16KB 的大事故；新增字段照此纪律 + 血泪注释在头文件里）。
8. 试点文件命名协议：`ops-prog-<名>.vtbc`（前缀）；裸 `ops-prog.vtbc` = 名 `ztp1`；`ops-prog.run` 内容 = 起跑名。
9. **验收收场三件套**：双侧还原（备份 md5 对账）+ 试点文件清零 + 无 TRACE 净重启；屏幕超时若有改动一并还原。
10. 用户会随时上机改数据；动手前对现状（mtime/存档序列/技能新条目），发现陌生改动按现状续接。
