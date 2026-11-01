package com.vtouch.editor;

import java.util.List;

/** 多文件发布的 shell 计划；先备齐、逐文件 rename，失败回滚，异常退出保留可恢复日志。 */
public final class PublishTransaction {
    private PublishTransaction() {}

    static String quote(String s) { return "'" + s.replace("'", "'\\''") + "'"; }

    static String command(String base, String id, List<String> targets, List<String> sources, boolean root) {
        return command(base, id, targets, sources, root, null);
    }

    static String command(String base, String id, List<String> targets, List<String> sources, boolean root, String expectedScheme) {
        if (targets.isEmpty() || targets.size() != sources.size()) throw new IllegalArgumentException("发布列表不完整");
        String lock = base + "/.publish-lock", stage = base + "/.publish-" + id;
        StringBuilder b = new StringBuilder("set -eu\n");
        b.append("lock=").append(quote(lock)).append("\nstage=").append(quote(stage)).append('\n');
        b.append("mkdir \"$lock\" || { echo '已有发布或恢复任务，请先检查 .publish-lock'; exit 1; }\n");
        b.append("mkdir \"$stage\" || { rmdir \"$lock\"; exit 1; }\n");
        b.append("printf '%s\\n' \"$stage\" > \"$lock/journal\"\n");
        b.append("cleanup() {\n");
        for (int i = 0; i < targets.size(); i++) {
            b.append("rm -f \"$stage/new").append(i).append("\" \"$stage/old").append(i)
             .append("\" \"$stage/had").append(i).append("\" \"$stage/put").append(i)
             .append("\" \"$stage/path").append(i).append("\"\n");
        }
        b.append("rmdir \"$stage\" && rm -f \"$lock/journal\" \"$lock/committed\" && rmdir \"$lock\"\n}\n");
        b.append("rollback() {\nset +e\nfailed=0\n");
        for (int i = targets.size() - 1; i >= 0; i--) {
            String dst = quote(targets.get(i));
            b.append("if [ -f \"$stage/put").append(i).append("\" ]; then\n")
             .append("if [ -f \"$stage/had").append(i).append("\" ]; then cp -p \"$stage/old")
             .append(i).append("\" ").append(dst).append(" || failed=1\nelse rm -f ")
             .append(dst).append(" || failed=1\nfi\nfi\n");
        }
        b.append("if [ \"$failed\" = 0 ]; then\n: > \"$lock/committed\"\nif cleanup; then echo '发布失败，已回滚'; else echo \"旧文件已恢复，但清理未完成，请检查 $stage\"; fi\n")
         .append("else echo \"回滚失败，保留恢复目录 $stage\"; fi\n}\n");
        b.append("trap 'rollback' EXIT\ntrap 'exit 1' HUP INT TERM\n");
        if (expectedScheme != null) b.append("test \"$(cat ").append(quote(base + "/current"))
                .append(")\" = ").append(quote(expectedScheme)).append(" || { echo '方案已切换，未发布，请重新打开编辑'; exit 1; }\n");
        for (int i = 0; i < targets.size(); i++) {
            String dst = quote(targets.get(i));
            b.append("printf '%s\\n' ").append(dst).append(" > \"$stage/path").append(i).append("\"\n")
             .append("if [ -e ").append(dst).append(" ]; then test -f ").append(dst)
             .append("; cp -p ").append(dst).append(" \"$stage/old").append(i)
             .append("\"; : > \"$stage/had").append(i).append("\"; fi\n")
             .append("cp ").append(quote(sources.get(i))).append(" \"$stage/new").append(i).append("\"\n")
             .append("chmod 644 \"$stage/new").append(i).append("\"\n");
            if (root) b.append("chown root:root \"$stage/new").append(i).append("\"\n");
        }
        for (int i = 0; i < targets.size(); i++) {
            b.append(": > \"$stage/put").append(i).append("\"\nmv -f \"$stage/new").append(i)
             .append("\" ").append(quote(targets.get(i))).append('\n');
        }
        b.append(": > \"$lock/committed\"\ntrap - EXIT HUP INT TERM\ncleanup\n");
        return b.toString();
    }

    /** 人工确认后的恢复：只使用发布计划记录的明确文件，不执行日志中的 shell 文本。 */
    static String recovery(String base) {
        StringBuilder b = new StringBuilder("set -eu\nbase=" + quote(base) + "\nlock=\"$base/.publish-lock\"\n");
        b.append("test -d \"$lock\"\n")
         .append("if [ ! -f \"$lock/journal\" ]; then rmdir \"$lock\"; echo '空发布锁已释放'; exit 0; fi\n")
         .append("IFS= read -r stage < \"$lock/journal\"\n")
         .append("case \"$stage\" in \"$base\"/.publish-*) ;; *) echo '恢复目录非法'; exit 1;; esac\n")
         .append("suffix=${stage#\"$base\"/.publish-}\ncase \"$suffix\" in ''|*/*) exit 1;; esac\n")
         .append("if [ ! -f \"$lock/committed\" ]; then\ntest -d \"$stage\"\n")
         .append("for marker in \"$stage\"/put*; do\n[ -f \"$marker\" ] || continue\n")
         .append("i=${marker##*/put}\ncase \"$i\" in ''|*[!0-9]*) exit 1;; esac\n")
         .append("IFS= read -r dst < \"$stage/path$i\"\n")
         .append("case \"$dst\" in \"$base\"/*) ;; *) echo '恢复目标越界'; exit 1;; esac\n")
         .append("case \"$dst\" in *'/../'*|*'/./'*) exit 1;; esac\n")
         .append("if [ -f \"$stage/had$i\" ]; then cp -p \"$stage/old$i\" \"$dst\"; else rm -f \"$dst\"; fi\ndone\nfi\n")
         .append("for file in \"$stage\"/new* \"$stage\"/old* \"$stage\"/had* \"$stage\"/put* \"$stage\"/path*; do\n")
         .append("[ -f \"$file\" ] || continue\nrm -f \"$file\"\ndone\n")
         .append("if [ -d \"$stage\" ]; then rmdir \"$stage\"; fi\nrm -f \"$lock/journal\" \"$lock/committed\"\nrmdir \"$lock\"\necho '发布日志已恢复或清理完成'\n");
        return b.toString();
    }
}
