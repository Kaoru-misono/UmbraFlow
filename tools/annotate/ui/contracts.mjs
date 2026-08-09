/**
 * The UI boundary mirrors the frozen P5 JSON shapes.  It intentionally keeps
 * validation small: the schemas remain the authority and this module only
 * prevents accidental use of an unrelated response at the transport edge.
 */

const isObject = (value) => value !== null && typeof value === "object" && !Array.isArray(value);
const has = (value, key) => Object.prototype.hasOwnProperty.call(value, key);
const isString = (value) => typeof value === "string" && value.length > 0;
const isInteger = (value) => Number.isInteger(value) && value >= 1;
const isArray = (value) => Array.isArray(value);

const hasCandidateShape = (value) => isObject(value)
  && isString(value.id)
  && isString(value.project_id)
  && isInteger(value.revision)
  && ["candidate", "accepted", "rejected", "compiled"].includes(value.status)
  && isArray(value.source_frame_ids)
  && isArray(value.targets)
  && isArray(value.entities)
  && isArray(value.patches)
  && isArray(value.conflicts);

const hasSummaryShape = (value) => isObject(value)
  && isString(value.id)
  && isString(value.project_id)
  && isInteger(value.revision)
  && typeof value.open_conflicts === "number"
  && typeof value.pending_patches === "number";

export function assertCandidateModel(value) {
  if (!hasCandidateShape(value)) throw new Error("P5 contract: expected CandidateModel");
  return value;
}

export function assertListCandidatesResponse(value) {
  if (!isObject(value) || !isArray(value.items) || !value.items.every(hasSummaryShape)) {
    throw new Error("P5 contract: expected ListCandidatesResponse");
  }
  return value;
}

export function assertDecisionResponse(value) {
  if (!isObject(value) || !isString(value.candidate_id) || !isString(value.patch_id)
    || !["accepted", "rejected"].includes(value.status) || !isInteger(value.revision)) {
    throw new Error("P5 contract: expected DecisionResponse");
  }
  return value;
}

export function assertValidationResponse(value) {
  if (!isObject(value) || !isString(value.candidate_id) || !isInteger(value.revision)
    || typeof value.valid !== "boolean" || !isArray(value.conflict_ids)
    || !isObject(value.coverage)) {
    throw new Error("P5 contract: expected ValidationResponse");
  }
  return value;
}

export function assertCompileResponse(value) {
  if (!isObject(value) || !isString(value.candidate_id) || !isInteger(value.revision)
    || typeof value.valid !== "boolean" || !isArray(value.diagnostics)) {
    throw new Error("P5 contract: expected CompileResponse");
  }
  return value;
}

export function assertSchemaManifest(value) {
  if (!isObject(value) || value.api_version !== 1 || !isString(value.runtime_schema)
    || !isString(value.offline_schema) || !isArray(value.capabilities)) {
    throw new Error("P5 contract: expected SchemaManifest");
  }
  return value;
}

export function cloneJson(value) {
  return JSON.parse(JSON.stringify(value));
}

export function own(value, key) {
  return isObject(value) && has(value, key);
}
