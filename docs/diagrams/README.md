# 流程图（docs/diagrams）

本项目所有工程图都放在这里。**每张图 = 一个 JSON 源 + 同名 SVG/PNG**：改图只改 `.json`，再跑一次渲染命令，
不要手改 SVG（校验门会拦下坐标/标注重叠这类问题）。

| 图 | 讲什么 | 源 |
|---|---|---|
| `vtouch-full-flow` | **全流程总览（当前实现）**：① 交付（build_bundle.py → /sdcard 单文件 → uiStart 释放）② 面板进程 = daemon（物理采集/合并转发/区域匹配/WS）③ 脚本（`vt.onRegion` + 库内自动 6 件事 + 退出钩子）④ 落地（uinput → 目标 App）；三条数据流：物理穿透 / 区域事件 / 虚拟回注 | `vtouch-full-flow.json` |
| `vtouch-region-to-task-current-flow` | 「区域命中 → AutoJs6 任务」现状慢路版本：脚本侧 6 步仪式逐步标注「必需 / 机制（该收进库）/ 纯仪式」（优化前的对照底片） | `vtouch-region-to-task-current-flow.json` |
| `vtouch-region-to-task-optimized` | 同一链路的**现状 vs 优化后**左右对照：左 7 段 → 右 2 行（`vt.onRegion(id, fn)`），并画出被收进库的 6 件事 | `vtouch-region-to-task-optimized.json` |

## 重新渲染

用 `fireworks-tech-graph` skill 的渲染器（本机已装）：

```sh
SKILL_ROOT="C:/Users/21102/AppData/Local/hermes/skills/fireworks-tech-graph"
# 1) JSON → SVG（带几何/构图校验报告）
python "$SKILL_ROOT/scripts/fireworks.py" render data-flow docs/diagrams/<图>.json \
       docs/diagrams/<图>.svg --report docs/diagrams/<图>.report.json
# 2) 五项校验（XML / marker / 碰撞 / 几何 / 构图）
for c in xml markers collisions geometry composition; do
  python "$SKILL_ROOT/scripts/fireworks.py" check docs/diagrams/<图>.svg --check $c
done
# 3) SVG → PNG（2x，CJK 字体正常；本机没有 cairosvg，用系统 Chrome headless）
"/c/Program Files/Google/Chrome/Application/chrome.exe" --headless=new --disable-gpu --hide-scrollbars \
  --force-device-scale-factor=2 --window-size=<宽>,<高> \
  --screenshot="docs/diagrams/<图>.png" "file:///<绝对路径>/docs/diagrams/<图>.svg"
```

`--window-size` 取该图 JSON 里的 `width`/`height`（2x 缩放 → PNG 是两倍尺寸）。

## 交付前自检（本项目踩过的坑，别省）

1. **五项校验必须全 OK**、`composition.score = 100`；
2. **节点两两重叠要自己查**：标准档 `min_node_gap = 0`，生成器**不会**报「两个盒子叠在一起」——
   叠了就变成后画的盒子盖住前一个的副标题，肉眼看像「文字被截断」（本轮就这么误诊过一次）；
3. **文字是否溢出用实测量，别靠目测**：Chrome 里 `getBBox()` 量 `<text class="node-sub">` 宽度对比盒子宽度
   （本机曾用 `--dump-dom` + 内联 SVG + 脚本把结果写进 DOM 的方式量，46 行 0 溢出）；
4. PNG 回来要**看图复核**（本项目用 `vision_analyze` 分区裁剪看），合成校验不覆盖「文字压线 / 遮挡」这类问题；
   注意**裁剪边界**会造成假的「被截断」结论——先确认裁的是整行、整个盒子。
