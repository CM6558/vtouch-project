package com.vtouch.editor;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;

/** root 命令执行与文件读写（全部经 su；应用本身不需要任何权限）。 */
public final class Su {
    private static String cacheDir = "/data/local/tmp";

    public static void init(String cache) { cacheDir = cache; }

    public static final class R {
        public final int code;
        public final String out;
        R(int c, String o) { code = c; out = o; }
        public boolean ok() { return code == 0; }
        public String trimmed() { return out == null ? "" : out.trim(); }
    }

    /** su 绝对路径（应用进程 PATH 里没有 su；探测常见位置）。 */
    private static String suBin() {
        for (String p : new String[]{"/system/bin/su", "/system/xbin/su", "/debug_ramdisk/su"}) {
            try {
                if (new File(p).exists()) return p;
            } catch (Exception e) { /* 忽略 */ }
        }
        return "su";
    }

    /** 执行 `su -c <cmd>`，合并 stdout/stderr，等待结束。 */
    public static R run(String cmd) {
        try {
            Process p = new ProcessBuilder(suBin(), "-c", cmd).redirectErrorStream(true).start();
            InputStream in = p.getInputStream();
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) bos.write(buf, 0, n);
            int code = p.waitFor();
            return new R(code, new String(bos.toByteArray(), "UTF-8"));
        } catch (Exception e) {
            return new R(-1, "su 执行失败: " + e);
        }
    }

    /** 读一个文本文件（经 su cat）；失败返回 null。 */
    public static String read(String path) {
        R r = run("cat '" + path + "' 2>/dev/null");
        return r.ok() ? r.out : null;
    }

    /** 原子写：先写应用缓存，再 su 复制到 <dst>.tmp（chmod/chown 对齐现有文件），最后 mv 覆盖。 */
    public static R writeAtomic(String dst, byte[] data) {
        try {
            File tmp = new File(cacheDir, "wt-write.tmp");
            FileOutputStream fo = new FileOutputStream(tmp);
            fo.write(data);
            fo.close();
            String t = tmp.getAbsolutePath();
            R r = run("cp '" + t + "' '" + dst + ".tmp'"
                    + " && chmod 644 '" + dst + ".tmp'"
                    + " && chown root:root '" + dst + ".tmp'"
                    + " && mv -f '" + dst + ".tmp' '" + dst + "'");
            tmp.delete();
            return r;
        } catch (Exception e) {
            return new R(-1, "写入失败: " + e);
        }
    }

    public static boolean fileExists(String path) {
        return run("test -f '" + path + "'").ok();
    }

    /** 备份：把 path 复制为 path.bak（存在才复制）。 */
    public static void backup(String path) {
        run("test -f '" + path + "' && cp -f '" + path + "' '" + path + ".bak'");
    }

    /** 核心是否在跑；返回 pid 字符串（空 = 未运行）。 */
    public static String corePid() {
        R r = run("pidof vtouchd_ui");
        return r.ok() ? r.trimmed() : "";
    }
}
