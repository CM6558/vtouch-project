/* vt_vision.c —— 视觉（找图/找色）匹配引擎（spec docs/VISION_PLAN.md §3.3 / §4 / §9）。
 *
 * 三块内容：
 *   1) 帧视图（vt_vis_frame_prepare）：RGBA → 灰度 + 1/2、1/4 盒式平均金字塔（静态单例）。
 *   2) 匹配：找色（单点 / 多点，per-channel max-diff ≤ tol）、找图（灰度 SAD + 行级早退 SSDA +
 *      金字塔 1/4 粗筛 → 1/2 定位 → 全分辨率精修；命中即停）。
 *   3) 坐标映射：帧坐标（当前方向）↔ 竖屏逻辑坐标（纯函数；与面板 p2c/c2p 同一约定）。
 *
 * 灰度公式（整数近似；NEON 与标量共用同一组权重，对 Rec.601 加权真值最大误差 ≤1）：
 *     gray = (77*r + 150*g + 29*b + 128) >> 8        （权重和 = 256，四舍五入）
 * 金字塔（帧与模板同一套）：1/2 = 2×2 盒式平均、四舍五入 (a+b+c+d+2)>>2；1/4 由 1/2 再降一次。
 *
 * 找图判定与金字塔口径（完整说明；报告 task-1.1 同步披露）：
 *   - 命中判据：SAD ≤ thresh×tw×th（64 位累加比较，含等号；「平均绝对差 ≤ thresh」的整数等价式）。
 *   - 粗筛两级（1/4、1/2）都用**中心裁剪模板**（去掉首行/首列粗像素）：4×4 下采样在图案边缘会
 *     混入背景，整幅粗模板的紧阈值会把未对齐图案的真命中胞筛掉；裁剪后对 ≥8×8 平坦图案，
 *     无论对齐与否，真命中所在胞/半胞必过筛（SAD = 0），精修再用精确 SAD 复核 —— 粗筛只筛候选、
 *     不判定命中（推导见报告）。粗筛阈值另带 +8 余量（内调参数；只放宽候选，不放宽命中）。
 *   - 精修按**全局行优先**（胞行 → 胞 → 半胞 → 像素），命中即停 —— 对平坦图案与全分辨率直搜
 *     结果一致（单测断言）；细小纹理 / 命中位于搜索范围极限边缘 / 首命中为部分重叠位（阈值宽松时）
 *     的少数场景允许漏检或给出不同的有效命中（启发式设计性质，报告披露）。
 *   - 模板任一边 < 8 时走全分辨率直搜（金字塔无收益）。不命中不回退直搜（spec §9 未命中 ≤2ms 口径）。
 *
 * NEON 纪律：热路径（灰度 / 找色扫描 / SAD 行和）双实现，编译期按 __ARM_NEON 选择；
 * -DVT_VIS_FORCE_SCALAR 强制标量（宿主对照编译用）。两条路径逐像素同算式（共用常量与判定口径）。
 * 金字塔构建为标量（一次性、非扫描热路径）。
 *
 * 守卫与依赖：整文件在 #ifdef VT_UI 内 —— 默认（无面板）构建里本文件是空 TU。零核心依赖：
 * 只 include 标准头 + 自家头（NEON 时 <arm_neon.h>），宿主 gcc 可直接编译（单测 build/test_vt_vision.c）。
 */
#ifdef VT_UI

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "vt_vision.h"

#if defined(__ARM_NEON) && !defined(VT_VIS_FORCE_SCALAR)
#include <arm_neon.h>
#define VIS_HAVE_NEON 1
#endif

/* 尺寸上限（静态单例缓冲按此定长；帧/模板超出 = VT_VIS_BAD）。本机帧 1440×3168 / 3168×1440。 */
#define VT_VIS_MAX_W 4096
#define VT_VIS_MAX_H 4096

/* 多点找色参考点偏移上限（防御；合法参考点必然落在 ±帧尺寸内）。 */
#define VT_VIS_PT_MAX 4096

/* 粗筛阈值余量（1/4、1/2 两级共用；为什么这么写见文件头）。 */
#define VT_VIS_COARSE_MARGIN 8

/* 灰度权重（NEON 与标量共用同一组；和 = 256，+128 四舍五入）。 */
#define VT_VIS_GRAY_WR 77
#define VT_VIS_GRAY_WG 150
#define VT_VIS_GRAY_WB 29

/* ---- 帧视图（静态单例；主循环单线程，不可并发） ---- */

static uint8_t s_gray [VT_VIS_MAX_W * VT_VIS_MAX_H];          /* 灰度帧 */
static uint8_t s_gray2[VT_VIS_MAX_W / 2 * VT_VIS_MAX_H / 2];  /* 灰度 1/2 金字塔 */
static uint8_t s_gray4[VT_VIS_MAX_W / 4 * VT_VIS_MAX_H / 4];  /* 灰度 1/4 金字塔 */
static uint8_t s_t2   [VT_VIS_MAX_W / 2 * VT_VIS_MAX_H / 2];  /* 模板 1/2（每次 find_image 重建） */
static uint8_t s_t4   [VT_VIS_MAX_W / 4 * VT_VIS_MAX_H / 4];  /* 模板 1/4 */
static const uint8_t *s_rgba;   /* 帧 RGBA 指针（prepare..release 间由调用方保持有效；引擎只读） */
static int s_w, s_h, s_stride;  /* 帧尺寸与行跨距（字节） */
static int s_ok;                /* 1 = 帧视图已准备 */

/* 金字塔精修的 1/4 胞行缓存（单线程静态；避免整帧 flag 大数组）。 */
static uint8_t s_flagrow[VT_VIS_MAX_W / 4 + 2];  /* 当前胞行的粗筛结果（按 X） */
static int s_flagrow_y = -1;                     /* s_flagrow 对应的胞行号 */
static int s_row_any;                            /* 当前胞行是否有过筛胞（整行快速跳过） */

/* ---- 灰度 / 金字塔 ---- */

#ifdef VIS_HAVE_NEON

/**
 * (vtouch-doc: vis_gray_neon)
 * @brief 灰度转换（NEON 版）：vld4q_u8 解交织 + 16 位加权，16 像素/次。
 * @param   dst      输出灰度（w×h 紧凑）
 * @param   rgba     输入帧（stride 字节/行，RGBA8888）
 * @param   w        宽（像素）
 * @param   h        高（像素）
 * @param   stride   行跨距（字节）
 * @note    16 位通道和上限 65408 不溢出；尾部标量；逐 lane 与标量版同一算式。
 */
static void vis_gray_neon(uint8_t *dst, const uint8_t *rgba, int w, int h, int stride)
{
    int x, y;

    for (y = 0; y < h; y++) {
        const uint8_t *p = rgba + (size_t)y * stride;
        uint8_t *d = dst + (size_t)y * w;

        for (x = 0; x + 16 <= w; x += 16) {
            uint8x16x4_t px = vld4q_u8(p + (size_t)x * 4);
            uint16x8_t rl = vmovl_u8(vget_low_u8(px.val[0]));
            uint16x8_t gl = vmovl_u8(vget_low_u8(px.val[1]));
            uint16x8_t bl = vmovl_u8(vget_low_u8(px.val[2]));
            uint16x8_t rh = vmovl_u8(vget_high_u8(px.val[0]));
            uint16x8_t gh = vmovl_u8(vget_high_u8(px.val[1]));
            uint16x8_t bh = vmovl_u8(vget_high_u8(px.val[2]));
            uint16x8_t lo, hi;

            lo = vshrq_n_u16(vaddq_u16(vaddq_u16(vmulq_n_u16(rl, VT_VIS_GRAY_WR),
                                                 vmulq_n_u16(gl, VT_VIS_GRAY_WG)),
                                        vaddq_u16(vmulq_n_u16(bl, VT_VIS_GRAY_WB),
                                                  vdupq_n_u16(128))), 8);
            hi = vshrq_n_u16(vaddq_u16(vaddq_u16(vmulq_n_u16(rh, VT_VIS_GRAY_WR),
                                                 vmulq_n_u16(gh, VT_VIS_GRAY_WG)),
                                        vaddq_u16(vmulq_n_u16(bh, VT_VIS_GRAY_WB),
                                                  vdupq_n_u16(128))), 8);
            vst1q_u8(d + x, vcombine_u8(vmovn_u16(lo), vmovn_u16(hi)));
        }
        for (; x < w; x++) {
            const uint8_t *q = p + (size_t)x * 4;

            d[x] = (uint8_t)((VT_VIS_GRAY_WR * q[0] + VT_VIS_GRAY_WG * q[1] +
                              VT_VIS_GRAY_WB * q[2] + 128) >> 8);
        }
    }
}

#else

/**
 * (vtouch-doc: vis_gray_scalar)
 * @brief 灰度转换（标量版）：逐像素 (77r+150g+29b+128)>>8。
 * @param   dst      输出灰度（w×h 紧凑）
 * @param   rgba     输入帧（stride 字节/行，RGBA8888）
 * @param   w        宽（像素）
 * @param   h        高（像素）
 * @param   stride   行跨距（字节）
 * @note    权重与 NEON 版共用同一组常量（逐像素同算式）。
 */
static void vis_gray_scalar(uint8_t *dst, const uint8_t *rgba, int w, int h, int stride)
{
    int x, y;

    for (y = 0; y < h; y++) {
        const uint8_t *p = rgba + (size_t)y * stride;
        uint8_t *d = dst + (size_t)y * w;

        for (x = 0; x < w; x++, p += 4)
            d[x] = (uint8_t)((VT_VIS_GRAY_WR * p[0] + VT_VIS_GRAY_WG * p[1] +
                              VT_VIS_GRAY_WB * p[2] + 128) >> 8);
    }
}

#endif /* VIS_HAVE_NEON */

/**
 * (vtouch-doc: vis_down2)
 * @brief 1/2 盒式下采样（四舍五入）：dst[Y][X] = (a+b+c+d+2)>>2。
 * @param   dst      输出（dw×dh）
 * @param   dw       输出宽
 * @param   dh       输出高
 * @param   src      输入（行跨距 sw）
 * @param   sw       输入行跨距（元素数）
 * @note    帧与模板金字塔共用；尺寸向下取整（奇数边最后一行/列不参与）。
 */
static void vis_down2(uint8_t *dst, int dw, int dh, const uint8_t *src, int sw)
{
    int x, y;

    for (y = 0; y < dh; y++) {
        const uint8_t *r0 = src + (size_t)(2 * y) * sw;
        const uint8_t *r1 = r0 + sw;
        uint8_t *d = dst + (size_t)y * dw;

        for (x = 0; x < dw; x++)
            d[x] = (uint8_t)((r0[2 * x] + r0[2 * x + 1] + r1[2 * x] + r1[2 * x + 1] + 2) >> 2);
    }
}

/* ---- 找色内核 ---- */

/**
 * (vtouch-doc: vis_px_match)
 * @brief 单像素颜色匹配：per-channel max-diff ≤ tol（只比 R/G/B）。
 * @param   p        像素指针（RGBA8888）
 * @param   rgb      目标色（低 24 位有效）
 * @param   tol      容差（0..255）
 * @return  1 匹配；0 不匹配。
 */
static int vis_px_match(const uint8_t *p, uint32_t rgb, int tol)
{
    int dr = (int)p[0] - (int)((rgb >> 16) & 0xFFu);
    int dg = (int)p[1] - (int)((rgb >> 8) & 0xFFu);
    int db = (int)p[2] - (int)(rgb & 0xFFu);

    if (dr < 0) dr = -dr;
    if (dg < 0) dg = -dg;
    if (db < 0) db = -db;
    return (dr <= tol && dg <= tol && db <= tol) ? 1 : 0;
}

#ifdef VIS_HAVE_NEON

/**
 * (vtouch-doc: vis_color_row_neon)
 * @brief 在一行 [x0..x1] 内找首个匹配像素（NEON 版）：vabdq_u8 块筛 + 块内逐 lane 定位。
 * @param   row      行首（RGBA8888）
 * @param   x0       起始 x（含）
 * @param   x1       结束 x（含）
 * @param   rgb      目标色（低 24 位有效）
 * @param   tol      容差（0..255）
 * @return  命中 x；-1 = 无。
 * @note    整块无命中直接跳过（vminvq_u8）；有命中时块内定位首个；尾部标量；与标量版同序同结果。
 */
static int vis_color_row_neon(const uint8_t *row, int x0, int x1, uint32_t rgb, int tol)
{
    uint8x16_t tr = vdupq_n_u8((uint8_t)((rgb >> 16) & 0xFFu));
    uint8x16_t tg = vdupq_n_u8((uint8_t)((rgb >> 8) & 0xFFu));
    uint8x16_t tb = vdupq_n_u8((uint8_t)(rgb & 0xFFu));
    int x = x0;

    while (x + 16 <= x1 + 1) {
        uint8x16x4_t px = vld4q_u8(row + (size_t)x * 4);
        uint8x16_t m = vmaxq_u8(vmaxq_u8(vabdq_u8(px.val[0], tr), vabdq_u8(px.val[1], tg)),
                                vabdq_u8(px.val[2], tb));

        if (vminvq_u8(m) > (uint8_t)tol) { x += 16; continue; }
        {
            uint8_t lanes[16];
            int i;

            vst1q_u8(lanes, m);
            for (i = 0; i < 16; i++)
                if ((int)lanes[i] <= tol) return x + i;
        }
        x += 16;
    }
    for (; x <= x1; x++)
        if (vis_px_match(row + (size_t)x * 4, rgb, tol)) return x;
    return -1;
}

#else

/**
 * (vtouch-doc: vis_color_row_scalar)
 * @brief 在一行 [x0..x1] 内找首个匹配像素（标量版）。
 * @param   row      行首（RGBA8888）
 * @param   x0       起始 x（含）
 * @param   x1       结束 x（含）
 * @param   rgb      目标色（低 24 位有效）
 * @param   tol      容差（0..255）
 * @return  命中 x；-1 = 无。
 */
static int vis_color_row_scalar(const uint8_t *row, int x0, int x1, uint32_t rgb, int tol)
{
    int x;

    for (x = x0; x <= x1; x++)
        if (vis_px_match(row + (size_t)x * 4, rgb, tol)) return x;
    return -1;
}

#endif /* VIS_HAVE_NEON */

/**
 * (vtouch-doc: vis_pts_ok)
 * @brief 多点参考点校验：全部点在帧内且各自色差 ≤ 自身容差 → 1。
 * @param   rgba     帧 RGBA 指针
 * @param   w        帧宽
 * @param   h        帧高
 * @param   stride   帧行跨距（字节）
 * @param   bx       基准命中 x
 * @param   by       基准命中 y
 * @param   pts      参考点数组
 * @param   n        参考点个数
 * @return  1 全对；0 有越界或不匹配。
 */
static int vis_pts_ok(const uint8_t *rgba, int w, int h, int stride, int bx, int by,
                      const struct vt_vis_pt *pts, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        int px = bx + pts[i].dx;
        int py = by + pts[i].dy;

        if (px < 0 || px >= w || py < 0 || py >= h) return 0;
        if (!vis_px_match(rgba + (size_t)py * stride + (size_t)px * 4, pts[i].rgb, pts[i].tol))
            return 0;
    }
    return 1;
}

/**
 * (vtouch-doc: vis_clip_span)
 * @brief 把 [v0, v0+n-1] 与 [0, lim-1] 求交（区域限定先与帧求交的统一入口）。
 * @param   lo       输出交集下界
 * @param   hi       输出交集上界
 * @param   v0       区间起点
 * @param   n        区间长度（像素数）
 * @param   lim      帧边长（像素数）
 * @return  1 有交集（lo/hi 已写）；0 交集空。
 */
static int vis_clip_span(int *lo, int *hi, int v0, int n, int lim)
{
    int a = v0, b = v0 + n - 1;

    if (a < 0) a = 0;
    if (b > lim - 1) b = lim - 1;
    if (b < a) return 0;
    *lo = a;
    *hi = b;
    return 1;
}

/* ---- 找图内核 ---- */

#ifdef VIS_HAVE_NEON

/**
 * (vtouch-doc: vis_row_absdiff_neon)
 * @brief 一行 |帧-模板| 绝对差之和（NEON 版）：vabdq_u8 + vpaddl 两级升位累加。
 * @param   f        帧行首
 * @param   t        模板行首
 * @param   n        本行像素数
 * @return  该行绝对差之和（u32；单行上限 255×4096 不溢出）。
 * @note    16 像素/次、u32 通道防溢出、尾部标量；与标量版同值。
 */
static uint32_t vis_row_absdiff_neon(const uint8_t *f, const uint8_t *t, int n)
{
    uint32x4_t acc = vdupq_n_u32(0);
    uint32_t s;
    int i = 0;

    for (; i + 16 <= n; i += 16) {
        uint8x16_t d = vabdq_u8(vld1q_u8(f + i), vld1q_u8(t + i));

        acc = vaddq_u32(acc, vpaddlq_u16(vpaddlq_u8(d)));
    }
    s = vaddvq_u32(acc);
    for (; i < n; i++)
        s += (f[i] > t[i]) ? (uint32_t)(f[i] - t[i]) : (uint32_t)(t[i] - f[i]);
    return s;
}

#else

/**
 * (vtouch-doc: vis_row_absdiff_scalar)
 * @brief 一行 |帧-模板| 绝对差之和（标量版，u32）。
 * @param   f        帧行首
 * @param   t        模板行首
 * @param   n        本行像素数
 * @return  该行绝对差之和（u32；单行上限 255×4096 不溢出）。
 */
static uint32_t vis_row_absdiff_scalar(const uint8_t *f, const uint8_t *t, int n)
{
    uint32_t s = 0;
    int i;

    for (i = 0; i < n; i++)
        s += (f[i] > t[i]) ? (uint32_t)(f[i] - t[i]) : (uint32_t)(t[i] - f[i]);
    return s;
}

#endif /* VIS_HAVE_NEON */

/* 双路径分发（热路径选择；标量回退与强制标量见文件头）。 */
#ifdef VIS_HAVE_NEON
#define VIS_GRAY      vis_gray_neon
#define VIS_COLOR_ROW vis_color_row_neon
#define VIS_ROW_AD    vis_row_absdiff_neon
#else
#define VIS_GRAY      vis_gray_scalar
#define VIS_COLOR_ROW vis_color_row_scalar
#define VIS_ROW_AD    vis_row_absdiff_scalar
#endif

/**
 * (vtouch-doc: vis_sad_ok)
 * @brief 模板匹配判定：全模板 SAD ≤ limit → 1（64 位累加、行级早退 SSDA）。
 * @param   f        帧（灰度）基址
 * @param   fw       帧行跨距（像素）
 * @param   x        模板锚点 x
 * @param   y        模板锚点 y
 * @param   t        模板基址（中心裁剪视图可用）
 * @param   tstride  模板行跨距（像素）
 * @param   tw       模板宽
 * @param   th       模板高
 * @param   limit    判定上限（SAD ≤ limit 即命中）
 * @return  1 命中（SAD ≤ limit）；0 超过。
 * @note    早退只影响速度：提前返回必 > limit，判定与完整累加一致；模板行跨距由 tstride 给（中心裁剪视图用）。
 */
static int vis_sad_ok(const uint8_t *f, int fw, int x, int y, const uint8_t *t, int tstride,
                      int tw, int th, uint64_t limit)
{
    const uint8_t *fr = f + (size_t)y * fw + (size_t)x;
    const uint8_t *tp = t;
    uint64_t sad = 0;
    int i;

    for (i = 0; i < th; i++) {
        sad += VIS_ROW_AD(fr, tp, tw);
        if (sad > limit) return 0;
        fr += fw;
        tp += tstride;
    }
    return 1;
}

/**
 * (vtouch-doc: vis_image_direct)
 * @brief 全分辨率直搜：行优先、步进 1、命中即停（模板 < 8 的回退路径）。
 * @param   x0,x1    有效锚点 x 范围（含）
 * @param   y0,y1    有效锚点 y 范围（含）
 * @param   tmpl     模板灰度（tw×th 紧凑）
 * @param   tw,th    模板尺寸
 * @param   limit    判定上限（SAD ≤ limit）
 * @param   ox,oy    命中输出（帧坐标）
 * @return  VT_VIS_OK（ox/oy 已写）/ VT_VIS_NO_MATCH。
 */
static int vis_image_direct(int x0, int x1, int y0, int y1, const uint8_t *tmpl, int tw, int th,
                            uint64_t limit, int *ox, int *oy)
{
    int x, y;
    int xe = x1 - tw + 1, ye = y1 - th + 1;

    for (y = y0; y <= ye; y++) {
        for (x = x0; x <= xe; x++) {
            if (vis_sad_ok(s_gray, s_w, x, y, tmpl, tw, tw, th, limit)) {
                *ox = x;
                *oy = y;
                return VT_VIS_OK;
            }
        }
    }
    return VT_VIS_NO_MATCH;
}

/**
 * (vtouch-doc: vis_image_pyramid)
 * @brief 金字塔路径：1/4 粗筛 → 1/2 定位 → 全分辨率精修（精确 SAD 复核）。
 * @param   x0,x1    有效锚点 x 范围（含）
 * @param   y0,y1    有效锚点 y 范围（含）
 * @param   tmpl     模板灰度（tw×th 紧凑；调用方保证 tw、th ≥ 8）
 * @param   tw,th    模板尺寸
 * @param   limit    判定上限（SAD ≤ limit；精确复核用）
 * @param   thresh   阈值（粗筛阈值 = thresh + 余量）
 * @param   ox,oy    命中输出（帧坐标）
 * @return  VT_VIS_OK（ox/oy 已写）/ VT_VIS_NO_MATCH。
 * @note    粗筛用中心裁剪模板（去首行/首列粗像素）——对 ≥8×8 平坦图案，无论对齐与否真命中所在胞必过筛（推导见报告）；精修按全局行优先、命中即停；对细小纹理 / 部分重叠位允许漏检（启发式，报告披露）。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   粗筛只决定「哪些胞/半胞值得精修」，命中一律由全分辨率精确 SAD 判定 —— 粗筛漏掉的是候选、
 *   不是正确性；裁剪模板让真命中胞在任意对齐下都过筛（SAD = 0）。精修按全局行优先（胞行 → 胞
 *   → 半胞 → 像素）逐像素扫描：与直搜同序，首个命中一致。胞行缓存 s_flagrow 避免整帧大数组。
 */
static int vis_image_pyramid(int x0, int x1, int y0, int y1, const uint8_t *tmpl, int tw, int th,
                             uint64_t limit, int thresh, int *ox, int *oy)
{
    int tw2 = tw / 2, th2 = th / 2, tw4 = tw / 4, th4 = th / 4;
    int tw2c = tw2 - 1, th2c = th2 - 1;   /* 1/2 中心裁剪：只去首行/首列粗像素（≥1 宽） */
    int tw4c = tw4 - 1, th4c = th4 - 1;   /* 1/4 中心裁剪 */
    uint64_t lim4 = (uint64_t)(thresh + VT_VIS_COARSE_MARGIN) * (uint64_t)tw4c * (uint64_t)th4c;
    uint64_t lim2 = (uint64_t)(thresh + VT_VIS_COARSE_MARGIN) * (uint64_t)tw2c * (uint64_t)th2c;
    int xe = x1 - tw + 1, ye = y1 - th + 1;
    int X0 = x0 / 4, X1 = xe / 4;
    int y, X;

    vis_down2(s_t2, tw2, th2, tmpl, tw);
    vis_down2(s_t4, tw4, th4, s_t2, tw2);

    s_flagrow_y = -1;
    for (y = y0; y <= ye; y++) {
        int Y = y / 4;
        int Y2 = y / 2;

        if (Y != s_flagrow_y) {                       /* 新胞行：1/4 粗筛一次（4 行共用） */
            s_flagrow_y = Y;
            s_row_any = 0;
            for (X = X0; X <= X1; X++) {
                s_flagrow[X] = (uint8_t)vis_sad_ok(s_gray4, s_w / 4, X + 1, Y + 1,
                                                   s_t4 + tw4 + 1, tw4, tw4c, th4c, lim4);
                if (s_flagrow[X]) s_row_any = 1;
            }
        }
        if (!s_row_any) continue;
        for (X = X0; X <= X1; X++) {
            int X2;

            if (!s_flagrow[X]) continue;
            for (X2 = 2 * X; X2 <= 2 * X + 1; X2++) {
                int xa, xb, x;

                if (2 * X2 > xe || 2 * X2 + 1 < x0) continue;   /* 半胞无有效像素（也防越界读） */
                if (!vis_sad_ok(s_gray2, s_w / 2, X2 + 1, Y2 + 1,
                                s_t2 + tw2 + 1, tw2, tw2c, th2c, lim2)) continue;
                xa = 2 * X2; if (xa < x0) xa = x0;
                xb = 2 * X2 + 1; if (xb > xe) xb = xe;
                for (x = xa; x <= xb; x++) {
                    if (vis_sad_ok(s_gray, s_w, x, y, tmpl, tw, tw, th, limit)) {
                        *ox = x;
                        *oy = y;
                        return VT_VIS_OK;
                    }
                }
            }
        }
    }
    return VT_VIS_NO_MATCH;
}

/* ---- 对外接口 ---- */

/**
 * (vtouch-doc: vt_vis_frame_prepare)
 * @brief 准备帧视图（静态单例）：RGBA→灰度 + 1/2、1/4 金字塔。
 * @param   rgba     帧 RGBA8888（stride 字节/行；调用方保证在 release 前有效）
 * @param   w        帧宽（像素）
 * @param   h        帧高（像素）
 * @param   stride   行跨距（字节；≥ w×4）
 * @return  VT_VIS_OK；VT_VIS_BAD（参数非法 / 尺寸超上限）。
 * @note    主循环单线程独占（不可并发调用）；rgba 缓冲在 release 前须保持有效（引擎只读、不拷贝）。灰度公式 (77r+150g+29b+128)>>8（对 Rec.601 加权真值误差 ≤1）；金字塔 = 四舍五入盒式平均。尺寸上限 4096×4096。失败会先把视图置为无效（不保留上一帧）。
 */
int vt_vis_frame_prepare(const uint8_t *rgba, int w, int h, int stride)
{
    s_ok = 0;                                        /* 失败即失效：不静默沿用上一帧 */
    if (!rgba || w <= 0 || h <= 0 || w > VT_VIS_MAX_W || h > VT_VIS_MAX_H) return VT_VIS_BAD;
    if (stride < w * 4) return VT_VIS_BAD;

    s_rgba = rgba;
    s_w = w;
    s_h = h;
    s_stride = stride;
    VIS_GRAY(s_gray, rgba, w, h, stride);
    vis_down2(s_gray2, w / 2, h / 2, s_gray, w);
    vis_down2(s_gray4, w / 4, h / 4, s_gray2, w / 2);
    s_ok = 1;
    return VT_VIS_OK;
}

/**
 * (vtouch-doc: vt_vis_frame_release)
 * @brief 释放帧视图（标记失效；静态缓冲不释放）。
 * @note    release 后一切匹配返回 VT_VIS_BAD，直到下一次 prepare。
 */
void vt_vis_frame_release(void)
{
    s_ok = 0;
    s_rgba = NULL;
}

/**
 * (vtouch-doc: vt_vis_find_color)
 * @brief 找色：区域内行优先找首个 per-channel 色差 ≤ tol 的像素。
 * @param   rx,ry,rw,rh 搜索区域（帧坐标；与帧求交后使用）
 * @param   rgb      目标色（低 24 位有效）
 * @param   tol      容差（0..255）
 * @param   ox,oy    命中输出（帧坐标；未命中不写）
 * @return  VT_VIS_OK（命中）；VT_VIS_NO_MATCH；VT_VIS_BAD（未准备 / 参数非法）。
 * @note    只比 R/G/B（忽略 alpha）；区域先与帧求交（越界部分不算、交集空 → 未命中）；命中即停。
 */
int vt_vis_find_color(int rx, int ry, int rw, int rh, uint32_t rgb, int tol, int *ox, int *oy)
{
    int x0, x1, y0, y1, y;

    if (!s_ok || !ox || !oy) return VT_VIS_BAD;
    if (tol < 0 || tol > 255 || rw <= 0 || rh <= 0) return VT_VIS_BAD;
    if (!vis_clip_span(&x0, &x1, rx, rw, s_w) || !vis_clip_span(&y0, &y1, ry, rh, s_h))
        return VT_VIS_NO_MATCH;
    for (y = y0; y <= y1; y++) {
        int x = VIS_COLOR_ROW(s_rgba + (size_t)y * s_stride, x0, x1, rgb, tol);

        if (x >= 0) {
            *ox = x;
            *oy = y;
            return VT_VIS_OK;
        }
    }
    return VT_VIS_NO_MATCH;
}

/**
 * (vtouch-doc: vt_vis_find_color_multi)
 * @brief 多点找色：基准色命中后逐点校验偏移参考点，全对为命中（n ≤ 16）。
 * @param   rx,ry,rw,rh 基准搜索区域（帧坐标；与帧求交后使用）
 * @param   base     基准色（低 24 位有效）
 * @param   base_tol 基准容差（0..255）
 * @param   pts      参考点数组（相对基准的偏移 + 目标色 + 容差）
 * @param   n        参考点个数（1..16）
 * @param   ox,oy    命中输出（基准坐标；未命中不写）
 * @return  VT_VIS_OK（命中）；VT_VIS_NO_MATCH；VT_VIS_BAD（未准备 / 参数非法）。
 * @note    参考点位置 = 基准 + 偏移，须落在帧内（越界 = 该候选不成立）；每点各自容差（0..255）；dx/dy 限 ±4096；区域只限定基准的搜索范围（参考点可越出区域）。
 */
int vt_vis_find_color_multi(int rx, int ry, int rw, int rh, uint32_t base, int base_tol,
                            const struct vt_vis_pt *pts, int n, int *ox, int *oy)
{
    int x0, x1, y0, y1, y, i;

    if (!s_ok || !ox || !oy || !pts) return VT_VIS_BAD;
    if (base_tol < 0 || base_tol > 255 || n < 1 || n > 16 || rw <= 0 || rh <= 0)
        return VT_VIS_BAD;
    for (i = 0; i < n; i++) {
        if (pts[i].tol < 0 || pts[i].tol > 255) return VT_VIS_BAD;
        if (pts[i].dx < -VT_VIS_PT_MAX || pts[i].dx > VT_VIS_PT_MAX) return VT_VIS_BAD;
        if (pts[i].dy < -VT_VIS_PT_MAX || pts[i].dy > VT_VIS_PT_MAX) return VT_VIS_BAD;
    }
    if (!vis_clip_span(&x0, &x1, rx, rw, s_w) || !vis_clip_span(&y0, &y1, ry, rh, s_h))
        return VT_VIS_NO_MATCH;
    for (y = y0; y <= y1; y++) {
        const uint8_t *row = s_rgba + (size_t)y * s_stride;
        int x = x0;

        while (x <= x1) {
            int hx = VIS_COLOR_ROW(row, x, x1, base, base_tol);

            if (hx < 0) break;
            if (vis_pts_ok(s_rgba, s_w, s_h, s_stride, hx, y, pts, n)) {
                *ox = hx;
                *oy = y;
                return VT_VIS_OK;
            }
            x = hx + 1;
        }
    }
    return VT_VIS_NO_MATCH;
}

/**
 * (vtouch-doc: vt_vis_find_image)
 * @brief 找图：灰度模板匹配，命中判据 = 平均绝对差 ≤ thresh。
 * @param   rx,ry,rw,rh 搜索区域（帧坐标；与帧求交后使用）
 * @param   tmpl     模板灰度（tw×th 紧凑）
 * @param   tw,th    模板尺寸
 * @param   thresh   阈值（0..255；平均绝对差上限）
 * @param   ox,oy    命中输出（帧坐标；未命中不写）
 * @return  VT_VIS_OK（命中）；VT_VIS_NO_MATCH；VT_VIS_BAD（未准备 / 参数非法）。
 * @note    比较用 64 位累加：SAD ≤ thresh×tw×th（含等号）。模板两边都 ≥ 8 走金字塔（1/4 粗筛 → 1/2 定位 → 全分辨率精修），任一边 < 8 走全分辨率直搜；SAD 行级早退；命中即停。金字塔为启发式（细小纹理 / 极限边缘允许漏检；不命中不回退直搜）。
 */
int vt_vis_find_image(int rx, int ry, int rw, int rh, const uint8_t *tmpl, int tw, int th,
                      int thresh, int *ox, int *oy)
{
    int x0, x1, y0, y1;
    uint64_t limit;

    if (!s_ok || !tmpl || !ox || !oy) return VT_VIS_BAD;
    if (tw <= 0 || th <= 0 || tw > s_w || th > s_h || tw > VT_VIS_MAX_W || th > VT_VIS_MAX_H)
        return VT_VIS_BAD;
    if (thresh < 0 || thresh > 255 || rw <= 0 || rh <= 0) return VT_VIS_BAD;
    if (!vis_clip_span(&x0, &x1, rx, rw, s_w) || !vis_clip_span(&y0, &y1, ry, rh, s_h))
        return VT_VIS_NO_MATCH;
    if (x1 - x0 + 1 < tw || y1 - y0 + 1 < th) return VT_VIS_NO_MATCH;
    limit = (uint64_t)thresh * (uint64_t)tw * (uint64_t)th;
    if (tw >= 8 && th >= 8)
        return vis_image_pyramid(x0, x1, y0, y1, tmpl, tw, th, limit, thresh, ox, oy);
    return vis_image_direct(x0, x1, y0, y1, tmpl, tw, th, limit, ox, oy);
}

/* ---- 坐标映射（纯函数；约定见 vt_vision.h 文件头，与面板 p2c/c2p 同一套） ---- */

/**
 * (vtouch-doc: vt_vis_frame_to_logic)
 * @brief 帧坐标（当前方向）→ 竖屏逻辑坐标（单点）。
 * @param   rotation 0..3（r 语义同 Android getRotation / AutoJs6 device.rotation）
 * @param   fw,fh    帧尺寸（当前方向）
 * @param   fx,fy    帧坐标
 * @param   lx,ly    输出逻辑坐标（可 NULL = 跳过）
 * @note    约定与面板 p2c/c2p 逐字同一套；rotation 越界按 r&3 归一。
 */
void vt_vis_frame_to_logic(int rotation, int fw, int fh, int fx, int fy, int *lx, int *ly)
{
    int W, H;

    if (!lx || !ly) return;
    rotation &= 3;
    if (rotation == 1 || rotation == 3) { W = fh; H = fw; }   /* 横屏帧：帧宽 = 逻辑高 */
    else { W = fw; H = fh; }
    switch (rotation) {
    case 1:  *lx = W - 1 - fy; *ly = fx;         break;
    case 2:  *lx = W - 1 - fx; *ly = H - 1 - fy; break;
    case 3:  *lx = fy;         *ly = H - 1 - fx; break;
    default: *lx = fx;         *ly = fy;         break;
    }
}

/**
 * (vtouch-doc: vt_vis_logic_rect_to_frame)
 * @brief 竖屏逻辑矩形 → 帧坐标矩形（区域限定换算用）。
 * @param   rotation 0..3
 * @param   fw,fh    帧尺寸（当前方向）
 * @param   lx,ly,lw,lh 逻辑矩形（含端点，lw×lh 个像素）
 * @param   fx,fy,fw2,fh2 输出帧矩形（含端点；可 NULL = 跳过）
 * @note    矩形含端点（lw×lh 个像素）；四角映射后取 min/max；输出矩形同样含端点。
 */
void vt_vis_logic_rect_to_frame(int rotation, int fw, int fh,
                                int lx, int ly, int lw, int lh,
                                int *fx, int *fy, int *fw2, int *fh2)
{
    if (!fx || !fy || !fw2 || !fh2) return;
    rotation &= 3;
    switch (rotation) {
    case 1:  *fx = ly;           *fy = fh - lx - lw; *fw2 = lh; *fh2 = lw; break;
    case 2:  *fx = fw - lx - lw; *fy = fh - ly - lh; *fw2 = lw; *fh2 = lh; break;
    case 3:  *fx = fw - ly - lh; *fy = lx;           *fw2 = lh; *fh2 = lw; break;
    default: *fx = lx;           *fy = ly;           *fw2 = lw; *fh2 = lh; break;
    }
}

#endif /* VT_UI */
