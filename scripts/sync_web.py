#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sync_web.py — 生成本地改动同步清单网页（git 只读，不推送）

原理:
  1. git fetch origin           # 只拉取远端状态, 绝不推送
  2. git diff origin/<branch> HEAD + git status   # 找出本地领先的改动
  3. 生成一个 HTML 清单页:
       - 新增/修改文件: "复制内容" 按钮(一键到剪贴板) + GitHub 在线编辑直链
       - 删除文件:     GitHub 删除确认页直链
       - 二进制文件:   GitHub 上传页直链

用法:
  python scripts/sync_web.py [--repo <path>] [--out <path>] [--no-fetch] [--no-open]

依赖: 仅 Python 3 标准库 + 系统 git。
"""
import argparse
import json
import os
import re
import subprocess
import sys
import webbrowser
from pathlib import Path

DEFAULT_OUT_NAME = "sync-report.html"


def detect_system_proxy() -> str | None:
    """读取 Windows 系统代理 (WinHTTP/IE 注册表设置)."""
    try:
        import winreg
        key = winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            r"Software\Microsoft\Windows\CurrentVersion\Internet Settings",
        )
        enabled, _ = winreg.QueryValueEx(key, "ProxyEnable")
        server, _ = winreg.QueryValueEx(key, "ProxyServer")
        winreg.CloseKey(key)
        if enabled and server:
            if not server.startswith(("http://", "https://", "socks://")):
                server = "http://" + server
            return server
    except Exception:
        pass
    return None


def resolve_proxy(cli_proxy: str | None) -> tuple[str | None, str]:
    """按优先级解析代理: --proxy > 环境变量 > Windows 系统代理 > git 全局配置."""
    if cli_proxy:
        return cli_proxy, "--proxy 参数"
    for env in ("HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy"):
        v = os.environ.get(env)
        if v:
            return v, f"环境变量 {env}"
    sp = detect_system_proxy()
    if sp:
        return sp, "Windows 系统代理"
    # git 全局 http.proxy
    r = subprocess.run(["git", "config", "--global", "--get", "http.proxy"],
                       capture_output=True, text=True, encoding="utf-8")
    if r.returncode == 0 and r.stdout.strip():
        return r.stdout.strip(), "git 全局配置"
    return None, "未配置"


def git(repo: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", "-C", str(repo), *args],
        capture_output=True, text=True, encoding="utf-8", errors="replace",
    )


def is_text(data: bytes) -> bool:
    try:
        data.decode("utf-8")
        return True
    except UnicodeDecodeError:
        return False


def numstat(repo: Path, base: str, path: str):
    """返回 (added, deleted) 行数; 失败返回 (0, 0)"""
    r = git(repo, "diff", "--numstat", base, "HEAD", "--", path)
    if r.returncode != 0 or not r.stdout.strip():
        return (0, 0)
    try:
        a, d = r.stdout.strip().split("\t")[:2]
        return (int(a), int(d))
    except ValueError:
        return (0, 0)


def main() -> int:
    ap = argparse.ArgumentParser(description="生成本地改动同步清单网页")
    ap.add_argument("--repo", default=str(Path(__file__).resolve().parent.parent),
                    help="仓库路径 (默认: 脚本所在仓库)")
    ap.add_argument("--out", default=None, help="输出 HTML 路径 (默认: 仓库父目录/sync-report.html)")
    ap.add_argument("--proxy", default=None, help="代理地址, 如 http://proxyhk.huawei.com:8080 (默认: 自动探测系统代理)")
    ap.add_argument("--no-fetch", action="store_true", help="跳过 git fetch")
    ap.add_argument("--no-open", action="store_true", help="不自动打开浏览器")
    ap.add_argument("--json", default=None, help="输出机器可读清单 JSON 路径 (供自动提交驱动)")
    args = ap.parse_args()

    repo = Path(args.repo).resolve()
    if not (repo / ".git").exists():
        print(f"[x] 不是 git 仓库: {repo}", file=sys.stderr)
        return 1

    proxy, proxy_src = resolve_proxy(args.proxy)
    print(f"[1/4] 仓库: {repo}")
    if proxy:
        print(f"      代理: {proxy} (来源: {proxy_src})")
        proxy_cfg = ["-c", f"http.proxy={proxy}", "-c", f"https.proxy={proxy}"]
    else:
        print("      代理: 未配置 (直连)")
        proxy_cfg = []
    if not args.no_fetch:
        print("      git fetch origin ...")
        r = git(repo, *proxy_cfg, "fetch", "origin")
        if r.returncode != 0:
            print(f"[x] fetch 失败: {r.stderr.strip()}", file=sys.stderr)
            return 1

    branch = git(repo, "symbolic-ref", "--short", "HEAD").stdout.strip() or "master"
    local_head = git(repo, "rev-parse", "HEAD").stdout.strip()
    remote_head = git(repo, "rev-parse", f"origin/{branch}").stdout.strip()
    if not local_head or not remote_head:
        print(f"[x] 无法解析 HEAD / origin/{branch}", file=sys.stderr)
        return 1

    ahead = git(repo, "rev-list", "--count", f"origin/{branch}..HEAD").stdout.strip() or "0"
    behind = git(repo, "rev-list", "--count", f"HEAD..origin/{branch}").stdout.strip() or "0"
    remote_url = git(repo, "remote", "get-url", "origin").stdout.strip()
    m = re.search(r"(?:github\.com[:/])([^/:]+)/([^/]+?)(?:\.git)?$", remote_url)
    full_name = f"{m.group(1)}/{m.group(2)}" if m else remote_url.rstrip("/")

    print(f"[2/4] 本地 {local_head[:8]} -> 远端 {remote_head[:8]} (领先 {ahead}, 落后 {behind})")

    # 未推送的已提交改动 + 工作区未提交改动（对比工作区 vs 远端）
    r = git(repo, "diff", "--name-status", f"origin/{branch}")
    if r.returncode != 0:
        print(f"[x] git diff 失败: {r.stderr.strip()}", file=sys.stderr)
        return 1
    entries = []
    for line in r.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split("\t")
        st, path = parts[0], parts[-1]
        entries.append({"status": st[0], "path": path})

    # 未跟踪文件 (新文件尚未 commit)
    r = git(repo, "status", "--porcelain")
    seen = {e["path"] for e in entries}
    for line in r.stdout.splitlines():
        if line.startswith("?? "):
            p = line[3:].strip()
            if p not in seen:
                entries.append({"status": "A", "path": p, "untracked": True})

    print(f"[3/4] 待同步条目: {len(entries)}")

    updates, deletes, binaries = [], [], []
    for e in entries:
        path = e["path"]
        fp = repo / path
        if e["status"] == "D":
            deletes.append({"path": path})
            continue
        if not fp.exists():
            continue
        data = fp.read_bytes()
        if not is_text(data):
            binaries.append({"path": path})
            continue
        a, d = numstat(repo, f"origin/{branch}", path) if not e.get("untracked") else (0, 0)
        if e.get("untracked"):
            a = data.decode("utf-8").count("\n") + 1
        updates.append({
            "path": path,
            "status": "A" if e["status"] == "A" else "M",
            "added": a,
            "deleted": d,
            "content": data.decode("utf-8"),
        })

    out_path = Path(args.out) if args.out else (repo.parent / DEFAULT_OUT_NAME)
    print(f"[4/4] 生成: {out_path}")
    html = render(full_name, branch, local_head[:8], remote_head[:8], ahead, behind,
                  updates, deletes, binaries)
    out_path.write_text(html, encoding="utf-8")

    if args.json:
        manifest = {
            "repo": full_name,
            "branch": branch,
            "local_head": local_head,
            "remote_head": remote_head,
            "ahead": int(ahead),
            "behind": int(behind),
            "generated_at": subprocess.run(
                ["date", "+%Y-%m-%dT%H:%M:%S%z"], capture_output=True, text=True
            ).stdout.strip(),
            "updates": updates,
            "deletes": deletes,
            "binaries": binaries,
        }
        json_path = Path(args.json)
        json_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"      清单 JSON: {json_path}")

    if not args.no_open:
        webbrowser.open(out_path.as_uri())
    return 0


def render(full_name, branch, local, remote, ahead, behind,
           updates, deletes, binaries) -> str:
    base = f"https://github.com/{full_name}"
    edit_base = f"{base}/edit/{branch}"
    new_base = f"{base}/new/{branch}"
    delete_base = f"{base}/delete/{branch}"

    def esc(s):
        return json.dumps(s, ensure_ascii=False).replace("<", "\\u003c").replace(">", "\\u003e")

    # 数据: 只放待更新文件内容 (按序号索引)
    data_obj = {f"f{i}": u["content"] for i, u in enumerate(updates)}

    cards = []
    for i, u in enumerate(updates):
        is_new = u["status"] == "A"
        link = f"{new_base}?filename={u['path']}" if is_new else f"{edit_base}/{u['path']}"
        badge = '<span class="badge new">新增</span>' if is_new else '<span class="badge mod">修改</span>'
        cards.append(f"""
<div class="card">
  <div class="row">
    <code class="path">{esc(u['path'])}</code>{badge}
    <span class="stat">+{u['added']} -{u['deleted']}</span>
  </div>
  <div class="row btns">
    <button class="btn copy" data-idx="{i}" data-path="{esc(u['path'])}">📋 复制内容</button>
    <a class="btn" href="{link}" target="_blank" rel="noopener">✏️ GitHub 在线编辑</a>
  </div>
  <details class="preview"><summary>内容预览 (前 200 行)</summary>
    <pre>{esc('\n'.join(u['content'].splitlines()[:200]))}</pre>
  </details>
</div>""")

    del_cards = ""
    for d in deletes:
        del_cards += f"""
<div class="card del">
  <div class="row"><code class="path">{esc(d['path'])}</code><span class="badge del">删除</span></div>
  <div class="row btns">
    <a class="btn" href="{delete_base}/{d['path']}" target="_blank" rel="noopener">🗑️ GitHub 删除确认页</a>
  </div>
  <div class="hint">GitHub 删除页需要填入提交信息后点 Commit changes 才生效。</div>
</div>"""

    bin_cards = ""
    for b in binaries:
        bin_cards += f"""
<div class="card bin">
  <div class="row"><code class="path">{esc(b['path'])}</code><span class="badge bin">二进制</span></div>
  <div class="hint">二进制文件无法粘贴，请用 GitHub 网页上传。</div>
  <div class="row btns">
    <a class="btn" href="{base}/upload/{branch}" target="_blank" rel="noopener">⬆️ 打开上传页</a>
  </div>
</div>"""

    ahead_warn = ""
    if behind and int(behind) > 0:
        ahead_warn = f"""
<div class="warn">⚠️ 远端领先 {behind} 个提交。先执行 <code>git pull</code> 合并，避免覆盖远端新内容；
若确认覆盖，直接在网页上粘贴保存即可。</div>"""

    html = f"""<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>vtouch 本地改动同步清单</title>
<style>
  :root {{ --border:#e2e5ea; --muted:#6b7280; --new:#0e8a3e; --mod:#b45309; --del:#b91c1c; --bin:#7c3aed; }}
  * {{ box-sizing:border-box; }}
  body {{ font-family:-apple-system,"Segoe UI","Microsoft YaHei",sans-serif; margin:0; background:#f6f7f9; color:#1f2328; }}
  .wrap {{ max-width:900px; margin:0 auto; padding:24px 16px 80px; }}
  header h1 {{ margin:0 0 4px; font-size:22px; }}
  header .sub {{ color:var(--muted); font-size:13px; margin-bottom:12px; }}
  .guide {{ background:#fff; border:1px solid var(--border); border-radius:10px; padding:12px 16px; font-size:13px; line-height:1.9; }}
  .guide b {{ color:#1d4ed8; }}
  .warn {{ background:#fef3c7; border:1px solid #f59e0b; border-radius:10px; padding:10px 14px; margin:12px 0; font-size:13px; }}
  h2 {{ font-size:16px; margin:28px 0 10px; }}
  h2 .count {{ color:var(--muted); font-weight:normal; font-size:13px; }}
  .card {{ background:#fff; border:1px solid var(--border); border-radius:10px; padding:12px 14px; margin-bottom:10px; }}
  .row {{ display:flex; align-items:center; gap:10px; flex-wrap:wrap; }}
  .row + .row {{ margin-top:10px; }}
  code.path {{ font-family:Consolas,Menlo,monospace; font-size:13px; word-break:break-all; }}
  .badge {{ font-size:11px; padding:1px 8px; border-radius:99px; color:#fff; }}
  .badge.new {{ background:var(--new); }} .badge.mod {{ background:var(--mod); }}
  .badge.del {{ background:var(--del); }} .badge.bin {{ background:var(--bin); }}
  .stat {{ color:var(--muted); font-size:12px; margin-left:auto; }}
  .btns .btn {{ display:inline-block; padding:6px 12px; border-radius:7px; font-size:13px;
               text-decoration:none; cursor:pointer; border:1px solid var(--border); background:#fff; color:#1f2328; }}
  .btns .btn.copy {{ background:#1d4ed8; color:#fff; border-color:#1d4ed8; }}
  .btns .btn.copy.ok {{ background:#0e8a3e; border-color:#0e8a3e; }}
  .btns .btn:hover {{ filter:brightness(1.08); }}
  details.preview {{ margin-top:10px; font-size:12px; color:var(--muted); }}
  details.preview pre {{ background:#0f172a; color:#e2e8f0; padding:10px; border-radius:8px;
                        max-height:280px; overflow:auto; font-size:12px; white-space:pre-wrap; word-break:break-all; }}
  .hint {{ font-size:12px; color:var(--muted); margin-top:8px; }}
  .card.del {{ border-left:3px solid var(--del); }}
  .card.bin {{ border-left:3px solid var(--bin); }}
  .toast {{ position:fixed; left:50%; bottom:28px; transform:translateX(-50%);
           background:#111827; color:#fff; padding:9px 18px; border-radius:8px; font-size:13px;
           opacity:0; transition:opacity .25s; pointer-events:none; z-index:99; }}
  .toast.show {{ opacity:1; }}
</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1>本地改动同步清单</h1>
    <div class="sub">仓库 <b>{full_name}</b> · 分支 <b>{branch}</b> · 本地 <code>{local}</code> → 远端 <code>{remote}</code> · 领先 {ahead} 个提交</div>
  </header>
  {ahead_warn}
  <div class="guide">
    <b>操作步骤</b><br>
    1. 点击文件的 <b>📋 复制内容</b>（自动复制到剪贴板）<br>
    2. 点击 <b>✏️ GitHub 在线编辑</b> 打开编辑页<br>
    3. 在编辑区 <b>Ctrl+V</b> 粘贴，填写提交信息，点 <b>Commit changes</b> 保存<br>
    4. 删除的文件走 <b>🗑️ GitHub 删除确认页</b>；二进制文件走 <b>⬆️ 上传页</b>
  </div>

  <h2>待更新（新增 / 修改）<span class="count">{len(updates)} 个</span></h2>
  {''.join(cards) if cards else '<div class="card"><span class="hint">没有需要更新的文件。</span></div>'}

  <h2>待删除 <span class="count">{len(deletes)} 个</span></h2>
  {del_cards if del_cards else '<div class="card"><span class="hint">没有需要删除的文件。</span></div>'}

  <h2>二进制文件（需网页上传）<span class="count">{len(binaries)} 个</span></h2>
  {bin_cards if bin_cards else '<div class="card"><span class="hint">没有二进制文件。</span></div>'}
</div>
<div class="toast" id="toast"></div>

<script>
window.SYNC_DATA = {json.dumps(data_obj, ensure_ascii=False).replace("</", "<\\/")};

function toast(msg) {{
  var t = document.getElementById('toast');
  t.textContent = msg; t.classList.add('show');
  clearTimeout(t._timer);
  t._timer = setTimeout(function () {{ t.classList.remove('show'); }}, 1800);
}}

function fallbackCopy(text) {{
  var ta = document.createElement('textarea');
  ta.value = text;
  ta.style.position = 'fixed'; ta.style.opacity = '0';
  document.body.appendChild(ta);
  ta.focus(); ta.select();
  try {{ document.execCommand('copy'); }} catch (e) {{}}
  document.body.removeChild(ta);
}}

function copyContent(idx, path) {{
  var text = window.SYNC_DATA['f' + idx];
  var btn = document.querySelector('.btn.copy[data-idx="' + idx + '"]');
  function ok() {{
    if (btn) {{ btn.textContent = '✓ 已复制'; btn.classList.add('ok'); }}
    toast('已复制: ' + path + '  →  去 GitHub 粘贴');
  }}
  function fail() {{
    toast('复制失败，请手动选择预览内容复制');
  }}
  if (navigator.clipboard && window.isSecureContext) {{
    navigator.clipboard.writeText(text).then(ok, function () {{ fallbackCopy(text); ok(); }});
  }} else {{
    fallbackCopy(text); ok();
  }}
}}

document.querySelectorAll('.btn.copy').forEach(function (btn) {{
  btn.addEventListener('click', function () {{
    copyContent(parseInt(btn.dataset.idx, 10), btn.dataset.path);
  }});
}});
</script>
</body>
</html>"""
    return html


if __name__ == "__main__":
    sys.exit(main())
