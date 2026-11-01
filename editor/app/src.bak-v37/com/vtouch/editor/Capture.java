package com.vtouch.editor;

import android.content.Context;
import android.view.WindowManager;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;

/**
 * 抓帧（su screencap raw）。P0 实测（2026-10-07）：raw 162ms（PNG 1354ms → 弃）；
 * 16B 头 = u32le{w, h, format=1(RGBA8888), colorspace}，数据区 = w*h*4 交错 RGBA、行跨距 w*4。
 * 流程：root 直接写 app 外部目录 → app 读回解析 → 删临时文件；读取失败回退 su cat。
 * 失败路径给可读原因（不静默）；调用方放后台线程（同步阻塞 ~0.2-1.5s）。
 */
public final class Capture {
    private static Context app;

    public static final class Shot { public byte[] rgba; public int w, h, rot; }

    public static void init(Context c) { app = c.getApplicationContext(); }

    /** 抓一帧。IOException = 可读失败原因。 */
    public static Shot grab() throws IOException {
        if (app == null && Ui.HOST != null) app = Ui.HOST.getApplicationContext();
        if (app == null) throw new IOException("抓帧失败：未初始化");
        File dir = app.getExternalFilesDir(null);
        if (dir == null) throw new IOException("抓帧失败：外部目录不可用");
        File f = new File(dir, "cap.raw");
        String p = f.getAbsolutePath();
        Su.R r = Su.run("screencap '" + p + "' && chmod 644 '" + p + "'");
        if (!r.ok()) throw new IOException("抓帧失败：su/screencap 被拒（" + r.trimmed() + "）");
        byte[] b;
        try {
            if (f.length() <= 0) throw new IOException("空文件");
            b = readAll(f);
        } catch (IOException e1) {
            byte[] b2 = Su.readBytes(p);
            if (b2 == null) throw new IOException("抓帧失败：读取帧文件失败（" + e1.getMessage() + "）");
            b = b2;
        }
        f.delete();
        if (b.length < 16) throw new IOException("抓帧失败：头截断（" + b.length + "B）");
        long w = u32(b, 0), h = u32(b, 4), fmt = u32(b, 8);
        if (fmt != 1) throw new IOException("抓帧失败：像素格式异常 fmt=" + fmt);
        if (w <= 0 || h <= 0 || w > 8192 || h > 8192) throw new IOException("抓帧失败：尺寸异常 " + w + "x" + h);
        long want = 16 + w * h * 4;
        if (b.length != want) throw new IOException("抓帧失败：数据长度异常 " + b.length + " ≠ " + want);
        Shot s = new Shot();
        s.w = (int) w;
        s.h = (int) h;
        s.rgba = new byte[(int) (w * h * 4)];
        System.arraycopy(b, 16, s.rgba, 0, s.rgba.length);
        s.rot = displayRotation();
        return s;
    }

    /** 当前屏幕方向（Surface.ROTATION_* 0..3）。 */
    public static int displayRotation() {
        try {
            WindowManager wm = (WindowManager) app.getSystemService(Context.WINDOW_SERVICE);
            return wm.getDefaultDisplay().getRotation() & 3;
        } catch (Throwable t) {
            return 0;
        }
    }

    private static byte[] readAll(File f) throws IOException {
        FileInputStream in = new FileInputStream(f);
        try {
            ByteArrayOutputStream bos = new ByteArrayOutputStream((int) f.length());
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) bos.write(buf, 0, n);
            return bos.toByteArray();
        } finally {
            in.close();
        }
    }

    private static long u32(byte[] b, int off) {
        return (b[off] & 0xFFL) | ((b[off + 1] & 0xFFL) << 8)
                | ((b[off + 2] & 0xFFL) << 16) | ((b[off + 3] & 0xFFL) << 24);
    }
}
