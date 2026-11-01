package com.vtouch.editor;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.ArrayList;

/** 离机发布失败注入；只改自身临时目录，不调用 su 或设备输入。 */
public final class PublishCheck {
    static void check(boolean b, String msg) { if (!b) throw new AssertionError(msg); }
    static byte[] bytes(String s) { return s.getBytes(StandardCharsets.UTF_8); }
    static int run(String command) throws Exception {
        Process p = new ProcessBuilder("sh", "-c", command).inheritIO().start();
        return p.waitFor();
    }
    public static void main(String[] args) throws Exception {
        File base = Files.createTempDirectory("vt-publish-check-").toFile();
        File a = new File(base, "a"), b = new File(base, "b");
        File na = new File(base, "new-a"), nb = new File(base, "new-b");
        Files.write(a.toPath(), bytes("old-a")); Files.write(b.toPath(), bytes("old-b"));
        Files.write(na.toPath(), bytes("new-a")); Files.write(nb.toPath(), bytes("new-b"));
        ArrayList<String> targets = new ArrayList<String>(Arrays.asList(a.getPath(), b.getPath()));
        ArrayList<String> sources = new ArrayList<String>(Arrays.asList(na.getPath(), nb.getPath()));
        String plan = PublishTransaction.command(base.getPath(), "failed", targets, sources, false);
        String last = "mv -f \"$stage/new1\" " + PublishTransaction.quote(b.getPath());
        check(run(plan.replace(last, "false # injected final rename failure")) != 0, "写入失败必须返回失败");
        check(Arrays.equals(Files.readAllBytes(a.toPath()), bytes("old-a")), "首文件已回滚");
        check(Arrays.equals(Files.readAllBytes(b.toPath()), bytes("old-b")), "末文件保持旧内容");
        check(!new File(base, ".publish-lock").exists(), "回滚成功释放发布锁");
        check(run(PublishTransaction.command(base.getPath(), "success", targets, sources, false)) == 0, "成功发布");
        check(Arrays.equals(Files.readAllBytes(a.toPath()), bytes("new-a")), "成功写入首文件");
        check(Arrays.equals(Files.readAllBytes(b.toPath()), bytes("new-b")), "成功写入末文件");
        File lock = new File(base, ".publish-lock"), stage = new File(base, ".publish-crash");
        check(lock.mkdir() && stage.mkdir(), "准备异常退出日志");
        Files.write(new File(lock, "journal").toPath(), bytes(stage.getPath() + "\n"));
        Files.write(new File(stage, "path0").toPath(), bytes(a.getPath() + "\n"));
        Files.write(new File(stage, "old0").toPath(), bytes("before-crash"));
        Files.write(new File(stage, "had0").toPath(), new byte[0]);
        Files.write(new File(stage, "put0").toPath(), new byte[0]);
        check(run(PublishTransaction.recovery(base.getPath())) == 0, "中断发布恢复");
        check(Arrays.equals(Files.readAllBytes(a.toPath()), bytes("before-crash")) && !lock.exists(), "恢复旧版并释放锁");
        Files.write(new File(base, "current").toPath(), bytes("other"));
        check(run(PublishTransaction.command(base.getPath(), "switched", targets, sources, false, "expected")) != 0,
                "方案已切换时拒绝发布");
        check(Arrays.equals(Files.readAllBytes(a.toPath()), bytes("before-crash")), "拒绝发布不改原文件");
        check(lock.mkdir() && stage.mkdir(), "准备已提交的中断清理");
        Files.write(new File(lock, "journal").toPath(), bytes(stage.getPath() + "\n"));
        Files.write(new File(lock, "committed").toPath(), new byte[0]);
        Files.write(new File(stage, "path0").toPath(), bytes(a.getPath() + "\n"));
        Files.write(new File(stage, "put0").toPath(), new byte[0]);
        check(run(PublishTransaction.recovery(base.getPath())) == 0, "提交完成后的清理可恢复");
        check(a.isFile() && Arrays.equals(Files.readAllBytes(a.toPath()), bytes("before-crash")), "清理中断不得删除已提交文件");
        String conf = "#vtouch-ops v8\n", hash = OpsV8.fnv8(bytes(conf));
        String status = "version=1\ncore_pid=123\nops_hash=" + hash + "\nscheme=默认\nstate=loaded\nbad=0\n";
        check(RuntimeStatus.describe("123", conf, status, "默认").contains("已加载"), "加载确认");
        check(!RuntimeStatus.describe("456", conf, status, "默认").contains("已加载"), "旧实例确认不可信");
        check(!RuntimeStatus.describe("123", conf + "x", status, "默认").contains("已加载"), "旧配置确认不可信");
        System.out.println("PublishCheck OK · 临时目录 " + base);
    }
}
