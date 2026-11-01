#!/data/data/com.termux/files/usr/bin/bash
# build.sh compiles these checks but intentionally does not run them.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
JAVA=/data/data/com.termux/files/usr/lib/jvm/java-17-openjdk/bin/java
for name in MatcherColorCheck PublishCheck OpsV8Check VtToStepsCheck VtFromStepsCheck VtMigrateCheck; do
    "$JAVA" -cp "$HERE/build/classes" "com.vtouch.editor.$name"
done
