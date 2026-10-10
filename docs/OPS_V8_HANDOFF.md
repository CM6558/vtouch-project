# OPS_V8 交接单（T6 续接版）

> 给下一个会话：V8「操作语言化」**T0..T5.3 全部完成**（含核心 NULL 缝修复 + 全方案迁移落位 + 备份全量核验 + 迁移报告/降级说明），**从这里续接 T6（双视图编辑器）**。
> 开工先读：本单 + 技能 `vtouch-project` + `docs/OPS_PLAN_V8.md` / `OPS_IMPL_PLAN_V8.md` + `docs/OPS_V8_T53_MIGRATION_REPORT.md`（迁移归档）+ `docs/OPS_V8_DOWNGRADE.md`（降级口径）。

## 现状快照（2026-10-10 晚）

- **设备核 `3a6e723c`**（T5.2 修复版；devbuild + `pack_su.py` 部署，`/sdcard/vtouch.sh` 已更新、安装回读 md5 一致）；面板正常；**无 TRACE**；起跑旗标 `ops-prog.run` 已清；屏幕超时 = 120000（已还原）。
- **用户数据 = 迁移终态（v8）**（2026-10-10 18:04 复核仍逐字节一致）：
  - live == `schemes/吕布` = `a200d4b9`〔op1 程序=500B fnv=4de7db61 / 保命装 程序=1874B fnv=7c8de217〕；
  - 干将莫邪 / 默认 / 默认2 = `b12b1639`〔op1 程序〕；橘右京 = `65cda445`〔op1 程序〕；
  - `progs/` 两侧同内容（全量逐字节审计过）；重启验净 = 程序行上表、零跳过/拒收。
- **回滚资产（T5.3 全量核验 ✓**，证据 `~/vtouch-shots/v8t5/t53-verify.txt`，脚本 `t53-verify.py` 可复跑）：6× `ops.conf.bak-v8` + 6× `regions.conf.bak-v8`（原始 v5 逐字节；regions 侧与现态一致）+ 核副本 `~/vtouch-shots/v8t5/vtouchd_ui-3a6e723c`；降级步骤 = `docs/OPS_V8_DOWNGRADE.md`（副本演练 `t53-dryrun.txt`）。
- 编辑器 v34 `108bd0e6`（未动；对 v8 产物「解析→序列化」逐字节保真已快核 `T52Round`）。

## T5 迁移器（已完成：T5.1 + T5.2 + T5.3）

- [x] **T5.1 steps→script 翻译器**（`VtFromSteps` + 38 例 + `VtMigSmoke`；真实 8/8 全译；产物 `~/vtouch-shots/v8t5/`）。
- [x] **T5.2 迁移执行 + 双跑对拍**（详见 `docs/OPS_V8_T52_ACCEPTANCE.md`；工具 `VtMigrate.java`〔编辑器侧纯逻辑〕+ `VtMigrateRun.java`〔vtchk-host 树执行器〕+ `VtMigrateCheck` 18 例〔run.sh 五连核〕；驱动 `~/vtouch-shots/v8t5/t52.sh`）。
- [x] **T5.3 迁移报告 + 降级说明 + 全量 `.bak-v8` 核验**：报告 `docs/OPS_V8_T53_MIGRATION_REPORT.md`（6 位置 8 操作 0 拒译；r2/r3 notes；zt 47/11 审计）；降级 `docs/OPS_V8_DOWNGRADE.md`（一屏 + 照抄命令 + md5 核对表；副本演练）；核验脚本 `~/vtouch-shots/v8t5/t53-verify.py`。
- 编辑器内「迁移报告」查看入口 = 可选项，归 T6（不单开批）。

## T6 双视图编辑器（下一批，从这里开码）

- [ ] **T6.1 AST → 块渲染**（复用现卡片/块渲染链）。
- [ ] **T6.2 文本编辑器**：高亮 / 缩进 / 错误行标红 / 行号 + 手机输入辅助条。
- [ ] **T6.3 模式锁 + 转换**：块→脚本恒可；脚本→块 = V8 §8 判据。
- [ ] **T6.4 测试台核 + 渲染件 + 手测**。**UI 类改动必须走 `impeccable` 技能流程；功能类先加载 `brainstorming`。**
- 过渡口径：v5 步骤读路径保留至 T6 结束后评估退役；`VTOUCH_UI_OPS_SAVE_TEST` 验收钩与起跑旗标退役点 = T6 验收后评估。

## 设备与部署口径

- 部署链：`sh ~/vtouch-devbuild.sh` → `python3 scripts/pack_su.py` → `su cp build/vtouch.sh /sdcard/vtouch.sh`（**必须先更新 sdcard 脚本再 start**——否则被内嵌旧载荷反向覆盖，血泪）→ `su -c 'sh /sdcard/vtouch.sh start'`（TRACE：`su -c 'VTOUCH_OPS_TRACE=1 sh /sdcard/vtouch.sh start'`）。
- 核回滚备份：`/data/local/tmp/vtouchd_ui.bak-ebf81f93`（T3.6 版）+ 现版副本 `~/vtouch-shots/v8t5/vtouchd_ui-3a6e723c`。
- 编辑器侧（`~/vtouch-editor`，非 git）改动经 APK 构建/测试台验证；改动前备份 `src.bak-*`（当前 v34）。

## 工具索引（T5.3 更新）

| 工具 | 用途 |
|---|---|
| `~/vtchk-host/run.sh` | 离机核**五连**（HostCheck + VtScriptCheck + OpsV8Check + VtFromStepsCheck + VtMigrateCheck）；改 `~/vtouch-editor` 的 java 后先 cp 过去再跑 |
| `python3 ~/vtouch-shots/v8t5/t53-verify.py` | 降级资产全量核验（只读；.bak-v8 / 语料 / 核三件套对账，可复跑） |
| `java -cp ~/vtchk-host com.vtouch.editor.VtMigrateRun <语料目录> <输出目录>` | 迁移器树执行（v8 表 + progs/ + 自检 + report.txt） |
| `sh ~/vtouch-shots/v8t5/t52.sh gate\|prep\|inject\|restart\|run <名>\|push <目录> <方案>\|cleanup` | T5.2 验收驱动（旗标起跑 + 双侧推送 + 收场） |
| `~/vtouch-shots/v8t4/t4.sh` | T4 验收驱动（v8 存储链路回归用；logmark2 双源日志） |
| `sh ~/vtouch-vm-test.sh` | VM 宿主单测（夹具→clang→test_vt_vm；85 断言） |
| `sh ~/vtouch-devbuild.sh` / `sh ~/vtouch-ui-check.sh` | 设备侧全链构建 / 面板语法门 |
| `python3 scripts/pack_su.py` → `su cp build/vtouch.sh /sdcard/vtouch.sh` → `sh /sdcard/vtouch.sh start` | **部署核心的唯一正确路径**（防载荷反向覆盖） |
| `python3 scripts/apply_funcdoc.py --check` | 核心函数文档门（0 调整才算过） |

## 纪律与坑（沿用 + T5.3 新增）

1. **前台闸门**：动作前 `dumpsys window | grep mCurrentFocus`——`launcher/vtouch/hermescontrol` 可动；视频/阅读/通知栏 = 停；检查只报告、与动作不串一条链。
2. **屏幕**：`Dozing` = 不唤醒、不注入；注入类验收要亮屏窗口且用户在场/离机静止。
3. **T8 渲染拍依赖触摸**：外改盘后需一次触摸（或重启核）才重放；息屏下用**重启式驱动**。
4. `su -c pidof vtouchd_ui` 才有真值；停起驱动必须校验**新 pid ≠ 旧 pid**。
5. **抄历史载荷先核对其「原预期结果」**；条件点坐标域 = 竖屏逻辑 0..1439。
6. `/tmp` 不可写；`su` 下 toybox `grep` 用 `-E`（无 `\|`）；长循环分段跑 + 每轮 flush。
7. **跨 TU 结构体字段名避开核心宏**（`g` 事故；新增字段加血泪注释）。
8. 试点扫描协议已退役（T4）；起跑旗标 `ops-prog.run` 内容 = 起跑名（指已删操作会正确 `op 丢弃 操作不存在`）。
9. **验收收场三件套**：双侧还原 / 试点清零 / 无 TRACE 净重启（+ 屏幕超时还原）。
10. 用户会随时上机改数据；动手前对现状，发现陌生改动按现状续接。
11. **日志双源**：面板 ALOGI → logcat（tag `VTouchUI`）；核心 fprintf → `/data/local/tmp/vt_ui_core.log`（**注意不是运行目录里的同名文件**）。
12. **toybox sh 条件组**：`{ [ -f x ] && …; }` 假分支断整条 `&&` 链 → 条件段用 `;` 独立语句。
13. **v8 元数据 gate `-` 归一空串**；恢复类操作要元数据 + 产物成对。
14. **空 progs 目录 vs 缺失**：哨兵 `0xFFFF0001` 区分（空↔空不触发重载）。
15. 启动期静默 save_regions→mirror 是既有行为；多一行「progs 同步」不是假象。
16. **[T5.2] 「核心心跳停滞 3s → 自杀退出」≠ 一定是挂死——先查崩溃**：`logcat -b crash -d`（SIGSEGV 墓碑）+ `/data/tombstones/`；判活/判死的线程级取证 = `/proc/<pid>/task/*/{stat,wchan,syscall}` 轮采（主线程 `do_sys_poll` 正常 poll + 进程随后消失 = 崩溃而非卡死）。
17. **[T5.2] VM↔执行器字符串缝**：`vm_cstr` 对「无」哨兵（-1）返回 **NULL**，老执行器路径恒非 NULL（空=空串指针）——任何接收 VM 可选串的 C 层函数都必须容忍 NULL（`op_vis_eval` 入口已归一；新增此类调用点照此纪律，别重演空指针）。
18. **[T5.2] 迁移/批量改盘的备用集**：`.bak-v8` = **原始 v5** 快照（不是前一版），降级/回滚用它；`~/vtouch-shots/v8t5/.bakv8-md5` 有基线。**[T5.3 修正] 实际计数 = 6 ops + 6 regions（含 live）**；对账脚本 `t53-verify.py` 可随时复跑。
19. **[T5.3] 降级 = 先还原数据（`.bak-v8` → `ops.conf` + 清 `progs/`）、后换软件**；命令 / 核对表 / 边界（无备份方案护栏、软件回退须连 `vtouch.sh`）见 `docs/OPS_V8_DOWNGRADE.md`。**改任何备份/还原逻辑前先读它 + 先跑副本演练。**
20. **[T5.3] 编辑器存盘按文件现有版本回写**：v5 保持 `#vtouch-ops v5`、无 prog 元数据不写（`Ops.java:414-420`）——降级后的 v5 数据在现版编辑器里编辑不会自动升回 v8。
