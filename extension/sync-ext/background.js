// VTouch Sync Helper — background service worker
// 职责: ① 创建并保持 offscreen 文档 (常驻 WS 通道);
//      ② 执行 GitHub 编辑提交任务 (offscreen 经 runtime 消息转发,
//         onMessage 事件处理期间 SW 不休眠 -> 长任务安全)。

async function ensureOffscreen() {
  try {
    const existing = await chrome.runtime.getContexts({ contextTypes: ['OFFSCREEN_DOCUMENT'] });
    if (existing && existing.length > 0) return;
  } catch (e) {}
  try {
    await chrome.offscreen.createDocument({
      url: "offscreen.html",
      reasons: ["WORKERS"],
      justification: "持有 WebSocket 连接以接收 Python 同步指令 (代理隧道绕行)",
    });
    console.log("[vtouch-sync] offscreen created");
  } catch (e) {
    console.warn("[vtouch-sync] offscreen create:", e);
  }
}

ensureOffscreen();
chrome.runtime.onStartup.addListener(ensureOffscreen);
chrome.runtime.onInstalled.addListener(ensureOffscreen);

chrome.alarms.create("vtouch-sync-keepalive", { periodInMinutes: 0.5 });
chrome.alarms.onAlarm.addListener((a) => {
  if (a.name === "vtouch-sync-keepalive") ensureOffscreen();
});

// ---------- 任务执行 (offscreen 转发来的) ----------
chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  handleMessage(msg || {})
    .then(sendResponse)
    .catch((err) => sendResponse({ ok: false, error: String(err && err.message || err) }));
  return true;  // 异步响应: 事件处理期间 SW 不休眠
});

async function handleMessage(msg) {
  if (msg.action === "ping") return { ok: true, pong: true };
  if (msg.action === "check_login") return { ok: true, ...(await checkLogin()) };
  if (msg.action === "commit") return await doCommit(msg);
  return { ok: false, error: "unknown action: " + msg.action };
}

async function checkLogin() {
  const tab = await chrome.tabs.create({ url: "https://github.com/", active: true });
  try {
    // 固定等待: 页面经代理加载约 1-3s, 手动测试证明可查。waitFor 轮询在 SW 中不可靠。
    await sleep(4000);
    const r = await evalInTab(tab.id, () =>
      document.querySelector('meta[name="user-login"]')?.content || '');
    if (r) return { user: r };
    // 第一次可能恰好赶上导航, 再等一轮
    await sleep(3000);
    const r2 = await evalInTab(tab.id, () =>
      document.querySelector('meta[name="user-login"]')?.content || '');
    return { user: r2 };
  } finally {
    chrome.tabs.remove(tab.id);
  }
}

async function doCommit(msg) {
  const base = `https://github.com/${msg.repo || "CM6558/vtouch-project"}`;
  const branch = msg.branch || "master";
  // 先试编辑页; 远端不存在时 GitHub 显示 404, 自动切新建页 (不依赖 is_new 猜测)
  const url = `${base}/edit/${branch}/${msg.path}`;
  const tab = await chrome.tabs.create({ url, active: false });
  let detectedNew = false;
  try {
    const mode = await waitFor(tab.id, 40000, () => {
      if (document.querySelector('.cm-content')) return 'edit';
      // GitHub 404 页的 h1 精确为 "Page not found" (避免 "Not Found" 泛匹配误判)
      const h1 = document.querySelector('h1');
      if (h1 && h1.textContent.trim() === 'Page not found') return '404';
      return 'wait';
    });
    if (mode === '404') {
      detectedNew = true;
      const newUrl = `${base}/new/${branch}?filename=${encodeURIComponent(msg.path)}`;
      await chrome.tabs.update(tab.id, { url: newUrl, active: false });
      await waitFor(tab.id, 40000, () => !!document.querySelector('.cm-content'));
    } else if (mode !== 'edit') {
      if (!(await waitFor(tab.id, 40000, () => !!document.querySelector('.cm-content')))) {
        return { ok: false, error: "编辑器加载超时" };
      }
    }
    // 注入: CRLF 会让 CM6 按 2 行处理 (\r 和 \n 都是行分隔) -> 先转 LF
    const normContent = String(msg.content || '').replace(/\r\n/g, '\n').replace(/\r/g, '\n');
    const inj = await evalInTab(tab.id, (content) => {
      const ce = document.querySelector('.cm-content');
      if (!ce) return 'no-editor';
      // 1) 从 React fiber 找 CM6 EditorView
      let view = null;
      const rootEl = document.querySelector('.cm-editor') || ce;
      const fk = Object.keys(rootEl).find(k => k.startsWith('__reactFiber'));
      if (fk) {
        let fiber = rootEl[fk];
        for (let i = 0; i < 40 && fiber; i++) {
          const p = fiber.memoizedProps;
          const s = fiber.memoizedState;
          if (p) {
            if (p.view && p.view.state && p.view.dispatch) { view = p.view; break; }
            if (p.editorView && p.editorView.state) { view = p.editorView; break; }
          }
          if (s) {
            if (s.view && s.view.state && s.view.dispatch) { view = s.view; break; }
            if (s.editorView && s.editorView.state) { view = s.editorView; break; }
          }
          fiber = fiber.return;
        }
      }
      if (view) {
        view.dispatch({ changes: { from: 0, to: view.state.doc.length, insert: content } });
        return 'view:' + view.state.doc.length;
      }
      // 2) 回退: execCommand (CM6 会 auto-indent, 空行膨胀但内容正确)
      ce.focus();
      const r = document.createRange(); r.selectNodeContents(ce);
      const sel = window.getSelection(); sel.removeAllRanges(); sel.addRange(r);
      document.execCommand('insertText', false, content);
      return 'exec:' + ce.innerText.length;
    }, [normContent]);
    if (inj === 'no-editor') return { ok: false, error: "编辑器未找到" };
    console.log("[vtouch-sync] inj:", inj, "expect≈", String(msg.content || '').length);
    await sleep(1500);

    // 注入后 CM6 处理 + 按钮启用: 大文件 15KB 约 2-5s, 内容未变则永不启用 (等 15s 即跳过)
    const clicked = await poll(tab.id, 15000, () => {
      const b = Array.from(document.querySelectorAll('button'))
        .find(x => x.textContent.trim() === 'Commit changes...');
      if (b && !b.disabled) { b.click(); return 'clicked'; }
      return 'wait';
    });
    if (clicked !== 'clicked') return { ok: false, error: "Commit 按钮未就绪 (内容可能无变化)" };
    await sleep(600);

    const commitMsg = (detectedNew ? "feat: " : "sync: ") + msg.path;
    const done = await poll(tab.id, 25000, (cm) => {
      const d = document.querySelector('[role="dialog"]');
      const inp = d && d.querySelector('input[placeholder^="Update"], input[placeholder^="Create"]');
      if (!inp) return 'wait';
      const setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set;
      setter.call(inp, cm);
      inp.dispatchEvent(new Event('input', { bubbles: true }));
      const confirm = Array.from(d.querySelectorAll('button'))
        .find(x => x.textContent.trim() === 'Commit changes');
      if (confirm) { confirm.click(); return 'committed'; }
      return 'no-confirm';
    }, [commitMsg]);
    if (done === 'no-confirm') return { ok: false, error: "提交对话框无确认按钮" };
    if (done !== 'committed') return { ok: false, error: "提交对话框超时" };

    // 5) 验证跳转 (代理导航慢, 最多等 20s)
    const okHref = await poll(tab.id, 20000, () => {
      const href = location.href;
      if (href && (href.includes('/blob/') || href.includes('/tree/'))) return href;
      return 'wait';
    });
    if (okHref !== 'wait') {
      return { ok: true, href: okHref };
    }
    const href = await evalInTab(tab.id, () => location.href);
    return { ok: false, error: "提交后未跳转: " + String(href).slice(0, 100) };
  } finally {
    chrome.tabs.remove(tab.id);
  }
}

// ---------- 工具 ----------
function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

async function evalInTab(tabId, func, args = []) {
  // 必须 MAIN world: CM6 编辑器只响应主世界的 execCommand/DOM 操作,
  // ISOLATED world 的注入不触发编辑器变更 (按钮永不启用)。
  const results = await chrome.scripting.executeScript({
    target: { tabId }, func, args,
    world: "MAIN",
  });
  return results && results[0] && results[0].result;
}

async function waitFor(tabId, timeoutMs, cond) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      if (await evalInTab(tabId, cond)) return true;
    } catch (e) {}
    await sleep(500);
  }
  return false;
}

async function poll(tabId, timeoutMs, func, args = []) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      const r = await evalInTab(tabId, func, args);
      if (r !== 'wait') return r;
    } catch (e) {}
    await sleep(400);
  }
  return 'wait';
}
