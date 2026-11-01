# 视觉素材实施计划（编辑台 v7 · 抓帧/看图器/模板·点集/本地试一下）

> **给执行者：** REQUIRED SUB-SKILL：用 `superpowers:subagent-driven-development`（推荐）或 `superpowers:executing-plans` 按任务逐条实施。步骤用 `- [ ]` 勾选跟踪。
> **单设备串行**：所有真机安装/测试一次只有一个执行者在跑，不得并发。

**Goal（一句话）**：把「抓帧 → 看图 → 框选/点选/吸色 → 模板·点集增删改查 → 本地试一下」的完整视觉素材编辑闭环做进 vtouch 编辑台（app 自含、仓库零改动），素材落盘后核心/面板直接可用。

**Architecture**：抓帧走 `su screencap` raw（P0 实测 162ms、16B 头 `u32le{w,h,format=1,colorspace=1}`、数据区 w×h×4）→ 帧库 = PNG + meta（最近 10 帧）；匹配 = `src/vt_vision.c` 标量路径 Java 直译（`Matcher`）做「本地试一下」；素材文件（`VTM1`/`VTP1`）逐字节镜像面板写端、经 `Su.writeAtomic` 落 `/data/local/vtouch-runtime/templates/`；看图器 `ImageView`（框选/点选/吸色/点集多模式）+ 会话页 `VisualPage`；悬浮抓帧浮标 `CaptureChip` 复用探针胶囊基建（前台服务 + SYSTEM_ALERT_WINDOW + 防速冻白名单）。

**Tech Stack**：Android 纯 Java（javac --release 8 → d8，minSdk 24 / target 29），零第三方依赖；构建 `~/vtouch-editor/build.sh`；自检 = TestBench（既有检查全保持 + 新增检查逐任务加）。

**Spec**：`docs/UI_V7_VISION_PLAN.md`（同目录；先读 §1 不可协商项与 §2 对等清单）。本计划实现其 §2、§3、§6；P0 门数字与对等清单即验收尺子。

## Global Constraints（每条任务隐含）

- 仓库（`~/vtouch-project`）零改动；面板/核心一行不改、不动共享内存/WS。
- 素材格式逐字节（写端参照 `vtouch_ui.cpp:7020-7022`、`:7275-7277`）：
  - `.tmpl` = `"VTM1"` + ver u32=1 + w u16 + h u16 + rot u8 + res u8=0 + gray[w*h] + 尾 `x u16 + y u16 + 平均色 r/g/b u8×3 + res u8`（8B，总是写）。
  - `.pts` = `"VTP1"` + ver u32=1 + n u16 + res u16=0 + base_rgb u32 + base_tol u16 + n×{dx i16, dy i16, rgb u32, tol u16} + 尾 `x u16 + y u16`。
  - 灰度 = `(77r+150g+29b+128)>>8`（唯一来源 `vt_vision.h:35-52`）；平均色 = 区域逐通道和 ÷ (w×h) 取整（照 `vtouch_ui.cpp:7044/7067-7069`）。
- 坐标契约：一切读数/落盘 = **竖屏逻辑坐标**；帧↔逻辑换算逐行镜像 `vt_vis_frame_to_logic` / `vt_vis_logic_rect_to_frame`（`vt_vision.c:655-701`）。
- 命名：`[A-Za-z0-9_-]`、1..15、裸 `-` 拒绝、`.tmpl`/`.pts` **共用名字空间**；重名拒绝，重编辑同名 = 覆盖，改名 = 另存。
- 写盘纪律：`Su.writeAtomic`（原子 + chmod 644 + chown root:root + mv）；**测试不得污染** `/data/local/vtouch-runtime/`（scratch 目录 + `Assets.dir` 重定向）。
- 测试台纪律：每任务收尾「构建 → 安装（`pm install -r` 后 `sleep 4`）→ `run-test.sh`」；新检查 [OK]、既有检查不回归；新页面/组件出离屏渲染件；报告首行时间戳核对是本轮。
- UI 口径照 v5.1 控制台风（`Ui`/`Forms` 既有控件优先）；按钮一律 TextView（`Ui.baseBtn` 系）；**表单输入框不设 setGravity(CENTER*)**（离屏渲染不可见血泪坑）；垂直 LinearLayout `addView` 默认 MATCH_PARENT 宽，需要 WRAP 的显式包装（既有血泪坑）。
- 编辑台非 git：改动前备份 `app/src.bak-v7`；每任务构建产物即版本（`out/vtouch-editor.apk`）。

**命令速查**
```
备份:   cp -r ~/vtouch-editor/app/src ~/vtouch-editor/app/src.bak-v7
构建:   bash ~/vtouch-editor/build.sh          # 期望尾行 OK -> .../out/vtouch-editor.apk
安装:   su -c 'pm install -r /data/data/com.termux/files/home/vtouch-editor/out/vtouch-editor.apk' && sleep 4
测试:   su -c 'sh /data/data/com.termux/files/home/vtouch-editor-run-test.sh'   # 唤屏+触发+等新报告
拉产物: su -c 'sh /data/data/com.termux/files/home/vtouch-shots-pull.sh v7'
核心:   su -c 'sh /sdcard/vtouch.sh start|stop|status'
```

## Review Focus（最可能咬人的五类输入；测试挂在所属任务）

1. **旋转态**：横屏帧（3168×1440）下取点/框选读数必须是竖屏逻辑坐标且往返一致 → T2 映射表 + T6/T10 横屏 E2E。
2. **名字边界**：共享名字空间互斥、裸 `-`、15 字符、非法字符 → T1。
3. **数值边界**：阈值/容差 0 与 255、点集 n=1/16、模板 <8 直搜 / ≥8 金字塔、SAD 恰等 limit（含等号）→ T2。
4. **文件损坏/旧格式**：坏 magic / ver≠1 / 截断 / 4B 旧尾（无平均色）/ 超尺寸 → T1（拒绝或降级，绝不崩）。
5. **抓帧失败路径**：su 被拒 / 息屏黑帧 / 写盘失败 → T3（可读原因，不静默；浮标态可见）。

---

## Task 0：P0 实测收口 + 备份

**Files:** Modify `docs/UI_V7_VISION_PLAN.md` §9（实施记录）。

- [ ] 备份：`cp -r ~/vtouch-editor/app/src ~/vtouch-editor/app/src.bak-v7`
- [ ] 已实测（记录进 §9）：raw=**162ms**、PNG=**1354ms** → 编码定 **raw**；16B 头 `u32le{w=3168,h=1440,fmt=1,colorspace=1}`；文件=16+w×h×4 逐字节吻合（当前设备横屏态）；内容抽样非黑（亮屏）。
- [ ] 待测门（各任务就位后补测）：P0-② 面板排除（T10）、P0-④ 映射对拍（T2 单测 + T6 真机）、P0-⑤ 免速冻（T5）。
- [ ] 记录 `su screencap` 落盘命令模板：`su -c "screencap '<path>' && chmod 644 '<path>'"`。

**验收**：§9 含上述数字与结论；`src.bak-v7` 存在。

## Task 1：Assets.java（素材文件层）+ 格式测试

**Files:**
- Create: `app/src/com/vtouch/editor/Assets.java`
- Modify: `app/src/com/vtouch/editor/TestBench.java`（新检查块 `assetsChecks`，接入 run()）

**Interfaces（后任务依赖）:**
```java
public final class Assets {
    public static String dir = Ops.TMPL_DIR;            // 测试重定向到 scratch
    public static final class Tmpl { public int w,h,rot,x,y; public boolean hasXy; public int avgRgb; public byte[] gray; }
    public static final class Pt   { public int dx,dy,rgb,tol; }
    public static final class Pts  { public int baseRgb,baseTol,x,y; public boolean hasXy; public Pt[] pts; }
    public static boolean nameOk(String s);             // [A-Za-z0-9_-]{1,15}、裸 "-" 拒绝
    public static byte[] bytesTmpl(Tmpl t);  public static Tmpl parseTmpl(byte[] b);   // 纯函数；坏 → null
    public static byte[] bytesPts(Pts p);    public static Pts  parsePts(byte[] b);
    public static List<String> namesTmpl();  public static List<String> namesPts();    // ls dir
    public static Tmpl readTmpl(String name); public static Pts readPts(String name);  // su cat + parse
    public static void writeTmpl(String name, Tmpl t); public static void writePts(String name, Pts p);
    public static boolean exists(String name);          // .tmpl 或 .pts
    public static boolean delete(String name);          // 删存在的文件；返回是否有删除
    public static boolean rename(String oldName, String newName, boolean isPts);
    public static Bitmap thumbTmpl(String name,int maxPx);   // .tmpl 灰度→Bitmap（列表/字段缩略图）
    public static Bitmap thumbPts(String name,int maxPx);    // .pts 基准色块 + 参考点示意
    public static List<String> refs(String name);       // "操作 op1 · 方案 默认" 条目（live + 各 schemes）
    static List<String> refsFromTexts(List<String> opsTexts, String name);  // 纯函数（用 Ops.File.parse）
    public static byte[] grayFromRgba(byte[] rgba,int w,int x,int y,int bw,int bh,int[] avgOut3);
    public static byte grayPx(int r,int g,int b);
}
```

- [ ] **步骤 1：写检查（先接线跑红）**——在 TestBench 加：金样 2×2 tmpl 逐字节（手算 hex：`VTM1`+ver+w=2,h=2,rot=0,res=0+gray{1,2,3,4}+x=5,y=6,avg 7/8/9,res=0）；bytesTmpl→parseTmpl 往返；4B 旧尾（无平均色）→ hasXy=true & avgRgb=-1；坏 magic / ver=2 / 截断 → null 不崩；名字边界（""、"裸-"、16 字符、"a.b" 拒；15 字符、`A-Z_09-` 过）；pts 往返（含负 dx/dy、n=16）；refsFromTexts 合成 ops 文本（找图 ref==名、找色多点 ref==名、其它步骤不匹配）。跑 `run-test.sh` → 新检查应 FAIL/异常（函数未实现）。
- [ ] **步骤 2：实现纯函数**（bytes/parse/gray/nameOk/refsFromTexts）→ 跑 → 纯函数检查全 [OK]。
- [ ] **步骤 3：实现 su 层**（list/read/write/delete/rename，经 `Su.run/writeAtomic`；`Assets.dir` 可重定向）→ 测试里把 `Assets.dir` 指到 `filesDir/assets-scratch` 跑写→读→改→删全链 → [OK]。
- [ ] **步骤 4：构建 + 全套测试台**（既有不回归）。

**验收**：新检查全 [OK]；总结行"失败 0"。

## Task 2：Matcher.java（引擎标量直译）+ 测试

**Files:**
- Create: `app/src/com/vtouch/editor/Matcher.java`
- Modify: `TestBench.java`（检查块 `matcherChecks`）

**Interfaces:**
```java
public final class Matcher {
    public static final class Frame { public byte[] rgba; public int w,h,rot; public byte[] gray,gray2,gray4; }
    public static Frame prepare(byte[] rgba,int w,int h,int rot);          // 灰度 + 1/2 + 1/4 金字塔
    public static final class Hit { public final int x,y; }                 // 帧坐标
    public static final class Pt  { public int dx,dy,rgb,tol; }              // 多点参考点（与 Assets.Pt 字段序一致，转换即逐字段拷贝）
    public static Hit findImage(Frame f,byte[] tmpl,int tw,int th,int thresh,int rx,int ry,int rw,int rh);
    public static Hit findColor(Frame f,int rx,int ry,int rw,int rh,int rgb,int tol);
    public static Hit findColorMulti(Frame f,int rx,int ry,int rw,int rh,int baseRgb,int baseTol, Pt[] pts,int n);
    public static boolean colorCheck(Frame f,int x,int y,int tw,int th,int avgRgb);  // 容差 48；avgRgb<0 → true(跳过)
    public static void frameToLogic(int rot,int fw,int fh,int fx,int fy,int[] out2);
    public static int[] logicRectToFrame(int rot,int fw,int fh,int lx,int ly,int lw,int lh); // {fx,fy,fw2,fh2}
    public static byte grayPx(int r,int g,int b);
    public static int[] rotateTmpl(byte[] gray,int tw,int th,int rotFrom,int rotTo,int[] outWH); // 模板方向差（照 vt_ops.c 旋转口径）
}
```
- 直译来源（只搬标量）：`vis_gray_scalar :134-145`、`vis_down2 :159-171`、`vis_sad_ok :388-403`（行级早退）、`vis_image_direct :416-432`、`vis_image_pyramid :452-505`（粗筛余量 `VT_VIS_COARSE_MARGIN=8` `:52`；中心裁剪 `+tw4+1`；flag 行缓存；1/4→1/2→像素全局行优先、命中即停）、`vis_px_match :183-193`、`vis_color_row_scalar :248-255`、`vis_pts_ok :272-286`、`vis_clip_span :298-308`、`vt_vis_find_color :557-575`、`vt_vis_find_color_multi :589-621`、`vt_vis_find_image :634-651`；上限 `MAX_W/H=4096`、`PT_MAX=4096`。模板旋转与颜色校验口径照 `src/vt_ops.c`（`op_vis_read_tmpl`/视觉步执行段）。
- [ ] **步骤 1：映射表用例**（rot0..3 四角期望值，按 `vt_vision.h:10-17` 注释手推；logicRect→frame 期望值）→ 红 → 实现 `frameToLogic`/`logicRectToFrame` → [OK]。
- [ ] **步骤 2：grayPx**（(0,0,0)→0；(255,255,255)→255；(100,150,200)→141）→ [OK]。
- [ ] **步骤 3：findColor**（行优先首个；diff==tol 命中 / ==tol+1 不中；区域裁剪；空交集 null）→ [OK]。
- [ ] **步骤 4：findColorMulti**（基准+参考点命中/候选否决后继续/参考点越界否决/n=16）→ [OK]。
- [ ] **步骤 5：findImage**——直搜（6×6 补丁@(10,12)；双匹配取行优先首个）；金字塔（16×16@(100,200)；与内联直搜参考实现在 SAD=0 场景一致）；thresh=0；SAD==limit 含等号；模板>区域 → null → [OK]。
- [ ] **步骤 6：性能冒烟**（1440×3168 合成帧 + 64×64 模板 → 正确且记录 ms，目标 ≤5s）。
- [ ] **步骤 7：构建 + 测试台**。

**验收**：全 [OK]；映射/判定与 `vt_vision.c` 逐字对账（评审对照一遍标量源码）。

## Task 3：Capture.java + FrameStore.java（抓帧 + 帧库）

**Files:**
- Create: `app/src/com/vtouch/editor/Capture.java`、`FrameStore.java`
- Modify: `TestBench.java`（检查块 `frameChecks`）

**Interfaces:**
```java
public final class Capture {
    public static final class Shot { public byte[] rgba; public int w,h,rot; }
    public static void init(Context c);                 // 静态注入（MainActivity/ProbeService/CaptureChip 各调）
    public static Shot grab() throws Exception;         // su screencap raw → 读回 → 16B 头解析
    public static int displayRotation();                // Display.getRotation()
}
public final class FrameStore {
    public static final class Entry { public String name; public int rot,w,h; public long ts; }
    public static void init(Context c);                 // dir = getExternalFilesDir(null)/frames
    public static Entry save(Capture.Shot s);           // PNG + meta.txt 追加（"name rot w h ts"）；prune 保留 10
    public static List<Entry> list();                   // 新→旧
    public static Entry latest();
    public static Bitmap loadBitmap(Entry e);           // PNG 解码
    public static byte[] loadRgba(Entry e);             // ARGB→RGBA 字节（供 Matcher）
}
```
- raw 解析：读文件校验 `len == 16 + w*h*4`；数据区即 RGBA 交错字节；异常带可读原因（"su 拒绝" / "截帧尺寸异常 a×b" / "写入失败"）。
- [ ] **步骤 1：实现 grab/displayRotation**；测试：抓一帧断言尺寸 vs `displayRotation`（横屏 3168×1440 / 竖屏 1440×3168）、字节数、抽样非黑（亮屏）、记录耗时 → [OK]。
- [ ] **步骤 2：FrameStore** save/list/latest/loadRgba↔loadBitmap 小图像素一致；塞 12 条出 10（prune）→ [OK]。
- [ ] **步骤 3：构建 + 测试台**。

**验收**：真机抓帧 [OK]；轮换 [OK]；失败路径文案经代码评审。

## Task 4：ImageView.java（看图器视图）

**Files:**
- Create: `app/src/com/vtouch/editor/ImageView.java`
- Modify: `TestBench.java`（渲染件 `frame_view.png` + 数学用例）

**Interfaces:**
```java
public class ImageView extends View {
    public static final int MODE_VIEW=0, MODE_BOX=1, MODE_POINT=2, MODE_PTS=3;
    public interface Listener { void onBox(int x,int y,int w,int h); void onPoint(int x,int y); }
    public void setFrame(Bitmap bmp,int rot);   public void setMode(int m);
    public void setListener(Listener l);
    public void setSelection(int x,int y,int w,int h);  public int[] selection();   // {x,y,w,h} 帧像素
    public void setHit(int x,int y,int w,int h);        // -1 = 清除
    public void setBase(int x,int y,int rgb);   public void setMarkers(int[] xs,int[] ys);
    public float zoom(); public void zoomStep(int dir); public void fit();
    int[] screenToFrame(int sx,int sy);   int[] frameToScreen(int fx,int fy);       // 包内（测试）
}
```
- 手势口径：面板既有（`vtouch_ui.cpp:271-279`：缩放 1..8x 步进 0.25、平移、四角手柄、框内拖动、已选态再调）+ 升级（双指捏合、拖动放大镜 120dp/4x+十字线+逻辑坐标文字、命中 ≥24dp、框最小 8×8 钳帧内）。
- [ ] **步骤 1：数学用例**：screenToFrame↔frameToScreen 往返（zoom {1,2.5,8} + pan）；缩放钳制；框 min/clamp → 红 → 实现 → [OK]。
- [ ] **步骤 2：离屏渲染**：合成位图 + 框 + 命中框 → `frame_view.png`；断言 `selection()` 回读一致、无异常。
- [ ] **步骤 3：构建 + 测试台 + 拉图目视**。

**验收**：渲染件产出、数学用例 [OK]；真机手感（放大镜/捏合/手柄）用户过目。

## Task 5：CaptureChip.java（悬浮抓帧浮标）+ 武装联动

**Files:**
- Create: `app/src/com/vtouch/editor/CaptureChip.java`
- Modify: `app/AndroidManifest.xml`（加 service 条目，照 ChipService 模式）、`TestBench.java`（可选检查）

**Interfaces:**
```java
public class CaptureChip extends Service {
    public static void start(Context c, boolean arm);   // 编辑器 [去抓帧] 调用；arm=true 武装态
    // onStartCommand extras：arm=true（武装）/ capture=true（立即抓帧，测试用）
}
```
- 胶囊：圆形"图"字浮标（48dp，`Ui.pressable`），拖动移位（更新 lp.x/y）、单击抓帧、长按收起；武装态琥珀描边 + "点我抓帧"。
- 抓帧流程：自隐 150ms → `Capture.grab()` → `FrameStore.save()` → 闪"✓"；arm 态再 `startActivity(MainActivity + extra "visual=latest")`（SYSTEM_ALERT_WINDOW 豁免后台启动）+ 收起。
- 前台服务化复用 `ProbeService.startAsForeground(this,"vtouch 抓帧")`；`Su.init/Capture.init/FrameStore.init` 都要调。
- [ ] **步骤 1：实现 + manifest + 构建**。
- [ ] **步骤 2：真机**：`su -c "am start-foreground-service -n com.vtouch.editor/.CaptureChip --ez capture true"` → `su -c "ls <external files>/frames"` 计数 +1；`dumpsys window | grep -i capturechip` 见窗 → [OK]。
- [ ] **步骤 3：P0-⑤ 真机全链**：游戏内点浮标抓帧 → app 帧库见新帧；跑前跑后 `topResumedActivity` 不变、无 freeze 日志。

**验收**：P0-⑤ 过；arm 回跳真机手过（回跳后在 T6 接线完成前允许先只提示）。

## Task 6：VisualPage.java —— 模板模式 + 看图 + 本地试一下（P1a 核心）

**Files:**
- Create: `app/src/com/vtouch/editor/VisualPage.java`
- Modify: `Shell.java`（layer 机制：`openLayer(View)/closeLayer()/layerOpen`；content 外包 FrameLayout 叠层）、`MainActivity.java`（back 链 + `visual=latest` extra）、`TestBench.java`（渲染件 `edit_tmpl.png`）

**Interfaces:**
```java
public class VisualPage {
    public static final int MODE_TMPL_NEW=1, MODE_TMPL_EDIT=2, MODE_PICK_POINT=3, MODE_PICK_COLOR=4, MODE_VIEW=5, MODE_PTS_NEW=6, MODE_PTS_EDIT=7;
    public static class Session {
        public int mode; public String name;            // EDIT 用
        public String region = "";                      // 步骤"区域限定"名（"" = 全帧）
        public int thresh = 8; public boolean colorCheck = false;
        public String frameName;                        // null = 用最新帧
    }
    public interface Done { void onTmplSaved(String name); void onPointPicked(int lx,int ly);
                            void onColorPicked(int rgb); void onCanceled(); }
    public static VisualPage open(Context c, Session s, Done d);   // 内部 shell.openLayer(root)
    public View root;
}
```
- 布局：顶栏 [取消][标题][保存/确认]；帧源行 [用帧 ▾（FrameStore.list 选择）/ 去抓帧（CaptureChip.start(c,true)）]；工具行 [−][适应][＋]；主区 ImageView；底部动作区按模式。
- TMPL：框选 + 数字微调行（x/y/w/h 四个 `Forms.Num`，±1 步进长按步 10）+ 阈值快捷行 `[−10][−1][值][＋1][＋10]` + 颜色校验 SwitchRow + `[保存并试一下]`。
  - 试一下 = `Matcher.findImage`（区域：`region` 非空→`Regions.File` 查几何→`logicRectToFrame`；空→全帧）+ `colorCheck` 开关 → 命中 `setHit` + 结果行「命中 (fx,fy) 逻辑 (lx,ly) · Nms」/「未命中」/>1.2s 显示"试查中…"。
  - 保存：name 自动建议（`tmpl1` 起，按 `Assets.namesTmpl()` 递增）；已存在 → 覆盖确认对话框（照面板口径）；`Assets.writeTmpl`。
- TMPL_EDIT：读 .tmpl → 灰度合成 RGB 预览（照 `vtouch_ui.cpp:7130-7168`）→ 有 x/y 摆回框 → `[重新截帧]` 换帧重选 → 保存（同名覆盖/改名另存）。
- PICK_*/VIEW：先占位（T7 实现；本任务仅保证模式枚举与渲染不崩）。
- [ ] **步骤 1：Shell layer + MainActivity back/extra** → 构建 + 空层渲染不崩。
- [ ] **步骤 2：TMPL_NEW 全实现**（含数字微调/阈值行/保存/覆盖确认）→ 离屏渲染 `edit_tmpl.png`。
- [ ] **步骤 3：试一下接入**（Matcher + 结果行 + 命中框）→ 合成帧冒烟 [OK]。
- [ ] **步骤 4：TMPL_EDIT**（预览/恢复框/重新截帧/覆盖另存）。
- [ ] **步骤 5：构建 + 测试台 + 拉图目视**。
- [ ] **步骤 6：P0-③/对拍（真机）**：抓帧→存模板→面板「编辑」可读 + 面板试一下稳定命中；app 本地试一下与面板试一下命中坐标一致（横屏也过一次）→ 记录。

**验收（P1a 门）**：步骤 6 全过并记录进 spec §9。

## Task 7：VisualPage —— 点集 + 取点 + 吸色（P1b）

**Files:** Modify `VisualPage.java`、`TestBench.java`（渲染件 `edit_pts.png`、`edit_pick.png`）。

- PTS_NEW：点基准（十字 + 自动吸色 base_rgb）→ 点参考点（自动记偏移 + 吸色；行列表可删；**逐点容差可改**，默认=基准容差——较面板 v1 的升级，核心读端本就支持 per-point tol）→ `[保存并试一下]` = `Matcher.findColorMulti`；模糊提示：全帧基准色命中像素数 > 200 → 结果行 WARN"该色满屏都是，建议加点集约束"（扫全帧计数，一次够用）。
- PTS_EDIT：读 .pts → 摆回（尾 x/y；无位置提示先点基准，照 `vtouch_ui.cpp:7221-7274` 口径）→ `[重选基准]` 整组挪 → 保存（覆盖/另存）。
- PICK_POINT：点选 → 结果行「逻辑 (lx, ly) · 帧 (fx, fy)」→ [确认] → `Done.onPointPicked(lx,ly)`。
- PICK_COLOR：点选 → 色块 + `#RRGGBB` + 容差行 → [确认] → `Done.onColorPicked(rgb)`。
- [ ] **步骤 1：PICK_POINT/PICK_COLOR** → 渲染件 `edit_pick.png`。
- [ ] **步骤 2：PTS_NEW**（含模糊提示与逐点容差）→ 渲染件 `edit_pts.png`。
- [ ] **步骤 3：PTS_EDIT** 摆回/重选基准/覆盖另存。
- [ ] **步骤 4：构建 + 测试台**。
- [ ] **步骤 5：真机**：点集闭环（帧上做点集 → 保存 → 面板可读 + 核心多点找色命中）；取点/吸色与面板取点对拍（同目标 ±0）→ 记录。

**验收（P1b 部分门）**：步骤 5 过并记录。

## Task 8：集成 —— StepEdit / OpsPage（字段入口 + 回填）

**Files:** Modify `StepEdit.java`、`OpsPage.java`、`TestBench.java`（更新 `edit_find.png`/`edit_step.png` 调用点）。

**Interfaces（新）:**
```java
// StepEdit 新增第 8 参（nullable）：
public interface Visual {
    void newTmpl(Forms.Chips field);                    // 会话保存 → 回填 field（名单刷新后 pick）
    void editTmpl(String name);
    void testTmpl(String name,int thresh,boolean colorCheck,String region);
    void newPts(Forms.Chips field);
    void editPts(String name);
    void testPts(String name,String region);
    void pickPoint(Forms.Num field);                    // PICK_POINT → setValue(lx/ly) 两处调用
    void pickColor(int[] rgbOut,int[] tolOut);          // PICK_COLOR → R/G/B（与容差）回填
}
// Forms.Num 新增：public void enablePick(Runnable onPick)   // 标签行加 [取点] 按钮
```
- 找图（`:232` 一带）：模板 chips 下加 `[＋新建][编辑][试一下]` + 缩略图（96dp；`Assets.thumbTmpl(name)` 灰度小图）。
- 找色单点：R/G/B 上方加 `[吸色]`（PICK_COLOR 回填 RGB + colorPrev 刷新）；多点：点集行同三按钮。
- `[试一下]` 从步骤发起：thresh=当前 a1、colorCheck=a2==1、region=expr（空=全帧）。
- `[取点]` 挂载清单：点按 x/y、滑动 起点/终点 x/y、按下 x/y、区域判断 判定点 x/y。
- OpsPage 实现 `Visual`：开 VisualPage 会话（`Done` 回调里更新对应控件 + `refresh()` 名单）；`StepEdit.build` 全部调用点同步（OpsPage 一处 + TestBench `:524/:529/:533` 三处）。
- [ ] **步骤 1：Forms.Num.enablePick + StepEdit 插槽**（接口占位）→ 构建。
- [ ] **步骤 2：找图/找色/点集入口 + 缩略图** → `edit_find.png` 目视。
- [ ] **步骤 3：坐标字段取点 + 吸色** → `edit_step.png` 目视。
- [ ] **步骤 4：真机全链**：步骤内 新建模板 → 保存 → 字段回填 + 缩略图；`[试一下]` 结果行；`[取点]` 回填。
- [ ] **步骤 5：构建 + 测试台**（编译期同步全部调用点）。

**验收（P1a 全门）**：真机「游戏抓帧 → 步骤内新建模板 → 框选 → 保存并试一下命中 → 字段已填 + 缩略图」全程 3-4 步无多余跳转；测试台全绿。

## Task 9：AssetPage.java（素材页）+ Shell 第 5 页

**Files:** Create `AssetPage.java`；Modify `Shell.java`（4→5 页签、`names/pages/showTab/refreshCurrent` 数组与循环）、`TestBench.java`（`asset_page.png`、render_p4）、`~/vtouch-shots-pull.sh`（产物清单加 `frame_view.png edit_tmpl.png edit_pts.png edit_pick.png asset_page.png render_p4.png`）。

- 三段 Seg「模板 / 点集 / 帧库」；行 = 缩略图 + 名 + 元数据（尺寸/点数/时间）+ `[预览][编辑][试一下][改名][删除]`；`[＋新建]`（模板/点集）→ VisualPage 会话。
- 删除：`Assets.refs(name)` 非空 → 列出"操作 op1 · 方案 默认"再确认；改名：同提示"引用会失效"。
- `[试一下]`（行级）：`FrameStore.latest()` + 默认阈值 8 开会话试一下；无帧 → 空态"先去抓帧"。
- 帧库段：列表（缩略图/时间/方向/尺寸）+ `[查看]`（VIEW）+ `[去抓帧]`。
- [ ] **步骤 1：AssetPage（模板/点集段 + CRUD/引用）** → `asset_page.png`。
- [ ] **步骤 2：Shell 5 页签**（小心等宽与 refresh 链）→ render_p0..p4 不回归。
- [ ] **步骤 3：帧库段 + VIEW + 去抓帧**。
- [ ] **步骤 4：构建 + 测试台 + 更新 pull 脚本 + 拉产物目视**。

**验收（P1b 全门）**：素材页真机全功能（增删改查/引用提示/试一下/帧库）。

## Task 10：E2E 验收 + 收尾

- [ ] E2E（竖屏 & 横屏各一）：抓帧 → 模板 → 点集 → 取点 → 吸色 全链。
- [ ] P0-② 面板排除：`su -c 'sh /sdcard/vtouch.sh start'`（面板可见）→ 浮标抓帧 → 拉帧目视：无面板/无浮标、有真实画面 → 记录。
- [ ] 对拍硬门复核：同静态画面 app 本地试一下 vs 面板试一下 命中坐标一致 → 记录。
- [ ] 更新 `UI_V7_VISION_PLAN.md` §9：P0 数字、测试台轮次、对拍结果、遗留项。
- [ ] `run-test.sh` 末轮报告核对（本轮时间戳、失败 0）；`vtouch-shots-pull.sh v7` 归档全部产物。

**验收**：全部硬门有记录；测试台"失败 0"；spec §9 完整。

---

**纪律备注**：本计划为 UI_V7_VISION_PLAN.md 的实现载体；两者冲突时以 spec 为准并回来改计划。编辑台无 git，每任务完成即构建产物留档；`src.bak-v7` 在 T0 固化，后续如需回滚以它为基准。
