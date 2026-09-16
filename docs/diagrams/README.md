# 调用图图鉴（vtouchd · 函数 × 调用关系 × 每个函数的作用）

这一套图回答一个问题：**这个 daemon 里 62 个函数谁调谁、各自干什么。**
节点 = 函数（副标题就是它的作用，取自代码里的函数文档）；箭头 = 调用方向；带（band）= 按调用关系自动分出的**调用深度层**。

| 图 | 讲什么 | 节点 | 调用边 | 校验 |
|---|---|---|---|---|
| `vtouch-callgraph-phys` | 物理触摸链：物理帧 → 合帧 → 转发 → 入队 → 区域判定 → 出站 | 31 | 22 | 五道 + 版面三项全过 |
| `vtouch-callgraph-proto` | WebSocket 协议链：握手 → SHA-1/Base64 → 发送 → 解帧 | 29 | 25 | 五道 + 版面三项全过 |
| `vtouch-callgraph-ctrl` | 控制链：进程 / 主循环 / 注入 | 14 | 8 | 五道 + 版面三项全过 |
| `vtouch-callgraph-interactive.html` | **全量交互版**：62 个函数 / 78 条调用边 | 62 | 78 | JS 语法 + 结构自检 |
| `vtouch-callgraph.drawio` | **可编辑版（draw.io）**：同样 62 函数 / 78 边，Graphviz 自动布局 | 62 | 78 | `validate.py`：0 error · 0 穿节点 · 0 重叠（95 处交叉，score 950） |

**三张静态图并集 = 全部 62 个函数、54/78 条调用边（69%）；剩下 24 条跨模块边只在交互版 HTML 里**（见文末补遗表）。
不是没画，是这套渲染器画不了：那 24 条边全是「主循环 / 命令族 → 各模块入口」的**纯扇出**，实测 7 源 → 17 目标时 `BRIDGE_BUDGET` 157~179（standard 档上限 8，本图档位放到 60 也过不去），窄走廊、宽走廊两版都试过。单张图放不下的另两个硬约束见下面"取舍"。

## draw.io 可编辑版（draw.io / diagrams.net 打开即改）

三件产物，同一份数据（`scripts/gen_callgraph_drawio.py` 用的提取器与静态图完全同一个）：

- `vtouch-callgraph.drawio` —— **源文件**，在 draw.io 桌面版/网页版里可直接拖动重排、改色、加注释
- `vtouch-callgraph.drawio.png` —— 导出图（**内嵌了 XML**，用 draw.io 打开这张 PNG 也能进编辑态）
- `vtouch-callgraph.drawio.svg` —— 矢量导出（8300px 宽的图用矢量最实用，放大不糊）

重画：

```sh
sh scripts/render_callgraph_drawio.sh   # 数据 → Graphviz 自动布局 → .drawio → 结构校验 → 页面贴合 → PNG + SVG
```

依赖两个外部程序（本机已装）：Graphviz `dot`（`winget install --id Graphviz.Graphviz -e`）、draw.io 桌面版 CLI
（`C:\Program Files\draw.io\draw.io.exe`，用 `-x` 导出）。布局与校验脚本来自 `drawio-skill`，**不复制进本仓库**。

两个坑写在这里省下次时间：

- **draw.io CLI 按页面裁剪导出**：autolayout 产出的页面是 A4（850×1100），而这张图内容有 8294×1536，不贴合的话
  PNG 只导出左上角一条（实测 2000×374）。所以流程里固定有一步 `scripts/fit_drawio_page.py` 把页面贴到内容外框。
- **缩小的图不要给视觉模型看**：把 8300px 宽的图缩到 1400px 后，模型会**编造**函数名（实测读成 `s3fs_*` 之类完全不存在的名字）。
  复核文字要按 **1:1 原像素裁剪**再看（裁 1400×500 一块，字是可读的）；版面缺陷以 `validate.py` 的结构结论为准。

## 每张图四件套

- `<图名>.json` —— **权威源**（改图只改它，再重渲；不要在 SVG 里挪像素）
- `<图名>.svg` / `<图名>.png` —— 产物（PNG 为 2× 像素）
- `<图名>.report.json` —— 五道校验的机读结果（composition 分数 + metrics）

## 重渲

```sh
sh scripts/render_callgraphs.sh          # 四张全套：生成 → 消解冲突 → 五道校验 → 版面自检三项 → 2× PNG
python scripts/gen_callgraph.py phys     # 只生成某一张的 JSON
python scripts/gen_callgraph_html.py     # 重生成全量交互版 HTML
```

数据一切现取：函数定义行与调用边从 `src/*.c` 里解析，作用文案取 `scripts/funcdoc_data.py` 的 `@brief`（与函数文档同一份源，不手抄）。

## 交付前自检清单

1. `scripts/render_check_export.py <json>`：五道校验全 ok（xml / markers / collisions / geometry / composition）+ 2× PNG。
2. `scripts/verify_svg_layout.py <json> <svg> <png>`：① 节点两两重叠 ② 节点文字实测宽度是否超盒宽 ③ PNG 里文字到边框的像素间距。
3. `vision_analyze` 读回 PNG（整图看遮挡/裁切；密集区按原图像素裁剪放大）：**机器全绿 ≠ 视觉干净**。
4. 覆盖统计：四张图的节点并集 = 62、边并集 = 78（本页那张表的最后一列就是它）。

## 取舍与机制（踩过的坑，改图前先看）

- **`route_points` 手工路点必挂**：渲染器会给显式路点报 `unresolved collinear overlap`（哪怕路点在容器外、单条边单独渲染也挂）。所以路点/端口一律交给路由器；只有它自己解不开的两条边抢走廊时，才用 `scripts/fix_diagram_ports.py` 给那条边加 `source_port/target_port`（实测 `right/left` 最常解），并把结果写回 JSON。
- **同带内互调不画箭头**：同一条带里从同一节点出发的多条边，端口挂不下 → 必报共线重叠（实测最小失败集 `sha1_block→rol32` + `sha1_block→be32`）。分层是按调用关系做的，正常不会出现同带边。
- **composition 档位放宽了两项**：调用图的交叉数与折弯数由拓扑决定，`standard` 档（交叉 ≤8、总折弯 ≤100）装不下；JSON 里只放宽 `max_bridged_crossings` / `max_total_bends` / `max_route_stretch`，其余仍按 standard。
- **容器标题是路由障碍物**，带标签保持短（层号 + 模块短名）。
- **扇出（一源多目标）是另一类硬伤**：7 源 → 17 目标的纯扇出图，无论走廊宽窄，`BRIDGE_BUDGET` 都在 157~179，过不了 composition——这类关系用交互版 HTML 看（它按模块分列、边只在悬停/聚焦时点亮，没有走廊预算问题）。
- **大图不要塞进一张**：密度一高，端口容量与共线冲突会连锁出现，越修越乱——按子系统拆图是这套渲染器的正确用法。

## 补遗：只在交互版里的 24 条跨模块调用

| 调用 | 调用者的作用 |
|---|---|
| `cmd_frame` → `emit_frame` | begin_frame / point / end_frame —— 帧内多点… |
| `cmd_frame` → `logical_to_raw` | begin_frame / point / end_frame —— 帧内多点… |
| `cmd_frame` → `parse_long` | begin_frame / point / end_frame —— 帧内多点… |
| `cmd_frame` → `set_virtual` | begin_frame / point / end_frame —— 帧内多点… |
| `cmd_meta` → `owner_reset` | ping / res / reset（不碰触点的元命令） |
| `cmd_point_once` → `emit_frame` | up / down / move —— 单点命令，每个命令提交一帧 |
| `cmd_point_once` → `logical_to_raw` | up / down / move —— 单点命令，每个命令提交一帧 |
| `cmd_point_once` → `parse_long` | up / down / move —— 单点命令，每个命令提交一帧 |
| `cmd_point_once` → `set_virtual` | up / down / move —— 单点命令，每个命令提交一帧 |
| `cmd_region` → `parse_long` | region add | clear | list |
| `drop_client` → `outq_reset` | 关连接 + 抬掉它的虚拟触点 + 清订阅位 + 清出站队列 |
| `drop_client` → `owner_reset` | 关连接 + 抬掉它的虚拟触点 + 清订阅位 + 清出站队列 |
| `vtouch_init` → `discover` | 尺寸门 → 清表 → 认设备 → 建 uinput → 先起监听 → 最后 EV… |
| `vtouch_init` → `make_listen` | 尺寸门 → 清表 → 认设备 → 建 uinput → 先起监听 → 最后 EV… |
| `vtouch_init` → `setup_uinput` | 尺寸门 → 清表 → 认设备 → 建 uinput → 先起监听 → 最后 EV… |
| `vtouch_poll_step` → `client_frame` | poll 四路 fd（物理 / 监听 / 客户端 / 出站）→ 各自处理 → 唯… |
| `vtouch_poll_step` → `drop_client` | poll 四路 fd（物理 / 监听 / 客户端 / 出站）→ 各自处理 → 唯… |
| `vtouch_poll_step` → `outq_flush` | poll 四路 fd（物理 / 监听 / 客户端 / 出站）→ 各自处理 → 唯… |
| `vtouch_poll_step` → `outq_pending` | poll 四路 fd（物理 / 监听 / 客户端 / 出站）→ 各自处理 → 唯… |
| `vtouch_poll_step` → `outq_reset` | poll 四路 fd（物理 / 监听 / 客户端 / 出站）→ 各自处理 → 唯… |
| `vtouch_poll_step` → `physical_events` | poll 四路 fd（物理 / 监听 / 客户端 / 出站）→ 各自处理 → 唯… |
| `vtouch_poll_step` → `websocket_handshake` | poll 四路 fd（物理 / 监听 / 客户端 / 出站）→ 各自处理 → 唯… |
| `vtouch_poll_step` → `ws_has_pending` | poll 四路 fd（物理 / 监听 / 客户端 / 出站）→ 各自处理 → 唯… |
| `vtouch_poll_step` → `ws_input_reset` | poll 四路 fd（物理 / 监听 / 客户端 / 出站）→ 各自处理 → 唯… |
