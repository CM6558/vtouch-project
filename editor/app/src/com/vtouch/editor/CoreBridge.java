package com.vtouch.editor;

import android.content.Context;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.UUID;

/** APK 自带控制助手 + root-only socket；不连接/挤走触摸 WebSocket，不自动启动核心。 */
public final class CoreBridge {
    private static Context app;
    private static CoreBundle bundle;
    private static File helper, launcher;
    private static String helperPath;
    private CoreBridge() {}

    public static synchronized void init(Context c) { app = c.getApplicationContext(); }
    private static String q(String s) { return PublishTransaction.quote(s); }

    private static boolean verified(File f, long size, String sha) throws Exception {
        if (!f.isFile() || f.length() != size) return false;
        InputStream in = new FileInputStream(f);
        try { return sha.equals(CoreBundle.sha(in)); } finally { in.close(); }
    }

    private static void extract(String asset, File dst, long size, String sha) throws Exception {
        if (verified(dst, size, sha)) return;
        File tmp = File.createTempFile("core-", ".tmp", dst.getParentFile());
        try {
            InputStream in = app.getAssets().open(asset);
            try {
                FileOutputStream out = new FileOutputStream(tmp);
                try {
                    byte[] b = new byte[32768]; int n; long total = 0;
                    while ((n = in.read(b)) != -1) { total += n; if (total > size) throw new Exception("内置核心长度超限"); out.write(b, 0, n); }
                    out.getFD().sync();
                } finally { out.close(); }
            } finally { in.close(); }
            if (!verified(tmp, size, sha) || !tmp.renameTo(dst)) throw new Exception("内置核心摘要不符或提取失败");
        } finally { tmp.delete(); }
    }

    private static synchronized CoreBundle prepare() throws Exception {
        if (app == null) throw new Exception("App 核心桥尚未初始化");
        if (bundle != null && helperPath != null && helper != null && helper.isFile()
                && launcher != null && launcher.isFile()) return bundle;
        if (bundle == null) {
            InputStream in = app.getAssets().open("core/core.properties");
            try { bundle = new CoreBundle(in); } finally { in.close(); }
        }
        if (!java.util.Arrays.asList(android.os.Build.SUPPORTED_ABIS).contains("arm64-v8a"))
            throw new Exception("内置核心仅支持 arm64-v8a 设备");
        File dir = new File(app.getFilesDir(), "paired-core/" + bundle.build);
        if (!dir.isDirectory() && !dir.mkdirs()) throw new Exception("创建核心缓存失败");
        helper = new File(dir, "vtouch-ctl"); launcher = new File(dir, "vtouch.sh");
        extract("core/vtouchd_ui", helper, bundle.coreSize, bundle.coreSha);
        extract("core/vtouch.sh", launcher, bundle.launcherSize, bundle.launcherSha);
        String root = "/data/local/tmp/vtouch-app", version = root + "/" + bundle.build;
        String preparedPath = version + "/vtouch-ctl";
        String cmd = "set -eu\ntest ! -L " + q(root) + " && test ! -L " + q(version) + "\nmkdir -p " + q(version) + "\n"
                + "test ! -L " + q(preparedPath) + "\n"
                + "chmod 700 " + q(root) + " " + q(version) + "\n"
                + "chown root:root " + q(root) + " " + q(version) + "\n"
                + "if [ ! -f " + q(preparedPath) + " ] || [ \"$(sha256sum " + q(preparedPath)
                + " | cut -d ' ' -f 1)\" != " + q(bundle.coreSha) + " ]; then\n"
                + "cp " + q(helper.getAbsolutePath()) + " " + q(preparedPath + ".tmp")
                + "\nchmod 700 " + q(preparedPath + ".tmp") + "\nchown root:root " + q(preparedPath + ".tmp")
                + "\nmv -f " + q(preparedPath + ".tmp") + " " + q(preparedPath) + "\nfi\n"
                + "test ! -L " + q(preparedPath) + "\ntest \"$(sha256sum " + q(preparedPath)
                + " | cut -d ' ' -f 1)\" = " + q(bundle.coreSha);
        Su.R r = Su.run(cmd);
        if (!r.ok()) throw new Exception("准备配套控制助手失败：" + r.trimmed());
        helperPath = preparedPath;
        return bundle;
    }

    public static CoreProtocol.Reply request(String command) {
        CoreProtocol.Reply error = new CoreProtocol.Reply();
        if (!java.util.Arrays.asList("status", "reload", "stop_script", "shutdown").contains(command)) {
            error.error = "控制命令不在允许列表"; return error;
        }
        try {
            CoreBundle b = prepare();
            String id = UUID.randomUUID().toString().replace("-", "");
            Su.R result = Su.run(q(helperPath) + " --control " + q(command) + " " + q(id) + " " + q(b.build));
            if (!result.ok()) { error.error = "核心控制通道不可达（旧核心或未启动）"; return error; }
            CoreProtocol.Reply reply = CoreProtocol.parse(result.out, id);
            String compatibility = CoreProtocol.compatible(reply, b.build);
            if (compatibility != null) reply.error = compatibility;
            return reply;
        } catch (Exception e) { error.error = e.getMessage(); return error; }
    }

    public static String statusText(CoreProtocol.Reply r) {
        if (r.error != null) return r.error;
        if (!r.ok()) return "核心拒绝请求：" + r.get("reason");
        return "配套核心 " + r.get("build_id") + " · pid " + r.get("core_pid")
                + ("1".equals(r.get("panel_ready")) ? " · 面板在线" : " · 面板未就绪")
                + ("1".equals(r.get("script_running")) ? " · 脚本运行中" : " · 脚本空闲");
    }

    public static String loadText(CoreProtocol.Reply live) {
        if (!live.ok()) return statusText(live);
        if (!"1".equals(live.get("panel_ready"))) return "配套核心已连接，面板暂未就绪";
        if (!"loaded".equals(live.get("load_state"))) return "配套核心已连接，加载状态：" + live.get("load_state");
        String conf = Su.read(Ops.PATH);
        if (conf == null || !OpsV8.fnv8(conf.getBytes(java.nio.charset.StandardCharsets.UTF_8)).equals(live.get("ops_hash"))
                || !Schemes.current().equals(live.get("scheme"))) return "已保存，等待核心加载当前方案/操作表";
        if (Su.run("test -d " + q(OpsV8.base() + "/.publish-lock") + " || test -f " + q(OpsV8.base() + "/.publish-pending")).ok())
            return "发布中或等待加载确认";
        return "配套核心实时确认：当前操作表已加载";
    }

    /** 离线允许编辑；有运行核心时只向配套且具备发布锁能力的版本写配置。 */
    public static String writableError() {
        if (Su.corePid().isEmpty()) return null;
        CoreProtocol.Reply reply = request("status");
        if (!reply.ok()) return statusText(reply) + "；为避免版本混用，未发布。可先停止旧核心。";
        return null;
    }

    /** 只有用户按启动/安装按钮才提取启动脚本到 root 目录；不读写 /sdcard/vtouch.sh。 */
    public static synchronized Su.R start(boolean installOnly) {
        try {
            CoreBundle b = prepare();
            if (!Su.corePid().isEmpty()) {
                CoreProtocol.Reply r = request("status");
                return new Su.R(-1, "核心已运行，不替换载荷；" + statusText(r));
            }
            String rootLauncher = "/data/local/tmp/vtouch-app/" + b.build + "/vtouch.sh";
            Su.R copy = Su.run("set -eu\ncp " + q(launcher.getAbsolutePath()) + " " + q(rootLauncher + ".tmp")
                    + "\nchmod 600 " + q(rootLauncher + ".tmp") + "\nchown root:root " + q(rootLauncher + ".tmp")
                    + "\ntest \"$(sha256sum " + q(rootLauncher + ".tmp") + " | cut -d ' ' -f 1)\" = " + q(b.launcherSha)
                    + "\nmv -f " + q(rootLauncher + ".tmp") + " " + q(rootLauncher));
            if (!copy.ok()) return copy;
            Su.R run = Su.run("sh " + q(rootLauncher) + (installOnly ? " install" : " start"));
            if (!run.ok() || installOnly) return run;
            CoreProtocol.Reply r = request("status");
            return new Su.R(r.ok() ? 0 : -1, run.out + "\n" + statusText(r));
        } catch (Exception e) { return new Su.R(-1, e.toString()); }
    }

    public static CoreProtocol.Reply reloadAfterSave() {
        if (Su.corePid().isEmpty()) {
            CoreProtocol.Reply r = new CoreProtocol.Reply(); r.error = "核心未运行，启动内置核心后生效"; return r;
        }
        return request("reload");
    }

    public static Su.R stop() {
        CoreProtocol.Reply r = request("shutdown");
        if (!r.ok()) return new Su.R(-1, statusText(r) + "；未向不匹配核心发停止信号");
        for (int i = 0; i < 30; i++) {
            if (Su.corePid().isEmpty()) return new Su.R(0, "配套核心已停止");
            try { Thread.sleep(100); } catch (InterruptedException e) { Thread.currentThread().interrupt(); break; }
        }
        return new Su.R(-1, "已请求安全退出，但尚未确认进程结束");
    }
}
