package com.vtouch.editor;

import java.util.LinkedHashMap;
import java.util.Map;

/** 绑定协议纯逻辑：每次响应必须完整、请求号相同且版本/构建能力匹配。 */
public final class CoreProtocol {
    public static final String VERSION = "1", SHM = "18";
    public static final class Reply {
        public final Map<String, String> fields = new LinkedHashMap<String, String>();
        public String error;
        public String get(String key) { String s = fields.get(key); return s == null ? "" : s; }
        public boolean ok() { return error == null && "ok".equals(get("state")); }
    }
    private CoreProtocol() {}

    static Reply parse(String text, String request) {
        Reply r = new Reply();
        if (text == null || text.length() > 4096) { r.error = "控制响应为空或过长"; return r; }
        boolean ended = false;
        for (String line : text.split("\n")) {
            if (ended && !line.isEmpty()) { r.error = "控制响应结束后仍有内容"; return r; }
            int eq = line.indexOf('=');
            if (eq <= 0 || r.fields.containsKey(line.substring(0, eq))) { r.error = "控制响应格式异常"; return r; }
            r.fields.put(line.substring(0, eq), line.substring(eq + 1));
            ended = "end=1".equals(line);
        }
        if (!ended || !VERSION.equals(r.get("protocol")) || !SHM.equals(r.get("shm")) ||
                !request.equals(r.get("request_id")) || !r.get("core_pid").matches("[1-9][0-9]*") ||
                !r.get("instance").matches("[1-9][0-9]*")) r.error = "核心协议/实例或请求号不匹配";
        return r;
    }

    static String compatible(Reply r, String build) {
        if (r.error != null) return r.error;
        if (!build.equals(r.get("build_id"))) return "当前核心与 App 不是配套版本，请停止后安装内置核心";
        String caps = "," + r.get("capabilities") + ",";
        for (String cap : new String[]{"status", "reload", "stop_script", "shutdown", "publish_lock"})
            if (!caps.contains("," + cap + ",")) return "核心缺少能力：" + cap;
        return null;
    }
}
