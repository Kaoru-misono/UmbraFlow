-- ctx: the trusted Luau framework's project-facing context object.
--
-- The host builds the private capability surface in C++ and hands it to this
-- chunk as its one argument, so the primitives live in a closure upvalue and
-- are never a key of any table a project script can name. The table returned
-- below is frozen by the loader and published into the project environment as
-- the global `ctx`; it is the whole of what a project task can reach besides
-- the `uf` data tables.
--
-- Deliberately thin. Every method here forwards one call and decides nothing:
-- the policy layer -- ctx:step, ctx:cycle, ctx:retry, a real wait loop over
-- cycle_open/cycle_page, and the interrupt registry -- is stage 3 of
-- docs/plans/2026-07-29-three-layer-task-system.md and must not be started
-- here. What exists today is exactly the set of bare verbs that used to hang
-- off the `uf` root, so removing them from the project environment costs a
-- project script nothing but a rename.
--
-- `native` is nil on a VM booted with no bound task session (the data-only
-- surface, used where only the resource name closure matters). That is
-- intended: this module must still load there, and a method then fails at the
-- call rather than at load.
local native = ...

-- The label the host stamps on every Tier B error carrier it mints, handed over
-- on the private surface rather than written out here. Spelling it in this file
-- would make it two strings that have to stay equal with nothing checking that;
-- taking it from the host leaves one, and a rename on the C++ side moves the
-- mint and this comparison together. Nil on a VM with no bound session, which
-- also mints no errors.
local errorTag = nil
if native ~= nil then
    errorTag = native.error_tag
end

-- A value that exists only inside the framework environment. It is assigned as
-- a framework global rather than exported on purpose: the exports below are
-- published into the project environment, so anything reachable from them is by
-- definition not framework-only. The environment-isolation tests need one value
-- that is, or their claim decays from a difference into an absence.
frameworkSentinel = "uf-framework-sentinel-6b21f0"

local ctx = {}

function ctx:cycle_open()
    return native.cycle_open()
end

function ctx:cycle_close(ticket)
    return native.cycle_close(ticket)
end

function ctx:cycle_page(ticket)
    return native.cycle_page(ticket)
end

function ctx:cycle_find(ticket, recognizer)
    return native.cycle_find(ticket, recognizer)
end

function ctx:cycle_click(ticket, hit)
    return native.cycle_click(ticket, hit)
end

function ctx:wait_for_page(page, options)
    return native.wait_for_page(page, options)
end

function ctx:now()
    return native.now()
end

function ctx:random(low, high)
    return native.random(low, high)
end

-- ctx:try(fn) -> (ok, err). Pure Luau, and pure Luau is the point: there is no
-- C closure anywhere on this path doing lua_pcall, so a hard cancel that lands
-- inside `fn` breaks the thread instead of degrading into a catchable
-- across-C-call error.
--
-- A Tier B automation error is a value only the host can mint: a userdata,
-- carrying the host tag, wearing this label behind a protected metatable. Both
-- halves of the test do work. Userdata is what a project script cannot produce
-- at all -- setmetatable and table.clone take tables, and newproxy is removed
-- from both environments -- and the label is what tells a Tier B error apart
-- from the other host userdata a script can hold, a page or a recognizer.
--
-- Anything else is the script's own failure -- including the Tier C
-- cancellation sentinel, which is a plain string -- and is re-raised unchanged,
-- so try never masks a bug and never swallows a cancel. Level 0 keeps the value
-- byte-identical instead of prefixing a second source position onto a string
-- error.
--
-- Swallowing the sentinel would still not buy a script anything: every
-- primitive checks the host's terminal latch on entry, so the next call
-- refuses.
function ctx:try(fn)
    local ok, err = pcall(fn)
    if ok then
        return true, nil
    end
    if type(err) == "userdata" and getmetatable(err) == errorTag then
        return false, err
    end
    error(err, 0)
end

return ctx
