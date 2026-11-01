package com.vtouch.editor;

/**
 * [框区域]（v12）：冻结帧框选 → 竖屏逻辑坐标 → 直写 regions.conf（备份 + 原子写 + 镜像）。
 * 纯函数（命名 / 换算 / 建记录）集中在这里，测试台可离线核（不碰 UI / root）。
 * 口径：
 *  - 命名 = 面板 gen_id 同款：矩形 r1/r2…、圆 c1/c2…，取第一个空闲（扫现存 id）。
 *  - 坐标 = 区域表「基帧视口坐标」口径：数值属于 regions.conf 的 #frame 基帧（viewport-carry，
 *    同面板 region_rot_map）；换算参数 (rot,fw,fh) 一律传**基帧**，不是当前帧/捕获帧。
 *    （Matcher.frameToLogic 直译；rot 90/270 两角归一化 min/max。）
 *  - 新区域默认：启用、未绑定（- / down）、普通型（kind 0）。
 */
public final class RegionDraw {
    /** 演示 / 测试写入重定向（非空 = 写别处，不碰真实运行文件；常规路径保持 null）。 */
    public static volatile String pathOverride = null;
    /** 演示 / 测试读取重定向（非空 = loadLive 读这里；常规路径保持 null）。 */
    public static volatile String readOverride = null;

    /** 下一个空闲区域名（矩形 r*、圆 c*）。 */
    public static String nextId(Regions.File f, boolean circle) {
        for (int i = 1; i < 1000; i++) {
            String cand = (circle ? "c" : "r") + i;
            if (f == null || f.find(cand) == null) return cand;
        }
        return (circle ? "c" : "r") + "999";
    }

    /** 帧框选矩形 → 逻辑矩形 [x1,y1,x2,y2]（rot 90/270 交换轴 → min/max 归一化）。 */
    public static int[] rectToLogic(int rot, int fw, int fh, int x, int y, int w, int h) {
        int[] a = new int[2], b = new int[2];
        Matcher.frameToLogic(rot, fw, fh, x, y, a);
        Matcher.frameToLogic(rot, fw, fh, x + w, y + h, b);
        return new int[]{Math.min(a[0], b[0]), Math.min(a[1], b[1]),
                         Math.max(a[0], b[0]), Math.max(a[1], b[1])};
    }

    /** 帧圆（圆心 + 半径）→ 逻辑 [cx,cy,r]（半径按边缘点逻辑距，旋转变换保距）。 */
    public static int[] circleToLogic(int rot, int fw, int fh, int cx, int cy, int r) {
        int[] c = new int[2], e = new int[2];
        Matcher.frameToLogic(rot, fw, fh, cx, cy, c);
        Matcher.frameToLogic(rot, fw, fh, cx + r, cy, e);
        int rr = (int) Math.round(Math.hypot(e[0] - c[0], e[1] - c[1]));
        return new int[]{c[0], c[1], rr};
    }

    /** 逻辑圆 [cx,cy,r] → 帧圆 [fx,fy,fr]（v12b 帧上编辑预载用；半径按两点逻辑距反译，保距）。 */
    public static int[] circleToFrame(int rot, int fw, int fh, int lx, int ly, int lr) {
        int[] c = Matcher.logicRectToFrame(rot, fw, fh, lx, ly, 0, 0);
        int[] e = Matcher.logicRectToFrame(rot, fw, fh, lx + lr, ly, 0, 0);
        int rr = (int) Math.round(Math.hypot(e[0] - c[0], e[1] - c[1]));
        return new int[]{c[0], c[1], rr};
    }

    /** 建一条区域记录（默认启用 / 未绑定 / 普通型）。circle=true 时 a1,a2=圆心、a3=半径、a4=0。 */
    public static Regions.Region make(String id, boolean circle, int a1, int a2, int a3, int a4) {
        Regions.Region r = new Regions.Region(id);
        r.type = circle ? 1 : 0;
        r.a1 = a1; r.a2 = a2; r.a3 = a3; r.a4 = a4;
        r.en = 1;
        r.bindOp = "-";
        r.bindEv = "down";
        r.kind = 0;
        return r;
    }

    /** 读 live 区域文件；文件缺失 → 空 v2。解析失败抛异常（调用方展示原因）。 */
    public static Regions.File loadLive() throws Exception {
        String text = Su.read(readOverride != null ? readOverride : Regions.PATH);
        if (text == null) text = "#vtouch-regions v2\n";
        return Regions.File.parse(text);
    }

    /** 追加一条并落盘（.bak 备份 → 原子写 → 镜像 schemes/<current>）。pathOverride 非空时只写别处。 */
    public static void save(Regions.File f, Regions.Region r) throws Exception {
        String err = Regions.validate(r, f, null);
        if (err != null) throw new IllegalArgumentException(err);
        f.regions.add(r);
        writeFile(f);
    }

    /** 整文件落盘（备份 + 原子写 + 镜像；pathOverride 非空时只写别处）——v12c 总览调整用。 */
    public static void writeFile(Regions.File f) throws Exception {
        String text = f.serialize();
        String path = pathOverride != null ? pathOverride : Regions.PATH;
        Su.backup(path);
        Su.R res = pathOverride == null ? Schemes.publishConfig(path, text.getBytes("UTF-8"))
                : Su.writeAtomic(path, text.getBytes("UTF-8"));
        if (!res.ok()) throw new Exception("写入失败：" + res.trimmed());
    }
}
