const categoryDefinitions = [
  ["proposals", "Agent proposals"],
  ["conflicts", "Conflicts"],
  ["low-confidence", "Low-confidence"],
  ["missing-identity", "Missing identity evidence"],
  ["unsafe-actions", "Unsafe actions"],
  ["uncovered-frames", "Uncovered frames"],
  ["compile-replay", "Compile / replay"],
  ["review-history", "Review history"],
];

export const QUEUE_CATEGORIES = categoryDefinitions.map(([id, label]) => ({ id, label }));

const riskLabel = (risk) => ({ low: "Low", medium: "Medium", high: "High", critical: "Critical" }[risk] || "Unknown");

function runtimeEffect(patch) {
  if (!patch) return "No runtime patch attached; resolve the evidence before promotion.";
  const change = patch.change;
  switch (change.kind) {
    case "create_entity": return `Create ${change.entity_kind} ${change.value?.id || "candidate"}.`;
    case "set_field": return `Set ${change.entity_kind} ${change.entity_id}.${change.field}.`;
    case "grant_action": return `Authorize ${change.action?.kind || "typed"} action on ${change.binding_id}.`;
    case "revoke_action": return `Remove the action grant from ${change.binding_id}.`;
    case "merge_entities": return `Merge ${change.merged_ids.join(", ")} into ${change.survivor_id}.`;
    case "split_entity": return `Split ${change.source_id} into ${change.part_ids.join(", ")}.`;
    default: return "Review semantic effect.";
  }
}

function blastRadius(patch, conflict) {
  if (patch?.risk === "critical" || conflict?.severity === "critical") return "Critical · action authorization / runtime safety";
  if (patch?.risk === "high" || conflict?.severity === "error") return "High · Surface resolution or compile gate";
  if (patch?.change?.entity_kind === "surface") return "Medium · candidate Surface matching";
  return `${riskLabel(patch?.risk || "low")} · local candidate evidence`;
}

function validationStatus(validation, conflict, patch) {
  if (conflict && validation?.conflict_ids?.includes(conflict.id)) return "Blocked by open conflict";
  if (validation?.valid) return "Validated";
  if (patch?.status === "accepted" || patch?.status === "rejected") return `Reviewed · ${patch.status}`;
  return "Needs validation";
}

function surfaces(model, conflict) {
  const ids = conflict?.subject_ids || [];
  const entitySurfaceIds = model.entities
    .filter((entity) => entity.entity_kind === "surface")
    .map((entity) => entity.value?.id)
    .filter(Boolean);
  return [...new Set([...ids.filter((id) => entitySurfaceIds.includes(id)), ...entitySurfaceIds.filter((id) => ids.includes(id))])];
}

function card(category, title, { model, validation, conflict, patch, frames = model.source_frame_ids, confidence, unsafe = false }) {
  return {
    id: `${category}:${conflict?.id || patch?.id || "summary"}`,
    category,
    title,
    conflict,
    patch,
    supportingFrames: [...new Set(frames || [])],
    competingSurfaces: surfaces(model, conflict),
    semanticPatch: patch ? `${patch.change.kind}: ${patch.summary}` : conflict?.message || "Review candidate coverage and evidence.",
    runtimeEffect: runtimeEffect(patch),
    validationStatus: validationStatus(validation, conflict, patch),
    blastRadius: blastRadius(patch, conflict),
    confidence: confidence ?? patch?.confidence,
    unsafe,
    status: patch?.status || conflict?.status || "open",
  };
}

export function buildQueue(model, validation) {
  const conflicts = model.conflicts.filter((conflict) => conflict.status === "open");
  const patches = model.patches;
  const cards = Object.fromEntries(QUEUE_CATEGORIES.map(({ id }) => [id, []]));

  patches.filter((patch) => patch.status === "proposed").forEach((patch) => {
    cards.proposals.push(card("proposals", patch.summary, { model, validation, patch, confidence: patch.confidence,
      unsafe: patch.risk === "critical" || patch.change.kind === "grant_action" || patch.change.kind === "revoke_action" }));
  });
  conflicts.forEach((conflict) => {
    const patch = patches.find((item) => conflict.suggested_patch_ids?.includes(item.id));
    cards.conflicts.push(card("conflicts", conflict.message, { model, validation, conflict, patch }));
    if (conflict.kind === "missing_identity") cards["missing-identity"].push(card("missing-identity", conflict.message, { model, validation, conflict, patch }));
    if (conflict.kind === "invalid_action") cards["unsafe-actions"].push(card("unsafe-actions", conflict.message, { model, validation, conflict, patch, unsafe: true }));
    if (conflict.kind === "uncovered_frame") cards["uncovered-frames"].push(card("uncovered-frames", conflict.message, { model, validation, conflict, patch, frames: conflict.subject_ids }));
  });
  patches.filter((patch) => patch.status === "proposed" && patch.confidence < 0.8)
    .forEach((patch) => cards["low-confidence"].push(card("low-confidence", patch.summary, { model, validation, patch, confidence: patch.confidence })));
  patches.filter((patch) => patch.status === "proposed" && ["grant_action", "revoke_action"].includes(patch.change.kind))
    .forEach((patch) => cards["unsafe-actions"].push(card("unsafe-actions", patch.summary, { model, validation, patch, confidence: patch.confidence, unsafe: true })));

  cards["compile-replay"].push({
    id: "compile-replay:current",
    category: "compile-replay",
    title: validation?.valid ? "Candidate is ready for compile / replay" : "Compile / replay is gated by review findings",
    conflict: null,
    patch: null,
    supportingFrames: model.source_frame_ids,
    competingSurfaces: [],
    semanticPatch: "Validate the CandidateModel, then compile with write=false and inspect replay coverage.",
    runtimeEffect: "Produces a runtime model only after validation succeeds; no runtime write is requested by this UI.",
    validationStatus: validation?.valid ? "Validated" : `${validation?.conflict_ids?.length || conflicts.length} conflict(s) block compile`,
    blastRadius: "High · deployment model boundary",
    confidence: null,
    unsafe: false,
    status: validation?.valid ? "ready" : "blocked",
  });

  patches.filter((patch) => ["accepted", "rejected", "applied", "superseded"].includes(patch.status))
    .forEach((patch) => cards["review-history"].push(card("review-history", patch.summary, { model, validation, patch })));
  return cards;
}

export function queueCounts(queue) {
  return Object.fromEntries(QUEUE_CATEGORIES.map(({ id }) => [id, queue[id]?.length || 0]));
}
