import { FetchP5ApiAdapter } from "./api.mjs";
import { FixtureP5ApiAdapter } from "./fixture.mjs";
import { buildQueue } from "./queue.mjs";
import { renderApp } from "./view.mjs";

function chooseAdapter() {
  const params = new URLSearchParams(window.location.search);
  return params.get("backend") === "1" ? { adapter: new FetchP5ApiAdapter(), source: "backend" } : { adapter: new FixtureP5ApiAdapter(), source: "fixture" };
}

export async function startAnnotator({ root = document.querySelector("#annotator"), adapter, source } = {}) {
  if (!root) throw new Error("Annotator root not found");
  const selected = adapter ? { adapter, source: source || "backend" } : chooseAdapter();
  const state = { adapter: selected.adapter, source: selected.source, queue: {}, drafts: {}, activeCategory: "proposals", loading: true, error: "", notice: "", editingPatchId: null };
  const draw = () => renderApp(root, state);
  draw();

  async function load() {
    state.loading = true; state.error = ""; draw();
    try {
      const list = await state.adapter.listCandidates();
      if (!list.items.length) throw new Error("P5 returned no CandidateModel records");
      state.model = await state.adapter.getCandidate(list.items[0].id);
      state.validation = await state.adapter.validate(state.model.id, state.model.revision);
      state.compile = state.compile || null;
      state.queue = buildQueue(state.model, state.validation);
      state.notice = "Loaded typed P5 CandidateModel data.";
    } catch (error) {
      state.error = error?.message || String(error);
    } finally {
      state.loading = false; draw();
    }
  }

  async function decide(patchId, operation) {
    if (!state.model) return;
    state.error = ""; state.notice = ""; draw();
    try {
      const body = { candidate_revision: state.model.revision, actor: "annotator-ui" };
      await state.adapter[operation === "accept" ? "acceptPatch" : "rejectPatch"](state.model.id, patchId, body);
      await load();
      state.notice = `${operation === "accept" ? "Accepted" : "Rejected"} patch ${patchId} at the reviewed revision.`;
      draw();
    } catch (error) {
      state.error = error?.message || String(error); draw();
    }
  }

  root.addEventListener("click", async (event) => {
    const button = event.target.closest("button");
    if (!button) return;
    const { action, category, patchId } = button.dataset;
    if (category) { state.activeCategory = category; state.editingPatchId = null; draw(); return; }
    if (action === "reload") { await load(); return; }
    if (action === "validate") {
      try { state.validation = await state.adapter.validate(state.model.id, state.model.revision); state.queue = buildQueue(state.model, state.validation); state.notice = "Validation refreshed from P5."; state.error = ""; }
      catch (error) { state.error = error?.message || String(error); }
      draw(); return;
    }
    if (action === "compile") {
      try { state.validation = await state.adapter.validate(state.model.id, state.model.revision); state.compile = await state.adapter.compile(state.model.id, state.model.revision, false); state.queue = buildQueue(state.model, state.validation); state.notice = "Compile / replay requested with write=false."; state.error = ""; }
      catch (error) { state.error = error?.message || String(error); }
      draw(); return;
    }
    if (!patchId) return;
    if (action === "accept" || action === "reject") { await decide(patchId, action); return; }
    if (action === "edit") { state.editingPatchId = state.editingPatchId === patchId ? null : patchId; draw(); return; }
    if (action === "save-draft") {
      const input = root.querySelector(`[data-draft-id="${CSS.escape(patchId)}"]`);
      if (input) state.drafts[patchId] = input.value;
      state.editingPatchId = null; state.notice = "Saved as a local draft; P5 state is unchanged."; draw();
    }
  });
  await load();
  return state;
}
