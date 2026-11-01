package com.vtouch.editor;

import java.util.HashMap;
import java.util.Map;

/** 面板确认来自当前核心实例，且哈希对应当前盘上的操作表；旧核心明确降级为未确认。 */
public final class RuntimeStatus {
    private RuntimeStatus() {}

    static String describe(String pid, String conf, String status, String scheme) {
        if (pid == null || pid.trim().isEmpty()) return "核心未运行，加载状态未确认";
        Map<String, String> values = new HashMap<String, String>();
        if (status != null) for (String line : status.split("\n")) {
            int eq = line.indexOf('=');
            if (eq > 0) values.put(line.substring(0, eq), line.substring(eq + 1));
        }
        if (!"1".equals(values.get("version")) || !pid.trim().equals(values.get("core_pid")))
            return "核心进程存在，尚无当前实例的加载确认";
        if (!"loaded".equals(values.get("state")) || !"0".equals(values.get("bad")))
            return "配置加载失败或存在坏记录，请查看核心日志";
        if (conf == null || !OpsV8.fnv8(conf.getBytes(java.nio.charset.StandardCharsets.UTF_8)).equals(values.get("ops_hash")))
            return "已保存，等待核心加载新操作表";
        if (scheme == null || !scheme.equals(values.get("scheme"))) return "方案切换尚未确认";
        return "当前操作表已加载（核心 pid " + pid.trim() + "）";
    }

    public static String read() {
        String pid = Su.corePid();
        String result = describe(pid, Su.read(Ops.PATH),
                Su.read(OpsV8.base() + "/reload-status.txt"), Schemes.current());
        if (pid.isEmpty() || result.startsWith("配置加载失败")) return result;
        if (Su.run("test -d " + PublishTransaction.quote(OpsV8.base() + "/.publish-lock")
                + " || test -f " + PublishTransaction.quote(OpsV8.base() + "/.publish-pending")).ok())
            return "已保存或发布中，等待核心确认新版本";
        return result;
    }
}
