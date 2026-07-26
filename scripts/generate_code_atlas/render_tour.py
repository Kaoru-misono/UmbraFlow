# -*- coding: utf-8 -*-
"""Render tour_content.py into docs/knowledge/tour.html (《UmbraFlow 深度导读》).

Self-contained output: inline CSS/JS, build-time C++ syntax highlighting, no
external requests. Verifies every code excerpt against the real source file and
exits 1 on drift (the page is still written so the drift can be inspected).
"""
import html
import re
import sys
from pathlib import Path

from tour_content import CHAPTERS

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
OUT = REPO / "docs" / "knowledge" / "tour.html"
VSCODE = "vscode://file/" + REPO.as_posix() + "/"

KEYWORDS = set(
    "alignas alignof auto bool break case catch char class concept const constexpr "
    "consteval constinit continue decltype default delete do double else enum explicit "
    "export extern false final float for friend goto if inline int long mutable namespace "
    "new noexcept nullptr operator override private protected public requires return short "
    "signed sizeof static struct switch template this thread_local throw true try typedef "
    "typename union unsigned using virtual void volatile while".split())

TOKEN_RE = re.compile(
    r"(//.*$)"                                   # comment
    r"|(\"(?:[^\"\\]|\\.)*\")"                   # string
    r"|('(?:[^'\\]|\\.)*')"                      # char
    r"|\b(0[xX][0-9a-fA-F]+[uUlL]*|\d[\d']*(?:\.\d+)?[uUlLfF]*)\b"  # number
    r"|([A-Za-z_]\w*)"                           # identifier
)


def esc(s):
    return html.escape(str(s), quote=False)


def highlight_line(line):
    stripped = line.lstrip()
    if stripped.startswith("#"):
        return "<span class='pp'>%s</span>" % esc(line)
    out, pos = [], 0
    for m in TOKEN_RE.finditer(line):
        out.append(esc(line[pos:m.start()]))
        text = m.group(0)
        if m.group(1):
            out.append("<span class='cm'>%s</span>" % esc(text))
        elif m.group(2) or m.group(3):
            out.append("<span class='st'>%s</span>" % esc(text))
        elif m.group(4):
            out.append("<span class='nb'>%s</span>" % esc(text))
        else:
            rest = line[m.end():].lstrip()
            if text in KEYWORDS:
                cls = "kw"
            elif rest.startswith("("):
                cls = "fn"
            elif text[0].isupper():
                cls = "ty"
            else:
                cls = None
            out.append("<span class='%s'>%s</span>" % (cls, esc(text)) if cls else esc(text))
        pos = m.end()
    out.append(esc(line[pos:]))
    return "".join(out)


def verify_excerpt(ex):
    src = REPO / ex["file"]
    if not src.exists():
        return "%s: file missing" % ex["file"]
    actual = src.read_text(encoding="utf-8", errors="replace").splitlines()
    lines = ex["code"].split("\n")
    start = ex["start"]
    for i, want in enumerate(lines):
        idx = start - 1 + i
        if idx >= len(actual) or actual[idx].rstrip() != want.rstrip():
            return "%s:%d drifted (excerpt line %d)" % (ex["file"], start + i, i + 1)
    return None


def render_excerpt(ex):
    lines = ex["code"].split("\n")
    start = ex["start"]
    ann = {line: (i + 1, note) for i, (line, note) in enumerate(ex.get("ann", []))}
    rows = []
    for i, line in enumerate(lines):
        n = start + i
        marked = n in ann
        badge = "<span class='badge' data-idx='%d'>%d</span>" % (ann[n][0], ann[n][0]) if marked else ""
        rows.append(
            "<div class='cl%s'%s><span class='ln'>%d</span><span class='lc'>%s</span>%s</div>"
            % (" hl" if marked else "", " data-idx='%d'" % ann[n][0] if marked else "",
               n, highlight_line(line) or "&nbsp;", badge))
    notes = "".join(
        "<li data-idx='%d'>%s</li>" % (i + 1, esc(note)) for i, (_, note) in enumerate(ex.get("ann", [])))
    href = VSCODE + ex["file"] + ":%d" % start
    return (
        "<figure class='codefig'>"
        "<figcaption><span class='cap'>%s</span><a class='floc' href='%s' title='在 VS Code 中打开'>%s:%d</a></figcaption>"
        "<div class='codebox'><pre class='code'>%s</pre></div>"
        "%s</figure>"
        % (esc(ex["caption"]), esc(href), esc(ex["file"]), start, "".join(rows),
           "<ol class='notes'>%s</ol>" % notes if notes else ""))


ASIDE_LABEL = {"whynot": "换个做法行不行？", "pitfall": "容易踩的坑", "lesson": "可以带走的"}


def render_aside(a):
    return (
        "<aside class='aside aside-%s'><p class='aside-label'>%s</p>"
        "<p class='aside-title'>%s</p><p>%s</p></aside>"
        % (a["kind"], ASIDE_LABEL.get(a["kind"], a["kind"]), esc(a["title"]), esc(a["body"])))


def render_chapter(ch):
    parts = [
        "<section class='chapter' id='ch-%s'>" % ch["key"],
        "<p class='eyebrow'>第 %d 章</p>" % ch["num"],
        "<h2>%s</h2>" % esc(ch["title"]),
        "<p class='thesis'>%s</p>" % esc(ch["thesis"]),
    ]
    for sec in ch["sections"]:
        parts.append("<h3>%s</h3>" % esc(sec["h"]))
        paras = sec.get("paras", [])
        codes = sec.get("code", [])
        for i, p in enumerate(paras):
            parts.append("<p>%s</p>" % esc(p))
            if i < len(codes):
                parts.append(render_excerpt(codes[i]))
        for ex in codes[len(paras):]:
            parts.append(render_excerpt(ex))
        for a in sec.get("asides", []):
            parts.append(render_aside(a))
    if ch.get("lessons"):
        items = "".join("<li>%s</li>" % esc(x) for x in ch["lessons"])
        parts.append("<div class='takeaway'><p class='aside-label'>本章要点</p><ul>%s</ul></div>" % items)
    parts.append("</section>")
    return "".join(parts)


CSS = """
:root {
  --bg:#F5F6F8; --surface:#FFFFFF; --ink:#1B222B; --muted:#5C6673;
  --line:#E2E6EA; --accent:#A8742C; --accent-ink:#7C5316; --accent-soft:rgba(168,116,44,.09);
  --code-bg:#F8F9FA; --hl-bg:rgba(168,116,44,.07);
  --kw:#2F5FA3; --ty:#22706A; --fn:#6A4FA3; --st:#9A3E24; --nb:#895B0D; --cm:#7A858F; --pp:#7C5316;
  --whynot:#2F5FA3; --pitfall:#A8422C; --lesson:#7C5316;
}
@media (prefers-color-scheme: dark) { :root {
  --bg:#14171C; --surface:#1B2026; --ink:#E7EAEE; --muted:#96A0AB;
  --line:#2A313A; --accent:#D9A45E; --accent-ink:#E5B876; --accent-soft:rgba(217,164,94,.10);
  --code-bg:#161A20; --hl-bg:rgba(217,164,94,.09);
  --kw:#7FA8DC; --ty:#6FBFB2; --fn:#AC93DC; --st:#E0876B; --nb:#D9A45E; --cm:#7C8892; --pp:#E5B876;
  --whynot:#7FA8DC; --pitfall:#E0876B; --lesson:#E5B876;
} }
:root[data-theme="light"] {
  --bg:#F5F6F8; --surface:#FFFFFF; --ink:#1B222B; --muted:#5C6673;
  --line:#E2E6EA; --accent:#A8742C; --accent-ink:#7C5316; --accent-soft:rgba(168,116,44,.09);
  --code-bg:#F8F9FA; --hl-bg:rgba(168,116,44,.07);
  --kw:#2F5FA3; --ty:#22706A; --fn:#6A4FA3; --st:#9A3E24; --nb:#895B0D; --cm:#7A858F; --pp:#7C5316;
  --whynot:#2F5FA3; --pitfall:#A8422C; --lesson:#7C5316;
}
:root[data-theme="dark"] {
  --bg:#14171C; --surface:#1B2026; --ink:#E7EAEE; --muted:#96A0AB;
  --line:#2A313A; --accent:#D9A45E; --accent-ink:#E5B876; --accent-soft:rgba(217,164,94,.10);
  --code-bg:#161A20; --hl-bg:rgba(217,164,94,.09);
  --kw:#7FA8DC; --ty:#6FBFB2; --fn:#AC93DC; --st:#E0876B; --nb:#D9A45E; --cm:#7C8892; --pp:#E5B876;
  --whynot:#7FA8DC; --pitfall:#E0876B; --lesson:#E5B876;
}
* { box-sizing:border-box; }
body {
  margin:0; background:var(--bg); color:var(--ink);
  font:15.5px/1.85 -apple-system,"Segoe UI","Microsoft YaHei","PingFang SC","Noto Sans CJK SC",sans-serif;
}
code, .code, .ln, .floc { font-family:"Cascadia Code",Consolas,"Sarasa Mono SC",ui-monospace,monospace; }
a { color:var(--accent-ink); text-decoration:none; }
a:hover { text-decoration:underline; }
a:focus-visible { outline:2px solid var(--accent); outline-offset:2px; border-radius:3px; }

.layout { display:flex; min-height:100vh; }
.rail {
  width:264px; flex:0 0 264px; position:sticky; top:0; height:100vh; overflow-y:auto;
  border-right:1px solid var(--line); padding:24px 16px 32px; background:var(--surface);
}
.rail .brand { font-weight:650; font-size:16px; margin-bottom:2px; }
.rail .brand-sub { font-size:12px; color:var(--muted); margin-bottom:16px; }
.rail nav { display:flex; flex-direction:column; gap:2px; }
.rail nav a {
  display:flex; gap:10px; align-items:baseline; padding:7px 10px; border-radius:6px;
  color:var(--ink); font-size:13.5px; line-height:1.4;
}
.rail nav a .no { color:var(--muted); font-size:12px; font-variant-numeric:tabular-nums; }
.rail nav a:hover { background:var(--accent-soft); text-decoration:none; }
.rail nav a.on { background:var(--accent-soft); }
.rail nav a.on .no { color:var(--accent-ink); }
.rail .xlink { display:block; margin-top:18px; padding-top:14px; border-top:1px solid var(--line); font-size:13px; color:var(--muted); }

.content { flex:1; min-width:0; padding:40px 48px 100px; }
.inner { max-width:800px; }

.chapter { margin-bottom:72px; scroll-margin-top:20px; }
.eyebrow { font-size:11.5px; letter-spacing:.14em; text-transform:uppercase; color:var(--accent-ink); font-weight:650; margin:0 0 6px; }
.chapter h2 { font-size:26px; font-weight:650; margin:0 0 14px; letter-spacing:.005em; text-wrap:balance; }
.thesis {
  font-size:16.5px; line-height:1.8; color:var(--ink); margin:0 0 26px;
  padding-left:16px; border-left:3px solid var(--accent);
}
.chapter h3 { font-size:18px; font-weight:650; margin:34px 0 10px; }
.chapter p { margin:0 0 14px; max-width:72ch; }

.codefig { margin:18px 0 22px; }
.codefig figcaption {
  display:flex; align-items:baseline; gap:12px; padding:7px 14px;
  background:var(--surface); border:1px solid var(--line); border-bottom:none;
  border-radius:8px 8px 0 0; font-size:12.5px;
}
.cap { color:var(--muted); }
.floc { margin-left:auto; font-size:12px; color:var(--muted); white-space:nowrap; }
.floc:hover { color:var(--accent-ink); }
.codebox { border:1px solid var(--line); border-radius:0 0 8px 8px; background:var(--code-bg); overflow-x:auto; }
.code { margin:0; padding:10px 0; font-size:12.8px; line-height:1.6; min-width:max-content; }
.cl { display:flex; padding:0 14px 0 0; }
.cl .ln {
  flex:0 0 44px; text-align:right; padding-right:14px; color:var(--muted); opacity:.55;
  user-select:none; font-variant-numeric:tabular-nums; font-size:11.5px; line-height:1.78;
}
.cl .lc { white-space:pre; }
.cl.hl { background:var(--hl-bg); }
.cl.hl.on { background:var(--accent-soft); }
.badge {
  flex:0 0 auto; margin-left:12px; align-self:center;
  min-width:16px; height:16px; border-radius:99px; background:var(--accent); color:var(--surface);
  font-size:10.5px; font-weight:700; line-height:16px; text-align:center; font-family:inherit;
}
.notes { margin:8px 0 0; padding-left:0; list-style:none; font-size:13.5px; color:var(--muted); }
.notes li { margin-bottom:4px; padding-left:26px; position:relative; max-width:72ch; }
.notes li::before {
  content:counter(list-item); position:absolute; left:0; top:3px;
  min-width:16px; height:16px; border-radius:99px; background:var(--accent); color:var(--surface);
  font-size:10.5px; font-weight:700; line-height:16px; text-align:center;
}
.notes li.on { color:var(--ink); }

.cm { color:var(--cm); font-style:italic; }
.st { color:var(--st); } .nb { color:var(--nb); } .kw { color:var(--kw); }
.ty { color:var(--ty); } .fn { color:var(--fn); } .pp { color:var(--pp); }

.aside {
  border:1px solid var(--line); border-left:3px solid var(--accent); border-radius:8px;
  background:var(--surface); padding:14px 18px 10px; margin:18px 0 22px; max-width:72ch;
}
.aside p { margin:0 0 8px; font-size:14px; }
.aside-label { font-size:10.5px; letter-spacing:.12em; text-transform:uppercase; font-weight:650; color:var(--muted); }
.aside-title { font-weight:650; font-size:14.5px; }
.aside-whynot { border-left-color:var(--whynot); }
.aside-whynot .aside-label { color:var(--whynot); }
.aside-pitfall { border-left-color:var(--pitfall); }
.aside-pitfall .aside-label { color:var(--pitfall); }
.aside-lesson { border-left-color:var(--lesson); }
.aside-lesson .aside-label { color:var(--lesson); }

.takeaway {
  border:1px solid var(--line); border-radius:8px; background:var(--accent-soft);
  padding:14px 18px 8px; margin-top:28px; max-width:72ch;
}
.takeaway .aside-label { color:var(--accent-ink); margin:0 0 6px; }
.takeaway ul { margin:0; padding-left:20px; font-size:14px; }
.takeaway li { margin-bottom:5px; }

.intro { margin-bottom:56px; }
.intro h1 { font-size:31px; font-weight:650; margin:0 0 14px; letter-spacing:.005em; text-wrap:balance; }
.intro p { max-width:72ch; color:var(--muted); margin:0 0 10px; font-size:15.5px; }

@media (max-width: 900px) {
  .layout { flex-direction:column; }
  .rail { position:static; width:auto; height:auto; flex:none; border-right:none; border-bottom:1px solid var(--line); }
  .content { padding:24px 18px 60px; }
}
@media (prefers-reduced-motion: no-preference) { html { scroll-behavior:smooth; } }
"""

JS = """
(function () {
  document.querySelectorAll('.codefig').forEach(function (fig) {
    function mark(idx, on) {
      fig.querySelectorAll("[data-idx='" + idx + "']").forEach(function (el) {
        el.classList.toggle('on', on);
      });
    }
    fig.querySelectorAll('.cl.hl, .notes li').forEach(function (el) {
      var idx = el.getAttribute('data-idx') ||
        (el.tagName === 'LI' ? String(Array.prototype.indexOf.call(el.parentNode.children, el) + 1) : null);
      if (!idx) return;
      el.setAttribute('data-idx', idx);
      el.addEventListener('mouseenter', function () { mark(idx, true); });
      el.addEventListener('mouseleave', function () { mark(idx, false); });
    });
  });
  var links = Array.prototype.slice.call(document.querySelectorAll('.rail nav a'));
  var map = {};
  links.forEach(function (a) { map[a.getAttribute('href').slice(1)] = a; });
  if ('IntersectionObserver' in window) {
    var current = null;
    var io = new IntersectionObserver(function (entries) {
      entries.forEach(function (e) {
        if (e.isIntersecting) {
          if (current) current.classList.remove('on');
          current = map[e.target.id];
          if (current) current.classList.add('on');
        }
      });
    }, { rootMargin: '0px 0px -70% 0px' });
    document.querySelectorAll('.chapter').forEach(function (s) { io.observe(s); });
  }
})();
"""

INTRO_HTML = (
    "<div class='intro'><p class='eyebrow'>UmbraFlow</p><h1>深度导读：这个系统是怎么设计的</h1>"
    "<p>本文假设你熟悉现代 C++，也知道这个项目是做什么的（看画面、认页面、后台点击），"
    "但不了解它内部的设计。八章的展开方式是一致的：先说要解决的问题，再看朴素做法差在哪里，"
    "然后读真实代码。所有代码都逐字摘自仓库并标注行号，点摘录右上角的 file:line 可以直接在 VS Code 里打开。</p>"
    "<p>需要按类型查细节的时候，配合<a href='atlas.html'>代码地图集</a>使用：本文回答为什么，地图集回答在哪里。</p></div>")


def main():
    drift = [d for ch in CHAPTERS for sec in ch["sections"] for ex in sec.get("code", [])
             if (d := verify_excerpt(ex))]
    nav = "".join(
        "<a href='#ch-%s'><span class='no'>%02d</span><span>%s</span></a>"
        % (ch["key"], ch["num"], esc(ch["title"])) for ch in CHAPTERS)
    body = (
        "<title>UmbraFlow 深度导读</title>"
        "<style>%s</style>"
        "<div class='layout'>"
        "<aside class='rail'><div class='brand'>UmbraFlow 深度导读</div>"
        "<div class='brand-sub'>8 章 · 代码逐字摘自仓库</div>"
        "<nav>%s</nav>"
        "<a class='xlink' href='atlas.html'>配套参考：代码地图集 →</a></aside>"
        "<main class='content'><div class='inner'>%s%s</div></main></div>"
        "<script>%s</script>"
        % (CSS, nav, INTRO_HTML, "".join(render_chapter(ch) for ch in CHAPTERS), JS))
    OUT.write_text(
        "<!doctype html>\n<html lang='zh-CN'>\n<head>\n<meta charset='utf-8'>\n"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>\n"
        "<title>UmbraFlow 深度导读</title>\n</head>\n<body>\n" + body + "\n</body>\n</html>\n",
        encoding="utf-8")
    excerpts = sum(len(sec.get("code", [])) for ch in CHAPTERS for sec in ch["sections"])
    print("wrote %s (%d chapters, %d excerpts)" % (OUT, len(CHAPTERS), excerpts))
    for d in drift:
        print("DRIFT: %s" % d)
    return 1 if drift else 0


if __name__ == "__main__":
    sys.exit(main())
