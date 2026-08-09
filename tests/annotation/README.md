# Annotation-v2 test package

This directory is the future test entry point for the behavior fixtures in
`tests/fixtures/annotation-v2/`.

The fixtures are intentionally stored outside the current `test-task` target.
The annotation-v2 runtime, resolver, and offline modules do not exist yet, and
adding a CMake target before those contracts are implemented would create a
test-shaped second schema. The future test target should load the fixture JSON
as data and assert behavior against the implementation's public result types.

The first test target should be able to run one fixture by `case_id`, then run
the complete manifest in deterministic order. It must not require uf-chaos,
committed screenshots, OCR model files, network access, wall-clock sleeps, or
randomness.

The intended assertion layers are:

1. model/resolver: resolution, stack, diagnostics, and evidence state;
2. receipt safety: minting, ticket/frame identity, and action authorization;
3. offline graph: declared versus observed transitions;
4. collection/placement: ordered items and surface-specific geometry.

Do not turn the fixture format into a copy of the runtime TOML format. The
fixtures are behavior contracts and should remain stable if the deployment
schema is reorganized.
