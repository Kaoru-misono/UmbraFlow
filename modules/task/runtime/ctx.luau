-- ctx: the trusted Luau framework's project-facing context object.
--
-- One rule decides what lives here: C++ owns every GUARANTEE, this file owns
-- every POLICY. How long to wait, how often to re-observe, which popup is
-- handled and how often, when a failure is worth another attempt, how steps
-- nest -- all of it is Luau, and none of it is C++.
--
-- It is Luau for a structural reason rather than for hot reloading. step,
-- cycle and retry take a PROJECT function. Written in C++ each of them would be
-- a C callback into Lua with a deep C stack underneath project code, which is
-- exactly the narrow case where a hard cancel degrades from an uncatchable
-- break into an ordinary catchable error -- turning a rare accident into the
-- normal shape. Luau calling Luau has none of that.
--
-- The host builds the private capability surface in C++ and hands it to this
-- chunk as its one argument, so the primitives live in a closure upvalue and
-- are never a key of any table a project script can name. The table returned
-- below is frozen by the loader and published into the project environment as
-- the global `ctx`; it is the whole of what a project task can reach besides
-- the `uf` error-kind table and the `task` declaration surface.
--
-- WAITING FOR A PAGE IS NOT HERE ANY MORE. `ctx:wait_for_page` rode the
-- `cycle_page` primitive, and the page model it resolved against moved to layer
-- two: `observe.wait_until` is the wait now, and `observe.walk_edge` is what
-- notices an interrupt page during one
-- (docs/plans/2026-07-31-script-owned-page-model.md 9). What stayed here is the
-- policy that never mentioned a page -- steps, retry, the scoped cycle, the
-- time verbs -- because none of it had a model in it to move.
--
-- The `native.emit` calls below are how the structure this file maintains --
-- which step is open, which retry attempt this is -- reaches the run's evidence
-- stream. Two things about them are worth stating
-- once, here, rather than at each call site.
--
-- They are an AUDIT LOG, not a replay log. Replaying a run rests on the seed the
-- host recorded in run.started plus the observation sequence a test feeds its
-- fake frame source; nothing emitted here can reconstruct a run, and nothing
-- should ever be added on the theory that it could.
--
-- `emit` is not a passthrough. The host runs a state machine over the requests
-- and refuses one that does not fit: a finish that does not name the innermost
-- open step, or an attempt beyond its policy's declared total. A refusal that
-- names a framework bug spends the
-- generation before it raises, so no `pcall` in this file or above it can
-- swallow one and carry on -- which is why every cleanup path below asks
-- `native.terminal()` before emitting its closing event rather than raising a
-- second time over the failure that spent it.
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

-- CALIBRATION: every constant below is a conservative placeholder awaiting the
-- first real daily, and every one of them is policy -- which is why they are
-- here and not in the host's config. The host used to carry the wait defaults
-- so its own wait loop could apply them; that loop is gone, and a default the
-- framework cannot see is a default the framework cannot own.
local k_defaultTimeoutMillis = 600000
local k_defaultPollMillis    = 500

-- How many attempts a retry policy takes when it names none.
local k_defaultRetryAttempts = 3

-- The two messages this file owns rather than letting C++ answer for.
--
-- Opening a second cycle is a real error and it must fail, but the host's
-- InternalInvariant is a backstop for a framework bug, not an author-facing
-- diagnostic: a project that nests two waits should be told what it did and
-- what to do, in a sentence, on its own line. The layering is the design's --
-- the framework owns the good message, C++ owns the guarantee -- so this
-- sentence reaching an author means the framework caught it first, and the
-- host's invariant firing means the framework has a bug.
local k_nestedCycleMessage =
    "cannot open an observation cycle while one is open; consume or close the "
    .. "outer cycle first"
local k_closedCycleMessage =
    "this observation cycle is closed; a cycle ends when its block returns, and "
    .. "a click consumes it"

-- Framework state for one VM, and therefore for one run: the host builds a
-- fresh VM per task run and destroys it afterwards, so none of this outlives
-- the run that wrote it and no reset path is needed (or possible to get wrong).
--
-- `openCycle` is the framework's own record of the generation's single open
-- cycle. It is not a second source of truth for the ledger C++ keeps -- it
-- cannot be, since the host refuses a second open either way -- it is what lets
-- the refusal happen HERE, with the sentence above, before a capture is spent.
local openCycle = nil
local stepStack = {}

local ctx = {}

-- Raises the closed-cycle message at the caller of the view method that asked,
-- so an author reads their own line rather than a line of this file.
local function requireLive(state)
    if not state.live then
        error(k_closedCycleMessage, 3)
    end
end

-- The project-facing view of one observation cycle: what `ctx:cycle` hands its
-- block.
--
-- The ticket itself never reaches a project. That is not secrecy -- a ticket
-- confers nothing without the primitives, which no project can name -- it is so
-- that a cycle a project holds past its close fails with the sentence above
-- instead of a StaleObservation from the ledger, because using a finished cycle
-- is a script bug and not an automation failure.
--
-- The view is frozen and carries no metatable. The freeze is the design's rule
-- for a project-visible framework object; the absence of a metatable is why
-- `table.freeze` accepts it at all (it refuses a table with a protected
-- metatable), and it costs nothing here because this object proves no identity:
-- its methods are closures over the real ticket, so a `table.clone` of it is
-- exactly as powerful as the original and no more.
local function newCycleView(state)
    local view = {}

    -- A delivered click CONSUMES the cycle, so the view goes dead here rather
    -- than at the enclosing block's exit. `consumed` is set only after the
    -- primitive returned: a click that failed may or may not have spent the
    -- ledger entry, and `cycle_close` is idempotent, so the close on the way
    -- out is the safe answer for that case.
    --
    -- The argument is a MATCH -- what `cycle_match` returned on this same cycle
    -- -- and nothing else. There is no element handle to pass any more: which
    -- pixels an element is and whether this page may click it are layer two's
    -- facts, checked in observe.luau, and what C++ still checks is that the
    -- match came from this frame.
    function view:click(match)
        requireLive(state)
        native.cycle_click(state.ticket, match)
        state.consumed = true
        state.live     = false
    end

    -- A delivered keystroke CONSUMES the cycle for the same reason a click does:
    -- the key changed the screen, so this frame no longer describes it. That is
    -- the host's rule rather than this file's -- the ledger spends the cycle
    -- either way -- and the bookkeeping here follows it so the view goes dead at
    -- the same moment the host's cycle does.
    --
    -- It takes no match. The whole difference between this and view:click is
    -- that a key names no position, so there is nothing to have found first;
    -- what the author writes is the name their target prints, "E" or "3" or
    -- "F1".
    function view:key(name)
        requireLive(state)
        native.key(state.ticket, name)
        state.consumed = true
        state.live     = false
    end

    return table.freeze(view)
end

-- Opens the generation's one observation cycle and records it as the
-- framework's. `level` is where the nested-cycle message should point: every
-- caller is a public method called directly by a project, so every caller
-- passes 3 (error -> this function -> the method -> the author's line).
local function beginCycle(level)
    if openCycle ~= nil then
        error(k_nestedCycleMessage, level)
    end

    local state = {
        live     = true,
        consumed = false,
    }
    state.ticket = native.cycle_open()
    state.view   = newCycleView(state)
    openCycle    = state
    return state
end

-- Ends a cycle, and is the whole of the framework's side of the design's
-- "framework is responsible for closing". The bookkeeping is dropped BEFORE the
-- primitive runs: cycle_close can itself raise (a cancelled generation, a trace
-- sink that failed), and a raise that left `openCycle` set would turn one
-- failure into a framework that believes a cycle is open forever.
local function endCycle(state)
    state.live = false
    if openCycle == state then
        openCycle = nil
    end
    if not state.consumed then
        native.cycle_close(state.ticket)
    end
end

-- ctx:step(name, fn) -> whatever fn returned.
--
-- A step is the named unit of task structure and nothing else: it decides no
-- policy, it only records where in the task the work below it belongs. The
-- stack is truncated to the depth this call opened, on the normal path and on a
-- raise alike, so steps are strictly well nested by construction -- which is
-- the property the host's coming validation state machine requires, and which
-- makes "a step left open at run.finished" a framework bug rather than an
-- ordinary script outcome.
function ctx:step(name, fn)
    if type(name) ~= "string" or name == "" then
        error("ctx:step needs a non-empty name", 2)
    end
    if type(fn) ~= "function" then
        error("ctx:step needs a function to run", 2)
    end

    -- The host owns everything about the NAME -- its length, its character set,
    -- and how deep the open path may go -- because the name is a project string
    -- literal and this file has no reason to hold a second copy of those rules.
    -- It is emitted before the stack grows, so a refused name opens no step here
    -- either and the two sides never disagree about what is open.
    native.emit("step_started", name)

    local depth = #stepStack + 1
    stepStack[depth] = name

    local ok, result = pcall(fn)
    for index = #stepStack, depth, -1 do
        stepStack[index] = nil
    end

    -- Skipped on a spent generation, and only there: the close would raise over
    -- whatever spent it and replace the real cause with its consequence. The
    -- host then sees a step still open at run.finished, which is the correct
    -- reading -- the run did end inside this step.
    if not native.terminal() then
        native.emit("step_finished", name)
    end

    if not ok then
        error(result, 0)
    end
    return result
end

-- The names of the steps currently open, outermost first. A fresh copy, so a
-- project reading it cannot reach the framework's own stack.
--
-- It exists because "steps are well nested" has to be observable to be worth
-- anything: without it the property is a claim about code nobody can check from
-- outside, and the state machine that will enforce it in C++ would be the first
-- thing to notice a leak.
function ctx:step_path()
    return table.clone(stepStack)
end

-- ctx:cycle(fn) -> whatever fn returned.
--
-- Opens one observation cycle, hands its view to fn, and closes it on every
-- exit path including a raise. The callback shape is what makes both halves
-- structural rather than conventional: a cycle cannot escape the block, so
-- "always closed" and "at most one open" are properties of the API rather than
-- of the author's discipline.
--
-- IT KNOWS NOTHING ABOUT PAGES. The cycle it hands over has captured a frame and
-- nothing more. Recognising what is on that frame is layer two's work and
-- belongs to `observe`, which opens and closes its own cycles by the same rule
-- this block does.
--
-- The block's own failure outranks a failure from the close, so an author is
-- told why their work failed rather than being handed the second-order
-- consequence.
function ctx:cycle(fn)
    if type(fn) ~= "function" then
        error("ctx:cycle needs a function to run with the cycle", 2)
    end

    local state = beginCycle(3)
    local ok, result          = pcall(fn, state.view)
    local closed, closeError  = pcall(endCycle, state)

    if not ok then
        error(result, 0)
    end
    if not closed then
        error(closeError, 0)
    end
    return result
end

-- Whether `err` is worth another attempt under `on`.
--
-- The design's correction to its own draft, and the reason it matters: `on`
-- OVERRIDES `retryable` for the kinds it names, and `retryable` is the default
-- only when `on` is absent. `retryable` is derived from the domain's unwind
-- axis, where only CaptureStalled and StaleObservation are Retry while Timeout
-- and TargetUnavailable are Abort. If `retryable` were a hard filter, "wait for
-- a page, and if the game was slow do the whole step again" -- the single most
-- common shape a daily script takes -- could never retry at all, because the
-- timeout observe.wait_until mints is an Abort.
local function retryAllowed(on, err)
    if on == nil then
        return err.retryable == true
    end
    for index = 1, #on do
        if on[index] == err.kind then
            return true
        end
    end
    return false
end

-- ctx:retry(policy, fn) -> whatever fn returned.
--
-- policy.attempts is the total number of tries, policy.on an optional list of
-- uf.errors.<kind> constants, policy.backoff_ms an optional pause between them.
--
-- Only a Tier B automation error is ever retried. Anything else is re-raised
-- unchanged and immediately: a project's own bug is not an automation failure,
-- and the Tier C cancellation sentinel is a plain string, so a retry loop can
-- never turn a cancelled run into three more attempts.
function ctx:retry(policy, fn)
    if type(policy) ~= "table" then
        error("ctx:retry needs a policy table", 2)
    end
    if type(fn) ~= "function" then
        error("ctx:retry needs a function to run", 2)
    end

    local attempts = policy.attempts
    if attempts == nil then
        attempts = k_defaultRetryAttempts
    end
    -- Whole numbers only. A fractional total would make "attempt 3 of 2.5" a
    -- sentence the host's monotone check has to refuse as a framework bug, when
    -- the only thing wrong is the number a project wrote.
    if type(attempts) ~= "number" or attempts < 1 or attempts % 1 ~= 0 then
        error(
            "ctx:retry policy.attempts must be a whole number of attempts, at "
            .. "least one",
            2
        )
    end

    local on = policy.on
    if on ~= nil and type(on) ~= "table" then
        error("ctx:retry policy.on must be a list of uf.errors.<kind>", 2)
    end

    local backoffMillis = policy.backoff_ms
    if backoffMillis ~= nil and type(backoffMillis) ~= "number" then
        error("ctx:retry policy.backoff_ms must be a number of milliseconds", 2)
    end

    local attempt = 1
    while true do
        -- Every pass is announced, including the first: "attempt 1 of 3" is what
        -- makes a later "attempt 2 of 3" readable as a repeat rather than as the
        -- start of something. The host checks the pair -- never beyond the
        -- declared total, never skipping a number -- so the count in the stream
        -- is the count this loop actually took.
        native.emit("retry_attempt", attempt, attempts)

        local ok, result = pcall(fn)
        if ok then
            return result
        end

        local automation =
            type(result) == "userdata" and getmetatable(result) == errorTag
        if not automation
            or attempt >= attempts
            or not retryAllowed(on, result)
        then
            error(result, 0)
        end

        attempt += 1
        if backoffMillis ~= nil then
            -- The pause between two attempts is the retry policy's, so it is
            -- recorded as one. It settles through the primitive directly rather
            -- than through ctx:settle, which would record a second,
            -- project-shaped pause for the same wait. Taken first and recorded
            -- after, so a pause the host refused or a cancel cut short leaves no
            -- record of a wait that did not happen.
            native.settle(backoffMillis)
            native.emit("retry_backoff", backoffMillis)
        end
    end
end

-- The primitive-level surface: one forward each, deciding nothing.
--
-- These are what the policy methods above are built out of, left reachable so
-- the host's primitive contracts can be driven directly. Mixing them with the
-- policy methods is legal but pointless: a cycle opened with ctx:cycle_open is
-- invisible to the framework's own bookkeeping, so a ctx:cycle after one is
-- refused by the host's InternalInvariant instead of by the sentence above.
-- They retire together with the `ctx` project global, once a task is a
-- descriptor the host runs rather than a chunk the host executes.
function ctx:cycle_open()
    return native.cycle_open()
end

function ctx:cycle_close(ticket)
    return native.cycle_close(ticket)
end

-- Takes a match `cycle_match` returned on this same cycle. See view:click.
function ctx:cycle_click(ticket, match)
    return native.cycle_click(ticket, match)
end

-- The script-owned page model's primitives. They forward and decide nothing,
-- like the block they sit in: the element and page vocabulary built on top of
-- them lives in `model`, `observe` and `navigation`.
--
-- The rectangle is four numbers rather than a table because the private surface
-- takes only host-minted handles and scalars, and a Luau table is neither.
function ctx:template_load(blob)
    return native.template_load(blob)
end

function ctx:cycle_match(ticket, template, x, y, width, height)
    return native.cycle_match(ticket, template, x, y, width, height)
end

function ctx:cycle_read(ticket, x, y, width, height)
    return native.cycle_read(ticket, x, y, width, height)
end

-- Clicking a bare coordinate is the framework's privilege and never a project's.
-- This forward exists so the host's contract can be driven directly, exactly as
-- the block's other forwards are; the project environment reaches `ctx` but the
-- wrappers a business task is meant to use are the ones that name an element.
function ctx:cycle_click_point(ticket, x, y)
    return native.cycle_click_point(ticket, x, y)
end

function ctx:project_read(name)
    return native.project_read(name)
end

function ctx:project_write(name, blob)
    return native.project_write(name, blob)
end

-- Named `key` rather than `cycle_key` because that is the primitive's name on the
-- private surface, and this block forwards each primitive under its own name. It
-- takes a ticket like every cycle verb does; what it does not take is a hit.
function ctx:key(ticket, name)
    return native.key(ticket, name)
end

-- There is no ctx:now: a script reads no clock, so the only shapes waiting can
-- take are a host-minted deadline polled by ctx:wait, and the declarative
-- ctx:settle.
function ctx:deadline(milliseconds)
    return native.deadline(milliseconds)
end

function ctx:wait(deadline, intervalMilliseconds)
    return native.wait(deadline, intervalMilliseconds)
end

-- The pause is taken first and recorded after, so a duration the host refused
-- leaves no record of a pause that never happened. The primitive already writes
-- its own task.native_call; this adds the framework's reading of it, which is
-- that a project declared a pause of its own rather than a retry policy taking
-- one between attempts.
function ctx:settle(milliseconds)
    native.settle(milliseconds)
    native.emit("settled", milliseconds)
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
-- from the other host userdata a script can hold, a page or an element.
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
