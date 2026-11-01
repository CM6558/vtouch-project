package com.vtouch.editor;

import java.io.ByteArrayInputStream;
import java.nio.charset.StandardCharsets;

/** 配套协议离机检查，不执行 su、不启动核心。 */
public final class CoreBindingCheck {
    private static void check(boolean b, String msg) { if (!b) throw new AssertionError(msg); }
    private static String reply(String extra) {
        return "protocol=1\nshm=18\nbuild_id=abcdef\nrequest_id=req1\ncore_pid=123\ninstance=987\n"
                + "capabilities=status,reload,stop_script,shutdown,publish_lock\nstate=ok\n" + extra + "end=1\n";
    }
    public static void main(String[] args) throws Exception {
        CoreProtocol.Reply r = CoreProtocol.parse(reply(""), "req1");
        check(r.ok() && CoreProtocol.compatible(r, "abcdef") == null, "配套握手");
        check(CoreProtocol.compatible(r, "other") != null, "旧构建拒绝");
        check(CoreProtocol.parse(reply(""), "another").error != null, "旧请求回执拒绝");
        check(CoreProtocol.parse(reply("").replace("end=1\n", ""), "req1").error != null, "半包拒绝");
        check(CoreProtocol.parse(reply("state=ok\n"), "req1").error != null, "重复字段拒绝");
        check(CoreProtocol.parse(reply("").replace("shm=18", "shm=17"), "req1").error != null, "共享契约版本拒绝");
        r = CoreProtocol.parse(reply("").replace("publish_lock", "unknown"), "req1");
        check(CoreProtocol.compatible(r, "abcdef") != null, "缺少能力拒绝");
        String manifest = "protocol=1\nshm=18\nabi=arm64-v8a\nbuild_id=0123456789abcdef01234567\n"
                + "core_sha256=" + repeat("0", 64) + "\nlauncher_sha256=" + repeat("1", 64)
                + "\ncore_size=123\nlauncher_size=456\n";
        CoreBundle b = new CoreBundle(new ByteArrayInputStream(manifest.getBytes(StandardCharsets.UTF_8)));
        check(b.coreSize == 123 && b.build.length() == 24, "配套清单");
        check(CoreBundle.sha(new ByteArrayInputStream("abc".getBytes(StandardCharsets.UTF_8)))
                .equals("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "SHA-256 金样");
        boolean rejected = false;
        try { new CoreBundle(new ByteArrayInputStream(manifest.replace("shm=18", "shm=17").getBytes(StandardCharsets.UTF_8))); }
        catch (Exception expected) { rejected = true; }
        check(rejected, "旧核心清单拒绝");
        System.out.println("CoreBindingCheck OK");
    }
    private static String repeat(String s, int n) { StringBuilder b = new StringBuilder(); while (n-- > 0) b.append(s); return b.toString(); }
}
