#!/bin/sh
# 一键重画 draw.io 版调用图：图数据 → Graphviz 自动布局 → .drawio → 结构校验 → 页面贴合 → 可编辑 PNG + SVG。
#
# 依赖（本机已装）：
#   Graphviz dot  : winget install --id Graphviz.Graphviz -e
#   draw.io 桌面版 : C:\Program Files\draw.io\draw.io.exe（CLI 用 -x 导出）
# 用到的渲染器在 drawio-skill 里，不复制到本仓库（技能自拥有，不要改它）。
#
# 用法： sh scripts/render_callgraph_drawio.sh
cd "$(dirname "$0")/.." || exit 1
SD="C:/Users/21102/AppData/Local/hermes/skills/drawio-skill/scripts"
DRAWIO="C:/Program Files/draw.io/draw.io.exe"
export PATH="$PATH:/c/Program Files/Graphviz/bin"
rc=0
python scripts/gen_callgraph_drawio.py || exit 1
python "$SD/autolayout.py" build/diagrams/vtouch-callgraph-graph.json --tune -o docs/diagrams/vtouch-callgraph.drawio || exit 1
python "$SD/validate.py" docs/diagrams/vtouch-callgraph.drawio --score || rc=1
python scripts/fit_drawio_page.py docs/diagrams/vtouch-callgraph.drawio || exit 1
"$DRAWIO" -x -f png -e -o docs/diagrams/vtouch-callgraph.drawio.png docs/diagrams/vtouch-callgraph.drawio || exit 1
python "$SD/repair_png.py" docs/diagrams/vtouch-callgraph.drawio.png || exit 1
"$DRAWIO" -x -f svg -e --embed-svg-images -o docs/diagrams/vtouch-callgraph.drawio.svg docs/diagrams/vtouch-callgraph.drawio || exit 1
ls -l docs/diagrams/vtouch-callgraph.drawio docs/diagrams/vtouch-callgraph.drawio.png docs/diagrams/vtouch-callgraph.drawio.svg
echo "rc=$rc"
exit $rc
