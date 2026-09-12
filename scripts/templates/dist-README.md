# vtouch 触摸合并包（arm64）

这不是一堆源码，**只有一个文件要推手机**：`vtouch_bundle.js`。它自带 arm64 后端二进制和 ImGui 面板，
运行时自己释放到 `/data/local/tmp/` 并起进程。本包另外附带 `example.js`（可直接跑的调用示例）和本说明。

- 包版本：`{{VERSION}}`（内置 bundle md5 `{{BUNDLE_MD5}}`，{{BUNDLE_SIZE}} B）
- 构建时间：{{DATE}}{{COMMIT_LINE}}
- 平台：Android arm64（真机，需 root）

## 1. 前置条件

| 条件 | 说明 |
|---|---|
| Android root | KernelSU / Magisk / 已 root 的 adb |
| AutoJs6 | 已授予 **root 权限**（包里的 `pidof` / `kill -9` / `app_process` 都靠它） |
| 手机存储可写 | 脚本从 `/sdcard/` 加载 |

## 2. 三步跑起来

```sh
# ① 推包（只有一个文件）
adb push vtouch_bundle.js /sdcard/vtouch_bundle.js
adb shell md5sum /sdcard/vtouch_bundle.js     # 回读应等于 {{BUNDLE_MD5}}

# ② 推示例（或直接照 example.js 自己写）
adb push example.js /sdcard/vtouch_example.js

# ③ 在 AutoJs6 里运行 /sdcard/vtouch_example.js
```

首次运行会花几百毫秒释放后端（日志会打印释放大小），随后面板进程起来——**面板就是后端**（抓触摸 + 本地 WebSocket 服务）。

## 3. 你要写的只有一件事

```javascript
var vt = require("/sdcard/vtouch_bundle.js");

vt.onRegion("s3", "down", function (h) {          // 区域 s3 被按下
    toastLog(h.id + " @" + h.x + "," + h.y);
    vt.finger().tap(h.x, h.y);                    // 回注一次点击
});
```

库里自动完成：起面板（= 起后端）→ 连接 → 订阅区域事件 → 校验区域 id → 常驻读线程 → 事件过滤 → 回调丢子线程 →
脚本退出时收尾（收面板、释放触摸独占）→ 主线程保活。**脚本结束不需要写任何清理代码。**

### 参数

| 参数 | 说明 |
|---|---|
| `id` | 区域名（面板卡片上的名字）。**省略**或 `"*"` = 所有区域 |
| `ev` | 事件白名单。**省略 = `down/up/enter/exit`（不含 move）**；可写 `"down"`、`"down,move"`、`["down","move"]`、`"*"`（= 全部，含 move） |
| `fn(h)` | 回调。`h = { id, ev, slot, x, y }`：id 区域名、ev 事件、slot 手指序号、x/y 当前屏幕坐标 |

回调**已经跑在子线程**，里面可以直接 `sleep()` 做长按 / 拖拽（`down → sleep → move → up`）。

### 其它常用 API

| 调用 | 作用 |
|---|---|
| `vt.onRegion(...)` | 唯一推荐入口，返回 `{ stop() }` 可中途停监听（不动面板） |
| `vt.finger()` | 虚拟手指：`.down(x,y)` / `.move(x,y)` / `.up()` / `.tap(x,y)` / `.swipe(x1,y1,x2,y2,ms)` |
| `vt.uiStart()` | 只想起面板（不起监听）时用；已起则直接返回 |
| `vt.autoStop(false)` | 逃生门：脚本退出**只关连接、把面板留着**（默认 `true` = 退出连面板一起收） |
| `vt.stop()` | 立即停后端 + 收面板 + 释放触摸独占 |

## 4. 区域从哪来（不是配置文件）

区域在**面板**里画（首次运行后屏幕上会出现面板/悬浮层）：点「＋矩形 / ＋圆形」框选，卡片上可以改名、开关、删除。
区域表存在设备侧 `/data/local/tmp/vtouch-runtime/regions.conf`，**面板是唯一归属**，脚本不要自己维护一份。
代码里写的 `id` 就是卡片名；写错 / 被禁用 / 面板里还没画，启动时会立刻 toast 提示「面板里没有区域 xxx；现有：…」，
不会静默没反应。

## 5. 退出与清理

| 情况 | 结果 |
|---|---|
| 脚本自己跑完 / 你在 AutoJs6 里停止脚本 | exit 钩子自动收面板、释放触摸独占 ✓ |
| `vt.autoStop(false)` | 面板留着（适合「起面板，业务在别处」） |
| 第二个脚本连上来 | 新实例接管，旧实例自退**且不收面板**（面板归新实例） |
| **强行停止 AutoJs6（强杀）** | ⚠️ 跳过 exit 钩子：面板会留在后台继续抓着触摸，手动清：`adb shell su -c "kill -9 $(pidof vtouch-ui)"` |

## 6. 自检 / 排错

```sh
adb shell su -c "pidof vtouch-ui"                                  # 有 pid = 面板（= 后端）在跑
adb shell su -c "tail -30 /data/local/tmp/vtouch-runtime/vtouch-ui.log"   # 后端日志
adb shell md5sum /sdcard/vtouch_bundle.js                          # 与包内 md5 对照
```

日志里看到 `ws client connected` = 脚本连上了；看到 `region <id> ... en1` = 区域表已载入；
区域命中时是 `vtouchd: ev <id> <ev> slot<n> x,y`。若出现 `(NO-CLIENT/UNSUB)` 说明当前没客户端订阅（或订阅的是另一路）。

## 7. 包内文件

| 文件 | 说明 |
|---|---|
| `vtouch_bundle.js` | **唯一需要推手机的交付物**（内嵌 arm64 后端 + 面板 dex/so + JS 库） |
| `example.js` | 可直接运行的调用示例（区域触发 → 回注点击），带逐段注释 |
| `README.md` | 本说明 |
| `md5.txt` | 各文件 md5，回读对账用 |

更详细的开发向文档（协议、构建、产物构成）在源码仓库 `docs/` 下：`VTOUCH_BUNDLE.md`（手册）、
`VTOUCH_PROTOCOL.md`（WS 线协议）、`ARTIFACTS.md`（产物构成与校验）。
