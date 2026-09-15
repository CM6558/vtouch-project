# docs/diagrams —— 工程图索引

## 先看：最小版（现况 = 虚拟触摸 + 合并物理触摸）

仓库现在是**最小版**：`src/vtouchd.c`（1015 行）+ `clients/vtouch.js`。对应的三张图（行号对着最小版源码）：

| 图 | 讲什么 | 源 |
|---|---|---|
| `vtouch-min-flow` | 全流程：启动九步定序 → 每轮 poll 两条输入（物理设备 / WS 客户端）→ 同一个 `emit_frame` 提交 → 收尾 | 草稿生成器 `build/_gen_min_diagrams.py` |
| `vtouch-min-merge-frame` | **合并机制**：物理手指与注入手指在 `SYN_REPORT` 处合流，物理在前虚拟在后、整帧一次 `writev` | 同上 |
| `vtouch-min-protocol` | 协议分支：9 条命令各自走哪条路径、什么应答 | 同上 |
| `vtouch-min-sequence` | **时序图**：四个参与者（物理屏/内核、poll 线程、系统/应用、AutoJS 脚本）+ 竖直虚线生命线 × 12 条消息，看清「物理按下与注入在同一帧序里交错」 | 草稿生成器 `build/_gen_min_sequence.py` |

重渲（四张一起）：`python build/_gen_min_diagrams.py && python build/_gen_min_sequence.py && bash build/_render_min.sh`

## 完整版归档（面板 / 区域那一代，**不是**现况代码）

下面这些图讲的是**完整版**实现（`src-ui/` 面板 + 1784 行 core + 区域系统），源码已删、只在
`build/_backup_full_<时间戳>/` 里有。留着是为了将来要恢复那些功能时能照着看——**别当现况用**。

**权威源是每张图的 `.json`**（渲染器读它出 `.svg`/`.png`/`.report.json` 四件套）。
草稿生成器：`build/_gen_walkthrough_diagrams.py`（数据驱动，可重复运行）；**改图改生成器或 JSON 后重渲，不要手改 SVG**。

这些图的节点副标题里的 `文件:行` 与 `docs/CODE_WALKTHROUGH.md` **同源**（那份文档的行号是逐行现读的）。
图只描述**当前盘上代码**这一代实现。

## 图（9 张，走读全套）

| 图 | 讲什么 | 对应文档 |
|---|---|---|
| `vtouch-map-overview` | 系统地图：四层 / 四个线程 / 三条边界（AutoJs6 进程 · 面板进程 · 内核框架），含坐标三域与线程未命名等注记 | §1、§2 |
| `vtouch-core-data` | core 数据面：启动定序 → 采集解析 → 合帧 → 一次 writev → 区域匹配 → 出站（含写失败重发） | §3.1–3.5、§3.7 |
| `vtouch-core-control` | core 控制面：accept/踢旧 → socket 选项 → 握手 → 解帧 → 命令表 → 回包 → 挂断退出 | §3.6、§3.8、§3.9 |
| `vtouch-failure-exits` | 失败出口与降级：启动码 -2/-3/-4/-5/-6、Java exit(2)/return、脚本 throw、`_exit(0)`、降级与兜底 | §9 |
| `vtouch-panel-java` | 面板侧四层：Java 建层 → native 启动与桥 → 交互与渲染（含重画门）→ 落盘 | §4、§5 |
| `vtouch-js-lib` | L1 脚本库：`build_bundle.py` 六段逐函数（CORE / UI_BOOT / ON_REGION / ONE_LIB / DEMO） | §6 |
| `vtouch-example-29-steps` | 示例贯通：一条 `onRegion` 触发链的 24 个执行步骤（对应 §8 的 29 行明细） | §8 |
| `vtouch-region-system` | 区域系统：表在 core / 存储归面板 / 脚本只读下发，含五事件与边界语义 | §3.5、§10 |
| `vtouch-build-chain` | 交付链：源码 → 构建五阶段 → 装配 → 对账 → 打包 → CI 产物 → 设备侧落盘 | §11.2、§11.5、§11.6 |

另有一张早期单图（同样基于现码，保留）：`vtouch-current-journey`（一次区域触发旅程 · 21 步概览版）。

## 重渲命令（本次实际用的）

```sh
cd /c/Users/21102/vtouch-project
python build/_gen_walkthrough_diagrams.py          # 生成全套 JSON（权威源）
bash   build/_render_all.sh                        # 逐张：渲染 + 五道校验 + 版面自检 + PNG
# 或单张：
python "C:/Users/21102/AppData/Local/hermes/skills/creative/technical-diagram-generation/scripts/render_check_export.py" \
       docs/diagrams/<name>.json
python "C:/Users/21102/AppData/Local/hermes/skills/creative/technical-diagram-generation/scripts/verify_svg_layout.py" \
       docs/diagrams/<name>.json docs/diagrams/<name>.svg docs/diagrams/<name>.png
```

## 交付前自检清单（本次全过）

- [x] 五道机检：`xml` / `markers` / `collisions` / `geometry` / `composition` 全 ok，composition score = 100
- [x] 版面自检三项：节点两两重叠 = 无、文字实测宽度无溢出、PNG 文字无贴边/裁切
- [x] 视觉复核：逐张读回 PNG（读图工具）确认无裁切/遮挡/压线/穿箱
- [x] 图内 `文件:行` 与 `docs/CODE_WALKTHROUGH.md` 一致（同源，改动一起改）
- [x] PNG 用 2× 导出（`--force-device-scale-factor=2`），尺寸 = 画布 ×2

## 与旧图的关系（重要）

HEAD 里曾归档 `vtouch-full-flow` / `vtouch-click-journey` / `vtouch-region-to-task-*`，**它们描述的是另一代实现**
（`recvBlocking`、`LinkedBlockingQueue(256)` + pump 线程、`reserved` 占位、`vtouch_set_consume_cb` 等，
这些在当前源码里 grep 计数均为 0）。工作区里这些文件已被删除；**不要直接恢复它们当现况用**——
要恢复得先按现码改 JSON 再重渲。上一代与本代的差异清单见 `docs/CODE_WALKTHROUGH.md` §0.1 / §10。
