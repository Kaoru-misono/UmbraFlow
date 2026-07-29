-- Placeholder module for the trusted Luau framework bundle.
--
-- The real framework -- task lifecycle, step, observation-cycle orchestration,
-- wait, retry, interrupt, and error translation -- arrives in stage 3 of
-- docs/plans/2026-07-29-three-layer-task-system.md. Until then this module
-- exists so the .luau build pipeline carries a real source end to end: the
-- [embed] manifest section, scripts/embed_luau.py, the generated translation
-- unit, and the syntax gate in tests/task/test-framework-bundle.cpp all have
-- something to act on.
--
-- It declares no framework API on purpose. Nothing should start depending on
-- this table; stage 3 deletes the file.
--
-- The one exception is `sentinel`. It is a distinctive value that exists only
-- inside the framework environment, so the environment-isolation tests can
-- state their claim as a concrete difference -- the framework side finds this
-- exact string, the project side must not find it by any route -- instead of
-- asserting a vacuous absence. Stage 3 keeps a value with this role even when
-- the rest of this file goes.

return {
    stage = "placeholder",
    sentinel = "uf-framework-sentinel-6b21f0",
}
