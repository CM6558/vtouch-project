# OPS_V8_T4 验收记录（存储 v8 / 面板透传 / T8 热载换轨）

日期：2026-10-10 傍晚；设备：OnePlus PJZ110；核心 `vtouchd_ui` **`2cc6f9cb`**（契约 v17 + 面板 v8 链路）。
条件：屏亮、前台 `com.m57.hermescontrol`（可动作）；用户在场（17:04:32 有一次面板触摸，未见干扰）。
规范：`docs/VTSTORE_V8.md`（格式唯一规范）。驱动：`~/vtouch-shots/v8t4/t4.sh`；原始记录：同目录 `t4-run.log` / `core.log` / `panel-logcat.txt`。

## T4.1 编辑器存储层（判据：读写往返 + 镜像对账）

- **离机核先行**：新增 `OpsV8Check`（38 例：FNV 标准向量与 VtBc 同式交叉、元数据行解析/逐字往返、混合文件（步骤+元数据）、语义校验域、产物完整性、对账比较器、`Ops.java` v8 保真）。`~/vtchk-host/run.sh` 三连（HostCheck + VtScriptCheck + OpsV8Check）**全绿**。
- **设备测试台**：**通过 85 · 警告 0 · 失败 0**（新项「ops v8 存储」：scratch 往返=true、镜像对账=true、篡改可见=true、修复=true、`.bak-v8` 备份=true、缺产物 null=true、**真实数据只读对账=一致**）。
  - 首轮逮到真雷：`backupV8` 的 `{ [ -f regions.conf ] && …; }` 组在缺文件时中断整条 `&&` 链 → progs 备份未执行（修=条件段改 `;` 独立语句，双用例复验）。
- **APK**：v34 `108bd0e6…`（装机 base.apk **逐字节一致**）；`app/src.bak-v34` 快照；`out/vtouch-editor-v34.apk` 存档。
- 接线：`Schemes.mirror/switchTo/saveAs` 带 progs 精确同步；`Ops.java` v8 记录一等公民（progHash/progSize/往返保真/validate 分支）。

## T4.2 面板透传化（判据：字节对拍：同一操作 vtbc md5 经面板往返不变）

- **部署**：`sh t4.sh deploy` → `vtouchd_ui = 2cc6f9cb`（PAYLOAD_MD5 对账 ✓；回滚备份 `/data/local/tmp/vtouchd_ui.bak-ebf81f93` + `/sdcard/vtouch.sh.bak-t4-pre`）。
- **v5 回归**：三次重启全量导入用户数据（r1/s1/c1/r2 + `op1` 13 步 + `保命装` 68 步；md5 `666a45f9` 收场还原）——过渡期读路径无恙。
- **v8 载入**：`ops.conf 程序 ztp1 上表（166B fnv=08fd5310）`——元数据（python 生成）与面板实算 FNV 一致（字节完整）；核心收到 `op 编辑 put ztp1 程序=166B`；旗标起跑 `完成 553/551/552ms`（对 T3.5/T3.6 基线 551ms ±2ms）。
- **字节对拍（正面）**：`VTOUCH_UI_OPS_SAVE_TEST=1` 启动 → 面板启动导入后自动 save 往返：`ops-test 启动写回往返 rc=0` + `ops.conf 已存 3 条（程序 1）` →
  **`progs/ztp1.vtbc` md5 `8349217d…` 不变**、**`ops.conf` md5 `805e6503…` 不变（逐字节）** —— 文件→面板→核心→面板→文件 全链保真。
- **负样例（均按条跳过、不带走其余记录）**：
  - 哈希篡改：`程序 ztp1 哈希不符（元数据 00000000 / 实际 79b80b71）→ 跳过`（共 3 条：补入 2、跳过 1）；
  - 大小篡改：`大小不符（元数据 999 / 实际 166）→ 跳过`。
- **镜像对账**：live vs `schemes/吕布` 三对（ops.conf / progs/ztp1.vtbc / regions.conf）**逐字节一致**（805e6503 / 8349217d / f8fdd781）。
- **pilot 扫描退役**：`ops-prog-*.vtbc` 扫描删除（不再注册）；**起跑旗标保留**（真机起跑验证；供 T5 双跑复用）。
- **方案切换链**（T8 重载内含）：③ flush live→scheme 与 ④ scheme→live 的 progs 同步均实测执行（logcat「拷 1 删 0」双证）；三 clear（regions/ops/progs）重放链正常。
- 操作页程序卡（「程序 N B · 门控 · 自动关」+ [运行]/[停止]；运行中显示「运行中 · 脚本」）：随批交付，真机目视留给用户。

## T4.3 T8 热载换轨（判据：改脚本 ~2s 生效实测）

- **实测**：双侧改 `progs/ztp1.vtbc`（等待 200→500 变体，fnv=79b80b71）+ 元数据 → **写入 → `方案 重载 吕布（热生效）` 确认 = 2055ms**（轮询 1s + 防抖 600ms + 重放，符合 ~2s 口径）。
- **生效证据**：重载后自动起跑 → `op 完成 ztp1 用时=851ms`（原版 551ms 档）；**核 pid 7008 全程未变（零重启）**。
- **恢复**：`fix`（元数据+产物成对）→ `上表（166B fnv=08fd5310）` → `完成 552ms` 回归。
- 监视集扩展生效：ops.conf（双侧）+ progs 目录签名（双侧）+ 原三文件；自写豁免（hr_files_note）无自激重载。

## 收场（三件套）

- 双侧还原：ops.conf md5 **`666a45f9`**（逐字节，含 schemes/吕布）；屏幕超时还原 120000；无 TRACE（本批未开；净重启验证）。
- 试点文件清零：`ops-prog.run` 已清、无 `ops-prog-*.vtbc` 残留；`progs/` 保留为**空目录**（同步机制按需创建；空目录 = 无产物，与缺失等价、两侧一致不触发重载）。
- 备份留档：`ops.conf.bak-t4-pre`（双侧）、`/sdcard/vtouch.sh.bak-t4-pre`、`/data/local/tmp/vtouchd_ui.bak-ebf81f93`。

## 遗留与明示

1. **门③（屏亮真实抓帧 ≤1%）仍未测**：需游戏画面里 `名刀CD` 模板在场（zt8/ztp8 配方已留 `~/vtouch-shots/v8pilot/t36.sh`），等用户窗口。
2. 过渡期 v5 步骤读路径**保留**（T5 迁移完成前）；迁移收尾后可评估剪除。
3. 程序运行名面板不可显示（核心 `op_run=-1` 口径；运行中显示「运行中 · 脚本」）——待契约扩展再显示具体名。
4. `VTOUCH_UI_OPS_SAVE_TEST` 验收钩与起跑旗标：退役点 = T5 验收后评估。

## 血泪（本轮新增）

1. **toybox sh 条件组**：`{ [ -f x ] && cp …; }` 假分支会中断整条 `&&` 链（不是只跳过该组）→ 条件段用 `;` 独立语句（backupV8 首轮被测试台逮到）。
2. **日志双源**：面板 ALOGI → **logcat（tag `VTouchUI`）**；核心/胶水 fprintf → `/data/local/tmp/vt_ui_core.log`。验收取证写脚本要**两源合并**（t4.sh `logmark2`）；只看核日志会「证据全丢」。
3. **元数据 gate `-` 归一**：v8 元数据行 gate 为 `-` 时必须转空串再投核（核心按「门控非法」拒收）——部署前代码复读逮到。
4. **fix 要成对**：恢复 = 元数据行 + 产物文件一起（只改一侧 = 哈希不符假阳性；本轮 t4.sh fix 首版即栽，已修）。
5. **空目录 vs 缺失**：目录签名对「缺失」用哨兵（0xFFFF0001）区分；空↔空两侧相等不触发，缺失↔空触发一次（幂等无害）。clean 后两侧均为空目录，等价且稳定。
6. **启动期既有多余镜像**：启动时会静默走一次 save_regions→mirror（历史行为、无成功日志），T4 起会多一行「progs 同步 拷 1」——不是 T4 引入的假象。
