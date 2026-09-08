// VTouch Sync Helper — offscreen document (常驻, 不受 SW 休眠影响)
// 职责: 仅持有 WebSocket 连 Python。收到任务转发给 SW 执行 (runtime.sendMessage
// 事件会唤醒 SW 且事件处理期间 SW 不休眠), 结果经 WS 回传 Python。

const WS_URL = "ws://10.164.120.30:9336";
let ws = null;
let reconnectTimer = null;

function connect() {
  try { ws = new WebSocket(WS_URL); } catch (e) { scheduleReconnect(); return; }
  ws.onopen = () => { console.log("[vtouch-sync] ws connected"); };
  ws.onmessage = async (ev) => {
    let msg;
    try { msg = JSON.parse(ev.data); } catch (e) { return; }
    console.log("[vtouch-sync] recv:", JSON.stringify(msg).slice(0, 60));
    try {
      const resp = await chrome.runtime.sendMessage(msg);
      console.log("[vtouch-sync] sw resp:", JSON.stringify(resp || {}).slice(0, 60));
      try { ws.send(JSON.stringify({ id: msg.id, ...(resp || { ok: false, error: "SW 无响应" }) })); } catch (e) {}
    } catch (err) {
      console.error("[vtouch-sync] 转发异常:", err);
      try { ws.send(JSON.stringify({ id: msg.id, ok: false, error: String(err && err.message || err) })); } catch (e) {}
    }
  };
  ws.onclose = () => { ws = null; scheduleReconnect(); };
  ws.onerror = () => { try { ws.close(); } catch (e) {} };
}

function scheduleReconnect() {
  if (reconnectTimer) clearTimeout(reconnectTimer);
  reconnectTimer = setTimeout(connect, 2000);
}

connect();
