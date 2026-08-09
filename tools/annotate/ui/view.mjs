import { QUEUE_CATEGORIES, queueCounts } from "./queue.mjs";

const escapeHtml = (value) => String(value ?? "").replace(/[&<>"']/g, (character) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[character]));
const list = (values, empty = "None recorded") => values?.length
  ? values.map((value) => `<span class="chip">${escapeHtml(value)}</span>`).join("")
  : `<span class="muted">${empty}</span>`;

function cardHtml(item, drafts, editing) {
  const patch = item.patch;
  const draft = drafts[item.patch?.id] ?? patch?.summary ?? "";
  const confidence = item.confidence == null ? "—" : `${Math.round(item.confidence * 100)}%`;
  const unsafe = item.unsafe ? `<span class="risk risk-critical">UNSAFE ACTION</span>` : "";
  const decisionButtons = patch && ["proposed"].includes(patch.status) ? `
    <div class="decision-actions">
      <button class="button button-primary" data-action="accept" data-patch-id="${escapeHtml(patch.id)}">Accept</button>
      <button class="button button-danger" data-action="reject" data-patch-id="${escapeHtml(patch.id)}">Reject</button>
      <button class="button button-quiet" data-action="edit" data-patch-id="${escapeHtml(patch.id)}">${editing ? "Close edit" : "Edit"}</button>
    </div>` : `<span class="review-state">${escapeHtml(item.status)}</span>`;
  const editor = editing && patch ? `
    <div class="editor">
      <label>Local semantic patch draft
        <textarea data-draft-id="${escapeHtml(patch.id)}">${escapeHtml(draft)}</textarea>
      </label>
      <button class="button button-secondary" data-action="save-draft" data-patch-id="${escapeHtml(patch.id)}">Save local draft</button>
    </div>` : "";
  return `<article class="queue-card ${item.unsafe ? "is-unsafe" : ""}">
    <div class="card-heading"><div><span class="eyebrow">${escapeHtml(item.category)}</span><h3>${escapeHtml(item.title)}</h3></div>${unsafe}</div>
    <div class="card-grid">
      <div><span class="field-label">Supporting frames</span><div class="chips">${list(item.supportingFrames)}</div></div>
      <div><span class="field-label">Competing Surface candidates</span><div class="chips">${list(item.competingSurfaces, "No competing Surface recorded")}</div></div>
      <div><span class="field-label">Semantic patch</span><p>${escapeHtml(item.semanticPatch)}</p></div>
      <div><span class="field-label">Expected runtime effect</span><p>${escapeHtml(item.runtimeEffect)}</p></div>
      <div><span class="field-label">Validation status</span><p class="status-line"><span class="status-dot ${item.validationStatus === "Validated" ? "is-good" : "is-blocked"}"></span>${escapeHtml(item.validationStatus)}</p></div>
      <div><span class="field-label">Blast radius</span><p class="blast ${item.unsafe ? "blast-critical" : ""}">${escapeHtml(item.blastRadius)}</p></div>
    </div>
    ${editor}
    <div class="card-footer">${decisionButtons}<span class="confidence">Confidence <strong>${confidence}</strong></span></div>
  </article>`;
}

export function renderApp(root, state) {
  const counts = queueCounts(state.queue || {});
  const active = state.activeCategory || "proposals";
  const items = state.queue?.[active] || [];
  const activeLabel = QUEUE_CATEGORIES.find((category) => category.id === active)?.label || active;
  const error = state.error ? `<div class="alert alert-error">${escapeHtml(state.error)}</div>` : "";
  const notice = state.notice ? `<div class="alert alert-info">${escapeHtml(state.notice)}</div>` : "";
  const loading = state.loading ? `<div class="loading">Loading P5 CandidateModel…</div>` : "";
  const nav = QUEUE_CATEGORIES.map(({ id, label }) => `<button class="nav-item ${id === active ? "is-active" : ""}" data-category="${id}"><span>${label}</span><b>${counts[id]}</b></button>`).join("");
  const compile = state.compile;
  const compileSummary = compile ? `${compile.valid ? "Compile passed" : "Compile blocked"} · ${compile.diagnostics.length} diagnostic(s)` : "Not run in this session";
  root.innerHTML = `<div class="shell">
    <header class="topbar"><div><div class="brand-mark">UF / P6</div><h1>CandidateModel decision queue</h1><p class="subtitle">Human review boundary for Agent proposals before runtime compilation.</p></div><div class="connection"><span class="connection-dot ${state.source === "fixture" ? "fixture" : "backend"}"></span>${state.source === "fixture" ? "Fixture adapter" : "P5 API"}</div></header>
    ${error}${notice}${loading}
    <div class="workspace">
      <aside class="sidebar"><div class="sidebar-label">Review queue</div>${nav}<div class="sidebar-foot"><span>Candidate revision</span><strong>${state.model?.revision ?? "—"}</strong><span>${escapeHtml(state.model?.project_id || "No candidate loaded")}</span></div></aside>
      <main class="content">
        <section class="overview"><div><span class="eyebrow">${escapeHtml(activeLabel)}</span><h2>${escapeHtml(state.model?.id || "Loading candidate")}</h2><p>Every decision is scoped to the reviewed revision. Individual approval is available; this queue has no bulk approval path.</p></div><div class="toolbar"><button class="button button-secondary" data-action="reload">Reload</button><button class="button button-primary" data-action="validate">Validate</button><button class="button button-secondary" data-action="compile">Compile / replay</button></div></section>
        <section class="metrics"><div><span>Open conflicts</span><strong>${state.model?.conflicts?.filter((conflict) => conflict.status === "open").length ?? "—"}</strong></div><div><span>Pending patches</span><strong>${state.model?.patches?.filter((patch) => patch.status === "proposed").length ?? "—"}</strong></div><div><span>Replay coverage</span><strong>${state.validation ? `${state.validation.coverage.resolved}/${state.validation.coverage.frames}` : "—"}</strong></div><div><span>Compile</span><strong>${escapeHtml(compileSummary.split(" · ")[0])}</strong></div></section>
        <section class="queue-list"><div class="section-heading"><div><h2>${escapeHtml(activeLabel)}</h2><span class="muted">${items.length} review item${items.length === 1 ? "" : "s"}</span></div></div>${items.length ? items.map((item) => cardHtml(item, state.drafts, state.editingPatchId === item.patch?.id)).join("") : `<div class="empty"><strong>Nothing in this queue.</strong><span>New evidence or Agent findings will appear here after the next P5 refresh.</span></div>`}</section>
      </main>
    </div>
  </div>`;
}
