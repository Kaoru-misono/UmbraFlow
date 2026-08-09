import { cloneJson } from "./contracts.mjs";

const provenance = (agent, runId) => ({
  run_id: runId,
  agent,
  agent_version: "fixture-1",
  created_at: "2026-08-09T04:00:00Z",
});

const target = {
  id: "confirm-button",
  kind: "control",
  geometry: { kind: "fixed", rect: [120, 740, 280, 72] },
  locators: ["confirm-button-template"],
  readers: ["confirm-button-text"],
};

export function fixtureCandidate() {
  return {
    id: "candidate-chaos-01",
    project_id: "uf-chaos",
    revision: 7,
    status: "candidate",
    source_frame_ids: ["frame-training-confirm", "frame-dark-modal", "frame-battle-inspect"],
    targets: [{
      candidate_id: "target-confirm-button",
      state: "proposed",
      value: target,
      confidence: 0.96,
      evidence_ids: ["obs-confirm-template"],
      provenance: provenance("perception-agent", "run-perception-17"),
    }],
    entities: [
      {
        candidate_id: "surface-training-confirm",
        entity_kind: "surface",
        state: "proposed",
        value: { id: "training-confirm", kind: "overlay", contexts: ["camp"], covers: ["camp-scene"] },
        confidence: 0.93,
        evidence_ids: ["obs-confirm-surface"],
        conflict_ids: ["conflict-surface-match"],
        provenance: provenance("structure-agent", "run-structure-8"),
      },
      {
        candidate_id: "surface-dark-modal",
        entity_kind: "surface",
        state: "proposed",
        value: { id: "dark-modal", kind: "interrupt", contexts: [] },
        confidence: 0.55,
        evidence_ids: ["obs-dark-modal"],
        conflict_ids: ["conflict-unknown-evidence", "conflict-dark-uncovered"],
        provenance: provenance("structure-agent", "run-structure-8"),
      },
      {
        candidate_id: "binding-confirm-action",
        entity_kind: "binding",
        state: "proposed",
        value: { id: "training-confirm-confirm-button", surface: "training-confirm", target: "confirm-button" },
        confidence: 0.61,
        evidence_ids: ["obs-confirm-template"],
        conflict_ids: ["conflict-invalid-action"],
        provenance: provenance("contract-agent", "run-contract-5"),
      },
    ],
    patches: [
      {
        id: "patch-create-training-confirm",
        change: { kind: "create_entity", entity_kind: "surface", value: { id: "training-confirm", kind: "overlay" } },
        summary: "Create the training confirmation overlay with explicit camp coverage.",
        confidence: 0.93,
        risk: "medium",
        evidence_ids: ["obs-confirm-surface"],
        conflict_ids: ["conflict-surface-match"],
        provenance: provenance("contract-agent", "run-contract-5"),
        status: "proposed",
      },
      {
        id: "patch-confirm-action",
        change: { kind: "grant_action", binding_id: "training-confirm-confirm-button", action: { id: "confirm", kind: "click" } },
        summary: "Grant a click action to the confirmation control.",
        confidence: 0.61,
        risk: "critical",
        evidence_ids: ["obs-confirm-template"],
        conflict_ids: ["conflict-invalid-action"],
        provenance: provenance("contract-agent", "run-contract-5"),
        status: "proposed",
      },
      {
        id: "patch-confirm-identity",
        change: { kind: "set_field", entity_kind: "surface", entity_id: "training-confirm", field: "identity.all", value: [] },
        summary: "Add positive identity evidence before treating the overlay as actionable.",
        confidence: 0.42,
        risk: "high",
        evidence_ids: ["obs-confirm-surface"],
        conflict_ids: ["conflict-missing-identity"],
        provenance: provenance("repair-agent", "run-repair-2"),
        status: "proposed",
      },
      {
        id: "patch-record-review",
        change: { kind: "set_field", entity_kind: "surface", entity_id: "camp-scene", field: "contexts", value: ["camp"] },
        summary: "Keep the reviewed camp scene context membership.",
        confidence: 0.99,
        risk: "low",
        evidence_ids: ["obs-camp-scene"],
        provenance: provenance("reviewer", "run-review-1"),
        status: "accepted",
      },
      {
        id: "patch-rejected-experiment",
        change: { kind: "merge_entities", survivor_id: "training-confirm", merged_ids: ["dark-modal"] },
        summary: "Merge the dark modal into the training confirmation surface.",
        confidence: 0.34,
        risk: "critical",
        evidence_ids: ["obs-dark-modal"],
        conflict_ids: ["conflict-unknown-evidence"],
        provenance: provenance("reviewer", "run-review-1"),
        status: "rejected",
      },
    ],
    conflicts: [
      {
        id: "conflict-surface-match",
        kind: "multiple_surface_match",
        severity: "warning",
        subject_ids: ["training-confirm", "camp-scene"],
        message: "The confirmation frame matches two Surface candidates without a sufficient margin.",
        evidence_ids: ["obs-confirm-surface"],
        suggested_patch_ids: ["patch-create-training-confirm"],
        status: "open",
      },
      {
        id: "conflict-missing-identity",
        kind: "missing_identity",
        severity: "error",
        subject_ids: ["training-confirm"],
        message: "No reviewed positive identity predicate distinguishes this Surface.",
        evidence_ids: ["obs-confirm-surface"],
        suggested_patch_ids: ["patch-confirm-identity"],
        status: "open",
      },
      {
        id: "conflict-invalid-action",
        kind: "invalid_action",
        severity: "critical",
        subject_ids: ["training-confirm-confirm-button", "patch-confirm-action"],
        message: "The proposed click has no validated locator and must not be authorized yet.",
        evidence_ids: ["obs-confirm-template"],
        suggested_patch_ids: ["patch-confirm-action"],
        status: "open",
      },
      {
        id: "conflict-unknown-evidence",
        kind: "unknown_evidence",
        severity: "warning",
        subject_ids: ["dark-modal"],
        message: "The dark modal evidence is low confidence; it cannot be promoted automatically.",
        evidence_ids: ["obs-dark-modal"],
        suggested_patch_ids: ["patch-rejected-experiment"],
        status: "open",
      },
      {
        id: "conflict-dark-uncovered",
        kind: "uncovered_frame",
        severity: "error",
        subject_ids: ["frame-dark-modal"],
        message: "No accepted Surface candidate explains the dark modal frame.",
        evidence_ids: ["obs-dark-modal"],
        status: "open",
      },
    ],
  };
}

export class FixtureP5ApiAdapter {
  constructor(candidate = fixtureCandidate()) {
    this.candidate = cloneJson(candidate);
  }

  async getSchema() {
    return {
      api_version: 1,
      runtime_schema: "umbraflow-runtime-v1.schema.json",
      offline_schema: "umbraflow-offline-v1.schema.json",
      capabilities: ["candidate-review", "compile", "validate", "replay-coverage"],
    };
  }

  async listCandidates() {
    const { id, project_id, revision, status, conflicts, patches } = this.candidate;
    return { items: [{ id, project_id, revision, status,
      open_conflicts: conflicts.filter((item) => item.status === "open").length,
      pending_patches: patches.filter((item) => item.status === "proposed").length }] };
  }

  async getCandidate() { return cloneJson(this.candidate); }
  async getConflicts() { return cloneJson(this.candidate.conflicts); }

  async decide(candidateId, patchId, operation, body) {
    if (candidateId !== this.candidate.id) throw new Error("Fixture candidate not found");
    if (body.candidate_revision !== this.candidate.revision) {
      throw new Error("revision_conflict: reload the candidate before deciding");
    }
    const patch = this.candidate.patches.find((item) => item.id === patchId);
    if (!patch) throw new Error("Fixture patch not found");
    patch.status = operation === "accept" ? "accepted" : "rejected";
    this.candidate.revision += 1;
    return { candidate_id: candidateId, patch_id: patchId, status: patch.status, revision: this.candidate.revision };
  }

  acceptPatch(candidateId, patchId, body) { return this.decide(candidateId, patchId, "accept", body); }
  rejectPatch(candidateId, patchId, body) { return this.decide(candidateId, patchId, "reject", body); }

  async validate(candidateId, revision) {
    const open = this.candidate.conflicts.filter((item) => item.status === "open").map((item) => item.id);
    return { candidate_id: candidateId, revision, valid: open.length === 0,
      conflict_ids: open, coverage: { frames: 3, resolved: 1, ambiguous: 1, unknown: 1 } };
  }

  async compile(candidateId, candidateRevision, write = false) {
    const open = this.candidate.conflicts.filter((item) => item.status === "open");
    return { candidate_id: candidateId, revision: candidateRevision, valid: open.length === 0,
      diagnostics: open.map((item) => ({ severity: item.severity === "critical" ? "error" : "warning", path: item.id, message: item.message })),
      ...(open.length === 0 && !write ? { runtime_model_hash: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" } : {}) };
  }
}
