# OPS_V8 交接单（V8 完成版）

> 给下一个会话：V8「操作语言化」**T0..T6 全部完成**（语言前端/编译/VM/契约/迁移/双视图编辑器），V8 收口。剩余 = 收尾评估项（见末节）+ 用户上机手测回执。
> 开工先读：本单 + 技能 `vtouch-project` + `docs/OPS_PLAN_V8.md` / `OPS_IMPL_PLAN_V8.md` + `docs/VT_SCRIPT_V1_SPEC.md`（r8 覆盖清单）+ `docs/OPS_V8_T6_ACCEPTANCE.md`（T6 验收）。

## 现状快照（2026-10-10 晚，T6 收尾）

- **设备核 `3a6e723c`**（T5.2 修复版；安装回读一致）；面板正常；无 TRACE；起跑旗标已清；屏幕超时 120000。
- **用户数据 = 迁移终态（v8）**：live == `schemes/吕布`（`a200d4b9`），其余四方案 `b12b1639` / `65cda445`；progs 双侧同内容；T6 起 live progs **可被编辑器双向编辑**（脚本/块两路保存 = .vts + .vtbc + 元数据）。
- **编辑器 v35**（md5 `5ff29f71…`〔装机逐字节；测试台 **87/0/0**〕；`out/vtouch-editor-v35.apk`；备份 `src.bak-v35`）：双视图（块/脚本）+ 模式侧线 `#模式` + 跨方案复制 v8 补链。
- **回滚资产**（T5.3 全量核验 ✓）：6× `ops.conf.bak-v8` + 6× `regions.conf.bak-v8` + 核副本；降级步骤 = `docs/OPS_V8_DOWNGRADE.md`（副本演练过）。

## T6 双视图编辑器（已完成）

- [x] **T6.1 AST → 块渲染**：`VtToSteps`（脚本→步骤投影，固定点自检；覆盖 = 迁移域全量）。见 `UX_BATCH_v35.md` §3/§4。
- [x] **T6.2 文本编辑器**：`ScriptEdit`（高亮/行号槽/错误行/辅助条；外层 ScrollView 拖拽滚动 + 光标跟随）。
- [x] **T6.3 模式锁 + 转换**：`#模式 块/脚本` 侧线（保存定锁）；[转脚本] 恒可 / [转块] = 子集内（SPEC §8 r8）。
- [x] **T6.4 测试台核 + 渲染件 + 上机**：见 `docs/OPS_V8_T6_ACCEPTANCE.md`（六连核 + 测试台 + 渲染件三件 + live 只读核）；**用户上机手测回执挂账**。
- 编辑器内「迁移报告」查看入口 = 未做（可选；迁移归档见 `OPS_V8_T53_MIGRATION_REPORT.md`）。

## 收尾评估项（T6 验收后，面板/核心侧）

1. **v5 步骤读路径退役评估**：面板/核心仍支持 v5 读（防御性保留）。降级路径（`OPS_V8_DOWNGRADE.md`）依赖「旧端对 v8 整份丢弃」——退役前确认无回退需求。
2. **`VTOUCH_UI_OPS_SAVE_TEST` 验收钩 + 起跑旗标 `ops-prog.run` 退役点**：均在面板侧，退役走 Windows 构建批次（本机 devbuild 也可验证）。
3. **块视图覆盖扩展**（函数/表/返回/调用/当/for）：脚本侧全量可用；块侧按批补 UI（`VtToSteps` 加映射 + StepEdit 加卡型）。

## 设备与部署口径

- 部署链：`sh ~/vtouch-devbuild.sh` → `python3 scripts/pack_su.py` → `su cp build/vtouch.sh /sdcard/vtouch.sh`（**必须先更新 sdcard 脚本再 start**——否则被内嵌旧载荷反向覆盖，血泪）→ `su -c 'sh /sdcard/vtouch.sh start'`。
- 核回滚备份：`/data/local/tmp/vtouchd_ui.bak-ebf81f93` + `~/vtouch-shots/v8t5/vtouchd_ui-3a6e723c`。
- 编辑器侧（`~/vtouch-editor`，非 git）改动经 APK 构建/测试台验证；改动前备份 `src.bak-*`（当前 v35）。

## 工具索引（T6 更新）

| 工具 | 用途 |
|---|---|
| `~/vtchk-host/run.sh` | 离机核**六连**（HostCheck + VtScriptCheck + OpsV8Check + VtFromStepsCheck + VtMigrateCheck + **VtToStepsCheck**）；改 `~/vtouch-editor` 的 java 后先 cp 过去再跑 |
| `sh ~/vtouch-editor-run-v35.sh` | 编辑器测试台一条龙（保活 + ChipService + 等报告）；跑前查前台（规则见纪律#1） |
| `su -c "sh ~/vtouch-shots-pull.sh <tag>"` | 拉测试台产物（**新渲染件先补清单**） |
| `python3 ~/vtouch-shots/v8t5/t53-verify.py` | 降级资产全量核验（只读，可复跑） |
| `java -cp ~/vtchk-host com.vtouch.editor.VtMigrateRun <语料目录> <输出目录>` | 迁移器树执行 |
| `sh ~/vtouch-vm-test.sh` | VM 宿主单测（夹具→clang→test_vt_vm） |
| `sh ~/vtouch-devbuild.sh` / `sh ~/vtouch-ui-check.sh` | 设备侧全链构建 / 面板语法门 |
| `python3 scripts/pack_su.py` → `su cp build/vtouch.sh /sdcard/vtouch.sh` → `sh /sdcard/vtouch.sh start` | **部署核心的唯一正确路径** |
| `python3 scripts/apply_funcdoc.py --check` | 核心函数文档门 |

## 纪律与坑（沿用 + T6 新增）

1. **前台闸门**：动作前 `dumpsys window | grep mCurrentFocus`——`launcher/vtouch/hermescontrol` 可动；视频/阅读/通知栏 = 停；检查只报告、与动作不串一条链。测试台起跑等白名单前台（`~/vtouch-shots/v8t6/wait-gate.sh` 为条件守候范例）。
2. **屏幕**：`Dozing` = 不唤醒、不注入；注入类验收要亮屏且用户在场/离机静止。
3. **T8 渲染拍依赖触摸**：外改盘后需一次触摸（或重启核）才重放；息屏下用**重启式驱动**。
4. `su -c pidof vtouchd_ui` 才有真值；停起驱动必须校验**新 pid ≠ 旧 pid**。
5. **抄历史载荷先核对其「原预期结果」**；条件点坐标域 = 竖屏逻辑 0..1439。
6. `/tmp` 不可写；`su` 下 toybox `grep` 用 `-E`（无 `\|`）；长循环分段跑 + 每轮 flush。
7. **跨 TU 结构体字段名避开核心宏**（`g` 事故；新增字段加血泪注释）。
8. 试点扫描协议已退役（T4）；起跑旗标 `ops-prog.run` 内容 = 起跑名。
9. **验收收场三件套**：双侧还原 / 试点清零 / 无 TRACE 净重启（+ 屏幕超时还原）。
10. 用户会随时上机改数据；动手前对现状，发现陌生改动按现状续接。
11. **日志双源**：面板 → logcat（`VTouchUI`）；核心 → `/data/local/tmp/vt_ui_core.log`。
12. **toybox sh 条件组**：`{ [ -f x ] && …; }` 假分支断整条 `&&` 链 → 条件段用 `;` 独立语句。
13. **v8 元数据 gate `-` 归一空串**；恢复类操作要元数据 + 产物成对。
14. **空 progs 目录 vs 缺失**：哨兵 `0xFFFF0001` 区分。
15. 启动期静默 save_regions→mirror 是既有行为。
16. **「核心心跳停滞」先查崩溃**（logcat -b crash + tombstones + /proc 线程采样）。
17. **VM↔执行器字符串缝**：`vm_cstr` 对哨兵返回 NULL——接收 VM 可选串的 C 层函数必须容忍 NULL。
18. **`.bak-v8` = 原始 v5 快照**（不是前一版）；基线 `~/vtouch-shots/v8t5/.bakv8-md5`；对账 `t53-verify.py`。
19. **降级 = 先还原数据、后换软件**；见 `OPS_V8_DOWNGRADE.md`（改备份/还原逻辑前先读 + 副本演练）。
20. **编辑器存盘按文件现有版本回写**（v5 保持 v5——`Ops.java:414-420`）。
21. **[T6] v8 编辑器链路**：打开 = 读 `progs/<名>.vts` + `#模式` 侧线 ??（可投影→块）；保存 = 脚本视图存原文 / 块视图 `VtFromSteps` 重生成 → 编译 → `.vts`+`.vtbc`+元数据行 + 镜像；**块视图保存会规范化**（缺省补显/默认折叠），**含注释脚本拒投块**。
22. **[T6] `VtPrint` 打字符串优先 `raw`**——AST 改写（如跨方案复制改区域名）必须同步 `raw` 与 `segs`。
23. **[T6] 编辑器内 `#模式` 侧线是编辑器管理字段**：工具/AI 改 `.vts` 时保留或按需改写；核心/面板无视它（普通注释）。
