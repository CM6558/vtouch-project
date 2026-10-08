# 面板缩编方案 —— ImGui 面板 = 纯「运行悬浮窗」（v0.5）

> 状态：**已落码（P1），待 Windows 构建 + 真机回执**（2026-10-08 用户：「app 里也可以增加一个框选工具，面板部分尽量保持运行时能力就好，开始吧」）——Q1–Q6/Q8/Q9 按推荐；Q7=**双方都做**（面板保留区域调整块 + app 增加框选工具〔另立小案〕）；先提交 v11（a761d72）再叠加缩编 —— **缩编已落码**：`src-ui/vtouch_ui.cpp` 9221 → 3809 行（净删 ≈5.41k），被删符号零残留 / static 未使用 0 命中 / 括号平衡全绿。v0.2 细化（函数级 D/W/K/G 表、任务 T1–T7、命令行判据）；v0.3 补 §3.6；v0.4 补 §3.7；
> **v0.5 剪裁线修订**——按用户口径「修改不许重启才生效；运行中最好当场就能调」：①「区域调整块」归运行时保留（§3.8）；②热生效（免重启-A）升入本期主范围（§3.7）；③ app 侧框选（§3.6）转可选后续。
> 范围：P1 只动 `src-ui/vtouch_ui.cpp`；核心/胶水/Java 零改动。
> 唯一可信源：盘上代码。行号为**现阶段工作区**（v11 未提交改动叠加后）现读值，`vtouch_ui.cpp` 共 9221 行；**动手时以函数名/唯一字符串定位，行号只用于对账**。

## 0. 目标与判定尺子

用户口径（2026-10-08）：
1. 「imgui 要完全作为运行时悬浮窗来使用，不需要的功能整理出来去掉」；
2. 「**我不希望修改需要重启核心生效**；并且在运行时发现需要调整的地方能快速方便的调整，**最好当场就能调**」。

| 判定 | 含义 | 例子 |
|---|---|---|
| **保留 = 运行时能力** | 观察 / 控制 / 为引擎服务且无处可去者 / **运行中当场调整** | 状态、日志、屏上描边、触发闪、运行/停止、方案热切换、显隐、关 UI、退出；conf 导入、抓帧引擎、区域跟随旋转、吞触摸；**区域调整块**（框选/拖动缩放/四键/触发行；§3.8）；**热生效补强**（§3.7） |
| **去掉 = 编辑能力** | 「改配置并落盘」的重型表单与工具（无小切片可留者） | 操作编辑器、方案 CRUD、模板/点集、试查、说明、改名 |

## 1. 不可协商项对账

| # | 条款 | 落实 |
|---|---|---|
| 1 | **核心零改动**（`src/` 一行不动） | 缩编全在面板侧；funcdoc 幂等门、默认核 md5 纪律自动不受影响 |
| 2 | **APK 功能对等先于拆除** | 每删一项有 app 归属（§3）；保留块不受影响 |
| 3 | **拆除不伤引擎** | 「必须保留」红榜 K1–K9（§3.3）+ 真机回执清单（§5 P1-e） |
| 4 | **旧数据/旧文件兼容** | regions.conf/ops.conf/schemes 读端原样保留（v1..v5 翻译、只补缺、#frame/bind/kind/hide 行） |
| 5 | **构建口径** | 仍为 Windows Git Bash + NDK r27d（`check_syntax.sh` / `build_ui.sh` / `build.sh ui` / `ci_check.py`）；设备侧只推 `vtouchd_ui` |
| 6 | **交付物** | 本文件 + 缩编后的 `src-ui/vtouch_ui.cpp`（单一提交，与 v11 分开回看/回滚） |
| 7 | **不扩容**（v0.5 修订） | 唯一新增 = **热生效补强 A**（§3.7 档 1，纯面板）；其余不加、不动 app、不动共享内存契约 |

## 2. 目标形态

### 2.1 缩编后界面速览

```
标题栏： ● 监听中 · 3/32 · 1.2ms                        [收起▾]     ← 拖动区（不变）
侧栏：   页面：区域列表 · 操作 · 方案 · 事件日志 · 设置
         框选：矩形框选 · 圆形框选                                  ← 保留（区域调整块，§3.8）
         显示：显隐（叠加层）· 触摸标记
         动作：关闭 UI · 退出
区域页： ● r1 矩形 [停用][选中][隐藏][删除]                          ← 保持编辑卡（v0.5）
         2325,686 - 2456,784   触发：op1 · 按下 · 开关型：开
操作页： ● op1  3 步 · 门控 无 · 自动关 关   [运行]  或  [停止]      ← 只读卡 + 运行/停止
方案页： 吕布（当前）· 5 个方案       [切换]                        ← 列表 + 切换（+[重载]兜底键）
事件日志 / 设置（抓帧间隔）：照旧/缩编
屏上：   区域描边·开关绿·触发闪·进出脉冲·触点标记·框选橡皮筋·选中手柄（触摸标记开关控制）
```

### 2.2 缩编后仍必须成立的数据流（= 回执清单）

- **D1 导入流**：conf → 面板 → 编辑邮箱 → 核心（只补缺；bind/kind 照灌）。核心不读 conf（src/ 仅 `vt_ops.c:1059/:1107` 读 `.tmpl`/`.pts`）⇒ 面板是唯一导入者。
- **D2 切换流**：schemes/<新> → 清空+重放 → live 直写 + current（现有热切换路径）。
- **D3 抓帧流**：核心 req_pending → Java vt-cap → shm 帧区 → 核心匹配（**全在 Java/胶水，面板 C++ 不参与**）。
- **D4 旋转流**：区域坐标基准帧换算写回（核心表 + regions.conf + #frame）。
- **D5 吞触摸流**：面板矩形 → 核心吞（含转屏稳定窗「宁放不吞」口径）。
- **D6 热生效流（v0.5 新增）**：app（或任何外部）写 live 两文件 → 面板 mtime 轮询+防抖 → **自动强制重放当前方案** → 生效；全程不重启核心（§3.7 档 1）。
- **D7 当场调流**：面板区域调整块编辑 → 编辑邮箱 → 核心 1–2ms 即时生效（§3.8）。

## 3. 处置总表

### 3.1 删除块（D1–D7；「净删 ≈4.4k 行」的构成）

| # | 块 | 范围/锚点 | 明细（点名） | 估算 |
|---|---|---|---|---|
| D1 | **操作编辑层** | 3324-6258 | 编辑层壳（build_edit_layer 8654、draw_ope_bar 8626、ope_bar_rect 1776）、键盘（draw_char_kb 3410）、改名（draw_ope_name_kb 4789）、校验/变量（ope_*/ne_* 3536-4319）、数字层（ne_* 3744-4695、draw_num_edit 4819）、表达式（4549-4683、5202-5595）、支路（4512-4548、5595-5680）、预览页（5681-6120）、draw_op_edit 6096-6258、入口 op_new/op_del/gen_op_name 3155-3202 | 约 2935 行 |
| D2 | **视觉段** | 6774-8546 | 全部 vis_*（模板/点集读写 6793-7482、采集覆盖层 7483-8010、试查 6982-7102、参数层 8015-8412、模板页 8417-8546）+ g_vis_* 全局 + `build_vis_cap`；屏上「试查命中标记」绘制 2473-2496 | 约 1780 行 |
| D3 | **方案 CRUD 包装** | 6452-6590、6684-6773 | scm_delete_dir 6452 / scm_new_empty 6465 / scm_save_as 6488 / scm_rename 6508 / scm_kb_open 6529 / scm_select 6544 / scm_act_rename 6551 / scm_act_delete 6567 / draw_scm_kb 6684 / draw_scm_del 6722（**保留** scm_switch_to 6440、scm_row 6592 改、page_scheme 6631 改） | 约 220 行 |
| D4 | **说明页** | 6291-6339 | g_help_lines（25 条）+ page_help | 49 行 |
| D5 | **区域编辑件（v0.5 缩减）** | 散布 | 只删：**改名链**（id_name_ok 2776 / rename_region 2789 / name_open 2804 / draw_name_edit 6343）+ **取点十字** 2460-2471。**保留**：del_region / rc_* 触发行 / sel_geom / cap_commit / edit_apply_live / 框选工具 / 卡片四键 / overlay 选中手柄与橡皮筋 / snapshot 框选拖改段 / gen_id（= 区域调整块，§3.8） | 约 200 行 |
| D6 | **键盘区/字形编辑扫描** | 6387-6404、g_zone_kb/SCR_KB 支线 | ui_glyph_scan_edits 6387（18） | 约 40 行 |
| D7 | **取点链（面板侧）** | 1822、4172 | pick_ev_apply + g_pick*/g_pickmk_*（含 ui_ev_cb 转发分支、snapshot 取点取消段） | 约 60 行 |

（合计 ≈ 5.3k 行毛删；改写回收 ≈500 行 ⇒ 净剩 ≈ 4.4k 行，约现量的 48%。）

### 3.2 改写块（W1–W11：改成什么样）

| # | 函数/处 | 改成 |
|---|---|---|
| W1 | `build_sidebar` 2879 | 页面组=区域列表/操作/方案/事件日志/设置（删「模板」「说明」项）；**「框选工具」组保留**（v0.5）；底部四键（显隐/触摸标记/关闭 UI/退出）不动 |
| W2 | `region_card` 3007 + `page_regions` 3099 | **保持编辑卡（v0.5）**：四键（停用/选中/隐藏/删除）+ 触发三行编辑键原样；仅：id 改普通文本（改名归 app）、提示行删「点名字改名」文案 |
| W3 | `op_card` 3209 + `page_ops` 3271 | 卡=状态点/名/meta（N 步·门控·自动关）/运行中行 + **单键 [运行]/[停止]**；删「＋新建」行 3295；空态文案改「还没有操作（在编辑器里新建）」；保留 ops_run_watch 3143 |
| W4 | `scm_row` 6592 + `page_scheme` 6631 + `scm_switch_to` 6440 | 行=名 + [切换]（当前行高亮+「当前」禁用）；保留切换结果提示槽（g_scm_msg 码表映射 6405）；**加 [重载当前方案] 兜底键（v0.5，随 T8）**；删选择态/底部四键/键盘/确认层。**`scheme_switch` 执行器本体不动**（仅加「强制」入参） |
| W5 | `page_settings` 6267 | 页头「设置」+ [抓帧间隔：N ms（点一下切换）]（预设 `{0,5,10,20,33,50,100,200}`，逻辑自 8468-8483 原样挪）+ 一行说明「面板=运行时窗口；编辑在编辑器 app」。删原说明性文案 |
| W6 | `build_overlay` 2363 | 删：取点十字 2460-2471、试查标记 2473-2496。**保留（v0.5）**：is_sel 手柄 2430-2446、框选橡皮筋 2497-2521、区域描边·开关绿·触发闪·进出脉冲·id 标签·触点/轨迹/扩散圈（g_show_mark 门控） |
| W7 | `build_panel` 8547 | 删 `scm_layer`/`g_name_i`/`g_ope_i` 分支与 `g_zone_kb` 清行；页面分发只留 0/1/2/3/5 |
| W8 | `draw_frame` 8685 | 分发改「overlay → panel」两态（删 build_vis_cap/build_edit_layer 分支 8715-8717）；删 ui_glyph_scan_edits 调用 8710（字形按需扩集三件**保留**） |
| W9 | `ui_rect_now` 1787 / `ui_hit_rect` 1801 / `snapshot_touches` 2037 | 吞触摸矩形=**仅面板窗矩形**（删 vis_cap/编辑层三态分支——区域框选不改变矩形）；snapshot 删「取点取消」段，**保留框选/拖改拦截（v0.5）**；保留矩形推送、稳定窗、鼠标喂 ImGui、触点快照、滚动分区 |
| W10 | `prune_panel_refs` 8759 / 头注释 1-5 | prune **不动**（g_sel/hide/flash/ex 均保留）；头注释改为「运行悬浮窗（含区域调整块）」口径 |
| W11 | `hide_toggle` 781 | 保留（随区域调整块） |

### 3.3 保留红榜（K1–K9，易误删——函数点名）

| # | 机制 | 函数点名（现读行号） |
|---|---|---|
| K1 | **conf 导入链** | save_regions 817 / region_exists 869 / load_regions 879（只补缺、bind/kind 回灌、#frame） / op_exists 1028 / ops_save_failed 1039 / save_ops 1045 / ops_translate_old 1093 / ops_load_put 1127 / load_ops 1146 / region_conf_dir 801 / save_failed 809 |
| K2 | **方案文件层+切换** | 1265-1490 全段（name_ok/known/exists/cmp/list/cur_get/cur_set/write_empty/ensure_empty/copy_file/count_lines/migrate/sync_live/mirror） / scheme_line_kw 1512 / scheme_bad_region_line 1521 / scheme_precheck 1545 / scheme_restore_file 1617 / scheme_rollback_to 1629 / **scheme_switch 1645** / scm_switch_to 6440 / scm_row(改) 6592 |
| K3 | **区域跟随旋转** | frame_sane 561 / region_rot_map 574（含 c2p_rot 517） / region_rot_step 611 / ui_region_changed 449 |
| K4 | **吞触摸** | in_panel 1761 / ui_rect_now(改) 1787 / ui_hit_rect 1801 / ui_consume_cb 1811 / snapshot_touches(改) 2037 |
| K5 | **观察** | ui_ev_cb 1827（触发闪/进出脉冲）/ ev_log_push 1859 / ev_note 1868 / ops_run_watch 3143 / page_log 6259 / push_ring 1766 |
| K6 | **抓帧引擎** | 全在 Java/胶水（VTouchUI.java captureDisplay/vt-cap/skip-screenshot）；C++ 侧仅保留 `vtouch_vis_cap_interval_set` 调用（ui_conf 272 + 设置页 W5） |
| K7 | **基础件** | ui_conf_load_once/save 240/274 / 字形三件 2273-2361 / EGL·字体 2202-2261 / zones/滚动 431/2717/2723 / JNI 全集 9088-9221（按 Java `native` 声明逐一对齐） |
| K8 | **运行控制** | build_titlebar 2816 / 收起-还原 2859-2865 / 侧栏底部 2913-2924（显隐/触摸标记/关闭 UI/退出）/ ui_show_cb 454 / draw_frame(改) |
| K9 | **区域调整块（v0.5 新增保留）** | 框选工具（2897-2909 + cap_commit 1894 + 橡皮筋 2497-2521）/ 选中拖改（sel_geom 1880 + snapshot 2102-2195 + edit_apply_live 1928）/ 卡片四键 3061-3089 / 触发三行 2939-3006+3046-3060 / overlay 手柄 2430-2446 / gen_id 1735 / hide_toggle 781 —— 详见 §3.8 |

### 3.4 全局/杂项处置（G）

- **删**：g_pick*·g_pick_finger·g_pickmk_x/y/t / g_ope_*（i/coll/pv/…）/ g_ne_* / g_ex_*（表达式撤销）/ g_vis_*（全部）/ g_name_i·g_name_buf·g_name_old·g_name_up·g_name_msg / g_scm_kb·g_scm_del·g_scm_sel·g_scm_kbup·g_scm_kbmsg·g_scm_ren_old / g_kb_*（字符键盘） / g_zone_kb + SCR_KB 支线 / g_ops_save_pending（写方全在编辑路径——收尾审计确认后删；g_save_pending **保留**）
- **保留**：**g_sel_id·g_cap_mode·g_cap_slot·g_cap_x0/y0/x1/y1·g_edit_slot·g_edit_kind·g_edit_e[]·g_edit_dx/dy·g_edit_last（区域调整块，v0.5）** / g_dots·g_rings / g_flash_id·g_ex_* / g_hidden·g_nhide / g_ov_show·g_show_mark·g_cap_ms / g_rr_* / g_evlog / g_pan_*·g_scr_*·EGL 槽位 / g_want_layer·g_ui_off / g_rot_settle_t
- **extern 声明段（95-160）**：**一行不动**（未用声明无告警；作为胶水接口存档）。P2 再议清理。

### 3.5 「框选缺口」的来龙去脉（v0.5 修订：缺口消解）

- 面板原状：唯一「在真屏上所见即所得画/调区域」的工具（框选 + 选中拖改，见 §3.8 锚点）。
- app 现状：区域只有数字表单 `RegionEdit`（id/类型/坐标/bind/kind；`RegionEdit.java:18-50`）；区域侧无看图器取点（`ImageView/frameToLogic` 只接在素材/步骤）。「区域底图可视化编辑」= app P1.5/P2 项，未做。
- **v0.5 裁决**：按「运行中当场就能调」口径，「区域调整」归运行时保留（§3.8）⇒ **缺口不再存在**；app 侧框选（§3.6）转**可选后续**（需要「离开游戏做精细编」时再立小案，做法已存档）。

### 3.6 app 侧承接「框选」的做法（**已拍板：做，另立小案** —— 能做，怎么做）

**结论：能做；拼件基本齐，真正的新增件只有四小块。** 证据（`~/vtouch-editor`）：

| 拼件 | 现状（实证） |
|---|---|
| 框选组件本体 | `ImageView` MODE_BOX：框外拖=新框 / 框内挪框 / 四角手柄 / 双指平移缩放 / 1–8x / ≥20dp 像素网格（模板与 [取图] 流程已量产）；`selection()` 返回帧像素 |
| 坐标换算 | `Matcher.frameToLogic` :255 / `logicRectToFrame` :270（帧↔竖屏逻辑；对拍硬门已过） |
| 会话壳 + 帧源 | `VisualPage`：[用帧▾]/[去抓帧]、`useLatestFrame`、模式提示/底栏/确认链全套（StepEdit 已有同款接线先例） |
| 写盘链 | `Regions.serialize`（含 bind/kind/hide 保留）+ 原子写 + schemes 镜像（app 现用） |
| 冻结浮层骨架 | [取图]（v9.4）：截屏→冻结→框选→直写，全链已跑通 |

**新增件（小）**：① 圆形模式（按下定圆心、拖定半径）② 已存区域在帧上描边叠加 / 当前项高亮 / 选中拖改 ③ 两种接线 ④ 测试台检查 + 离屏渲染件。

**两个形态**（可同案，先 A 后 B）：
- **A 悬浮快速通道**：编辑悬浮窗加 [框区域]（矩形/圆切换）→ 截屏冻结 → 框选 → 自动命名 → 直写 regions.conf（+镜像）→ 生效走热重生（§3.7）。复用 [取图] 骨架，新增最少。
- **B 编辑器会话**：`RegionEdit` 加 [底图框选/取圆] → 帧会话 → 已存区域叠加 → 拖框/拖圆 → [确认] 回填表单 → 既有保存链。适合精调与一条龙编辑。

**取舍**：冻结帧 = 按下那一刻的画面（非实时）；精度手段 = 缩放/平移/像素网格（放大镜按既有口径保持仅取色会话）；生效 = 面板热重生（§3.7，不重启）。工作量 = 小案（v7 任务尺度 2–4 个任务）。

### 3.7 热生效阶梯（Q9 —— 「修改不许重启」的答案）

现状口径：**面板里的修改从来不重启**（编辑邮箱 1–2ms 即时，D7）；**app 里的修改现状要**（核心不读 conf；默认应用路径=重启核心；唯一手动偷跑=面板「方案切换」往返）。下面把「不重启生效」补齐：

| 档 | 做法 | 代价 | 效果 |
|---|---|---|---|
| 0 现有（零新代码） | 面板「方案切换」往返：存盘 → 切走再切回（两次全量重放） | 0 | 秒级生效、不重启；手动两步 |
| **1 免重启-A（v0.5 升入本期主范围）** | 面板轮询 live 两文件 mtime + 防抖 → **自动强制重放当前方案**；附「重载当前方案」兜底键。要点：面板自写豁免（旋转写回 / hide / 区域调整落盘不自激）、绕过同名 no-op（`scheme_switch` 加强制入参，执行器本体不动） | 纯面板改动，与缩编**同一次 Windows 构建** | app 存盘后 **~1–2s 自动生效、零操作、全程不重启** |
| 2 免重启-B（P2 正式） | 核心 WS 加 `reload` → 投编辑邮箱 → 面板重放；APK 发一条 WS 即生效（127.0.0.1:27183） | 动核心 `src/` + 并入 WS 控制面 | 事件驱动、可扩展（试运行/取点/日志订阅同案） |
| 3 远期（另行立项） | WS 多客户端控制面 + app 实时调整悬浮层（画面上直接拖区域） | 中大型 | 完全体；本期不做（「当场调」已由 §3.8 承担） |

注意：全量重放 = 清空+重放（每条毫秒级；与现有手动切换**同语义**——含静默边界：起跑中的操作会被停止；重复重放安全）。对应任务 = **T8**。

### 3.8 区域调整块（v0.5 保留包 —— 「当场就能调」的落点）

按用户口径「运行中最好当场就能调」，把「区域调整」从删除栏挪到运行时栏：

- **保留**：框选工具（矩形/圆形 2897-2909 + cap_commit 1894 + 橡皮筋 2497-2521）、选中拖动/四角缩放（sel_geom 1880 + snapshot 2102-2195 + edit_apply_live 1928）、卡片四键（停用/选中/隐藏/删除 3061-3089）、触发三行（触发/时机/开关型 2939-3006 + 3046-3060）、overlay 选中手柄 2430-2446、gen_id 1735、hide_toggle 781。
- **归 app**：改名（字符键盘整体不保留）。
- **生效**：原路径不变——编辑邮箱 1–2ms 即时生效，**从不重启**。
- **理由**：唯一能「拖一下、当场生效」的路径（无重载延迟、不用开 app）；代码占比小（几百行 vs 总删 ~5k）。
- **边界**：操作编辑器（步骤/阈值）仍删——2900 行整体无小切片；其「不重启生效」由 §3.7 档 1 承担。若日后操作也常需「当场」微调，再单独立小案（如操作快捷微调）。

## 4. 实施（拍板后执行）

### P0 前置
1) 拍板记录并入本文；2) 建议先 `git commit` 提交 v11 现状，缩编独立成下一提交；3) 设备侧开工前不打扰（查 `topResumedActivity` 纪律照旧）。

### P1 任务（只动 `src-ui/vtouch_ui.cpp`；Native 串行；逐任务以唯一字符串 patch，**不按绝对行号整段删**）

| 任务 | 内容 | 收尾自查（应 0 命中；命中=漏删） |
|---|---|---|
| T1 壳 | 侧栏 W1（**框选组保留**）、build_panel W7、draw_frame W8、nav 分发、g_zone_kb 支线 | `grep -nE 'g_nav == 4\|g_nav == 6\|page_help\(\|page_template\(\|build_edit_layer\(\|build_vis_cap\(\|g_zone_kb'` |
| T2 区域 | W2（id 文本化）、W6、W9、W10、D5（仅改名链+取点十字） | `grep -nE 'id_name_ok\|rename_region\|name_open\|draw_name_edit\|g_pick\b\|g_pickmk\|pick_ev_apply\|g_name_'`（**sel_geom/cap_commit/edit_apply_live/gen_id/rc_\* 保留，不在清单**） |
| T3 操作 | W3、D1 入口 | `grep -nE 'op_new\|op_del_by_name\|op_edit_open\|draw_ope_name_kb\|\bope_\|draw_num_edit\|\bne_\|g_ope_\|gen_op_name\|draw_char_kb\|g_kb_'`（保留 ops_run_watch/op_card/page_ops 自身） |
| T4 方案 | W4、D3 | `grep -nE 'scm_kb\|scm_del\|scm_select\|scm_act\|scm_new_empty\|scm_save_as\|scm_rename\|scm_delete_dir\|draw_scm'`（保留 scheme_switch/scm_switch_to/scm_row；新增 [重载] 键） |
| T5 视觉 | D2 | `grep -nE '\bvis_\|g_vis_'`（含 extern 声明一并删） |
| T6 说明/键盘区 | D4、D6 | `grep -nE 'g_help_lines\|page_help\|ui_glyph_scan_edits\|SCR_KB'` |
| T7 清扫与自检 | 头注释 W10、注释里引用已删功能的文案、未用符号扫描 | ① `python3 scripts/apply_funcdoc.py --check`（应“共调整 0 处”）② 未用 static 扫描（启发式，人工复核）：

```sh
cd ~/vtouch-project && for f in $(grep -oE 'static [A-Za-z_].* [a-z_][a-z_0-9]*\(' src-ui/vtouch_ui.cpp \
  | sed -E 's/.* ([a-z_][a-z_0-9]*)\(/\1/' | sort -u); do n=$(grep -c "\b$f\b" src-ui/vtouch_ui.cpp); \
  [ "$n" -le 1 ] && echo "可能未用: $f($n)"; done
```
③ `grep -nE '框选|编辑层|取点|试查|模板页' src-ui/vtouch_ui.cpp`（剩余处人工核对） |
| **T8 热生效**（免重启-A） | mtime 轮询（~1s）+ 防抖（~0.5s）→ 强制重放当前方案；自写豁免（面板自写后刷新 stat 基线）；`scheme_switch` 加强制入参（执行器本体不动）；方案页 [重载当前方案] 兜底键 | `grep -nE 'reload\|重载\|mtime'`；真机：app 存盘 → ~1–2s 自动（重）载日志；面板自写（拖区域/转屏）**不得**触发重载 |

### P2 链路清理 + 文档同步（P1 验毕后；默认保守）

| 项 | 锚点 | 默认 |
|---|---|---|
| Java 面板抓帧旁路 | `VTouchUI.java`：nativeVisPanelPoll :28 / nativeVisPanelFrame :30 / captureToPanel :338 / 调用点 :391-394、:695-725 | **保存档、不删**（零风险下线；备选=三文件同步删） |
| 胶水/桩对应件 | `ui_glue.c`：vtouch_vis_panel_capture_req/frame_take/err_take :933-975、nativeVisPanel{Frame,Poll,Fail} 同段（912-1030 一带）；`ui_stubs.c` :327-336 | 同上 |
| app 侧框选（§3.6） | `~/vtouch-editor` | 可选后续；需要时立小案 |
| 文档口径同步 | AGENTS.md（结构行+「操作编辑器/执行器/触发侧」段+「说明页」段）、README、`docs/UI_INTEGRATION.md` 状态行 | 必改 |
| 说明页文案留档指针 | 源=`OPS_PLAN_V5 §9`+`VISION_PLAN §12`+`EDITOR_V2_PLAN` 文案节 | 记档备搬 app |

## 5. 验收门（P0–P2）

**P0**：本方案 + 拍板记录落库；v11 提交（建议）。

**P1-a 语法/构建（Windows 侧）**：
```sh
sh scripts/check_syntax.sh src-ui/vtouch_ui.cpp        # 0 错 0 警（-Wall）
sh scripts/build_ui.sh && sh scripts/build.sh ui       # 全绿；gen_ui_chars 自动重扫（字形集变小属预期）
python3 scripts/ci_check.py                            # 全绿（real 模式 / su 内嵌 md5）
```
**P1-c 零核**：收工 `git diff --stat` 仅 `src-ui/vtouch_ui.cpp`+docs；`src/` 与开工基线逐字一致。

**P1-d 形态（真机，起核后）**：
```sh
su -c 'sh /sdcard/vtouch.sh start'                 # 或 ui-deploy.sh all
su -c 'pidof vtouch-ui'                            # 面板在
su -c screencap /data/local/tmp/p1.png             # 目检：速览版式在；框选/区域编辑在；无操作编辑/模板/说明入口
```
**P1-e 保留回执（10 项，逐条）**：
```sh
su -c 'grep -E "面板已启动|regions.conf 共|丢弃清空|方案 切换|区域跟随旋转|op 步 .*找图|面板吞" /data/local/tmp/vt_ui_core.log | tail -40'
```
① 重启核心 → `regions.conf 共 3 条：补入 3` + 区域屏上重现（现盘上 3 区域/2 操作，以验收时点为准）；② 面板切「默认」再切回 → 2 条 `方案 切换` + 屏上随动；③ 转屏 → `区域跟随旋转：… 第 k 批完成（失败 0 条）` + 显示=命中；④ 跑一条含找图的操作用 [运行] 手动触发（现无此操作则临时用 app 造一条）→ `op 步 k/n 找图 …`（证明抓帧链在；辅助证据 `grep -l vt-cap /proc/$(pidof vtouch-ui)/task/*/comm`）；⑤ 点面板不穿透；⑥ 显隐/触摸标记/关闭 UI/退出（退出后 grab 释放、物理触摸恢复）；⑦ `su -c 'ls -l /proc/$(pidof vtouch-ui)/fd'` → `memfd:vtouch-shm` 在、`/dev/input/event*` 0 条；⑧ 杀面板 → 看门狗重启（≤3 次/分钟）；⑨ **区域调整块**：框选新建一条 + 拖动 r1 改位 + 改开关 + 删除 → 核心 `region …` 增改日志、毫秒级、全程不重启；⑩ **热生效-A**：app 保存一次（如改一条区域坐标/操作）→ ~1–2s 内出现自动（重）载日志、屏上随动、无重启；随后**面板自写**（拖一条区域/转屏）→ **不得**再触发重载（自写豁免）。
**P1-f 回归**：`sh scripts/ui-deploy.sh status` 全绿；双图层转屏日志与现版一致。

**P2**：按 §4 P2 表完成；`check_syntax.sh` 三文件绿；复建通过。

## 6. 风险与对策

| 风险 | 对策 |
|---|---|
| `-Wall` 未用符号（删不干净） | T1–T7 每步自查 grep + 未用扫描脚本 + P1-a 兜底 |
| 误删 JNI/被 Java 调用符号 | JNI 段（9088-9221）全保；对照 VTouchUI.java `native` 声明逐一对齐 |
| 误删红榜机制（含 K9 区域调整块） | §3.3 点名表 + P1-e 回执逐条 |
| 热生效-A 自激（面板自写被当外部源） | 自写后刷新 stat 基线 + 防抖；P1-e-⑩ 含「自写不触发」判据 |
| 行号漂移 | 全程函数名/唯一字符串定位；行号只做对账 |
| 与 v11 混览 | 先提交 v11，缩编独立提交 |
| 真机验证撞用户用车 | 查 `topResumedActivity`；用户声明「先别实测」→ 只交构建+静态审查 |
| 文案源误改 | 只删 UI 数组，docs 源一行不动 |

## 7. 待拍板表（推荐默认值）

| # | 事项 | 选项说明 | 推荐 |
|---|---|---|---|
| Q1 | 操作页 | A 只读卡+[运行]/[停止]; B 纯只读; C 整页删 | **A** |
| Q2 | 方案页 | A 列表+[切换]（+[重载]兜底键随 T8）; B 整页删; C 加别的 | **A** |
| Q3 | 区域页 | **A 保持编辑卡（v0.5：区域调整块随运行时保留，见 §3.8）**; B 只读+[隐藏/显示]; C 整页删 | **A** |
| Q4 | 试一下 | A 删（app 编辑悬浮窗「测试素材」已对等）; B 保留 | **A** |
| Q5 | 说明页 | A 删（文案源在 docs，搬 app 另期）; B 保留 | **A** |
| Q6 | 设置页 | A 保留缩编=抓帧间隔一行; B 删（仅 ui.conf） | **A** |
| Q7 | 框选归属 | **已拍板：双方都做——面板保留框选+区域调整块；app 增加框选工具（§3.6，另立小案）** | ✔ |
| Q8 | 执行与次序 | A Native 串行 + 先提交 v11; B 子代理制; C 合并一次构建 | **A** |
| Q9 | 热生效 | A 只要现有「切换往返」（手动两步，不符硬要求）; **B 本期随做免重启-A（mtime 自动重放 + 兜底键，纯面板同构建）**; C 直上免重启-B（动核，并 P2 控制面） | **B** |

## 8. 变更记录

- v0.1（2026-10-08）：初稿——目标/尺子、版图、红榜、验收门、待拍板 8 条。
- v0.2（2026-10-08）：细化——函数级 D/W/K/G 处置表；§2.1 界面速览；§3.5 框选缺口展开；§4 任务级 T1–T7 含自查命令；§5 带命令行判据与当前盘上数据（3 区域/2 操作/current=吕布/schemes×5）；Q3 调整为「只读+[隐藏/显示]」。
- v0.3（2026-10-08）：补 §3.6「app 侧承接『框选』的做法」（拼件清单/新增件/两形态 A·B/取舍）；Q7 指向 §3.6。
- v0.4（2026-10-08）：补 §3.7「运行中调整」阶梯（档 0 切换往返 / 档 1 免重启-A / 档 2 免重启-B / 档 3 实时悬浮层）+ 拍板 Q9（推荐 B=顺带免重启-A）。
- v0.5（2026-10-08）：**剪裁线修订**（用户口径「修改不许重启才生效；运行中最好当场就能调」）——①「区域调整块」（框选/拖动缩放/四键/触发行）归运行时保留（§3.8，新增 K9）；② 热生效-A 升入本期主范围（§3.7 档 1 + 新增 T8）；③ app 侧框选（§3.6）转可选后续；④ D5/G/速览/Q3/Q7/Q9 随修订；⑤ P1-e 增加 ⑨⑩ 两项回执。
- v0.6（2026-10-08）：**拍板记录**——Q1–Q6/Q8/Q9 按推荐；Q7 修订为「双方都做」（面板保留 + app 新增框选工具，另立小案）；执行开始（先提交 v11）。
- v0.7（2026-10-08）：**落码完成（T1–T8）** —— `vtouch_ui.cpp` 9221 → 3809 行（毛删 5674 / 增 262 / 净删 5412）：操作编辑层·视觉段·方案 CRUD·说明页·改名链·取点/键盘·编辑层壳 全删；区域调整块（框选/拖动缩放/四键/触发行）保留；**T8 热生效-A** 落地（live 两文件 + current 的 mtime 轮询 1s → 防抖 600ms → 强制重放当前方案；面板自写经 `hr_files_note()` 豁免；方案页新增 [重载当前方案] 键）；`ope_var_ok` 上移（load 翻译链仍用）。自查：零残留 grep ✓ / static 未使用扫描 0 命中 ✓ / 括号平衡 0 ✓。待办：Windows 构建 + 真机 8 项回执。
