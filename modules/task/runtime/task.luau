-- task: the project-facing task declaration surface.
--
-- Where `ctx` is what a task does WHILE it runs, this is what a task declares
-- ABOUT itself before it runs: which popups it knows how to dismiss, and what
-- its body is. The two are separate surfaces because they answer to different
-- moments -- a declaration is read once, a context is used throughout -- and
-- because the design spells them apart (`uf.task.*` against `ctx:*`).
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

-- The descriptors this module minted, held by identity.
--
-- It is what makes `define{ interrupts = ... }` refuse a hand-written table
-- that merely looks like an interrupt. Not because a forged one would confer
-- anything -- the handler would be the project's own function and `when` has to
-- be a real page handle or the match raises -- but because the failure mode of
-- accepting one is silence: a misspelled field yields an interrupt that never
-- matches, and a popup that is never dismissed looks exactly like a game that
-- never left the page.
local minted = {}

local task = {}

-- task.interrupt{ id, when, max_hits, handle } -> frozen descriptor.
--
-- `when` is a uf.pages.<name> handle: an interrupt is addressed by the page it
-- recognises, because a popup IS a page as far as the recognition runtime is
-- concerned. `handle` is called as handle(ctx, cycle) with the CURRENT
-- observation cycle -- not a fresh one -- so a handler that clicks consumes the
-- observation the waiting loop was about to test, and the loop re-observes.
function task.interrupt(spec)
    if type(spec) ~= "table" then
        error("task.interrupt needs a table", 2)
    end
    if type(spec.id) ~= "string" or spec.id == "" then
        error("an interrupt needs a non-empty string id", 2)
    end
    if type(spec.when) ~= "userdata" then
        error("an interrupt needs when = uf.pages.<name>", 2)
    end
    if type(spec.handle) ~= "function" then
        error("an interrupt needs handle = function(ctx, cycle)", 2)
    end

    local maxHits = spec.max_hits
    if maxHits == nil then
        maxHits = frameworkTaskRegistry.defaultMaxHits
    end
    if type(maxHits) ~= "number" or maxHits < 1 then
        error("an interrupt's max_hits must be at least one hit", 2)
    end

    local descriptor = table.freeze({
        id       = spec.id,
        when     = spec.when,
        max_hits = maxHits,
        handle   = spec.handle,
    })
    minted[descriptor] = true
    return descriptor
end

-- task.define{ interrupts, run } -> frozen descriptor.
--
-- Registration order is the order of the list, and it is the order the wait
-- loop tries matches in, so first-match is a property an author can read off
-- their own declaration rather than a rule they have to remember.
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

    local declared = spec.interrupts
    if declared ~= nil and type(declared) ~= "table" then
        error("task.define needs interrupts = { <task.interrupt> ... }", 2)
    end

    local list = {}
    local seen = {}
    if declared ~= nil then
        for index = 1, #declared do
            local entry = declared[index]
            if not minted[entry] then
                error(
                    "task.define takes interrupts built by task.interrupt{...}",
                    2
                )
            end
            if seen[entry.id] then
                error(
                    "two interrupts share the id '" .. entry.id
                        .. "'; ids name a popup and a hit budget, so they "
                        .. "cannot be shared",
                    2
                )
            end
            seen[entry.id] = true
            list[index]    = entry
        end
    end

    if spec.run ~= nil and type(spec.run) ~= "function" then
        error("task.define needs run = function(ctx)", 2)
    end

    local descriptor = table.freeze({
        interrupts = table.freeze(list),
        run        = spec.run,
    })

    frameworkTaskRegistry.setInterrupts(descriptor.interrupts)
    if descriptor.run ~= nil then
        descriptor.run(ctx)
    end
    return descriptor
end

return task
