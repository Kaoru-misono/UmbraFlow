# Framework verification gaps

Date: 2026-08-19
Scope: `umbraflow-cpp` only

This plan owns framework obligations exposed while removing cross-repository
status transcription. It carries work and acceptance conditions, not consumer
status, foreign commit hashes or dated rulings. The rulings that created these
obligations are frozen under [`docs/decisions/`](../decisions/README.md).

## G-01 - Verify every project-manifest dependency

`project check` currently rebuilds and compares its generated closure but does
not open every file named by the project directory manifest. A project can name
tool catalogs, project schemas and manifests that do not exist on disk and still
pass this command.

Completion requires one canonical loader-backed verification path. A fixture
that names a missing tool catalog, project schema, manifest or journal payload
schema must fail and name the missing path. The implementation must not add a
second parser to `project`; either the loader boundary is made available without
creating a dependency cycle, or the ownership boundary is changed explicitly.

## G-02 - Make the Operator public-surface scan recurring

The ruling in
[`2026-08-17-public-surface-scan-and-harm-tiers.md`](../decisions/2026-08-17-public-surface-scan-and-harm-tiers.md)
requires repeatable detection of public Operator mutators that have only test
callers. Completion requires a check that reports candidates for human tiering;
it must not infer a defect from caller absence and must preserve explicit
exclusions such as reader methods intended for tests.

Prefer extending an existing repository check or contract test over adding a
standalone gate script. Prove the check can fail by introducing one temporary
public mutator with no production caller, then remove the mutation.

## G-03 - Remove the duplicate observed-instance resolution spelling

`submitCommand` and `resolveObservedInstance` contain two spellings of the same
binding lookup and scope/freshness refusal sequence. Completion means both paths
call one canonical implementation, preserve error ordering and messages, and
retain the existing cross-scope and stale-observation negative coverage.

## G-04 - Close the remaining terminology-layer inconsistencies

The framework has two terminology debts identified by
[`2026-08-17-effect-envelope-and-expected-effect-are-two-layers.md`](../decisions/2026-08-17-effect-envelope-and-expected-effect-are-two-layers.md):

- reconcile the local `EffectEnvelope` helper in `effective-plan.cpp` with the
  published Operator schema without merging it with project `ExpectedEffect`;
- choose and propagate one offline/online Agent vocabulary in the framework.

Completion requires one canonical spelling per layer and a blast-radius sweep
through code, current documents and skills. Archived documents remain frozen.
