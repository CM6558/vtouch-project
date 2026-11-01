package com.vtouch.editor;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

/**
 * 帧库（app 外部目录 frames/）：PNG + meta.txt（每行 "name rot w h ts"），最近 KEEP=10 帧自动轮换。
 * 名字 = 单调递增毫秒（同毫秒内自增防撞）。setDir = 测试目录重定向（null 恢复默认）。
 */
public final class FrameStore {
    public static final int KEEP = 10;
    private static Context app;
    private static File override;
    private static long lastTs;

    /** 内存直通（同进程跨组件；v7.1）：浮标抓完 → 查看器直接取，跳过 PNG 落盘/解码往返。 */
    public static volatile Capture.Shot flashShot;
    public static volatile long flashTs;

    public static final class Entry { public String name; public int rot, w, h; public long ts; }

    public static void init(Context c) { app = c.getApplicationContext(); }

    /** 测试用目录重定向；null = 恢复默认（files/frames）。 */
    public static void setDir(File d) { override = d; }

    static File dir() {
        if (override != null) return override;
        if (app == null && Ui.HOST != null) app = Ui.HOST.getApplicationContext();
        File d = new File(app.getExternalFilesDir(null), "frames");
        if (!d.exists()) d.mkdirs();
        return d;
    }

    /** 存一帧：PNG + meta 追加；超过 KEEP 清最旧。 */
    public static Entry save(Capture.Shot s) throws Exception {
        File d = dir();
        if (!d.exists()) d.mkdirs();
        long ts = System.currentTimeMillis();
        if (ts <= lastTs) ts = lastTs + 1;
        lastTs = ts;
        String name = String.valueOf(ts);
        Bitmap bm = bitmapFromRgba(s.rgba, s.w, s.h);
        FileOutputStream fo = new FileOutputStream(new File(d, name + ".png"));
        try {
            if (!bm.compress(Bitmap.CompressFormat.PNG, 100, fo)) throw new Exception("PNG 编码失败");
        } finally {
            fo.close();
        }
        appendMeta(d, name + " " + (s.rot & 3) + " " + s.w + " " + s.h + " " + ts);
        prune(d);
        Entry e = new Entry();
        e.name = name;
        e.rot = s.rot & 3;
        e.w = s.w;
        e.h = s.h;
        e.ts = ts;
        return e;
    }

    /** 帧列表（新→旧）。 */
    public static List<Entry> list() {
        List<Entry> ls = new ArrayList<Entry>();
        try {
            File meta = new File(dir(), "meta.txt");
            if (!meta.exists()) return ls;
            String all = new String(readAll(meta), "UTF-8");
            for (String ln : all.split("\n")) {
                String[] p = ln.trim().split("\\s+");
                if (p.length != 5) continue;
                try {
                    Entry e = new Entry();
                    e.name = p[0];
                    e.rot = Integer.parseInt(p[1]);
                    e.w = Integer.parseInt(p[2]);
                    e.h = Integer.parseInt(p[3]);
                    e.ts = Long.parseLong(p[4]);
                    ls.add(e);
                } catch (Exception ig) { /* 跳过坏行 */ }
            }
            Collections.sort(ls, new Comparator<Entry>() {
                public int compare(Entry a, Entry b) { return b.ts < a.ts ? -1 : (b.ts > a.ts ? 1 : 0); }
            });
        } catch (Throwable t) { /* 空列表 */ }
        return ls;
    }

    public static Entry latest() {
        List<Entry> l = list();
        return l.isEmpty() ? null : l.get(0);
    }

    public static Bitmap loadBitmap(Entry e) {
        return BitmapFactory.decodeFile(new File(dir(), e.name + ".png").getAbsolutePath());
    }

    public static byte[] loadRgba(Entry e) {
        Bitmap bm = loadBitmap(e);
        if (bm == null) return null;
        int w = bm.getWidth(), h = bm.getHeight();
        int[] px = new int[w * h];
        bm.getPixels(px, 0, w, 0, 0, w, h);
        byte[] out = new byte[w * h * 4];
        for (int i = 0, j = 0; i < px.length; i++, j += 4) {
            int v = px[i];
            out[j] = (byte) ((v >> 16) & 0xFF);
            out[j + 1] = (byte) ((v >> 8) & 0xFF);
            out[j + 2] = (byte) (v & 0xFF);
            out[j + 3] = (byte) ((v >> 24) & 0xFF);
        }
        return out;
    }

    private static void appendMeta(File d, String line) throws Exception {
        FileOutputStream fo = new FileOutputStream(new File(d, "meta.txt"), true);
        try {
            fo.write((line + "\n").getBytes("UTF-8"));
        } finally {
            fo.close();
        }
    }

    private static void prune(File d) {
        List<Entry> ls = list();
        if (ls.size() <= KEEP) return;
        StringBuilder keep = new StringBuilder();
        for (int i = 0; i < ls.size(); i++) {
            Entry e = ls.get(i);
            if (i >= KEEP) {
                new File(d, e.name + ".png").delete();
            } else {
                keep.append(e.name).append(' ').append(e.rot).append(' ').append(e.w).append(' ')
                        .append(e.h).append(' ').append(e.ts).append('\n');
            }
        }
        try {
            FileOutputStream fo = new FileOutputStream(new File(d, "meta.txt"));
            try {
                fo.write(keep.toString().getBytes("UTF-8"));
            } finally {
                fo.close();
            }
        } catch (Throwable ig) { }
    }

    /** RGBA 字节 → ARGB Bitmap（save 与内存直通共用）。 */
    public static Bitmap bitmapFromRgba(byte[] b, int w, int h) {
        int[] px = new int[w * h];
        for (int i = 0, j = 0; i < px.length; i++, j += 4)
            px[i] = ((b[j + 3] & 0xFF) << 24) | ((b[j] & 0xFF) << 16)
                    | ((b[j + 1] & 0xFF) << 8) | (b[j + 2] & 0xFF);
        return Bitmap.createBitmap(px, w, h, Bitmap.Config.ARGB_8888);
    }

    /** 删一帧（png + meta 重写）。 */
    public static boolean delete(Entry e) {
        if (e == null) return false;
        File d = dir();
        boolean del = new File(d, e.name + ".png").delete();
        List<Entry> ls = list();
        StringBuilder keep = new StringBuilder();
        for (Entry x : ls) {
            if (x.name.equals(e.name)) continue;
            keep.append(x.name).append(' ').append(x.rot).append(' ').append(x.w).append(' ')
                    .append(x.h).append(' ').append(x.ts).append('\n');
        }
        try {
            FileOutputStream fo = new FileOutputStream(new File(d, "meta.txt"));
            try {
                fo.write(keep.toString().getBytes("UTF-8"));
            } finally {
                fo.close();
            }
        } catch (Throwable ig) { }
        return del;
    }

    /** 清空帧库（png 全删 + meta 清空）。 */
    public static boolean clearAll() {
        File d = dir();
        for (Entry x : list()) new File(d, x.name + ".png").delete();
        try {
            FileOutputStream fo = new FileOutputStream(new File(d, "meta.txt"));
            fo.close();
        } catch (Throwable ig) { }
        return list().isEmpty();
    }

    private static byte[] readAll(File f) throws Exception {
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
}
