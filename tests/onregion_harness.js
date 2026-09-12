/* tests/onregion_harness.js — 主机侧（Node）跑 bundle 的 vt.onRegion 全路径。
 *
 * 目的：没有手机也能把 onRegion 的真实代码路径跑通——
 *   握手 → sub → 读线程取帧 → rgParseEv 过滤 → 回调分发 → 保活 / 收尾 / 句柄 stop。
 * 做法：把 bundle 依赖的 AutoJs6/Java 接口桩掉（java.net.Socket 换成同进程「假 daemon」，
 *   同步 socket 语义 + 累积字节流解析真 WS 帧），事件用服务端方向文本帧（不掩码）推给客户端。
 * 读线程模型：threads.start 只入队；drain() 驱动一轮（读线程取空后 sleep() 让出，本轮不再跑它），
 *   于是每个小节能「推事件 → drain() → 断言」。
 *
 * 运行： node tests/onregion_harness.js      （退出码 0 = 全部通过）
 */
"use strict";
const crypto = require("crypto");
const path = require("path");

const CRLF = String.fromCharCode(13) + String.fromCharCode(10);

/* 可控时钟：CLOCK_STEP > 0 时每次 Date.now() 前进这么多毫秒（用来在同步驱动里跨过 2s/6s 门限） */
let CLOCK = 1757000000000, CLOCK_STEP = 1;   /* 默认每次取时间 +1ms：探针的 600ms 窗口才会收敛 */
Date.now = () => { CLOCK += CLOCK_STEP; return CLOCK; };

/* ---------- 断言 ---------- */
let fails = 0, checks = 0;
function ok(cond, label, extra) {
  checks++;
  if (cond) console.log("  PASS  " + label);
  else { fails++; console.log("  FAIL  " + label + (extra === undefined ? "" : "  → " + JSON.stringify(extra))); }
}
function section(t) { console.log("\n== " + t); }

/* ---------- 假 daemon：同步 socket + 累积流解析 ---------- */
class FakeDaemon {
  constructor() { this.reset(); }
  reset() {
    this.handshake = 0; this.texts = []; this.closed = 0;
    this.stream = Buffer.alloc(0); this.upgraded = false; this.client = null; this.failWrites = false;
    this.regions = [];        /* 面板区域表：[{id, enabled}]，答 region list 用 */
    this.answerRegionList = true;
  }
  pushText(text) {
    const p = Buffer.from(text, "utf8");
    const head = p.length < 126 ? Buffer.from([0x81, p.length])
      : Buffer.from([0x81, 126, p.length >> 8, p.length & 255]);
    this.client.queue(Buffer.concat([head, p]));
  }
  pushClose() { this.client.queue(Buffer.from([0x88, 0x00])); }
  /* 客户端方向：先握手请求，后 WS 帧（帧被拆成多次 write，必须按流累积） */
  recvFromClient(bytes) {
    this.stream = Buffer.concat([this.stream, Buffer.from(bytes)]);
    if (!this.upgraded) {                        /* 只在握手阶段解析 HTTP 头 */
      const i = this.stream.indexOf(CRLF + CRLF);
      if (i < 0) return;                         /* 握手头还没收全 */
      const head = this.stream.slice(0, i).toString("latin1");
      if (!head.startsWith("GET /")) return;
      this.stream = this.stream.slice(i + 4);
      this.handshake++;
      const m = /Sec-WebSocket-Key: ([^\r\n]+)/.exec(head);
      const accept = crypto.createHash("sha1")
        .update(m[1] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest("base64");
      this.upgraded = true;
      this.client.pushString("HTTP/1.1 101 Switching Protocols" + CRLF + "Upgrade: websocket" + CRLF
        + "Connection: Upgrade" + CRLF + "Sec-WebSocket-Accept: " + accept + CRLF + CRLF);
    }
    for (;;) {                                   /* 帧循环：够一帧才消费 */
      if (this.stream.length < 2) return;
      const opcode = this.stream[0] & 15, masked = (this.stream[1] & 128) !== 0;
      let len = this.stream[1] & 127, off = 2;
      if (len === 126) { if (this.stream.length < 4) return; len = this.stream.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (this.stream.length < 10) return; len = Number(this.stream.readBigUInt64BE(2)); off = 10; }
      const total = off + (masked ? 4 : 0) + len;
      if (this.stream.length < total) return;
      const payload = Buffer.from(this.stream.slice(off + (masked ? 4 : 0), total));
      if (masked) { const mk = this.stream.slice(off, off + 4); for (let i = 0; i < payload.length; i++) payload[i] ^= mk[i & 3]; }
      this.stream = this.stream.slice(total);
      if (opcode === 8) { this.closed++; this.client.serverClosed = true; }
      else {
        const text = payload.toString("utf8");
        this.texts.push(text);
        if (text === "region list" && this.answerRegionList) {
          const lines = this.regions.map((r) => "region " + r.id + " 0 0 0 10 10 " + (r.enabled === false ? 0 : 1));
          this.pushText(lines.length ? lines.join("\n") + "\nend " + this.regions.length : "ok 0");
        }
      }
    }
  }
}
const DAEMON = new FakeDaemon();

class FakeInputStream {
  constructor() { this.q = []; }
  available() { return this.q.length; }
  read(a, off, len) {                       /* 同时支持 ins.read() 与 ins.read(buf, off, len) */
    if (arguments.length === 0) return this.q.length ? this.q.shift() : -1;
    if (!this.q.length) return -1;
    let i = 0;
    while (i < len && this.q.length) a[off + i++] = this.q.shift();
    return i;
  }
}
class FakeSocket {
  constructor() { this.in = new FakeInputStream(); this.closed = false; }
  connect(addr) { this.addr = addr; DAEMON.client = this; DAEMON.upgraded = false; DAEMON.stream = Buffer.alloc(0); }
  setSoTimeout() {}
  getInputStream() { return this.in; }
  getOutputStream() {
    return {
      write: (b) => { if (DAEMON.failWrites) throw new Error("broken pipe"); DAEMON.recvFromClient(b); },
      flush() {}
    };
  }
  close() { this.closed = true; }
  queue(buf) { for (const b of buf) this.in.q.push(b); }
  pushString(s) { this.queue(Buffer.from(s, "latin1")); }
}

/* ---------- AutoJs6 / Java 接口桩 ---------- */
let UI_PID = "4242";
const RUNTIME = { shell: [], toasts: [], logs: [], onNames: [], intervals: 0 };
const BYTE = "byte";
const stubArray = (type, n) => (type === BYTE ? new Uint8Array(n) : new Array(n).fill(0));

global.java = {
  net: { Socket: FakeSocket, InetSocketAddress: class { constructor(h, p) { this.h = h; this.p = p; } } },
  lang: {
    String: function (s) { this.s = String(s); },
    StringBuilder: class {
      constructor() { this.parts = []; }
      append(s) { this.parts.push(String(s)); return this; }
      toString() { return this.parts.join(""); }
    },
    reflect: { Array: { newInstance: stubArray } },
    Byte: { TYPE: BYTE }
  },
  util: { Random: class { nextBytes(b) { for (let i = 0; i < b.length; i++) b[i] = i; } } },
  security: {
    /* 真 Java 的 MessageDigest.digest(byte[]) 返回 byte[]；Node 的 hash.digest(arg) 把参数当编码，
     * 传 Buffer 会静默算成「空输入摘要」，所以这里包一层。 */
    MessageDigest: { getInstance: () => ({ digest: (bytes) => crypto.createHash("sha1").update(Buffer.from(bytes)).digest() }) }
  }
};
global.java.lang.String.prototype.getBytes = function () { return Buffer.from(this.s, "utf8"); };
global.android = {
  util: { Base64: {
    DEFAULT: 0, NO_WRAP: 2,
    encodeToString: (b) => Buffer.from(b).toString("base64"),
    decode: (s) => new Uint8Array(Buffer.from(s, "base64"))
  } }
};

/* 线程/时间模型：threads.start 入队；sleep() 让出（读线程与回调交替推进） */
const pending = [];
const YIELD = { harnessYield: true };
global.threads = {
  start: (fn) => { if (String(fn).indexOf("conn.recv()") >= 0) fn.__reader = true; pending.push(fn); },
  lock: () => ({ lock() {}, unlock() {} }),
  shutDown: () => {}
};
let IN_DRAIN = false;
/* 读线程里 sleep 必须「让出」（否则同步驱动会死循环）；主线程上下文里 sleep 是阻塞等待，
   桩里当无事发生（收尾路径 VTouchKillAll 会 sleep(150) 再复查）。 */
global.sleep = () => { if (IN_DRAIN) throw YIELD; };
const REAL_SET_INTERVAL = setInterval, REAL_CLEAR_INTERVAL = clearInterval;
global.setInterval = (fn, ms) => { RUNTIME.intervals++; const h = REAL_SET_INTERVAL(fn, ms); if (h.unref) h.unref(); return h; };
global.clearInterval = (h) => REAL_CLEAR_INTERVAL(h);
global.shell = (cmd) => {
  RUNTIME.shell.push(cmd);
  if (cmd.indexOf("kill -9") === 0 && cmd.indexOf(UI_PID) >= 0) { UI_PID = ""; return { code: 0, result: "", error: null }; }
  if (cmd.indexOf("pidof vtouch-ui") === 0) return { code: 0, result: UI_PID, error: null };
  if (cmd.indexOf("app_process") >= 0) { UI_PID = "4242"; return { code: 0, result: "", error: null }; }   /* 拉起面板 */
  return { code: 0, result: "", error: null };
};
global.toastLog = (m) => RUNTIME.toasts.push(String(m));
global.log = (m) => RUNTIME.logs.push(String(m));
global.toast = (m) => RUNTIME.toasts.push(String(m));
RUNTIME.exits = 0;
global.exit = () => { RUNTIME.exits++; };
global.device = { width: 1440, height: 3168, wakeUpIfNeeded() {}, keepScreenOn() {}, cancelKeepingAwake() {} };
global.context = { getSystemService: () => null, getFilesDir: () => ({ getAbsolutePath: () => "." }) };
global.events = { on: (name, fn) => RUNTIME.onNames.push({ name, fn }), broadcast: () => {}, emit: () => {} };

function drainTimes(n) { for (let i = 0; i < n; i++) drain(); }

/* 驱动挂起任务直到跑不动（上限防死循环）。读线程每迭代一次就 sleep 让出 → 重新入队，
 * 所以一轮 drain 会把它推进很多次；已结束的线程抛 YIELD 之外就消失。 */
function drain(maxRounds = 400) {
  let rounds = 0;
  while (pending.length && rounds++ < maxRounds) {
    const fn = pending.shift();
    IN_DRAIN = true;                            /* 只有在这个上下文里 sleep 才是「让出」 */
    try { fn(); }
    catch (e) {
      if (e !== YIELD) throw e;
      if (fn.__reader) pending.push(fn);        /* 读线程让出：下一轮继续驱动 */
    }
    finally { IN_DRAIN = false; }
  }
}

/* ---------- 加载被测 bundle ---------- */
const BUNDLE = path.join(__dirname, "..", "clients", "vtouch_bundle.js");
const vt = require(BUNDLE);
console.log("bundle: " + BUNDLE);

/* ---------- 用例 ---------- */
section("T1 不指定 ev = 默认集（down/up/enter/exit，**不含 move**）；pev 与其他区域被过滤");
{
  DAEMON.reset();
  const hits = [];
  const h = vt.onRegion("s3", (e) => hits.push(e));      /* 不传 ev */

  ok(DAEMON.handshake === 1, "onRegion 自动完成握手（1 次）", DAEMON.handshake);
  ok(DAEMON.texts.includes("sub region"), "自动订阅区域通道（sub region，不白收 pev）", DAEMON.texts);
  ok(RUNTIME.onNames.some((x) => x.name === "exit"), "自动注册 events.on(\"exit\") 收尾");
  ok(RUNTIME.intervals >= 1, "主线程保活定时器已挂");

  DAEMON.pushText("pev 0 down 720 1584\n");              /* 原始轨迹：不该进回调 */
  DAEMON.pushText("region_ev s3 down 0 720 1584\n");
  DAEMON.pushText("region_ev r9 down 0 10 20\n");        /* 别的区域：不该进回调 */
  DAEMON.pushText("region_ev s3 move 0 740 1610\n");     /* move：默认集里没有，不该进回调 */
  DAEMON.pushText("region_ev s3 enter 0 730 1600\n");
  DAEMON.pushText("region_ev s3 move 0 741 1611\n");
  DAEMON.pushText("region_ev s3 up 0 745 1615\n");
  DAEMON.pushText("region_ev s3 down 1 800 1600\n");     /* 第二根手指 */
  drain();

  ok(hits.map((x) => x.ev).join(",") === "down,enter,up,down", "默认集按序传到（move 被挡）", hits.map((x) => x.ev));
  ok(hits.every((x) => x.ev !== "move"), "默认不监听 move", hits.map((x) => x.ev));
  ok(hits[0] && hits[0].id === "s3" && hits[0].slot === 0 && hits[0].x === 720 && hits[0].y === 1584, "回调负载正确", hits[0]);
  ok(hits[3] && hits[3].slot === 1 && hits[3].x === 800, "第二根手指 slot/x 正确", hits[3]);
  h.stop();                                              /* 最后一个注册 → 停监听 */
}

section("T2 指定 ev = 白名单；追加注册共用同一条连接");
{
  DAEMON.reset();
  const downs = [], ups = [];
  const h1 = vt.onRegion("s3", "down", (e) => downs.push(e));
  const after1 = DAEMON.handshake, sub1 = DAEMON.texts.filter((t) => t === "sub").length;
  const h2 = vt.onRegion("s3", "up", (e) => ups.push(e));
  ok(DAEMON.handshake === after1, "第二个注册没有重开连接", { before: after1, now: DAEMON.handshake });
  ok(DAEMON.texts.filter((t) => t === "sub").length === sub1, "sub 只发过一次", DAEMON.texts);

  DAEMON.pushText("region_ev s3 down 0 5 6\n");
  DAEMON.pushText("region_ev s3 move 0 6 7\n");          /* 谁都没要 move：不该进任何回调 */
  DAEMON.pushText("region_ev s3 up 0 7 8\n");
  drain();
  ok(downs.length === 1, "down 回调只吃到 down", downs);
  ok(ups.length === 1 && ups[0].ev === "up" && ups[0].x === 7, "up 回调只吃到 up", ups);

  section("T3 摘掉单个注册：另一个继续工作");
  h2.stop();
  DAEMON.pushText("region_ev s3 down 0 9 9\n");
  drain();
  ok(downs.length === 2 && downs[1].x === 9, "剩下的注册仍在收事件", downs);

  section("T4 最后一个注册摘掉 → 停监听（unsub + 关 socket），不断面板");
  const shellBefore = RUNTIME.shell.length;
  h1.stop();
  ok(DAEMON.texts.includes("unsub"), "停监听时发了 unsub", DAEMON.texts);
  ok(DAEMON.client.closed === true, "连接已关闭");
  ok(!RUNTIME.toasts.some((t) => t.indexOf("事件通道已断开") >= 0), "正常收尾不误报「通道断开」", RUNTIME.toasts);
  ok(RUNTIME.shell.length === shellBefore, "停监听不碰面板（没多跑 shell）", RUNTIME.shell.slice(shellBefore));
}

section("T5 不传 id / id=\"*\" = 所有区域；ev=\"*\" = 全部（含 move）；回调抛错不杀脚本");
{
  DAEMON.reset();
  const all = [], any = [];
  const h = vt.onRegion((e) => { if (e.id === "boom") throw new Error("回调故意抛错"); all.push(e.id); });
  const h2 = vt.onRegion("*", "*", (e) => any.push(e.id + ":" + e.ev));
  DAEMON.pushText("region_ev boom down 0 1 2\n");
  DAEMON.pushText("region_ev r1 down 0 3 4\n");
  DAEMON.pushText("region_ev r2 up 0 5 6\n");
  drain();
  ok(all.length === 2 && all[0] === "r1" && all[1] === "r2", "不传 id 时所有区域都分发（含 up）", all);
  ok(any.join(",") === "boom:down,r1:down,r2:up", "id=\"*\" 且 ev=\"*\" 收全部（含 move，本用例没推 move）", any);
  ok(RUNTIME.toasts.some((t) => t.indexOf("onRegion 回调异常") >= 0), "回调异常被兜住并 toast", RUNTIME.toasts);
  h.stop(); h2.stop();
}

section("T6 vt.stop() 收尾：先停监听再收面板，不误报断开");
{
  DAEMON.reset();
  vt.onRegion("s3", () => {});
  RUNTIME.toasts.length = 0;
  RUNTIME.shell.length = 0;
  vt.stop();
  ok(DAEMON.client ? DAEMON.client.closed === true : true, "socket 已关", DAEMON.client && DAEMON.client.closed);
  ok(!RUNTIME.toasts.some((t) => t.indexOf("事件通道已断开") >= 0), "不误报「通道断开」", RUNTIME.toasts);
  ok(RUNTIME.shell.some((c) => c.indexOf("pidof vtouch-ui") >= 0), "vt.stop() 走了收面板路径", RUNTIME.shell);
}

section("T7 显式要 move：\"move\" 单事件 / [\"down\",\"move\"] 集合 / \"*\" 含 move");
{
  DAEMON.reset();
  UI_PID = "4242";                 /* 面板本来就活着：避免 T6 收掉后走部署路径（桩里没有 java.io） */
  const onlyMove = [], dm = [], star = [];
  const h1 = vt.onRegion("s3", "move", (e) => onlyMove.push(e.ev));
  const h2 = vt.onRegion("s3", ["down", "move"], (e) => dm.push(e.ev));
  const h3 = vt.onRegion("s3", "*", (e) => star.push(e.ev));
  DAEMON.pushText("region_ev s3 down 0 1 2\n");
  DAEMON.pushText("region_ev s3 move 0 3 4\n");
  DAEMON.pushText("region_ev s3 up 0 5 6\n");
  drain();
  ok(onlyMove.join(",") === "move,move" || onlyMove.join(",") === "move", "\"move\" 只收 move", onlyMove);
  ok(dm.join(",") === "down,move", "[\"down\",\"move\"] 收到 down+move", dm);
  ok(star.join(",") === "down,move,up", "\"*\" 收全部（含 move）", star);
  h1.stop(); h2.stop(); h3.stop();
}

section("T12 底层写法（connect+sub，不调 onRegion）也会自动收尾");
{
  DAEMON.reset(); UI_PID = "4242"; RUNTIME.shell.length = 0;
  const c = vt.connect();
  vt.sub(c, "region");
  const onExit = RUNTIME.onNames.filter((x) => x.name === "exit").pop();
  ok(!!onExit, "connect 之后就注册了退出钩子", RUNTIME.onNames.map((x) => x.name));
  onExit.fn();                                   /* 模拟脚本结束 */
  ok(RUNTIME.shell.some((x) => x.indexOf("kill -9") === 0), "退出自动收面板（kill 面板）", RUNTIME.shell);
  ok(UI_PID === "", "面板 pid 已清", UI_PID);
}

section("T13 vt.autoStop(false)：退出只关连接、面板留着");
{
  DAEMON.reset(); UI_PID = "4242"; RUNTIME.shell.length = 0;
  const c = vt.connect();
  vt.sub(c, "region");
  vt.autoStop(false);
  const onExit = RUNTIME.onNames.filter((x) => x.name === "exit").pop();
  onExit.fn();
  ok(!RUNTIME.shell.some((x) => x.indexOf("kill -9") === 0), "不动面板（没 kill）", RUNTIME.shell);
  ok(UI_PID === "4242", "面板仍然活着", UI_PID);
  ok(DAEMON.client && DAEMON.client.closed === true, "连接已关闭", DAEMON.client && DAEMON.client.closed);
  vt.autoStop(true);                              /* 复位 */
}

section("T8 被新实例顶掉 → 让位自退（exit），且不收面板");
{
  DAEMON.reset();
  UI_PID = "4242";
  RUNTIME.toasts.length = 0;
  const h = vt.onRegion("s3", () => {});
  const exitsBefore = RUNTIME.exits;
  DAEMON.pushClose();                       /* 模拟「新客户端连上 → daemon 关掉旧连接」 */
  drain();
  ok(RUNTIME.exits > exitsBefore, "旧实例调了 exit() 自退", { before: exitsBefore, now: RUNTIME.exits });
  ok(RUNTIME.toasts.some((t) => t.indexOf("接管") >= 0), "报了「已被接管」", RUNTIME.toasts);
  const shellBefore = RUNTIME.shell.length;
  const onExit = RUNTIME.onNames.filter((x) => x.name === "exit").pop();
  if (onExit) onExit.fn();                  /* 模拟脚本结束时的 exit 钩子 */
  const kills = RUNTIME.shell.slice(shellBefore).filter((c) => c.indexOf("kill -9") === 0);
  ok(kills.length === 0, "接管后退出不动面板（没 kill 面板）", kills);
  h.stop();
}

section("T9 空闲 2s 自动 ping；ping 收不到 pong → 判断开并让位（不是死等）");
{
  DAEMON.reset();
  UI_PID = "4242";
  RUNTIME.toasts.length = 0;
  const h = vt.onRegion("s3", () => {});
  const exitsBefore = RUNTIME.exits;
  CLOCK_STEP = 700;                       /* 每次取时间前进 0.7s：很快跨过 2s/6s 门限 */
  drain();
  CLOCK_STEP = 1;
  ok(DAEMON.texts.includes("ping"), "空闲后确实发了 ping（探活）", DAEMON.texts);
  ok(RUNTIME.exits > exitsBefore, "收不到 pong → 判断开并 self-exit（不是死等）", { before: exitsBefore, now: RUNTIME.exits });
  ok(!RUNTIME.toasts.some((t) => t.indexOf("面板退出") >= 0), "面板还活着时不误报「面板退出」", RUNTIME.toasts);
  h.stop();
}

section("T10 通道参数：vt.sub(c) 裸订 = 两个都订；\"phys\"/\"region\" 只订一个");
{
  DAEMON.reset();
  UI_PID = "4242";
  const c = vt.connect();
  vt.sub(c);
  ok(DAEMON.texts.includes("sub"), "裸 sub 不带参数（向后兼容）", DAEMON.texts);
  vt.unsub(c);
  vt.sub(c, "phys");
  ok(DAEMON.texts.includes("sub phys"), "sub phys 只订轨迹", DAEMON.texts);
  vt.unsub(c);
  vt.sub(c, "region");
  ok(DAEMON.texts.includes("sub region"), "sub region 只订区域事件", DAEMON.texts);
  c.close();
}

section("T11 B：启动校验区域 id（写错 / 被禁用 / 面板空 / 没答复）");
{
  const T = () => RUNTIME.toasts.join(" | ");
  /* (1) id 不在面板表里 → 报错并列出已有的 */
  DAEMON.reset(); UI_PID = "4242"; RUNTIME.toasts.length = 0;
  DAEMON.regions = [{ id: "r1" }, { id: "c8" }];
  let h = vt.onRegion("nope", () => {});
  ok(/没有区域 "nope"/.test(T()) && /r1, c8/.test(T()), "id 写错 → 报错并列出面板现有 id", RUNTIME.toasts);
  h.stop();

  /* (2) id 存在但 enabled=0 */
  DAEMON.reset(); UI_PID = "4242"; RUNTIME.toasts.length = 0;
  DAEMON.regions = [{ id: "r1" }, { id: "s3", enabled: false }];
  h = vt.onRegion("s3", () => {});
  ok(/已被禁用/.test(T()), "区域被禁用 → 明确提示", RUNTIME.toasts);
  h.stop();

  /* (3) 面板一个区域都没有 */
  DAEMON.reset(); UI_PID = "4242"; RUNTIME.toasts.length = 0;
  DAEMON.regions = [];
  h = vt.onRegion("s3", () => {});
  ok(/还没有区域/.test(T()), "面板空 → 提示先画一个", RUNTIME.toasts);
  h.stop();

  /* (4) id 正确 → 不吵 */
  DAEMON.reset(); UI_PID = "4242"; RUNTIME.toasts.length = 0;
  DAEMON.regions = [{ id: "s3" }];
  h = vt.onRegion("s3", () => {});
  ok(RUNTIME.toasts.filter((x) => x.indexOf("vtouch:") === 0).length === 0, "id 正确 → 一句都不吵", RUNTIME.toasts);
  h.stop();

  /* (5) 面板不答复（探针失败）→ 静默，不误报 */
  DAEMON.reset(); UI_PID = "4242"; RUNTIME.toasts.length = 0;
  DAEMON.answerRegionList = false;
  h = vt.onRegion("s3", () => {});
  ok(RUNTIME.toasts.filter((x) => x.indexOf("面板里") >= 0).length === 0, "探针没答复 → 不误报", RUNTIME.toasts);
  DAEMON.answerRegionList = true;
  h.stop();
}

console.log("\n" + (fails ? "FAILED " + fails + "/" + checks : "ALL PASS " + checks + "/" + checks));
process.exit(fails ? 1 : 0);
