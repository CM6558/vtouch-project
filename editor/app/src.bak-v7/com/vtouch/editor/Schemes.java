package com.vtouch.editor;

import java.util.ArrayList;
import java.util.List;

/**
 * 方案（schemes/）管理。文件级操作，维持「live == schemes/<current>」不变量：
 *  - 保存 live 后调用 mirror()，把两份文件镜像进当前方案目录；
 *  - 切换 = 方案文件复制为 live + 写 current（先 regions 后 ops 的原子替换）。
 */
public final class Schemes {
    public static final String DIR = "/data/local/vtouch-runtime/schemes";
    public static final String CURRENT = "/data/local/vtouch-runtime/current";

    public static List<String> list() {
        List<String> out = new ArrayList<String>();
        Su.R r = Su.run("ls -1 '" + DIR + "' 2>/dev/null");
        if (!r.ok()) return out;
        for (String s : r.out.split("\n")) {
            s = s.trim();
            if (!s.isEmpty()) out.add(s);
        }
        return out;
    }

    public static String current() {
        String c = Su.read(CURRENT);
        return c == null ? "" : c.trim();
    }

    /** 每个方案的两表条目数：名字 → "区域 N · 操作 M"（读不到的按 0 计；一次 su 拿全量）。 */
    public static java.util.Map<String, String> counts() {
        java.util.Map<String, String> out = new java.util.HashMap<String, String>();
        Su.R r = Su.run("cd '" + DIR + "' 2>/dev/null && for d in */; do n=${d%/};"
                + " rr=$(grep -c '^region ' \"$d/regions.conf\" 2>/dev/null); oo=$(grep -c '^op ' \"$d/ops.conf\" 2>/dev/null);"
                + " echo \"$n|${rr:-0}|${oo:-0}\"; done");
        if (!r.ok()) return out;
        for (String line : r.out.split("\n")) {
            String[] t = line.trim().split("\\|");
            if (t.length >= 3) out.put(t[0], "区域 " + t[1] + " · 操作 " + t[2]);
        }
        return out;
    }

    /** 切换：方案文件 → live（tmp+mv），写 current（无换行）。 */
    public static Su.R switchTo(String name) {
        return Su.run("cp '" + DIR + "/" + name + "/regions.conf' '" + Regions.PATH + ".tmp'"
                + " && cp '" + DIR + "/" + name + "/ops.conf' '" + Ops.PATH + ".tmp'"
                + " && chmod 644 '" + Regions.PATH + ".tmp' '" + Ops.PATH + ".tmp'"
                + " && chown root:root '" + Regions.PATH + ".tmp' '" + Ops.PATH + ".tmp'"
                + " && mv -f '" + Regions.PATH + ".tmp' '" + Regions.PATH + "'"
                + " && mv -f '" + Ops.PATH + ".tmp' '" + Ops.PATH + "'"
                + " && printf '%s' '" + name + "' > '" + CURRENT + "'"
                + " && chmod 644 '" + CURRENT + "' && chown root:root '" + CURRENT + "'");
    }

    /** 镜像：live → 当前方案目录（保存 live 后调用；无 current 则跳过）。 */
    public static void mirror() {
        String cur = current();
        if (cur.isEmpty()) return;
        Su.run("test -d '" + DIR + "/" + cur + "'"
                + " && cp -f '" + Regions.PATH + "' '" + DIR + "/" + cur + "/regions.conf'"
                + " && cp -f '" + Ops.PATH + "' '" + DIR + "/" + cur + "/ops.conf'");
    }

    /** 从当前 live 另存为新方案（不自动切换）。 */
    public static Su.R saveAs(String name) {
        return Su.run("mkdir -p '" + DIR + "/" + name + "'"
                + " && cp -f '" + Regions.PATH + "' '" + DIR + "/" + name + "/regions.conf'"
                + " && cp -f '" + Ops.PATH + "' '" + DIR + "/" + name + "/ops.conf'"
                + " && chmod 644 '" + DIR + "/" + name + "/regions.conf' '" + DIR + "/" + name + "/ops.conf'"
                + " && chown root:root '" + DIR + "/" + name + "' '" + DIR + "/" + name + "/regions.conf' '" + DIR + "/" + name + "/ops.conf'");
    }

    /** 新建空方案（版本行取自 live 文件，保证与当前格式一致；不自动切换）。 */
    public static Su.R createEmpty(String name) {
        String regHeader = firstLine(Su.read(Regions.PATH));
        String opsHeader = firstLine(Su.read(Ops.PATH));
        if (regHeader == null) regHeader = "#vtouch-regions v2";
        if (opsHeader == null) opsHeader = "#vtouch-ops v5";
        Su.R r = Su.run("mkdir -p '" + DIR + "/" + name + "'");
        if (!r.ok()) return r;
        Su.R r2 = Su.writeAtomic(DIR + "/" + name + "/regions.conf", (regHeader + "\n").getBytes());
        if (!r2.ok()) return r2;
        return Su.writeAtomic(DIR + "/" + name + "/ops.conf", (opsHeader + "\n").getBytes());
    }

    /** 重命名（当前方案改名时同步更新 current）。 */
    public static Su.R rename(String oldName, String newName) {
        Su.R r = Su.run("mv '" + DIR + "/" + oldName + "' '" + DIR + "/" + newName + "'");
        if (!r.ok()) return r;
        if (oldName.equals(current()))
            return Su.run("printf '%s' '" + newName + "' > '" + CURRENT + "'");
        return r;
    }

    /** 删除（调用方保证不是当前方案）。 */
    public static Su.R delete(String name) {
        return Su.run("rm -rf '" + DIR + "/" + name + "'");
    }

    /** 方案名规则与区域/操作 id 同一把尺子。 */
    public static boolean nameOk(String s) { return Regions.idOk(s); }

    private static String firstLine(String text) {
        if (text == null) return null;
        for (String s : text.split("\n")) {
            s = s.trim();
            if (!s.isEmpty()) return s;
        }
        return null;
    }
}
