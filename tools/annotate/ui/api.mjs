import {
  assertCandidateModel,
  assertCompileResponse,
  assertDecisionResponse,
  assertListCandidatesResponse,
  assertSchemaManifest,
  assertValidationResponse,
  cloneJson,
} from "./contracts.mjs";

export class P5ApiError extends Error {
  constructor(message, { code = "transport_error", retryable = false, details } = {}) {
    super(message);
    this.name = "P5ApiError";
    this.code = code;
    this.retryable = retryable;
    this.details = details;
  }
}

async function jsonResponse(response) {
  let body;
  try {
    body = await response.json();
  } catch {
    throw new P5ApiError(`P5 returned non-JSON response (${response.status})`, { retryable: response.status >= 500 });
  }
  if (!response.ok) {
    if (body && typeof body.code === "string" && typeof body.message === "string") {
      throw new P5ApiError(body.message, body);
    }
    throw new P5ApiError(`P5 request failed (${response.status})`, { retryable: response.status >= 500 });
  }
  return body;
}

export class FetchP5ApiAdapter {
  constructor(baseUrl = "") {
    this.baseUrl = baseUrl.replace(/\/$/, "");
  }

  async request(path, init = {}) {
    const response = await fetch(`${this.baseUrl}${path}`, {
      ...init,
      headers: { Accept: "application/json", "Content-Type": "application/json", ...(init.headers || {}) },
    });
    return jsonResponse(response);
  }

  async getSchema() {
    return assertSchemaManifest(await this.request("/api/schema"));
  }

  async listCandidates({ status, cursor } = {}) {
    const params = new URLSearchParams();
    if (status) params.set("status", status);
    if (cursor) params.set("cursor", cursor);
    const suffix = params.toString() ? `?${params}` : "";
    return assertListCandidatesResponse(await this.request(`/api/candidates${suffix}`));
  }

  async getCandidate(candidateId) {
    return assertCandidateModel(await this.request(`/api/candidates/${encodeURIComponent(candidateId)}`));
  }

  async decide(candidateId, patchId, operation, body) {
    const response = await this.request(
      `/api/candidates/${encodeURIComponent(candidateId)}/patches/${encodeURIComponent(patchId)}/${operation}`,
      { method: "POST", body: JSON.stringify(body) },
    );
    return assertDecisionResponse(response);
  }

  acceptPatch(candidateId, patchId, body) { return this.decide(candidateId, patchId, "accept", body); }
  rejectPatch(candidateId, patchId, body) { return this.decide(candidateId, patchId, "reject", body); }

  async validate(candidateId, revision) {
    return assertValidationResponse(await this.request(`/api/candidates/${encodeURIComponent(candidateId)}/validate`, {
      method: "POST",
      body: JSON.stringify(revision),
    }));
  }

  async compile(candidateId, candidateRevision, write = false) {
    return assertCompileResponse(await this.request(`/api/candidates/${encodeURIComponent(candidateId)}/compile`, {
      method: "POST",
      body: JSON.stringify({ candidate_revision: candidateRevision, write }),
    }));
  }

  async getConflicts(candidateId) {
    const candidate = await this.getCandidate(candidateId);
    return cloneJson(candidate.conflicts);
  }
}
