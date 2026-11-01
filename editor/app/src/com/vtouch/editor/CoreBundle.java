package com.vtouch.editor;

import java.io.InputStream;
import java.util.Properties;
import java.security.MessageDigest;

/** APK 配套清单；摘要用于构建/复制完整性，不代替 APK 签名或系统 root 边界。 */
public final class CoreBundle {
    public final String build, coreSha, launcherSha;
    public final long coreSize, launcherSize;

    public CoreBundle(InputStream in) throws Exception {
        Properties p = new Properties(); p.load(in);
        build = p.getProperty("build_id", "");
        coreSha = p.getProperty("core_sha256", ""); launcherSha = p.getProperty("launcher_sha256", "");
        coreSize = Long.parseLong(p.getProperty("core_size", "0"));
        launcherSize = Long.parseLong(p.getProperty("launcher_size", "0"));
        if (!"1".equals(p.getProperty("protocol")) || !"18".equals(p.getProperty("shm")) ||
                !"arm64-v8a".equals(p.getProperty("abi")) || !build.matches("[a-f0-9]{24}") ||
                !coreSha.matches("[a-f0-9]{64}") || !launcherSha.matches("[a-f0-9]{64}") ||
                coreSize < 1 || coreSize > 33554432 || launcherSize < 1 || launcherSize > 67108864)
            throw new IllegalArgumentException("APK 配套核心清单非法");
    }

    static String sha(InputStream in) throws Exception {
        MessageDigest d = MessageDigest.getInstance("SHA-256");
        byte[] b = new byte[32768]; int n;
        while ((n = in.read(b)) != -1) d.update(b, 0, n);
        StringBuilder out = new StringBuilder();
        for (byte v : d.digest()) out.append(String.format(java.util.Locale.US, "%02x", v & 255));
        return out.toString();
    }
}
