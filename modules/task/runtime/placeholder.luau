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

return {
    stage = "placeholder",
}
