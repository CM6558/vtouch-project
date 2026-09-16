#!/bin/sh
# 一键渲染「vtouchd 调用图」三张：生成 JSON（权威源）→ 自动消解路由冲突 → 五道校验 → 版面自检三项 → 2x PNG。
# 产物落 docs/diagrams/（随仓库走）；临时/探针文件落 build/diagrams/（被 .gitignore）。
# 用法： sh scripts/render_callgraphs.sh
cd "$(dirname "$0")/.." || exit 1
SK="C:/Users/21102/AppData/Local/hermes/skills/creative/technical-diagram-generation/scripts"
TMP="build/diagrams"
mkdir -p "$TMP" docs/diagrams
rc=0
for s in ctrl phys proto; do
    echo "==================== 生成 $s"
    python scripts/gen_callgraph.py "$s" || rc=1
done
python scripts/fix_diagram_ports.py \
    docs/diagrams/vtouch-callgraph-ctrl.json \
    docs/diagrams/vtouch-callgraph-phys.json \
    docs/diagrams/vtouch-callgraph-proto.json || rc=1
for s in ctrl phys proto; do
    echo "==================== 校验 $s"
    python "$SK/render_check_export.py" "docs/diagrams/vtouch-callgraph-$s.json" || rc=1
    python "$SK/verify_svg_layout.py" "docs/diagrams/vtouch-callgraph-$s.json" \
        "docs/diagrams/vtouch-callgraph-$s.svg" "docs/diagrams/vtouch-callgraph-$s.png" || rc=1
done
echo "==================== rc=$rc"
exit $rc
