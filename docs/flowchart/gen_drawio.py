#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""vtouch 示例主线 → 可编辑 .drawio (drawio-skill: seqlayout.py + 阶段帧 + desc 说明注入)."""
import json
import sys
from pathlib import Path
from xml.sax.saxutils import escape

SKILL = Path(r"C:\Users\c30080002\AppData\Local\hermes\skills\drawio-skill\skills\drawio-skill\scripts")
sys.path.insert(0, str(SKILL))
import seqlayout  # noqa: E402

OUT = Path(r"D:\MYP\vtouch-project\docs\flowchart")
SEQ_JSON = OUT / "vtouch-example-seq.json"
DRAWIO = OUT / "vtouch-example.drawio"

SPEC = {
    "title": "vtouch-project 完整实现逻辑 (vtouch_touchback.js 示例)",
    "participants": [
        {"id": "a", "label": "GitHub Actions"},
        {"id": "b", "label": "AutoJs6 bundle"},
        {"id": "c", "label": "vtouchd 核心 (root)"},
        {"id": "d", "label": "/dev/uinput"},
        {"id": "e", "label": "Android 应用"},
    ],
    "messages": [
        {"from": "a", "to": "a", "label": "① Actions: 交叉编译 + bundle 生成",
         "desc": "NDK r27d → vtouchd(arm64+x86_64) → build_bundle.py → artifact"},
        {"from": "a", "to": "b", "label": "② adb push 部署",
         "desc": "vtouch_bundle-arm64.js → /sdcard/vtouch_bundle.js → AutoJs6 打开示例脚本"},
        {"from": "b", "to": "b", "label": "③ 示例加载仪式",
         "desc": "require(bundle) → eval(vt.uiSource) 进主上下文 → bootWatch(handlers)"},
        {"from": "b", "to": "b", "label": "④ 单实例互踢 (vt-takeover)",
         "desc": "events.broadcast 唯一 token; 旧实例 → 关 UI + vt.stop + exit"},
        {"from": "b", "to": "b", "label": "⑤ vt.ensure(): 幂等启动",
         "desc": "pid 存活→直返 · 缺则 install: Base64→su cp→chmod 755→/data/local/tmp"},
        {"from": "b", "to": "b", "label": "⑥ wm size 竖屏归一 → nohup",
         "desc": "W<H 归一 → nohup vtouchd -w W -h H -p 27183 → 写 pid"},
        {"from": "b", "to": "c", "label": "⑦ 二进制自释放启动", "desc": "vtouchd 单进程拉起 (root)"},
        {"from": "c", "to": "c", "label": "⑧ vtouch_init: 发现+初始化",
         "desc": "扫 event0..63 (Type-B) → uinput「vtouch-merged」→ listen 27183 → EVIOCGRAB 最后"},
        {"from": "b", "to": "c", "label": "⑨ WS 握手连接",
         "desc": "GET + Sec-WebSocket-Key → SHA-1 accept 校验 · 300ms 重试至超时"},
        {"from": "b", "to": "b", "label": "⑩ loadRegions + ovShow overlay",
         "desc": "storages「vtouch_regions」→ 区域红描边预览 · 手指蓝点由 pev 驱动"},
        {"from": "b", "to": "c", "label": "⑪ region 配置下发",
         "desc": "region clear → add swipeL 矩形(60,2200,660,2900) → add tapR"},
        {"from": "b", "to": "c", "label": "⑫ sub 订阅触摸流", "desc": "daemon 此后推 pev · 断连/被踢需重 sub"},
        {"from": "b", "to": "b", "label": "⑬ 读线程死循环 (loop)",
         "desc": "recv → dispatch; 无数据 3s ping; region_ev → 五事件 handlers"},
        {"from": "e", "to": "e", "label": "⑭ 用户手指按下 swipeL", "desc": "示例: 屏幕左下方 (500,2500) 按下"},
        {"from": "d", "to": "c", "label": "⑮ 物理触摸事件流 (24B)",
         "desc": "ABS_MT_SLOT=0 · TRACKING_ID → POSITION_X/Y → 迁移 phys[0]"},
        {"from": "c", "to": "c", "label": "⑯ SYN 帧边界三连",
         "desc": "emit_frame 物理透传 → region_match → vtouch_ui_sync"},
        {"from": "c", "to": "d", "label": "⑰ 合并帧注入 uinput",
         "desc": "物理槽 0 原样 + BTN_TOUCH + SYN_REPORT → 单次 writev"},
        {"from": "c", "to": "c", "label": "⑱ region_match 命中判定",
         "desc": "500,2500 ∈ swipeL → slot_hit=1 → 推 down · 纯监听不拦截"},
        {"from": "c", "to": "b", "label": "⑲ region_ev swipeL down",
         "desc": "进程内回调(面板闪烁) → WS 推送 region_ev swipeL down 0 500 2500"},
        {"from": "b", "to": "b", "label": "⑳ onDown 分发 + 触发回触",
         "desc": "dispatch → P2C → onDown → ovFlash 绿闪 → toast → busy(threads.start)"},
        {"from": "b", "to": "b", "label": "㉑ 虚拟手指 swipe 序列",
         "desc": "finger().swipe(720,2400→720,1200,350ms): down→move×N→up · 16.7ms 插值"},
        {"from": "b", "to": "c", "label": "㉒ 虚拟触点注入命令",
         "desc": "down 0 720 2400 / move 0 x y ×N / up 0 (逻辑坐标)"},
        {"from": "c", "to": "c", "label": "㉓ 坐标换算 + 虚拟槽",
         "desc": "logical_to_raw → 槽 phys_slots+0 · tracking id 自增(回绕)"},
        {"from": "c", "to": "d", "label": "㉔ 合并帧发射 (writev)",
         "desc": "物理+虚拟同帧 → 单次 SYN_REPORT → 零碎片"},
        {"from": "d", "to": "e", "label": "㉕ InputReader → 应用响应",
         "desc": "系统识别上滑手势 · 虚拟触摸不产生 region_ev → 回触不自激"},
        {"from": "b", "to": "b", "label": "㉖ pev 持续推送",
         "desc": "物理手指 move/up 状态变化才推 → overlay 手指更新"},
        {"from": "b", "to": "b", "label": "㉗ 5s 兜底轮询",
         "desc": "regions JSON 比对 → 变更即重新下发 · rotSync 旋转自适应"},
        {"from": "b", "to": "b", "label": "㉘ 断线重连闭环 (异常)",
         "desc": "读线程异常 → close → ensure → connect → 重新 sub + pushRegions"},
        {"from": "c", "to": "c", "label": "㉙ 退出清理",
         "desc": "exit → vt.stop(killall)+关UI · vtouchd cleanup: 释放 EVIOCGRAB + 销毁 uinput"},
    ],
}

# 阶段帧 (seqlayout 行高: self +70 / msg +50; 起点 y=130)
PHASES = [
    ("阶段 1 · 构建与部署", 100, 280, "#ea580c"),
    ("阶段 2 · 启动与自释放", 290, 610, "#06b6d4"),
    ("阶段 3 · 连接与配置下发", 620, 900, "#06b6d4"),
    ("阶段 4 · 示例触发: 物理按下 → 合并注入 → 区域事件", 910, 1280, "#059669"),
    ("阶段 5 · 回触注入: 虚拟上滑 → 系统识别", 1270, 1690, "#06b6d4"),
    ("阶段 6 · 保活 / 断线恢复 / 清理", 1700, 1900, "#64748b"),
]

SEQ_JSON.write_text(json.dumps(SPEC, ensure_ascii=False, indent=1), encoding="utf-8")
xml = seqlayout.layout(SPEC)

# 阶段帧: 插到 root 最底层 (先绘制)
frames = []
for i, (label, y0, y1, color) in enumerate(PHASES):
    frames.append(
        '        <mxCell id="phase%d" value="%s" '
        'style="rounded=1;whiteSpace=wrap;html=1;dashed=1;dashPattern=6 4;'
        'fillColor=none;strokeColor=%s;fontColor=%s;fontStyle=1;fontSize=12;'
        'verticalAlign=top;align=left;spacing=10;arcSize=10;" vertex="1" parent="1">\n'
        '          <mxGeometry x="40" y="%d" width="1100" height="%d" as="geometry"/>\n'
        '        </mxCell>' % (i, escape(label), color, color, y0, y1 - y0)
    )
xml = xml.replace('        <mxCell id="1" parent="0"/>\n',
                  '        <mxCell id="1" parent="0"/>\n' + "\n".join(frames) + "\n", 1)

# desc 说明文本 cells (最上层)
y = 130
cx = {pid: 40 + i * 220 + 50 for i, pid in enumerate(["a", "b", "c", "d", "e"])}
descs = []
for m in SPEC["messages"]:
    kind = "self" if m["from"] == m["to"] else "msg"
    d = m.get("desc")
    if d:
        if kind == "self":
            x, yy = cx[m["from"]] + 68, y + 34
        else:
            x, yy = min(cx[m["from"]], cx[m["to"]]) + 14, y + 20
        descs.append(
            '        <mxCell id="d%d" value="%s" '
            'style="text;html=1;fontSize=9;fontColor=#94a3b8;align=left;spacing=0;" '
            'vertex="1" parent="1">\n'
            '          <mxGeometry x="%d" y="%d" width="430" height="26" as="geometry"/>\n'
            '        </mxCell>' % (len(descs), escape(d), x, yy)
        )
    y += 70 if kind == "self" else 50
xml = xml.replace("      </root>\n", "\n".join(descs) + "\n      </root>\n", 1)

xml = xml.replace('pageWidth="850" pageHeight="1100"', 'pageWidth="1200" pageHeight="2000"')
DRAWIO.write_text(xml, encoding="utf-8", newline="\n")
print("wrote", DRAWIO, len(xml), "bytes,", len(SPEC["messages"]), "messages,", len(PHASES), "phase frames")
