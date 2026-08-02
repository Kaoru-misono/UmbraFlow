-- task: the project-facing task declaration surface.
--
-- Where `ctx` is what a task does WHILE it runs, this is what a task declares
-- ABOUT itself before it runs. An interrupt page is a flag on the page in the
-- project file rather than a registration here, and `observe.walk_edge` is what
-- notices one (docs/plans/2026-07-31-script-owned-page-model.md 9).
--
-- TRANSITIONAL SPELLING. The design's name for this surface is `uf.task`, and it
-- cannot be that today: `uf` is built in C++ and deep-frozen by the host
-- installer, which runs AFTER the framework bundle has loaded, so no framework
-- value can join it without a new host seam. It is published under its own name
-- by the mechanism `ctx` uses, and the eventual move is one word per call site.
--
-- This module takes no capability argument. Declaring a task reaches no engine
-- verb, mints no handle and reads no clock, and a declaration surface holding
-- primitives would be a second door to them.

local task = {}

-- task.define{ run } -> frozen descriptor.
--
-- `run` is called here, with the context, only until the host gains a seam that
-- hands the frozen descriptor to a framework runner; today a project chunk is
-- executed for effect and its return value discarded. When that seam lands this
-- call moves and no project script changes.
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
