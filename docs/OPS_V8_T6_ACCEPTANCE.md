# OPS_V8_T6_ACCEPTANCE —— T6 双视图编辑器（V8 收口批）验收记录

> 批次：T6（T6.1–T6.4），2026-10-10 晚。编辑器 **v35**（`5ff29f71`，装机逐字节一致）；核心 `3a6e723c`（在跑，未动）。
> 文档分工：设计纪要/实现口径 = `~/vtouch-editor/docs/UX_BATCH_v35.md`；规范（r8 覆盖清单/模式侧线）= `docs/VT_SCRIPT_V1_SPEC.md`；实施计划 = `docs/OPS_IMPL_PLAN_V8.md`（T6 已勾）。
> 证据档案：`~/vtouch-shots/v8t6/`（`evidence.txt` + `test_report.txt` / `test_run.txt` + 全量渲染件与探针）。

## 1. 交付件（编辑器 v35）

| 件 | 说明 |
|---|---|
| `VtToSteps.java`（新） | 脚本→步骤投影器：注释扫描 → 解析 → `VtCheck` → 投影 → **固定点自检**（再翻译逐字一致才放行）；覆盖 = 步骤域全量；拒绝词带行号（跳转/函数/表/当/for/注释/负数字面量/复合表达式/16 槽/15 字/4 子条件/8 层…） |
| `ScriptStore.java`（新） | 模式侧线 `#模式 块\|脚本`（modeOf/withPragma）、存盘 prepare（检查+编译+至少 1 语句）、`updateMetaLine`（行级元数据）、`regionRefs/remapRegions`（AST 级，跨方案复制用） |
| `ScriptEdit.java`（新） | 文本编辑器：高亮（轻扫扫描器：保留字琥珀/字符串绿/注释灰）、行号槽（按实际 Layout 基线自绘，中英混排不漂）、错误行（防抖 VtCheck → 行底标红+槽内红号+错误条点击跳行）、辅助条（底部横滚文字键，( ) { } [ ] " 成对光标居中）、外层 ScrollView 拖拽滚动 + `ensureCursorVisible` |
| `OpsPage.java`（改） | 打开路由（v8 读源 → `#模式` 侧线 ?? 可投影性推导）、`editorHost` 脚本宿主、[转脚本]/[转块]/[保存]、`commitEdit` 双路归一、`saveV8`（.vts+.vtbc+conf+备份+镜像）、列表卡「脚本 N 行」、删除连产物清扫 |
| `CrossCopy.java`（改） | v8 补链：`Vts` 脚源口、脚本级依赖扫描（`opRegionRefs(o,vts)`）、改名改写 + 重编译 + 记录哈希/大小更新、`Artifact` 产物清单（原样复制‖重编译）、执行期产物先落 |
| `VtPrint.java`/`OpsV8.java`（改） | `printExpr`；`srcPath/readSrc/writeSrc` |
| `VtToStepsCheck.java`（新） | 离机核 66 例（入 `run.sh` **六连核**） |
| APK | `out/vtouch-editor-v35.apk`（md5 `5ff29f71173bbd6693a1aea2a1f9d511`）；备份 `app/src.bak-v35` |

## 2. 离机核（六连核）

`bash ~/vtchk-host/run.sh` —— HostCheck / VtScriptCheck / OpsV8Check / VtFromStepsCheck **38** / VtMigrateCheck **18** / **VtToStepsCheck 66** 全绿。
其中 VtToStepsCheck：往返金样（复用 VtFromSteps 全部金样）＋规范化（缺省补显/默认折叠/德摩根）＋负样例 20＋字段抽查＋**真实产物扫描：out-mig 8 件 + out-mig-zt 42 件逐字往返全绿；负例夹具 5 件（ztc3/ztc12/ztd4/zte2）仍拒**。

## 3. 设备测试台（两轮，均经「白名单前台」起跑门）

- 轮 1（18:53:47–18:54:25，装机版 `cb3afd52`）：通过 85 · 警告 0 · 失败 2 —— 两处均属**测试代码自伤**：① T6「非子集拒/块保存」= c5b 用例拿了合法函数脚本当非法例（保存成功→断言错）；② 脚本编辑「溢出」= `assertOverflow` 把辅助条横滚内容误报为溢出。修正（c5b 换确定非法例 + `assertOverflow` 豁免横滚容器子级 + 失败时输出调试位）后重建 `5ff29f71`。
- **轮 2（19:13:49–19:14:20，终版 `5ff29f71`）：通过 87 · 警告 0 · 失败 0（30.7s）** ✅
  - `[OK] 跨方案复制：… v8 携带+改写=true`
  - `[OK] T6 双视图：投影往返=true 拒绝词=true 侧线/prepare=true 打开路由=true 脚本保存对账=true 脚本→块保存=true 非子集拒=true 块保存=true live 只读（2/2 件可投影）=true`
  - `[OK] T6 脚本编辑预览：edit_script / err / long（打开路由/高亮/辅助条/行号槽/错误行/滚动/拖拽滚动=true · 溢出核 无）`
- 渲染件目检：`edit_script.png`（正常态，行号-代码基线对齐、琥珀关键字/绿字符串、辅助条可见）、`edit_script_err.png`（第 3 行行底红衬+槽内红号+「第 3 行: 表达式为空」）、`edit_script_long.png`（滚动态行号 15–42 连续对位）；像素剖面核色（琥珀 8606 / 绿 162 / 错误红 724 / FAINT 1521）——视觉模型对颜色误读二次出现，仍以剖面为准。
- 收场：无编辑器窗口残留、保活自停、核心未动（pid 24653）、用户前台不受扰（起跑门 = `wait-gate.sh`：白名单前台+亮屏条件守候）。

## 4. 数据安全

- 全程未写真实运行目录：测试重定向 scratch（`/data/local/tmp/vte-t6*`，用后即清）；live 只读核 2/2 件可投影。
- live 保持迁移终态（ops.conf `a200d4b9` 等）；本批无核心/面板/数据改动。

## 5. 遗留（交用户/后续批）

- **上机手测**（用户）：打开（块/脚本两路自动路由）/ 编辑 / [转脚本][转块] / 保存与热生效 / 观感手感（脚本编辑器滚动、辅助条、错误定位）。
- **收尾评估项**（面板侧，Windows 构建批次）：v5 步骤读路径退役评估；`VTOUCH_UI_OPS_SAVE_TEST` 钩 + 起跑旗标退役。
- 块视图覆盖扩展（函数/表/返回/调用/当/for——脚本侧已全量可用；块侧按批补 UI）。
