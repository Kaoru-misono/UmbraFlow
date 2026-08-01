-- task: the project-facing task declaration surface.
--
-- Where `ctx` is what a task does WHILE it runs, this is what a task declares
-- ABOUT itself before it runs. The two are separate surfaces because they answer
-- to different moments -- a declaration is read once, a context is used
-- throughout -- and because the design spells them apart (`uf.task.*` against
-- `ctx:*`).
--
-- WHAT IT DECLARED AND NO LONGER DOES. `task.interrupt` named a popup by the
-- page that recognises it, and the framework's own wait loop offered every cycle
-- it opened to that list. Both the loop and the page handle it was addressed by
-- were C++ page-model machinery, and both retired with it
-- (docs/plans/2026-07-31-script-owned-page-model.md 9). An interrupt page is a
-- flag on the page in the project file now, and `observe.walk_edge` is what
-- notices one when a destination never arrives -- which leaves what to do about
-- it to the caller instead of to a registry.
--
-- TRANSITIONAL SPELLING. The design's name for this surface is `uf.task`, a
-- member of the one script root. It cannot be that today: `uf` is built in C++
-- and deep-frozen by the host installer, which runs AFTER the framework bundle
-- has loaded, so no framework value can become a member of it without a new
-- host seam for exactly that. So it is published under its own name by the same
-- mechanism `ctx` already uses -- a framework module whose frozen exports the
-- project environment names -- and the eventual move is one word at every call
-- site. Nothing here depends on which of the two it is.
--
-- This module takes no capability argument. Declaring a task reaches no engine
-- verb, mints no handle and reads no clock; everything effectful is behind the
-- context, and a declaration surface that held primitives would be a second
-- door to them for no reason.

local task = {}

-- task.define{ run } -> frozen descriptor.
--
-- `run` is called here, with the context, and that is the one part of this
-- surface that is not yet its final shape. The design has the host hand the
-- frozen descriptor to a framework runner, which is a seam that does not exist:
-- a project chunk today is executed for effect and its return value discarded.
-- Calling it here keeps `run` from being a field nobody reads while the
-- descriptor is already exactly what that runner will take; when the seam
-- lands, this call moves and no project script changes.
function task.define(spec)
    if type(spec) ~= "table" then
        error("task.define needs a table", 2)
    end

    if spec.run ~= nil and type(spec.run) ~= "function" then
        error("task.define needs run = function(ctx)", 2)
    end

    local descriptor = table.freeze({
        run = spec.run,
    })

    if descriptor.run ~= nil then
        descriptor.run(ctx)
    end
    return descriptor
end

return task
