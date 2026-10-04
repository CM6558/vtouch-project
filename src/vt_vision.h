/* vt_vision.h —— 视觉（找图/找色）匹配引擎接口（Task 1.1；spec docs/VISION_PLAN.md §3.3/§4）。
 *
 * 零核心依赖：只依赖标准头（<stdint.h>），宿主 gcc 可直接编译 —— 宿主单测口径见
 * build/test_vt_vision.c（不入库）。实现（src/vt_vision.c）整体在 #ifdef VT_UI 内：默认（无面板）
 * 构建里编成空 TU。本头不加 VT_UI 守卫（纯声明；谁需要谁包含）。
 *
 * 坐标契约：
 *   - 帧坐标 = 抓帧返回的**当前方向**原生像素（左上原点；本机竖屏 1440×3168 / 横屏 3168×1440）。
 *   - 逻辑坐标 = 固定竖屏（现有契约：区域表 / 脚本 / 注入同一套）。
 *   - 旋转约定与面板 p2c/c2p（src-ui/ui_glue.c、src-ui/vtouch_ui.cpp）**逐字同一套**（r 语义同
 *     Android getRotation）——帧坐标 (fx,fy) → 竖屏逻辑 (lx,ly)：
 *       r=0（竖屏）：lx = fx,        ly = fy；
 *       r=1（横屏，帧宽=逻辑高）：lx = fh-1-fy,   ly = fx；
 *       r=2：lx = fw-1-fx,   ly = fh-1-fy；
 *       r=3（横屏，帧宽=逻辑高）：lx = fy,        ly = fw-1-fx。
 *     例（r=1）：帧 (0,0) ↔ 逻辑 (fh-1,0)；竖屏左上角 (0,0) 落在帧左下角 (0,fh-1)。
 *     四方向往返恒等（单测断言）。
 *
 * 语义要点（逐条对应实现与单测）：
 *   - 找色 / 多点找色：per-channel max-diff ≤ tol（只比 R/G/B，alpha 不参与）；行优先、命中即停；
 *     命中写首个命中坐标（帧坐标；未命中不写）。
 *   - 找图：命中判据 = 平均绝对差 ≤ thresh ⇔ SAD ≤ thresh×tw×th（64 位累加比较、含等号；
 *     thresh 0..255 整数域）。金字塔 1/4→1/2→全分辨率精修；细小纹理 / 极限边缘允许漏检
 *     （启发式口径见 vt_vision.c 文件头）。
 *   - 区域（rx,ry,rw,rh）：先与帧求交（越界部分不算；交集空 = 未命中）；rw/rh ≤ 0 = 参数非法。
 *   - 帧视图：vt_vis_frame_prepare 用**静态单例**（主循环单线程，不可并发调用）；rgba 缓冲在
 *     release 前须保持有效（引擎只读、不拷贝）。
 *   - 灰度公式 = 本头的 vt_vis_gray_px（唯一来源；引擎与面板写端共用，见下）。
 */
#ifndef VT_VISION_H
#define VT_VISION_H

#include <stdint.h>

/* 灰度公式（唯一来源；spec §4.1 / 实施计划「灰度公式单一来源」）：RGBA → 8bit 灰度，整数近似。
 * gray = (77*r + 150*g + 29*b + 128) >> 8 —— 权重和 = 256、+128 四舍五入（对 Rec.601 加权真值误差 ≤1）。
 * 引擎（vt_vision.c 的标量路径与 NEON 路径共用这组权重常量）与面板写端（T3.2：.tmpl 存灰度）
 * **同用这一处定义**；改公式只改这里，禁止在别处重写算式。 */
#define VT_VIS_GRAY_WR 77
#define VT_VIS_GRAY_WG 150
#define VT_VIS_GRAY_WB 29
/**
 * (vtouch-doc: vt_vis_gray_px)
 * @brief 灰度公式（唯一来源）：(77r+150g+29b+128)>>8。
 * @param   r,g,b  像素通道值（0..255）
 * @return  8bit 灰度（0..255）。
 * @note    引擎（vt_vision.c）与面板写端（.tmpl 存灰度）共用；NEON 路径复用同组权重常量（逐 lane 同算式）。
 */
static inline uint8_t vt_vis_gray_px(int r, int g, int b)
{
    return (uint8_t)((VT_VIS_GRAY_WR * r + VT_VIS_GRAY_WG * g + VT_VIS_GRAY_WB * b + 128) >> 8);
}

/* 返回码（全接口统一）。 */
#define VT_VIS_OK        0    /* 命中（匹配类接口）/ 成功（prepare） */
#define VT_VIS_NO_MATCH  1    /* 未命中（合法搜索、无结果） */
#define VT_VIS_BAD     (-1)   /* 参数非法 / 帧视图未准备（核心侧映射「视觉错」） */

/* 多点找色的一个参考点：相对基准的偏移 + 目标色 + 容差。 */
struct vt_vis_pt { int dx, dy; uint32_t rgb; int tol; };

/* 帧视图（一次性准备：灰度 + 1/2、1/4 金字塔）。静态单例（主循环单线程）。 (vtouch-doc: vt_vis_frame_prepare) */
int  vt_vis_frame_prepare(const uint8_t *rgba, int w, int h, int stride);
/* 释放帧视图（标记失效；静态缓冲不释放）。 (vtouch-doc: vt_vis_frame_release) */
void vt_vis_frame_release(void);

/* 找色：区域（帧坐标 rx,ry,rw,rh）；命中写首个命中坐标（帧坐标）。 (vtouch-doc: vt_vis_find_color) */
int vt_vis_find_color(int rx, int ry, int rw, int rh, uint32_t rgb, int tol, int *ox, int *oy);
/* 多点找色：基准色命中后校验全部参考点；n ≤ 16。 (vtouch-doc: vt_vis_find_color_multi) */
int vt_vis_find_color_multi(int rx, int ry, int rw, int rh, uint32_t base, int base_tol,
                            const struct vt_vis_pt *pts, int n, int *ox, int *oy);
/* 找图：模板灰度（tw×th）；阈值 = 平均绝对差上限（0..255）。 (vtouch-doc: vt_vis_find_image) */
int vt_vis_find_image(int rx, int ry, int rw, int rh, const uint8_t *tmpl, int tw, int th,
                      int thresh, int *ox, int *oy);

/* 坐标映射（纯函数，宿主可测）：rotation 0..3；帧尺寸 fw×fh（当前方向）；
 * 逻辑 = 固定竖屏。帧↔逻辑单点 + 逻辑矩形→帧矩形（用于区域限定换算）。 */
/* 帧坐标（当前方向）→ 竖屏逻辑坐标（单点）。 (vtouch-doc: vt_vis_frame_to_logic) */
void vt_vis_frame_to_logic(int rotation, int fw, int fh, int fx, int fy, int *lx, int *ly);
/* 竖屏逻辑矩形 → 帧坐标矩形（区域限定换算用）。 (vtouch-doc: vt_vis_logic_rect_to_frame) */
void vt_vis_logic_rect_to_frame(int rotation, int fw, int fh,
                                int lx, int ly, int lw, int lh,
                                int *fx, int *fy, int *fw2, int *fh2);

#endif /* VT_VISION_H */
