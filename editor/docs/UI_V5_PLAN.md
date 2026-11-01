# vtouch 编辑台 UI v5 方案（控制台风）

日期：2026-10-07 · 状态：**已实施（2026-10-07 晚，P0–P3 全绿）** · 范围：仅 `~/vtouch-editor`（APK 一期口径不变：核心 / 面板零改动）

## 0. 一句话目标

在 v4 基础上把视觉方向换成「控制台风」：**近黑暖调底 + 琥珀单强调 + 锐角 + 等宽数据 + 控制台状态行**；
同时做系统化收口（三档表面 / 语义 token / 组件统一 / 密度收紧）；光效只留 2 处点缀；状态页改 Bento 瓦片。

方向对照（现状 / A / B / C 四格示意）：`~/vtouch-shots/style-compare.html`
现状体检依据：`~/vtouch-shots/render_p0..3.png`（2026-10-07 17:19 轮）
基线：test_report.txt = 通过 15 · 警告 0 · 失败 0（mode=all，12.9s）

## 1. 不可协商项（对账用）

1. **数据口径零改动**：regions.conf v2 / ops v5 解析、序列化、校验、写盘、备份、live==schemes 镜像逻辑全部不动
   （本轮不碰 Regions.java / Ops.java / Schemes.java / Su.java / 各 save() 链路）。
2. **测试台能力不减**：检查项全保留；允许新增探针（mono / glyph），不允许删检查；
   关键文本 4 字样（新建区域 / 新建操作 / 新建空方案 / 核心控制）保留，避免动 `TestBench.java:367` 的 expect[]。
3. **交互铁律**：按钮一律 TextView（Ui.baseBtn 系）；EditText 不设 setGravity；垂直 LinearLayout addView 一律显式 WRAP；
   热区 ≥48dp（密集 40dp）。
4. **离屏渲染友好**：不用 RenderEffect / 实时模糊；光晕用 RadialGradient 手画；renderView / renderWrap 软件 Canvas 可画。
5. **文案零改**（除新增）；字号阶梯 20/16/14/13/12/11 保持。
6. **构建 / 安装 / 测试口径不变**：`build.sh` 管线；ChipService 无感测试台；读报告先核对时间戳的纪律。

## 2. 设计定稿

### 2.1 Token 表（全部落 `Ui.java:31-33`）

| token | v4 | v5 | 用途 |
|---|---|---|---|
| BG | 0B0F16 | **0C0E10** | 应用底（近黑暖调） |
| HEADER 渐变 | 17212F→111826 | **14171B→0C0E10** | 头部（`Shell.java:36`） |
| CARD (SURF1) | 1A2330 | **14171A** | 卡片面 |
| CARD2 (SURF2) | 232E3E | **1D2126** | 次级面：输入框 / 面板 / 胶囊底 |
| LINE | 283349 | **22262B** | 细分割线 |
| CARD_LINE | 32405A | **272D34** | 卡片 / 组件描边 |
| TEXT | E9EDF5 | **E9EDF1** | 主文字 |
| DIM | 9EACC2 | **9BA7B4** | 次要文字（对比度微提） |
| FAINT（新增） | — | **6E7986** | 禁用 / 弱化（替换散落 0xFF5A6473 等） |
| ACCENT | 5C93FF | **E8B14F** | 琥珀强调 |
| ACCENT_PRESS（新增） | — | **C9973F** | 按压 / 次级强调 |
| ON_ACCENT（新增） | — | **17130A** | 琥珀底上的文字 |
| OK | 3ECF8E | 3ECF8E | 成功 / 运行 |
| WARN | FF6B6B | **F07178** | 危险 / 警告 |

### 2.2 光效（仅 2 处）

- 头部「核心运行中」胶囊：OK 色 35% 光晕（新增 `Ui.glow()`，RadialGradient 手画，软件渲染可画）。
- 当前方案卡：琥珀 1dp 描边 + 顶部内高光；外光晕 P2 视离屏效果决定（差则降级为纯描边）。

### 2.3 形状与密度

- 圆角：卡片 16→**8**、面板 12→**8**、按钮 10→**6**、chip 8→**4**、对话框 22→**12**、toast 12→**8**、徽标 8→**5**。
- 密度：卡片内边距 16→12、列表卡间距 10/5→8/4；热区口径不变（48 / 40）。

### 2.4 组件规范

1. **头部**：渐变幅度减小；V 徽标 = 琥珀底 + ON_ACCENT 字 + r5；标题 20 bold；刷新 = btnFlat 琥珀。
2. **状态行（控制台行）**：两枚方胶囊（r4、1px 描边、mono 字）：`● 核心 pid 6035`（OK + 光晕）/ `方案 juzi`（琥珀描边）；未运行 = FAINT 描边、无光晕。
3. **页签**：文字 14；选中 = 琥珀文字 + 2dp 琥珀下划线；未选 = DIM；行高 44；删除 v4 的 20% 胶囊底（含 `Shell.java:120` 硬编码 0x335C93FF）。
4. **卡片**：SURF1 + CARD_LINE r8 + 内距 12；可点卡 pressableStroked；停用 α0.62 保持；ID 标题 mono bold 15。
5. **标签胶囊**：改 1px 描边式（透明底 + 着色文字 + r4）：类型=DIM、开关型=琥珀、绑定=OK；状态页「当前」胶囊 = 琥珀实心 + 深字（唯一实心胶囊）。
6. **按钮五态**（统一 r6）：primary=琥珀实心 + ON_ACCENT 字；ghost=透明 + 1px CARD_LINE 描边；flat=纯文字（保持）；tonal=WARN 14% 底 + WARN 字（危险）；disabled=FAINT 字 + SURF2 底。
7. **表单（Forms）**：输入框 = SURF2 底 + 1px 描边（聚焦琥珀；不设 gravity 铁律保持）；Seg / 类型 chips 选中 = 琥珀 20% 底 + 琥珀字 + 1px 琥珀描边；Switch 拇指琥珀；步进按钮 SURF2 / 按压 token 化。
8. **对话框 / toast / 空态 / StepFlow**：对话框 r12 卡片面；toast 改 SURF2 + 描边 r8；StepFlow 编号圆点 mono、连线 CARD_LINE 保持、控制流类型琥珀（随 ACCENT 自动）；空态按钮换 ghost。

### 2.5 状态页 Bento（D）

2×2 瓦片 + 全宽控制台：

- 瓦片①「核心控制」：● 状态 + pid + [启动 / 停止]（**保留“核心控制”字样**，expect[] 不破）
- 瓦片②「su」：● su root 可用 / 不可用
- 瓦片③「方案」：当前方案名 + `区域 N · 操作 M`（mono 计数）
- 瓦片④「工具」：自检 / 日志尾 / 刷新
- 下方：控制台面板（mono 保持；首两行着色保持）

## 3. 改动清单（文件:行号）

| 文件 | 改动 |
|---|---|
| **Ui.java**（核心） | :31-33 常量组替换 + 新增 FAINT/ACCENT_PRESS/ON_ACCENT；:42-79 新增 glow()，其余工具保持；:117-127 card() r8/内距12/间距8,4；:130-136 panel() r8；:139-217 按钮系 r6 + 新配色 + ghost/disabled 规范；:220-230 chip() 描边式 r4；:233-257 emptyState 按钮换 ghost；:262-337 对话框 r12；:347-367 输入框 SURF2 描边式；:369-374 divider token；:408-420 不动 |
| **Shell.java** | :36 渐变 token；:40-45 徽标琥珀；:59-65 状态行（方胶囊 + 光晕）；:67-90 页签下划线式；:110-123 showTab 删 0x335C93FF；:140-156 refreshHeader 光晕开关 |
| **MainActivity.java** | :66-103 toast 配色 token 化（0xFFF2F5FA / 0xF0262E42） |
| **StatusPage.java** | :18-61 build() Bento 重构；:64-71 applyCoreState 用 FAINT；:73-97 refresh 计数行 mono；:119-147 保持 |
| **RegionsPage.java** | :111-121 工具行（按钮换型自动）；:136-137 卡片 r8；:143-177 标题 / 标签 / 坐标（:163 已 mono） |
| **OpsPage.java** | :153-154、:315-317 卡片 r8；:173-184 标签行；:302-315 步骤行；:364-375 bottomBar |
| **SchemesPage.java** | :87-88 当前卡 0x665C93FF → 琥珀 token；:94-118 行内按钮色 |
| **Forms.java** | :89-90 变量按钮；:133 步进按压色；:243-244 Switch tint 琥珀；:258-315 Seg 选中态；:317-373 Chips；:378-411 pickDialog |
| **StepFlow.java** | :36-48 连线 / 圆点（圆点 mono）；:60 控制流色随 ACCENT；:70 按压 |
| **StepEdit.java** | :90 类型 chips 选中色 |
| **ProbeService.java** | :99-102 胶囊配色跟 token（微调） |
| **TestBench.java** | :508-510 符号探针追加 `▸ › ⚠ ● ✓`；新增 mono_probe.png（数字/ID/中文混排等宽探针）；:426/:436/:488 渲染底色随 token |

## 4. 分期与验收门

> 纪律：每门全绿才进下一门；跑测试前先唤醒屏幕（`su -c "input keyevent 224"`）；读报告先核对时间戳；
> 装包后 sleep 3 再起服务。动手前备份：`cp -r app/src app/src.bak-v5`（无 git，回滚用文件级备份）。

### P0 基建（Ui.java + Shell.java + MainActivity.toast + StatusPage 色值）

1. 前置：先跑一轮基线（亮屏 → `su -c "am start-foreground-service -n com.vtouch.editor/.ChipService"` → 约 15s → 报告全绿）。
2. 判据：`~/vtouch-editor/build.sh` 构建过 + 安装；复跑全绿（失败 0，警告 ≤ 基线 0）；
   render_p0..3 审图（头部 / 页签 / 按钮 / 卡片已换风格）；新增 mono_probe.png 与 glyphs.png 无豆腐块。

### P1 页面与表单（四页细节 + RegionEdit / StepEdit / StepFlow / Forms / 对话框）

- 判据：全套渲染产物（render_p0..3 / edit_region / edit_step / step_flow / dlg_confirm / dlg_form / move_dialog / field_probe）更新且逐张审；布局断言 0 提示；关键文本 4/4 命中。

### P2 Bento + 光效

- 判据：render_p3 新布局审图（四瓦片 + 控制台）；光晕离屏渲染可接受（不可接受 → 降级纯描边并记录）；真机点按抽查（胶囊复跑 + toast 可见）。

### P3 真机终验

- 判据：全页点按走查；`logcat -d | grep 'freeze uid'` 只允许出现在跑完之后；`topResumedActivity` 跑前跑后不变、无编辑器 Activity 残留；data 自检全绿；写盘回归由 data 检查覆盖。

## 5. 待拍板表（推荐默认）

| # | 项 | 推荐默认 | 备选 |
|---|---|---|---|
| 1 | 强调色 | **琥珀 E8B14F** | 仪器青 4FD8E0 / 保留蓝 5C93FF |
| 2 | 页签样式 | **琥珀下划线式** | 保留胶囊（换琥珀） |
| 3 | 状态页 | **Bento 四瓦片** | 只美化控制台、不动布局 |
| 4 | mono 范围 | **数字 / ID / 坐标 / pid / 日志 / 计数** | 仅坐标 / 日志（现状） |
| 5 | V 徽标 | **琥珀方章 r5** | 保留蓝渐变 |
| 6 | 光效强度 | **仅 2 处** | 全关（纯描边） |
| 7 | 测试胶囊 | **跟随 token 微调** | 不动 |

## 6. 实施记录（2026-10-07）

- P0–P3 全部完成；测试台四轮复跑全绿（通过 15 · 警告 0 · 失败 0，约 12.5s/轮）。
- 产物：`render_p0..3 / edit_region / edit_step / step_flow / dlg_* / field_probe / glyphs / mono_probe / glow_probe`（`~/vtouch-shots/v5-final/`）。
- 真机抽查：状态胶囊光晕可见（柔和不刺眼）、toast 正常、四页渲染无异常；测试跑前跑后前台应用未被测试抢占。
- 探针新增：mono_probe（等宽渲染）、glow_probe（光晕/内高光）；glyphs 增补 `› ⚠ ●`。
- 测试台 expect[] 未动（「核心控制」字样保留在瓦片①）。
- 备份：`app/src.bak-v5`（源码）、`out/vtouch-editor-v4.apk`（v4 安装包）。

## 7. v5.1 头部重设计（2026-10-07 晚，已实施）

- 名称：应用名 / 桌面名 = **vtouch**（原「vtouch 编辑台」；改名 4 处：Shell 标题、manifest ×2、自检报告抬头）。
- 品牌：字标（**Geist Medium**，assets/fonts/geist-medium.ttf）+ 记号「**区域方点**」（圆角方框 + 中心触点；代码绘制 Ui.brandMark）。
- 头部结构：品牌行（记号 + 字标 + ⟳）→ 控制台读数行（mono：左核心状态 + 光点 / 右方案名）→ 下划线页签；刷新降噪为图标；原大分割线去除（页签细线即分隔）。
- 等宽升级：Ui.mono / monoBold 改用 **JetBrains Mono**（assets/fonts/jetbrains-mono.ttf；中文回退系统，探针验证无豆腐块）。
- 图标：应用图标重绘为同款记号（PIL 脚本 ~/vt-fonts/make_icon.py，v2.1 比例：线宽 26 / 点 38 / 圆角 78 + 微渐变）；旧图标留档 `app/icon/ic_launcher-v1.png`。
- 构建：build.sh 增加 assets/fonts 两条附加条目（pack_apk 支持 `apk路径=本地文件`）；APK 约 398KB。
- 验证：测试台复跑全绿 ×2（12.5s / 13.0s）；新增 brand_probe.png；render_p0..3 审图通过。
