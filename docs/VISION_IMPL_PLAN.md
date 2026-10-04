# VISION_IMPL_PLAN — 视觉（找图/找色）实施计划

> **Spec（约束权威）**：`docs/VISION_PLAN.md`（行为/文案/性能唯一来源；与本计划冲突时以 spec 为准）。
> 执行方式：Subagent-Driven（每任务：精确简报 → 实现子代理 → 任务评审 → 修复环；控制方裁决并记账）。
> 任务号是精确号（1.1 / 2.1 / 3.1 / 3.2 / 4.1 / 5.1）；台账 = `.superpowers/sdd/VISION_IMPL_PLAN/progress.md`。

## Global Constraints（所有任务适用）

- **构建门**（每任务收尾必跑，rc 全 0）：
  - `sh scripts/build.sh`（默认核心；**允许 md5 变化**——state/结构增长所致，报告记录新旧值；行为不得变）；
  - `sh scripts/build.sh ui`（UI 核心）与 `VTOUCH_UI_CORE=real sh scripts/build_ui.sh`（面板 real；我方文件 0 告警）；
  - `python scripts/apply_funcdoc.py --check` → 「共调整 0 处、不变式全满足」；
  - `python scripts/ci_check.py`（打包产物变化后需先重打——**由 5.1 统一做**；中间任务 ci 允许因产物未重打而红，报告注明）。
- **funcdoc 纪律**：`src/` 下每个函数（含 static）定义正上方紧贴唯一 Doxygen 块（含 `(vtouch-doc: 名字)`）；文案唯一来源 `scripts/funcdoc_data.py`（新增函数必须加条目）。
- **VT_UI 守卫**：默认（无面板）构建中 `vt_ops.c`/`vt_shm.c`/`vt_panel.c`/`vt_expr.c` 是空 TU；`vt_vision.c` 同样整体 `#ifdef VT_UI`。
- **C 口径**：`-O2 -Wall -Wextra -Werror -D_GNU_SOURCE`；注释/日志中文；日志词照 spec §8 逐字。
- **NEON 纪律**：引擎热路径双实现（NEON + 标量回退，`#if defined(__ARM_NEON)` 分支；`-DVT_VIS_FORCE_SCALAR` 可强制标量以便宿主对照）；两条路径语义一致（单测对照）。
- **行尾**：`src-ui/vtouch_ui.cpp` 全 CRLF（编辑工具注意）；`src/*.c` 保持原行尾；`src-ui/VTouchUI.java` 照原行尾。
- **最小 diff**：只动本任务点名的文件/位置；不改 WS 协议；不动无关代码。
- **提交**：master 直做；**不推送**（推送由用户拍板）；每任务一提交（或一小组提交），提交信息中文、带验收摘要。
- **报告**：写到工作区 `task-<N>-report.md`（状态/提交/测试证据/披露），返回只给状态+提交+一行测试摘要。

## 背景（给所有实施者）

vtouch = Android root 触摸合成引擎（EVIOCGRAB + uinput）+ 原生 ImGui 面板（root app_process + JNI）；「操作」= 步骤序列（现有 9 类：点按/滑动/等待/按下/弹起/区域判断/开关判断/跳转/计算），核心主线程执行。本计划新增**视觉能力**（找图/找色）——先读 spec `docs/VISION_PLAN.md` §1–§3、§6。关键现状锚点：

- 抓屏（已实测，spec §2）：面板进程 binder 直连 `SurfaceFlingerAIDL`（`android.gui.ISurfaceComposer`，事务码 6/7、回复 skip 4B）拿 display token → `ScreenCapture.captureDisplay`（4–7ms/次，100 连抓 avg 8.5ms / 110fps）→ `AHardwareBuffer` → JNI 读回。参考实现 = `build/probe/VProbe7.java`（可读，不入库）。
- 契约：`VT_SHM_VERSION 7u`（`src/vt_shm.h:29`）；memfd 布局在 `src/vt_shm.c`（memfd_create+ftruncate，三区）；面板半边 `-DVT_UI_PANEL` 编译（`scripts/build_ui.sh:77-82`）。
- 面板 JNI：静态命名（`Java_VTouchUI_nativeXxx`，`src-ui/vtouch_ui.cpp:6041+`，无 RegisterNatives）。
- 条件步四档：`OP_COND_ABORT/SKIP/CONT/JUMP`（`src/vt_internal.h`）；分支助手 static 函数在 `src/vt_ops.c:645` 附近（成立侧 a4/j1、不成立侧 a3/j2；「两侧四档一处实现防漂移」）——**视觉步骤复用，不改条件步行为**。
- 步骤：`struct vt_step`（type/a1..a4/ms/ref[16]/j1/j2/expr[64]）；`op_valid` 逐类型校验（`src/vt_ops.c:55+`）。
- 面板：`g_ope_steps`/`g_ope_refs`/`g_ope_exprs`（`src-ui/vtouch_ui.cpp:2951+`）；加步区；说明页 `g_help_lines[]` 16 条（`:5054`）；ops.conf `#vtouch-ops v4` 10 字段。
- 胶水：`src-ui/ui_glue.c`（`vtouch_get_op_step` :435 / `vtouch_op_put` 链）。

**模板/点集文件格式（定稿，本计划权威；2.1 读端、3.2 写端共用）**：
- `.tmpl`（小端）：`"VTM1"`(4B) + `ver u32=1` + `w u16` + `h u16` + `rot u8` + `res u8=0` + `gray[w*h]`（8bit 灰度）。
- `.pts`（小端）：`"VTP1"`(4B) + `ver u32=1` + `n u16`（≤16）+ `res u16=0` + `base_rgb u32` + `base_tol u16` + `pts[n]{ dx i16, dy i16, rgb u32, tol u16 }`。
- 目录：`/data/local/vtouch-runtime/templates/`；文件名 = `<名>.tmpl` / `<名>.pts`；名字规则同 `vt_id_ok`（`[A-Za-z0-9_-]`、1..15、裸 `-` 除外）。

---

## Task 1.1 — 匹配引擎 `src/vt_vision.c`（+ `src/vt_vision.h` + 宿主单测）

**产出**：新文件 `src/vt_vision.c` + `src/vt_vision.h`；宿主单测通过；funcdoc 条目；`scripts/build.sh` ui 分支编译清单加入 `src/vt_vision.c`。

**接口（精确；写进 `src/vt_vision.h`）**：
```c
#define VT_VIS_OK        0
#define VT_VIS_NO_MATCH  1
#define VT_VIS_BAD     (-1)

/* 多点找色的一个参考点：相对基准的偏移 + 目标色 + 容差 */
struct vt_vis_pt { int dx, dy; uint32_t rgb; int tol; };

/* 帧视图（一次性准备：灰度 + 1/2、1/4 金字塔）。静态单例（主循环单线程）。 */
int  vt_vis_frame_prepare(const uint8_t *rgba, int w, int h, int stride);
void vt_vis_frame_release(void);

/* 找色：区域（帧坐标 rx,ry,rw,rh）；命中写首个命中坐标（帧坐标）。 */
int vt_vis_find_color(int rx, int ry, int rw, int rh, uint32_t rgb, int tol, int *ox, int *oy);
/* 多点找色：基准色命中后校验全部参考点；n ≤ 16。 */
int vt_vis_find_color_multi(int rx, int ry, int rw, int rh, uint32_t base, int base_tol,
                            const struct vt_vis_pt *pts, int n, int *ox, int *oy);
/* 找图：模板灰度（tw×th）；阈值 = 平均绝对差上限（0..255）。 */
int vt_vis_find_image(int rx, int ry, int rw, int rh, const uint8_t *tmpl, int tw, int th,
                      int thresh, int *ox, int *oy);

/* 坐标映射（纯函数，宿主可测）：rotation 0..3；帧尺寸 fw×fh（当前方向）；
 * 逻辑 = 固定竖屏。帧↔逻辑单点 + 逻辑矩形→帧矩形（用于区域限定换算）。 */
void vt_vis_frame_to_logic(int rotation, int fw, int fh, int fx, int fy, int *lx, int *ly);
void vt_vis_logic_rect_to_frame(int rotation, int fw, int fh,
                                int lx, int ly, int lw, int lh,
                                int *fx, int *fy, int *fw2, int *fh2);
```
- **零核心依赖**：只 include `<stdint.h> <string.h> <stddef.h>`（NEON 时 `<arm_neon.h>`）；整体 `#ifdef VT_UI`（默认构建空 TU）。
- **算法（照 spec §4）**：灰度（RGBA→灰，整数近似，误差 ≤1/255）；金字塔 1/2、1/4（盒式平均）；找色（NEON `vabdq_u8` + 归约，per-channel max-diff ≤ tol，命中即停，行优先扫描）；多点（基准命中后逐点校验，全对 = 命中）；找图（1/4 粗搜 → 1/2 → 全分辨率精修；粗搜步进 = 金字塔倍数、精修步进 1；SAD 行级早退 SSDA；命中即停；**金字塔路径与全分辨率直搜结果必须一致**——单测断言）。
- **宿主单测**（不入库，`build/test_vt_vision.c`；两种编译：`gcc -O2 -Wall -Wextra -Werror -DVT_UI -Isrc build/test_vt_vision.c src/vt_vision.c -o build/test_vt_vision.exe && ./build/test_vt_vision.exe`，与加 `-DVT_VIS_FORCE_SCALAR` 再跑一遍）：合成帧覆盖——找色命中坐标精确/容差边界（tol 与 tol−1）/区域限定（区域外有同色不命中）/多点（参考点缺一不中）；找图（图案多位置命中首个、缩放不命中、阈值边界、全屏未命中、金字塔 vs 全分辨率一致、NEON vs 标量逐字段一致）；坐标映射四方向（0..3 往返恒等 + 已知点抽查）。全过打印 `PASS n/n`。
- **funcdoc**：所有对外函数 + 关键 static 加 `scripts/funcdoc_data.py` 条目；`apply_funcdoc.py --check` 0/0。
- **验收**：默认构建 md5 **不变**（空 TU）；`sh scripts/build.sh ui` rc=0（已含 vt_vision.c）；宿主单测两种编译全过；funcdoc 0/0。

## Task 2.1 — 契约 v8 + 核心集成（vt_shm.h / vt_shm.c / vt_internal.h / vt_ops.c）

**产出**：契约 8（帧区；步骤结构不变）+ 新步骤类型；核心可跑视觉步骤（请求→等待→匹配→结果槽→四档）；胶水/面板无需签名变更（区域名随 expr 列）。

**点**：
1. `src/vt_shm.h`：`VT_SHM_VERSION 7u → 8u`（+版本注释行）；新增帧区（照 spec §3.1）：`struct vt_shm_frame_hdr`（magic `VFRM`/version/seq(seqlock)/width/height/stride/format/rotation/req_seq/req_pending/buf_idx/flags/err/ts_ns）+ 双缓冲（各 w×h×4，w/h = 逻辑尺寸——**两方向同字节数**）。
2. `src/vt_shm.c`：memfd total 计算 + 帧区（头 + 2 缓冲）；面板半边可见帧区访问；初始清零；尺寸断言（同现有区纪律）。
3. `src/vt_internal.h`：`#define OP_STEP_FINDIMAGE 10` / `#define OP_STEP_FINDCOLOR 11`（注释：字段映射照 spec §6.1 定稿；**区域限定复用 expr 列**，不新增字段——保 4096 页限）；视觉常量（抓帧超时默认 1000ms）。
4. `src/vt_ops.c`：
   - `op_valid`：+ `case OP_STEP_FINDIMAGE`（ref=模板名合法非空；expr 空或合法区域名；a1=0..255；a2/ms=0；a3/a4 档位 0..3；j1/j2 跳转目标域同条件步）；+ `case OP_STEP_FINDCOLOR`（a1=0/1 模式；ref 多点必填/单点必空；expr 同；单点 a2=(颜色<<8)|容差 两段校验、多点 a2=0；a3/a4 档位；ms=0；j1/j2 同）；**其余所有 case** 防御：expr 非空 → 拒收（calc 与视觉两类除外）。
   - 执行器：视觉步骤 = static `op_vis_run()`：写 `req_pending = ++请求序号` → 轮询等待（`usleep(1000)`，总超时 1000ms；「面板不在」= ui_hb 冻结 ≥3s 判据，同看门狗口径）→ 校验（req_seq 匹配 + flags 无错 + 尺寸合法）→ `vt_vis_frame_prepare` → 区域换算（expr 区域名 → 区域几何 → 逻辑矩形 → `vt_vis_logic_rect_to_frame`；空 = 全屏；区域名不存在 → 中止 `区域不存在`）→ 匹配（找图先读 `.tmpl`、找色多点先读 `.pts`——读失败 → 中止 `模板不存在`）→ 命中：`r1/r2 = vt_vis_frame_to_logic(命中帧坐标)`（**逻辑坐标**）+ 四档「成立」侧；未命中：四档「不成立」侧；抓帧失败 → 中止 `无画面`；内部错 → 中止 `视觉错`。一帧多步复用：`now-ts<50ms` 且帧有效/方向一致 → 复用不重抓（spec §9 行）。
   - 模板方向：`.tmpl` 记录 rot；匹配前若与当前帧 rot 不同 → 旋转模板灰度（90° 数组变换）再匹配（spec §11-#7）。
   - 分支复用：把条件步的分支助手（vt_ops.c:645 附近 static）抽成可复用（或按同款参数调用）；**不改条件步行为**；日志照 spec §8（`vis 找图 <模板> 命中 x,y (耗时 <ms>)` 等 + TRACE `vis 抓帧 请求 → 完成 <ms>`）。
   - 按住期允许集 += 视觉步骤（spec §6.2）。
5. 胶水/面板：**无需签名变更**（expr 已在 get/put 链上，区域名随 expr 传）——原「加 ref2 尾参」项取消；面板真实数据接线在 3.2。
6. `scripts/build.sh`：确认 ui 分支已含 `src/vt_vision.c`（1.1 已加）；默认分支保持空 TU 口径。
- **验收**：默认核心可编译（md5 变化记录）；`build.sh ui` rc=0；面板 real rc=0；funcdoc 0/0。**真机留 5.1**。

## Task 3.1 — 面板抓帧层（VTouchUI.java + JNI + 帧循环接线）

**产出**：面板可响应核心抓帧请求：binder 拿 token → captureDisplay → JNI 读回 shm 帧区 → 更新帧头（双缓冲翻转）。

**点**：
1. `src-ui/VTouchUI.java`：
   - 初始化（systemMain 后，懒初始化+失败重试）：`ServiceManager.getService("SurfaceFlingerAIDL")` → 事务 6（skip 4B → `long[]` ids）→ 事务 7（skip 4B → token）。**逐字照 spec §2.1**（参考实现 `build/probe/VProbe7.java`）。
   - `captureToShm()`：`DisplayCaptureArgs$Builder(token).build()` → `captureDisplay` → `nativeVisCopyFrame(hb, rotation, w, h, stride)` → 更新帧头（seq 奇偶 + `buf_idx` 翻转 + ts + 尺寸 + rotation + req_seq + flags）。
   - 帧循环接线：每帧读 `nativeVisPollRequest()`（JNI 读帧头）≠ 已完成 → `captureToShm()`；异常/失败写 `flags` 错误码；**面板不阻塞渲染**（capture 4–7ms 同步做可接受）。
2. `src-ui/ui_glue.c`（JNI 段，静态命名照 `vtouch_ui.cpp:6041+` 风格）：
   - `Java_VTouchUI_nativeVisCopyFrame(env, cls, jobject hb, jint rotation, jint w, jint h, jint stride)`：`AHardwareBuffer_fromHardwareBuffer` → `AHardwareBuffer_lock(CPU_READ_OFTEN)` → memcpy 进帧区**后备缓冲** → unlock → 返回耗时 ms。
   - `Java_VTouchUI_nativeVisPollRequest(env, cls)`：读帧头（req_pending vs req_seq）返回待抓标志。
   - shm 帧区指针：复用面板现有 shm 映射（`-DVT_UI_PANEL` 半边）。
   - **帧区内存序契约（承 T2.1 评审 I-3）**：读 `req_pending` 用 acquire；写像素完成后 `req_seq` 用 release 存（或等价 store-store 屏障）——以 `vt_shm.h` 帧区注释为准（核心侧已按 release/acquire 配对实现）。
3. 构建：`<android/hardware_buffer.h>`（NDK）+ 链接 `-landroid`（查 `scripts/build_ui.sh` 现有链接清单补）。
- **验收**：`VTOUCH_UI_CORE=real sh scripts/build_ui.sh` rc=0（0 告警）；`build.sh ui` rc=0；**真机冒烟统一在 5.1**（本任务静态对账 + 编译）。

## Task 3.2 — 面板：编辑 + 模板管理 + 说明页 18 条

**产出**：找图/找色步骤可编辑；模板页（截帧/框选/存/删/列 + 点集吸色点选）；说明页 18 条；ops.conf 保持 v4（区域名随 expr 列）。

**点**：
1. ops.conf：**保持 v4**（视觉步区域名随现有 `expr` 列；无需升版/兼容改动）。
2. 加步区 **4×3**（11 键：点按/滑动/等待/找图 · 按下/弹起/跳转/找色 · 区域判断/开关判断/计算）；自适应阈值行数同步（现有 5 行口径不动，新增按钮进现有行结构）。
3. 步骤编辑子层：找图（模板下拉枚举 `templates/*.tmpl`、区域下拉枚举区域表、阈值数字）；找色（模式切换单点/多点、单点：颜色十六进制输入 + [取点]吸色 + 容差、多点：点集下拉）；复用数字/字符键盘子层与字段校验口径。
4. 模板页：截帧（复用 3.1 抓帧 → 面板侧缓冲）→ ImGui 纹理显示（缩放）→ 手指框选 → 命名（字符键盘）→ 存 `.tmpl`（格式照本计划「模板/点集文件格式」）；列表/删除；点集：截帧上吸基准色 + 点选参考点（点击加点、显示偏移/色）→ 命名存 `.pts`。
5. 说明页：`g_help_lines[]` **18 条**（第 12 条更新 + 新增 17/18，**逐字**照 spec §12）。
- **验收**：`build.sh ui` rc=0；面板 real rc=0；说明页 18/18 与 spec §12 逐字节（报告附比对命令与结果）；ops.conf v4→v5 兼容（仿真/静态走查）。

## Task 4.1 — 文档同步（README / AGENTS 双份）

- 更新点（照 spec 与实现）：视觉能力段（抓屏口径 §2 / 引擎 / 步骤类型 9→11 / 模板与点集 / 契约 v7→v8 / ops.conf v4→v5 / 中止词 +未命中/无画面/模板不存在/视觉错 / 日志 `vis ` 族 / 性能数字）；README.md + 仓库根 AGENTS.md + `C:/Users/21102/AGENTS.md`（后两者**逐字节一致**，md5 自查）。
- **验收**：三份 md5 对齐；锚点抽查命中。

## Task 5.1 — 收尾：全量门 + 重打包 + 真机端到端 + 台账（控制方/收尾子代理）

- 全量门（real ×3 / 默认 / funcdoc / syntax）+ `pack_client.py`/`pack_su.py` + `ci_check.py` 10/0 + 产物 md5 全表。
- 真机端到端（**横屏游戏场景优先**；部署走现有 ui-deploy 流）：
  a) 找色单点：已知色块 → 命中 r1/r2 坐标与真实像素对账；b) 多点找色；c) 找图：存模板 → 命中/未命中两档；d) 四档分支（成立跳转 / 不成立中止 = `未命中`）；e) `无画面`（面板停掉跑视觉步骤）；f) `模板不存在`；g) **横屏坐标映射**（横屏命中坐标回竖屏逻辑验证）；h) 性能表实测（spec §9 全表填数）。
- 台账 + 提交 +（用户拍板后）推送。

---

## 预检要点（控制方已核）

- 串行依赖：1.1 → 2.1 → 3.1 → 3.2 → 4.1 → 5.1；同文件串行：`ui_glue.c`/`vtouch_ui.cpp`（3.1 JNI → 3.2 大块）；`vt_ops.c` 由 2.1 独享。
- 接口对账：1.1 引擎 API → 2.1 调用；2.1 帧区契约/文件格式 → 3.1（帧头）/3.2（模板存盘）；expr 列语义（视觉 = 区域名）→ 3.2 编辑。
- 风险点：契约 v8 中间态（核心新面板旧 → 拒启；开发期同树同升）；默认核心 md5 变化（**仅 symtab 元数据级**——vt_vision.c FILE 符号；.text 不变，记录）；T3.1 是唯一不可宿主自测的任务（真机冒烟在 5.1）。
