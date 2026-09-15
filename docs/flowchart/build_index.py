#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Final gallery: drawio-generated sequence diagram (inline SVG) + supporting tables."""
from pathlib import Path

FLOW = Path(r"D:\MYP\vtouch-project\docs\flowchart")
SVG = (FLOW / "vtouch-example.svg").read_text(encoding="utf-8")
# strip xml declaration + doctype for inline embedding
if SVG.startswith("<?xml"):
    SVG = SVG[SVG.index("?>") + 2:].lstrip("\n")
if SVG.startswith("<!DOCTYPE"):
    end = SVG.index(">") + 1
    SVG = SVG[end:].lstrip("\n")
# remove fixed width/height so it scales responsively
import re
SVG = re.sub(r' width="[^"]*" height="[^"]*"', "", SVG, count=1)

STAGES = [
    ("阶段 1 · 构建与部署", "① Actions: NDK r27d 交叉编译 vtouchd + build_bundle.py 生成双 ABI bundle → ② adb push 到 /sdcard → ③ AutoJs6 运行示例(require + eval UI + bootWatch)。二进制永不入库。"),
    ("阶段 2 · 启动与自释放", "④ vt-takeover 单实例互踢 → ⑤ vt.ensure():pid 存活直返,缺则 install(Base64→su cp→chmod 755,绕 noexec)→ ⑥ wm size 竖屏归一 → nohup 启动 → ⑧ vtouch_init:动态发现 Type-B 设备 → uinput 创建 → listen 27183 → 最后 EVIOCGRAB。"),
    ("阶段 3 · 连接与配置下发", "⑨ 手写 java.net.Socket WS 握手(101 + Sec-WebSocket-Accept 校验,300ms 重试)→ ⑩ overlay 描边 → ⑪ region clear + add 下发 swipeL/tapR → ⑫ sub 订阅 → ⑬ 读线程死循环(3s ping)。"),
    ("阶段 4 · 示例触发", "⑭ 手指按下 swipeL → ⑮ 物理事件流填 phys[0] → ⑯ SYN 帧边界:emit_frame 1:1 透传 → ⑰ 合并帧 writev 注入 uinput → ⑱ region_match 命中(slot_hit=1,纯监听)→ ⑲ region_ev down 双通道通知。"),
    ("阶段 5 · 回触注入", "⑳ onDown → busy(threads.start)→ ㉑ swipe 序列(down/move×N/up,16.7ms 插值)→ ㉒ WS 注入 → ㉓ logical_to_raw + 虚拟槽 → ㉔ 合并帧单 SYN 提交 → ㉕ InputReader 识别上滑。虚拟触摸不自激。"),
    ("阶段 6 · 保活/恢复/清理", "㉗ 5s 兜底热改配置 + rotSync → ㉘ 断线重连闭环(重新 sub + pushRegions)→ ㉙ 退出清理(释放 EVIOCGRAB + 销毁 uinput)。"),
]

CSS = """
*{box-sizing:border-box;margin:0;padding:0}
body{background:#0f0f1a;color:#e2e8f0;font-family:'JetBrains Mono','Cascadia Mono',Consolas,'Microsoft YaHei',monospace;padding:26px 16px 70px;line-height:1.55}
.wrap{max-width:1280px;margin:0 auto}
header{border-bottom:1px solid #334155;padding-bottom:14px;margin-bottom:10px}
h1{font-size:20px;letter-spacing:.4px}
.sub{color:#94a3b8;font-size:12px;margin-top:6px}
.files{display:flex;gap:8px;flex-wrap:wrap;margin:12px 0}
.files a{color:#0f0f1a;background:#22d3ee;text-decoration:none;font-size:11px;font-weight:700;padding:4px 12px;border-radius:20px}
.files a.alt{background:#334155;color:#e2e8f0}
.diagram{background:#ffffff;border:1px solid #1e293b;border-radius:12px;padding:6px;overflow-x:auto;margin-top:10px}
.diagram svg{display:block;width:100%;height:auto}
.stage{display:flex;gap:14px;margin:10px 0;padding:10px 14px;background:#0f172a;border:1px solid #1e293b;border-radius:10px}
.stage .n{flex:none;color:#22d3ee;font-size:11px;font-weight:700;padding-top:2px;min-width:150px}
.stage p{font-size:11.5px;color:#cbd5e1}
h3{margin:24px 0 4px;font-size:14px;color:#a78bfa}
table{border-collapse:collapse;width:100%;font-size:11.5px;margin-top:8px;background:#0f172a}
th,td{border:1px solid #334155;padding:6px 10px;text-align:left;vertical-align:top}
th{background:#1e293b;color:#22d3ee;font-weight:700}
td code{color:#fbbf24;font-size:11px}
footer{margin-top:30px;border-top:1px solid #334155;padding-top:12px;color:#64748b;font-size:10.5px;display:flex;justify-content:space-between;flex-wrap:wrap;gap:8px}
.pill{display:inline-block;border:1px solid #334155;border-radius:20px;padding:1px 8px;font-size:10px;color:#94a3b8;margin-right:6px}
"""

stages_html = "\n".join(
    f'<div class="stage"><div class="n">{n}</div><p>{d}</p></div>' for n, d in STAGES
)

html = f"""<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>vtouch-project 实现逻辑 (drawio 时序图)</title><style>{CSS}</style></head>
<body><div class="wrap">
<header><h1>vtouch-project 完整实现逻辑 — 以「监听+回触」示例为入口</h1>
<div class="sub">drawio-skill 生成: seqlayout.py 时序布局 + 阶段帧 · 手指按下 swipeL → 1:1 透传合并 → region 命中 → 五事件回调 → 虚拟上滑回触 → Android 识别手势 · 全链路 29 步 / 5 参与者 / 6 阶段</div>
<div class="files">
<a href="vtouch-example.drawio">✏️ 可编辑 .drawio (draw.io)</a>
<a class="alt" href="vtouch-example.png">PNG 导出</a>
<a class="alt" href="vtouch-example.svg">SVG 导出</a>
<a class="alt" href="vtouch-example-seq.json">时序输入 JSON</a>
</div></header>

<div class="diagram">{SVG}</div>

<h3>阶段说明 (对应上图 ①-㉙)</h3>
{stages_html}

<h3>WebSocket 命令一览 (docs/VTOUCH_PROTOCOL.md)</h3>
<table>
<tr><th>命令</th><th>说明</th><th>响应</th></tr>
<tr><td><code>ping</code></td><td>保活探测</td><td><code>pong</code></td></tr>
<tr><td><code>res</code></td><td>逻辑分辨率与 raw 轴范围</td><td><code>res &lt;W&gt; &lt;H&gt; raw &lt;xmin..ymax&gt;</code></td></tr>
<tr><td><code>reset</code></td><td>全部虚拟触点复位</td><td><code>ok</code></td></tr>
<tr><td><code>region clear / list / add</code></td><td>区域表管理(同 id add = 原地更新, ≤32)</td><td><code>ok &lt;n&gt;</code> / <code>err region</code></td></tr>
<tr><td><code>sub</code> / <code>unsub</code></td><td>订阅/退订 pev 物理触摸流</td><td><code>ok</code></td></tr>
<tr><td><code>down|move|up &lt;slot&gt; &lt;x&gt; &lt;y&gt;</code></td><td>虚拟触点注入(逻辑坐标)</td><td><code>ok</code> / <code>err point</code></td></tr>
<tr><td><code>begin_frame / point / end_frame</code></td><td>原子多指帧 → 单 SYN 提交</td><td><code>ok</code> / <code>err frame</code></td></tr>
</table>

<h3>关键设计决策与已知陷阱</h3>
<table>
<tr><th>主题</th><th>结论 (源码证据)</th></tr>
<tr><td>单进程合并</td><td>vtouchmerge(EVIOCGRAB+uinput) 与 vtouchws 合并为一个 poll 循环 — 无 UDS 跳转、无半死状态。</td></tr>
<tr><td>失败语义</td><td>坏 WS 客户端只关连接(owner_reset 复位虚拟触点), 物理 grab 不丢; 只有整体崩溃才丢触摸, bootWatch 重连秒级恢复。</td></tr>
<tr><td>1:1 透传 + 纯监听</td><td>物理触摸永远原样注入; region_match 在 SYN 注入后执行(ps_down 滞后一帧判定新按下/刚抬起), 只推事件不拦截不代点; onUp = 抬起且按下期间命中过且抬起点仍命中。</td></tr>
<tr><td>回触不自激</td><td>daemon 只对物理槽做区域匹配 → 虚拟注入不产生 region_ev。</td></tr>
<tr><td>坐标系</td><td>daemon 恒竖屏物理坐标(wm size 归一化 W&lt;H); 客户端 C2P/P2C 旋转, R1 真机验证、R3 待验证。</td></tr>
<tr><td>线程纪律</td><td>主线程 sleep 卡 WS 回调 → 阻塞动作(swipe 插值/回触)必须 threads.start; 读线程只负责 recv/dispatch。</td></tr>
<tr><td>UI 单进程整合 (定案)</td><td>vtouchd 编译为 C 库嵌入 app_process → JNI → libtestimgui.so(EGL GLES2 + Dear ImGui)→ SurfaceControl; 面板与核心同进程内存直连。</td></tr>
<tr><td>平台陷阱</td><td>/sdcard noexec(必须 cp 到 /data/local/tmp)· SELinux setenforce 0 · su 走 v8 包装器 · AVD 需 ABS_X/Y Type-A 兜底。</td></tr>
</table>

<footer><div><span class="pill">drawio-skill</span><span class="pill">seqlayout.py 生成</span><span class="pill">validate.py: 0 errors</span><span class="pill">PNG/SVG 视觉复核通过</span></div>
<div>生成于项目根目录分析 · 行号锚点对应当前 master (6d9270e)</div></footer>
</div></body></html>"""

out = FLOW / "index.html"
out.write_text(html, encoding="utf-8", newline="\n")
print("wrote", out, len(html), "bytes")
