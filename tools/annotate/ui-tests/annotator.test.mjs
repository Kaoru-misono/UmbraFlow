import assert from "node:assert/strict";
import test from "node:test";
import { FixtureP5ApiAdapter } from "../ui/fixture.mjs";
import { assertCandidateModel } from "../ui/contracts.mjs";
import { buildQueue, QUEUE_CATEGORIES } from "../ui/queue.mjs";

test("fixture implements the typed P5 CandidateModel contract", async () => {
  const adapter = new FixtureP5ApiAdapter();
  const list = await adapter.listCandidates();
  assert.equal(list.items.length, 1);
  const candidate = assertCandidateModel(await adapter.getCandidate(list.items[0].id));
  assert.equal(candidate.status, "candidate");
  assert.equal(candidate.revision, 7);
  assert.ok(candidate.patches.length >= 3);
});

test("decision queue exposes every required review lane and evidence fields", async () => {
  const adapter = new FixtureP5ApiAdapter();
  const candidate = await adapter.getCandidate();
  const validation = await adapter.validate(candidate.id, candidate.revision);
  const queue = buildQueue(candidate, validation);
  assert.deepEqual(Object.keys(queue), QUEUE_CATEGORIES.map(({ id }) => id));
  for (const item of queue.proposals) {
    assert.ok(item.supportingFrames.length);
    assert.ok("competingSurfaces" in item);
    assert.ok(item.semanticPatch);
    assert.ok(item.runtimeEffect);
    assert.ok(item.validationStatus);
    assert.ok(item.blastRadius);
  }
  assert.ok(queue["unsafe-actions"].some((item) => item.unsafe));
  assert.ok(queue["missing-identity"].length > 0);
  assert.ok(queue["uncovered-frames"].length > 0);
  assert.ok(queue["compile-replay"].length === 1);
  assert.ok(queue["review-history"].length > 0);
});

test("decisions use optimistic candidate revision and never add a bulk operation", async () => {
  const adapter = new FixtureP5ApiAdapter();
  const candidate = await adapter.getCandidate();
  const result = await adapter.acceptPatch(candidate.id, "patch-create-training-confirm", {
    candidate_revision: candidate.revision,
    actor: "ui-test",
  });
  assert.equal(result.status, "accepted");
  assert.equal(result.revision, 8);
  await assert.rejects(
    adapter.rejectPatch(candidate.id, "patch-confirm-action", { candidate_revision: 7, actor: "ui-test" }),
    /revision_conflict/,
  );
});

test("unrelated JSON is rejected at the P5 boundary", () => {
  assert.throws(() => assertCandidateModel({ id: "x", status: "candidate", revision: 1 }), /CandidateModel/);
});
