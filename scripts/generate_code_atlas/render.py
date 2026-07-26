# -*- coding: utf-8 -*-
"""Render the per-module atlas JSON files in data/ into docs/knowledge/atlas.html.

The output is a fully self-contained page (inline CSS/JS/SVG, no external
requests) that renders identically as a local file and as a claude.ai artifact.
Pass --artifact-out <path> to also write the content-only variant (no
doctype/head wrapper) that the Artifact tool expects.
"""
import html
import io
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
ATLAS = HERE / "data"
LOCAL_OUT = REPO / "docs" / "knowledge" / "atlas.html"
REPO_URL_ROOT = "vscode://file/" + REPO.as_posix() + "/"

MODULE_ORDER = [
    "core", "domain", "vision-image", "annotation",
    "engine", "controller", "script", "entry-cli", "entry-workbench",
]

KIND_LABEL = {
    "class": "class", "struct": "struct", "enum": "enum",
    "functions": "fn", "alias": "alias", "concept": "concept",
}

# Hand-laid dependency DAG rendered as inline SVG so the page works both on
# claude.ai and as a plain local file (no mermaid runtime needed). Update the
# coordinates below when modules are added or removed.
NODES = {
    "core":       (390, 480, 120, "core", "base"),
    "domain":     (390, 400, 120, "domain", "base"),
    "vision":     (300, 320, 100, "vision", "mid"),
    "image":      (480, 320, 100, "image", "mid"),
    "annotation": (390, 240, 130, "annotation", "mid"),
    "engine":     (390, 160, 120, "engine", "mid"),
    "controller": (645, 240, 150, "controller (Win)", "win"),
    "script":     (645, 400, 110, "script", "mid"),
    "cli":        (300, 70, 150, "entry/cli", "entry"),
    "wb":         (560, 70, 170, "entry/workbench", "entry"),
}
EDGES = [
    ("domain", "core", 0), ("vision", "domain", -30), ("image", "domain", 30),
    ("annotation", "vision", 0), ("annotation", "image", 0),
    ("engine", "annotation", 0), ("controller", "domain", 45),
    ("script", "core", 50), ("cli", "engine", -20), ("cli", "controller", -55),
    ("wb", "annotation", -20), ("wb", "engine", 25), ("wb", "controller", 0),
    ("wb", "image", 40),
]
NODE_H = 34

INTRO = (
    "UmbraFlow 根据视觉证据授权严格后台点击：平台无关模块负责识别与授权，"
    "Windows 代码负责捕获与投递，两者只在 entry/ 组合。下图箭头表示「依赖于」，"
    "越靠下越基础。本页由脚本从源码抽取生成，每个类型的 file:line 都可点击，"
    "在 VS Code 中直接打开对应位置。"
)

GLOBAL_READING = [
    ("core → domain", "先建立词汇：Result/Status、强类型、帧身份、坐标空间、租约。"),
    ("vision + image", "两个纯算法层：Gray8/SAD 匹配与 PNG 编解码，都有确定性与资源上限。"),
    ("annotation", "系统的语义中心：标注文档、编译、页面识别、证据与授权。"),
    ("engine", "运行时编排：Observation 单帧决策、act 的 fail-closed 顺序、trace。"),
    ("controller", "Windows 侧：窗口发现、目标代际、WGC 捕获、严格后台输入。"),
    ("entry/cli → entry/workbench", "组合根：一次 run 怎么串起来，工作台怎么编辑与发布。"),
]


def esc(s):
    return html.escape(str(s), quote=True)


def slug(name):
    return "t-" + re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")


def code_href(path, line=None):
    p = str(path).replace("\\", "/")
    frag = ":%d" % line if line else ""
    return REPO_URL_ROOT + p + frag


def file_link(path, line=None, cls="floc"):
    label = esc(path) + ((":<span class='lno'>%d</span>" % line) if line else "")
    return "<a class='%s' href='%s' title='在 VS Code 中打开'>%s</a>" % (cls, esc(code_href(path, line)), label)


def dep_svg():
    parts = [
        "<svg viewBox='0 0 880 530' role='img' aria-label='模块依赖图' "
        "style='width:100%;max-width:880px;height:auto;display:block'>",
        "<defs><marker id='arr' viewBox='0 0 8 8' refX='7' refY='4' markerWidth='7' "
        "markerHeight='7' orient='auto-start-reverse'>"
        "<path d='M0,0.5 L7.5,4 L0,7.5 z' fill='var(--muted)'/></marker></defs>",
    ]
    for src, dst, dx in EDGES:
        sx, sy, sw, _, _ = NODES[src]
        tx, ty, tw, _, _ = NODES[dst]
        x1, y1 = sx, sy + NODE_H / 2          # bottom of source
        x2, y2 = tx + dx, ty - NODE_H / 2      # top of target (+offset)
        parts.append(
            "<line x1='%g' y1='%g' x2='%g' y2='%g' stroke='var(--muted)' "
            "stroke-width='1.2' opacity='0.65' marker-end='url(#arr)'/>" % (x1, y1, x2, y2))
    stroke = {"base": "var(--line)", "mid": "var(--line)", "win": "var(--k-class-fg)", "entry": "var(--accent)"}
    for key, (cx, cy, w, label, klass) in NODES.items():
        parts.append(
            "<g><rect x='%g' y='%g' width='%g' height='%g' rx='6' fill='var(--surface)' "
            "stroke='%s'/><text x='%g' y='%g' text-anchor='middle' fill='var(--ink)' "
            "font-size='13' font-family='Cascadia Code,Consolas,monospace'>%s</text></g>"
            % (cx - w / 2, cy - NODE_H / 2, w, NODE_H, stroke[klass], cx, cy + 4.5, esc(label)))
    parts.append(
        "<text x='12' y='522' fill='var(--muted)' font-size='12'>箭头 = 依赖于（指向更基础的模块）；"
        "橙框 = 组合根，蓝框 = 仅 Windows</text>")
    parts.append("</svg>")
    return "".join(parts)


def load_modules():
    mods = {}
    for p in ATLAS.glob("*.json"):
        with io.open(p, encoding="utf-8") as f:
            d = json.load(f)
        mods[d["module"]] = d
    return [mods[k] for k in MODULE_ORDER if k in mods] + [d for k, d in sorted(mods.items()) if k not in MODULE_ORDER]


SIG_QUALIFIERS = {"virtual", "static", "explicit", "constexpr", "consteval", "friend", "inline"}


def split_top_params(params):
    parts, dp, da, last = [], 0, 0, 0
    for j, c in enumerate(params):
        if c in "([{":
            dp += 1
        elif c in ")]}":
            dp -= 1
        elif c == "<":
            da += 1
        elif c == ">" and (j == 0 or params[j - 1] != "-"):
            da = max(0, da - 1)
        elif c == "," and dp == 0 and da == 0:
            parts.append(params[last:j].strip())
            last = j + 1
    parts.append(params[last:].strip())
    return [p for p in parts if p]


def parse_function_sig(sig):
    """Parse a member signature into (name, params, return, qualifiers).

    Returns None for non-function members (data members, enum values, aliases)
    so they keep the plain single-line rendering.
    """
    da, p = 0, -1
    for j, c in enumerate(sig):
        if c == "<":
            da += 1
        elif c == ">" and (j == 0 or sig[j - 1] != "-"):
            da = max(0, da - 1)
        elif c == "(" and da == 0:
            p = j
            break
    if p <= 0:
        return None
    depth, q = 0, -1
    for j in range(p, len(sig)):
        if sig[j] in "([{":
            depth += 1
        elif sig[j] in ")]}":
            depth -= 1
            if depth == 0:
                q = j
                break
    if q == -1:
        return None
    tokens = sig[:p].split()
    pre_quals = [t for t in tokens if t in SIG_QUALIFIERS]
    rest = [t for t in tokens if t not in SIG_QUALIFIERS and t not in ("auto", "[[nodiscard]]")]
    if not rest:
        return None
    suffix = sig[q + 1:].strip()
    ret, post_quals = None, ""
    if "->" in suffix:
        post_quals, ret = suffix.split("->", 1)
        post_quals, ret = post_quals.strip(), ret.strip()
    elif len(rest) >= 2:
        # old-style return type before the name -> normalize to trailing style
        ret, rest, post_quals = " ".join(rest[:-1]), rest[-1:], suffix
    else:
        post_quals = suffix
    name = " ".join(rest)
    if any(c in name for c in "{};"):
        return None  # a definition snippet, not a plain signature
    return name, split_top_params(sig[p + 1:q].strip()), ret, pre_quals, post_quals


def split_top_decls(sig):
    parts, dp, da, last = [], 0, 0, 0
    for j, c in enumerate(sig):
        if c in "([{":
            dp += 1
        elif c in ")]}":
            dp -= 1
        elif c == "<":
            da += 1
        elif c == ">" and (j == 0 or sig[j - 1] != "-"):
            da = max(0, da - 1)
        elif c == ";" and dp == 0 and da == 0:
            parts.append(sig[last:j].strip())
            last = j + 1
    parts.append(sig[last:].strip())
    return [p for p in parts if p]


def format_decl(decl):
    """Code-style declaration text, or None when it is not a function."""
    fn = parse_function_sig(decl)
    if not fn:
        return None
    name, params, ret, pre_quals, post_quals = fn
    head = " ".join(pre_quals + (["auto"] if ret else []) + [name])
    tail = (" " + post_quals if post_quals else "") + (" -> " + ret if ret else "") + ";"
    if not params:
        return "%s()%s" % (head, tail)
    body = "".join("\n    %s%s" % (prm, "," if k < len(params) - 1 else "")
                   for k, prm in enumerate(params))
    return "%s(%s\n)%s" % (head, body, tail)


def render_sig_cell(sig):
    decls = split_top_decls(sig)
    if len(decls) == 1:
        text = format_decl(decls[0])
        if text is None:
            return "<code>%s</code>" % esc(sig)
        return "<code class='sigcode'>%s</code>" % esc(text)
    texts = [format_decl(d) or (d + ";") for d in decls]
    return "<code class='sigcode'>%s</code>" % esc("\n".join(texts))


def render_members(cls):
    members = cls.get("members") or []
    if not members:
        return ""
    rows = "".join(
        "<tr><td class='sig'>%s</td><td class='note'>%s</td></tr>"
        % (render_sig_cell(m["sig"]), esc(m["note"]))
        for m in members)
    return "<table class='members'>%s</table>" % rows


def render_related(cls, registry):
    rel = cls.get("related") or []
    chips = []
    for name in rel:
        s = slug(name)
        short = esc(name)
        if s in registry:
            chips.append("<a class='chip' href='#%s'>%s</a>" % (s, short))
        else:
            chips.append("<span class='chip chip-dead'>%s</span>" % short)
    return "<div class='related'><span class='rel-label'>相关</span>%s</div>" % "".join(chips) if chips else ""


def render_class(cls, registry):
    kind = cls.get("kind", "class")
    lifetime = cls.get("lifetime")
    search_blob = " ".join([
        cls["name"], cls.get("responsibility", ""), cls.get("file", ""),
        lifetime or "", " ".join(m["sig"] + " " + m["note"] for m in cls.get("members") or []),
    ]).lower()
    parts = [
        "<article class='card' id='%s' data-search='%s'>" % (slug(cls["name"]), esc(search_blob)),
        "<header class='card-head'>",
        "<span class='kind kind-%s'>%s</span>" % (esc(kind), esc(KIND_LABEL.get(kind, kind))),
        "<h4 class='cname'><code>%s</code></h4>" % esc(cls["name"]),
        file_link(cls["file"], cls.get("line")),
        "</header>",
        "<p class='resp'>%s</p>" % esc(cls.get("responsibility", "")),
        render_members(cls),
    ]
    if lifetime:
        parts.append("<p class='lifetime'><span class='rel-label'>生命周期</span>%s</p>" % esc(lifetime))
    parts.append(render_related(cls, registry))
    parts.append("</article>")
    return "".join(parts)


def render_flow(flow):
    steps = []
    for s in flow.get("steps", []):
        loc = " " + file_link(s["file"], s.get("line"), cls="floc floc-inline") if s.get("file") else ""
        steps.append("<li>%s%s</li>" % (esc(s["text"]), loc))
    return "<div class='flow'><h4>%s</h4><ol>%s</ol></div>" % (esc(flow["title"]), "".join(steps))


def render_module(mod, registry):
    key = esc(mod["module"])
    classes = mod.get("classes", [])
    head = [
        "<section class='module' id='m-%s' data-mtitle='%s'>" % (key, esc(mod["title"].lower())),
        "<header class='mod-head'><h2>%s</h2>" % esc(mod["title"]),
        "<span class='mod-count'>%d 个类型</span></header>" % len(classes),
        "<p class='mod-summary'>%s</p>" % esc(mod.get("summary", "")),
    ]
    if mod.get("notOwned"):
        items = "".join("<li>%s</li>" % esc(x) for x in mod["notOwned"])
        head.append("<details class='not-owned'><summary>不负责什么</summary><ul>%s</ul></details>" % items)
    if mod.get("readingOrder"):
        items = "".join(
            "<li>%s<span class='why'>%s</span></li>" % (file_link(r["path"]), esc(r["why"]))
            for r in mod["readingOrder"])
        head.append("<details class='reading' open><summary>建议阅读顺序</summary><ol>%s</ol></details>" % items)
    for f in mod.get("flows") or []:
        head.append(render_flow(f))
    head.append("<div class='cards'>%s</div>" % "".join(render_class(c, registry) for c in classes))
    head.append("</section>")
    return "".join(head)


def render_nav(mods):
    items = ["<a class='nav-item' href='#overview'>总览与依赖</a>",
             "<a class='nav-item' href='#reading'>全局阅读路径</a>"]
    for m in mods:
        short = m["title"].split(" — ")
        label = esc(short[0].replace("modules/", "").replace("entry/", "entry/"))
        hint = esc(short[1]) if len(short) > 1 else ""
        items.append(
            "<a class='nav-item nav-mod' href='#m-%s'><code>%s</code><span class='nav-hint'>%s</span>"
            "<span class='nav-count'>%d</span></a>" % (esc(m["module"]), label, hint, len(m.get("classes", []))))
    return "".join(items)


def render_overview(mods):
    tiles = []
    for m in mods:
        first = re.split(r"(?<=[。；])", m.get("summary", ""))[0]
        tiles.append(
            "<a class='tile' href='#m-%s'><h3>%s</h3><p>%s</p><span class='tile-count'>%d 个类型</span></a>"
            % (esc(m["module"]), esc(m["title"]), esc(first), len(m.get("classes", []))))
    reading = "".join(
        "<li><strong>%s</strong><span class='why'>%s</span></li>" % (esc(a), esc(b)) for a, b in GLOBAL_READING)
    return (
        "<section class='module' id='overview'><h2>总览与依赖</h2>"
        "<p class='mod-summary'>%s</p>"
        "<div class='diagram'>%s</div>"
        "<div class='tiles'>%s</div></section>"
        "<section class='module' id='reading'><h2>全局阅读路径</h2>"
        "<p class='mod-summary'>按依赖方向从底向上读，每层只需要上一层的词汇。各模块小节里另有文件级的建议顺序。</p>"
        "<ol class='global-reading'>%s</ol></section>"
        % (esc(INTRO), dep_svg(), "".join(tiles), reading))


CSS = """
:root {
  --bg:#F5F6F8; --surface:#FFFFFF; --ink:#1B222B; --muted:#5C6673;
  --line:#E2E6EA; --accent:#A8742C; --accent-ink:#7C5316; --accent-soft:rgba(168,116,44,.09);
  --code-bg:#F0F2F5; --card-shadow:0 1px 2px rgba(27,34,43,.04);
  --k-class-fg:#2F5FA3; --k-class-bg:rgba(63,111,180,.12);
  --k-struct-fg:#22706A; --k-struct-bg:rgba(46,133,119,.12);
  --k-enum-fg:#6A4FA3; --k-enum-bg:rgba(123,95,176,.12);
  --k-functions-fg:#49702F; --k-functions-bg:rgba(79,125,58,.12);
  --k-alias-fg:#5C6673; --k-alias-bg:rgba(107,116,128,.12);
  --k-concept-fg:#5C6673; --k-concept-bg:rgba(107,116,128,.12);
}
@media (prefers-color-scheme: dark) { :root {
  --bg:#14171C; --surface:#1B2026; --ink:#E7EAEE; --muted:#96A0AB;
  --line:#2A313A; --accent:#D9A45E; --accent-ink:#E5B876; --accent-soft:rgba(217,164,94,.10);
  --code-bg:#12151A; --card-shadow:none;
  --k-class-fg:#7FA8DC; --k-class-bg:rgba(127,168,220,.14);
  --k-struct-fg:#6FBFB2; --k-struct-bg:rgba(111,191,178,.13);
  --k-enum-fg:#AC93DC; --k-enum-bg:rgba(172,147,220,.14);
  --k-functions-fg:#8FBB7A; --k-functions-bg:rgba(143,187,122,.13);
  --k-alias-fg:#96A0AB; --k-alias-bg:rgba(150,160,171,.13);
  --k-concept-fg:#96A0AB; --k-concept-bg:rgba(150,160,171,.13);
} }
:root[data-theme="light"] {
  --bg:#F5F6F8; --surface:#FFFFFF; --ink:#1B222B; --muted:#5C6673;
  --line:#E2E6EA; --accent:#A8742C; --accent-ink:#7C5316; --accent-soft:rgba(168,116,44,.09);
  --code-bg:#F0F2F5; --card-shadow:0 1px 2px rgba(27,34,43,.04);
  --k-class-fg:#2F5FA3; --k-class-bg:rgba(63,111,180,.12);
  --k-struct-fg:#22706A; --k-struct-bg:rgba(46,133,119,.12);
  --k-enum-fg:#6A4FA3; --k-enum-bg:rgba(123,95,176,.12);
  --k-functions-fg:#49702F; --k-functions-bg:rgba(79,125,58,.12);
  --k-alias-fg:#5C6673; --k-alias-bg:rgba(107,116,128,.12);
  --k-concept-fg:#5C6673; --k-concept-bg:rgba(107,116,128,.12);
}
:root[data-theme="dark"] {
  --bg:#14171C; --surface:#1B2026; --ink:#E7EAEE; --muted:#96A0AB;
  --line:#2A313A; --accent:#D9A45E; --accent-ink:#E5B876; --accent-soft:rgba(217,164,94,.10);
  --code-bg:#12151A; --card-shadow:none;
  --k-class-fg:#7FA8DC; --k-class-bg:rgba(127,168,220,.14);
  --k-struct-fg:#6FBFB2; --k-struct-bg:rgba(111,191,178,.13);
  --k-enum-fg:#AC93DC; --k-enum-bg:rgba(172,147,220,.14);
  --k-functions-fg:#8FBB7A; --k-functions-bg:rgba(143,187,122,.13);
  --k-alias-fg:#96A0AB; --k-alias-bg:rgba(150,160,171,.13);
  --k-concept-fg:#96A0AB; --k-concept-bg:rgba(150,160,171,.13);
}
* { box-sizing:border-box; }
body {
  margin:0; background:var(--bg); color:var(--ink);
  font:15px/1.65 -apple-system,"Segoe UI","Microsoft YaHei","PingFang SC","Noto Sans CJK SC",sans-serif;
}
code, .floc { font-family:"Cascadia Code",Consolas,"Sarasa Mono SC",ui-monospace,monospace; }
a { color:var(--accent-ink); text-decoration:none; }
a:hover { text-decoration:underline; }
a:focus-visible, input:focus-visible, summary:focus-visible { outline:2px solid var(--accent); outline-offset:2px; border-radius:3px; }

.layout { display:flex; min-height:100vh; }
.sidebar {
  width:280px; flex:0 0 280px; position:sticky; top:0; height:100vh; overflow-y:auto;
  border-right:1px solid var(--line); padding:20px 16px 32px; background:var(--surface);
  display:flex; flex-direction:column; gap:12px;
}
.brand { font-weight:650; letter-spacing:.01em; font-size:16px; }
.brand .sub { display:block; font-size:12px; font-weight:400; color:var(--muted); margin-top:2px; }
#search {
  width:100%; padding:7px 10px; border:1px solid var(--line); border-radius:6px;
  background:var(--bg); color:var(--ink); font:inherit; font-size:13.5px;
}
#search::placeholder { color:var(--muted); }
.match-count { font-size:12px; color:var(--muted); min-height:18px; }
.xlink { font-size:12.5px; color:var(--muted); }
.xlink:hover { color:var(--accent-ink); }
.nav { display:flex; flex-direction:column; gap:1px; }
.nav-item {
  display:flex; align-items:baseline; gap:8px; padding:6px 8px; border-radius:6px;
  color:var(--ink); font-size:13.5px;
}
.nav-item:hover { background:var(--accent-soft); text-decoration:none; }
.nav-item code { font-size:12.5px; }
.nav-hint { color:var(--muted); font-size:12px; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; flex:1; }
.nav-count { color:var(--muted); font-size:11.5px; font-variant-numeric:tabular-nums; }
.content { flex:1; min-width:0; padding:28px 40px 80px; }
.content-inner { max-width:980px; }

.module { margin-bottom:44px; scroll-margin-top:16px; }
.module h2 {
  font-size:21px; font-weight:650; margin:0; letter-spacing:.005em; text-wrap:balance;
}
.mod-head { display:flex; align-items:baseline; gap:12px; border-bottom:2px solid var(--line); padding-bottom:8px; margin-bottom:12px; }
#overview h2, #reading h2 { border-bottom:2px solid var(--line); padding-bottom:8px; margin-bottom:12px; }
.mod-count { color:var(--muted); font-size:12.5px; font-variant-numeric:tabular-nums; white-space:nowrap; }
.mod-summary { color:var(--ink); max-width:70ch; margin:6px 0 14px; }
.not-owned, .reading { margin:0 0 14px; font-size:14px; }
.not-owned summary, .reading summary { cursor:pointer; color:var(--muted); font-size:13px; letter-spacing:.04em; }
.not-owned ul, .reading ol { margin:8px 0 0; padding-left:22px; color:var(--muted); }
.reading ol li { margin-bottom:3px; }
.why { color:var(--muted); margin-left:8px; }

.diagram { border:1px solid var(--line); border-radius:8px; background:var(--surface); padding:12px; overflow-x:auto; margin-bottom:18px; }
.tiles { display:grid; grid-template-columns:repeat(auto-fill,minmax(280px,1fr)); gap:10px; }
.tile {
  display:block; border:1px solid var(--line); border-radius:8px; background:var(--surface);
  padding:12px 14px; color:var(--ink); box-shadow:var(--card-shadow);
}
.tile:hover { border-color:var(--accent); text-decoration:none; }
.tile h3 { margin:0 0 4px; font-size:14px; font-weight:650; }
.tile p { margin:0 0 6px; font-size:13px; color:var(--muted); }
.tile-count { font-size:11.5px; color:var(--muted); font-variant-numeric:tabular-nums; }
.global-reading { padding-left:22px; max-width:70ch; }
.global-reading li { margin-bottom:8px; }

.flow { border:1px solid var(--line); border-left:3px solid var(--accent); border-radius:8px; background:var(--surface); padding:12px 16px; margin:0 0 14px; }
.flow h4 { margin:0 0 6px; font-size:14px; font-weight:650; }
.flow ol { margin:0; padding-left:20px; font-size:13.5px; }
.flow li { margin-bottom:3px; }

.cards { display:flex; flex-direction:column; gap:10px; }
.card {
  border:1px solid var(--line); border-radius:8px; background:var(--surface);
  padding:12px 16px 10px; box-shadow:var(--card-shadow); scroll-margin-top:16px;
}
.card:target { border-color:var(--accent); background:var(--accent-soft); }
.card-head { display:flex; align-items:baseline; gap:10px; flex-wrap:wrap; }
.kind {
  font-size:11px; font-weight:600; letter-spacing:.06em; padding:1px 7px; border-radius:99px;
  font-family:"Cascadia Code",Consolas,ui-monospace,monospace;
}
.kind-class { color:var(--k-class-fg); background:var(--k-class-bg); }
.kind-struct { color:var(--k-struct-fg); background:var(--k-struct-bg); }
.kind-enum { color:var(--k-enum-fg); background:var(--k-enum-bg); }
.kind-functions { color:var(--k-functions-fg); background:var(--k-functions-bg); }
.kind-alias { color:var(--k-alias-fg); background:var(--k-alias-bg); }
.kind-concept { color:var(--k-concept-fg); background:var(--k-concept-bg); }
.cname { margin:0; font-size:15px; font-weight:650; }
.cname code { background:none; }
.floc { margin-left:auto; font-size:12px; color:var(--muted); white-space:nowrap; }
.floc:hover { color:var(--accent-ink); }
.floc .lno { font-variant-numeric:tabular-nums; }
.floc-inline { margin-left:6px; }
.resp { margin:6px 0 8px; max-width:75ch; }
.members { border-collapse:collapse; width:100%; font-size:13px; margin-bottom:8px; }
.members td { border-top:1px solid var(--line); padding:4px 12px 4px 0; vertical-align:top; }
.members td.sig { max-width:56ch; }
.members td.sig > code {
  display:inline-block; background:var(--code-bg); padding:3px 10px; border-radius:5px;
  font-size:12.5px; line-height:1.55; white-space:pre; overflow-x:auto; max-width:100%;
}
.members td.note { color:var(--muted); }
.lifetime { font-size:13px; color:var(--muted); margin:4px 0 8px; max-width:75ch; }
.rel-label {
  font-size:10.5px; letter-spacing:.09em; color:var(--muted); text-transform:uppercase;
  margin-right:8px; font-weight:600;
}
.related { display:flex; align-items:baseline; gap:6px; flex-wrap:wrap; margin-bottom:2px; }
.chip {
  font-size:11.5px; font-family:"Cascadia Code",Consolas,ui-monospace,monospace;
  background:var(--code-bg); border:1px solid var(--line); border-radius:99px; padding:1px 8px;
  color:var(--ink);
}
a.chip:hover { border-color:var(--accent); text-decoration:none; }
.chip-dead { color:var(--muted); }
.hidden { display:none; }

@media (max-width: 900px) {
  .layout { flex-direction:column; }
  .sidebar { position:static; width:auto; height:auto; flex:none; border-right:none; border-bottom:1px solid var(--line); }
  .content { padding:20px 16px 60px; }
  .floc { margin-left:0; flex-basis:100%; }
}
@media (prefers-reduced-motion: no-preference) { html { scroll-behavior:smooth; } }
"""

JS = """
(function () {
  var input = document.getElementById('search');
  var count = document.getElementById('match-count');
  var cards = Array.prototype.slice.call(document.querySelectorAll('.card'));
  var sections = Array.prototype.slice.call(document.querySelectorAll('.module[data-mtitle]'));
  function apply() {
    var q = input.value.trim().toLowerCase();
    if (!q) {
      cards.forEach(function (c) { c.classList.remove('hidden'); });
      sections.forEach(function (s) { s.classList.remove('hidden'); });
      document.getElementById('overview').classList.remove('hidden');
      document.getElementById('reading').classList.remove('hidden');
      count.textContent = '';
      return;
    }
    document.getElementById('overview').classList.add('hidden');
    document.getElementById('reading').classList.add('hidden');
    var shown = 0;
    sections.forEach(function (s) {
      var any = false;
      var titleHit = (s.getAttribute('data-mtitle') || '').indexOf(q) !== -1;
      s.querySelectorAll('.card').forEach(function (c) {
        var hit = titleHit || (c.getAttribute('data-search') || '').indexOf(q) !== -1;
        c.classList.toggle('hidden', !hit);
        if (hit) { any = true; shown += 1; }
      });
      s.classList.toggle('hidden', !any);
    });
    count.textContent = shown + ' 个类型匹配';
  }
  input.addEventListener('input', apply);
  document.addEventListener('keydown', function (e) {
    if (e.key === '/' && document.activeElement !== input) { e.preventDefault(); input.focus(); }
    if (e.key === 'Escape' && document.activeElement === input) { input.value = ''; apply(); input.blur(); }
  });
})();
"""


def main():
    artifact_out = None
    args = sys.argv[1:]
    if "--artifact-out" in args:
        artifact_out = Path(args[args.index("--artifact-out") + 1])
    mods = load_modules()
    registry = {slug(c["name"]) for m in mods for c in m.get("classes", [])}
    total = sum(len(m.get("classes", [])) for m in mods)
    body = [
        "<title>UmbraFlow 代码地图集</title>",
        "<style>%s</style>" % CSS,
        "<div class='layout'>",
        "<aside class='sidebar'>",
        "<div class='brand'>UmbraFlow 代码地图集<span class='sub'>%d 个类型 · 按 / 搜索</span></div>" % total,
        "<a class='xlink' href='tour.html'>配套阅读：深度导读 →</a>",
        "<input id='search' type='search' placeholder='搜索类型、成员、文件…' autocomplete='off'>",
        "<div class='match-count' id='match-count'></div>",
        "<nav class='nav'>%s</nav>" % render_nav(mods),
        "</aside>",
        "<main class='content'><div class='content-inner'>",
        render_overview(mods),
        "".join(render_module(m, registry) for m in mods),
        "</div></main></div>",
        "<script>%s</script>" % JS,
    ]
    content = "\n".join(body)
    local = (
        "<!doctype html>\n<html lang='zh-CN'>\n<head>\n<meta charset='utf-8'>\n"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>\n"
        "<title>UmbraFlow 代码地图集</title>\n"
        "</head>\n<body>\n" + content + "\n</body>\n</html>\n"
    )
    LOCAL_OUT.write_text(local, encoding="utf-8")
    print("wrote %s (%d modules, %d classes)" % (LOCAL_OUT, len(mods), total))
    if artifact_out:
        artifact_out.write_text(content, encoding="utf-8")
        print("wrote artifact content %s" % artifact_out)


if __name__ == "__main__":
    main()
