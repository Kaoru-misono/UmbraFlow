-- ctx: the trusted Luau framework's project-facing context object.
--
-- C++ owns every guarantee, this file owns every policy: how long to wait, how
-- often to re-observe, when a failure is worth another attempt, how steps nest.
--
-- It is Luau rather than C++ because step, cycle and retry take a PROJECT
-- function, and a C callback into Lua puts a C stack under project code -- the
-- one case where a hard cancel degrades from an uncatchable break into an
-- ordinary catchable error.
--
-- The host hands the private capability surface to this chunk as its one
-- argument, so the primitives are a closure upvalue and never a key of a table a
-- project script can name. The table returned below is frozen by the loader and
-- published as the global `ctx`. Waiting for a page is not here: it belongs to
-- the page model in `observe.wait_until` / `observe.walk_edge`.
--
-- `native.emit` is how this file's structure -- which step is open, which retry
-- attempt this is -- reaches the run's evidence stream. It is an AUDIT log, not
-- a replay log; replay rests on the seed in run.started plus the observation
-- sequence a test feeds its fake frame source, so nothing here should be added
-- on the theory that it could reconstruct a run. It is also not a passthrough:
-- the host refuses a request that does not fit its state machine (a finish not
-- naming the innermost open step, an attempt beyond the declared total), and
-- such a refusal spends the generation before it raises. That is why every
-- cleanup path below asks `native.terminal()` before emitting its closing event.
--
-- `native` is nil on a VM booted with no bound task session (the data-only
-- surface). This module must still load there; a method then fails at the call
-- rather than at load.
local native = ...

-- The label the host stamps on every Tier B error carrier it mints. Taken from
-- the private surface rather than spelled here, so a rename on the C++ side
-- moves the mint and this comparison together. Nil on a VM with no bound
-- session, which also mints no errors.
local errorTag = nil
if native ~= nil then
    errorTag = native.error_tag
end

-- A framework global rather than an export: the exports below are published into
-- the project environment, so anything reachable from them is not
-- framework-only. The environment-isolation tests need one value that is.
frameworkSentinel = "uf-framework-sentinel-6b21f0"

-- CALIBRATION: how many attempts a retry policy takes when it names none. It is
-- policy, which is why it is here and not in the host's config.
local k_defaultRetryAttempts = 3

-- The two messages this file owns rather than letting C++ answer for. The
-- framework owns the author-facing sentence and C++ owns the guarantee, so the
-- host's InternalInvariant firing on a nested cycle means the framework has a
-- bug rather than that a project nested two waits.
local k_nestedCycleMessage =
    "cannot open an observation cycle while one is open; consume or close the "
    .. "outer cycle first"
local k_closedCycleMessage =
    "this observation cycle is closed; a cycle ends when its block returns, and "
    .. "a click consumes it"

-- Framework state for one VM, and therefore for one run: the host builds a fresh
-- VM per task run, so none of this outlives the run that wrote it and there is
-- no reset path. `openCycle` is not a second source of truth for the ledger C++
-- keeps -- the host refuses a second open either way -- it is what lets the
-- refusal happen here, before a capture is spent.
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
-- block. The ticket itself never reaches a project, so a cycle held past its
-- close fails with k_closedCycleMessage -- a script bug -- rather than with a
-- StaleObservation from the ledger, which is an automation failure.
--
-- The view is frozen and carries no metatable; `table.freeze` refuses a table
-- with a protected one. Proving identity is not needed here because the methods
-- are closures over the real ticket, so a `table.clone` is no more powerful.
local function newCycleView(state)
    local view = {}

    -- A delivered click CONSUMES the cycle, so the view goes dead here rather
    -- than at the enclosing block's exit. `consumed` is set only after the
    -- primitive returned: a click that failed may or may not have spent the
    -- ledger entry, and `cycle_close` is idempotent.
    --
    -- The argument is a match `cycle_match` returned on this same cycle, and
    -- nothing else. Which pixels an element is and whether this page may click it
    -- are observe.luau's checks; C++ checks only that the match came from this
    -- frame.
    function view:click(match)
        requireLive(state)
        native.cycle_click(state.ticket, match)
        state.consumed = true
        state.live     = false
    end

    -- A delivered keystroke CONSUMES the cycle: the key changed the screen, so
    -- this frame no longer describes it. It takes no match, because a key names
    -- no position; `name` is what the target prints, "E" or "3" or "F1".
    function view:key(name)
        requireLive(state)
        native.key(state.ticket, name)
        state.consumed = true
        state.live     = false
    end

    return table.freeze(view)
end

-- Opens the generation's one observation cycle and records it as the
-- framework's. `level` is where the nested-cycle message points; every caller is
-- a public method called directly by a project, so it is 3 (error -> here -> the
-- method -> the author's line).
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

-- Ends a cycle. The bookkeeping is dropped BEFORE the primitive runs, because
-- cycle_close can itself raise (a cancelled generation, a failed trace sink) and
-- a raise that left `openCycle` set would leave the framework believing a cycle
-- is open forever.
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
-- A step decides no policy; it records where in the task the work below it
-- belongs. The stack is truncated to the depth this call opened on the normal
-- path and on a raise alike, so steps are well nested by construction and "a
-- step left open at run.finished" is a framework bug.
function ctx:step(name, fn)
    if type(name) ~= "string" or name == "" then
        error("ctx:step needs a non-empty name", 2)
    end
    if type(fn) ~= "function" then
        error("ctx:step needs a function to run", 2)
    end

    -- The host owns the name's length, character set and maximum depth. Emitted
    -- before the stack grows, so a refused name opens no step here either.
    native.emit("step_started", name)

    local depth = #stepStack + 1
    stepStack[depth] = name

    local ok, result = pcall(fn)
    for index = #stepStack, depth, -1 do
        stepStack[index] = nil
    end

    -- Skipped on a spent generation, where the close would raise over whatever
    -- spent it. The host then sees a step still open at run.finished, which is
    -- the correct reading: the run did end inside this step.
    if not native.terminal() then
        native.emit("step_finished", name)
    end

    if not ok then
        error(result, 0)
    end
    return result
end

-- The names of the steps currently open, outermost first. A fresh copy, so a
-- project reading it cannot reach the framework's own stack. It exists so that
-- "steps are well nested" is observable from outside rather than only a claim.
function ctx:step_path()
    return table.clone(stepStack)
end

-- ctx:page_resolved(name) -> ()
--
-- Records that a page resolved on the observation just examined.
--
-- It is the one conclusion no primitive can write for itself: a resolution is
-- this framework's judgement over matches and readings, so without this line the
-- stream holds the searches and nothing about what they were taken to mean.
--
-- Only `observe.resolve_page` calls it, and only where it mints a receipt. A
-- refusal is deliberately not recorded: a caller tries every page it cares about
-- against one frame, so recording those would give one line per page tried
-- rather than the sequence of pages the run believed it walked.
--
-- The host owns the name's length and character set, as it does a step's.
function ctx:page_resolved(name)
    native.emit("page_resolved", name)
end

-- ctx:replay_steps() -> { { kind = , seq = , label = }, ... }
--
-- The recorded run this run was handed, already projected off a trace file
-- before this VM existed. Empty on every run that is not a replay.
--
-- It is here rather than on `replay` so that module stays what it says it is:
-- a judgement over a projection and a model, reaching no host at all. Every
-- route to the host is a `ctx` verb, and this is the only one that costs
-- nothing -- no capture, no budget, no observation.
function ctx:replay_steps()
    return native.replay_steps()
end

-- ctx:cycle(fn) -> whatever fn returned.
--
-- Opens one observation cycle, hands its view to fn, and closes it on every exit
-- path including a raise. The callback shape is what makes "always closed" and
-- "at most one open" properties of the API rather than of author discipline: a
-- cycle cannot escape the block.
--
-- The cycle has captured a frame and nothing more; recognising what is on it
-- belongs to `observe`. The block's own failure outranks a failure from the
-- close, so an author is told why their work failed rather than the consequence.
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

-- Whether `err` is worth another attempt under `on`. `on` OVERRIDES `retryable`
-- for the kinds it names; `retryable` is the default only when `on` is absent.
-- It has to override: `retryable` follows the domain's unwind axis, where only
-- CaptureStalled and StaleObservation are Retry, so a hard filter would refuse
-- to retry the timeout observe.wait_until mints -- an Abort, and the most common
-- thing a daily script wants another attempt at.
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
-- Only a Tier B automation error is ever retried; anything else is re-raised
-- unchanged. A project's own bug is not an automation failure, and the Tier C
-- cancellation sentinel is a plain string, so a retry loop can never turn a
-- cancelled run into three more attempts.
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
    -- Whole numbers only: a fractional total makes "attempt 3 of 2.5", which the
    -- host's monotone check refuses as a framework bug.
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
        -- Every pass is announced, including the first, so a later "attempt 2 of
        -- 3" reads as a repeat. The host checks the pair: never beyond the
        -- declared total, never skipping a number.
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
            -- Settles through the primitive rather than through ctx:settle,
            -- which would record a second, project-shaped pause for the same
            -- wait. Taken first and recorded after, so a pause the host refused
            -- or a cancel cut short leaves no record of a wait that never was.
            native.settle(backoffMillis)
            native.emit("retry_backoff", backoffMillis)
        end
    end
end

-- The primitive-level surface: one forward each, deciding nothing. Left
-- reachable so the host's primitive contracts can be driven directly. A cycle
-- opened with ctx:cycle_open is invisible to the framework's bookkeeping, so a
-- ctx:cycle after one is refused by the host's InternalInvariant rather than by
-- k_nestedCycleMessage.
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

-- The script-owned page model's primitives; the element and page vocabulary
-- built on them lives in `model`, `observe` and `navigation`. The rectangle is
-- four numbers rather than a table because the private surface takes only
-- host-minted handles and scalars.
function ctx:template_load(blob)
    return native.template_load(blob)
end

function ctx:cycle_match(ticket, template, x, y, width, height)
    return native.cycle_match(ticket, template, x, y, width, height)
end

function ctx:cycle_read(ticket, x, y, width, height)
    return native.cycle_read(ticket, x, y, width, height)
end

-- The block read: every line the frame holds inside the region, each with its
-- own rectangle in target pixels. `observe.read_lines` is where an element's
-- region, its capabilities and the authorisation a click needs are added.
function ctx:cycle_read_lines(ticket, x, y, width, height)
    return native.cycle_read_lines(ticket, x, y, width, height)
end

-- The downsampled colour census: the region in cells of `cellWidth` by
-- `cellHeight`, each cell reporting how many of its pixels the key took and how
-- far it moved between frames. It answers with a frozen table carrying
-- `columns`, `rows`, `cell_width`, `cell_height` and the flat row-major
-- `counts` and `spreads` arrays.
--
-- It is forwarded here, where ctx:cycle_crop is not, because it hands back
-- counts rather than pixels: measuring a region is ordinary business work, and
-- the crop-then-probe route a script would otherwise take pays a PNG decode per
-- rectangle. The key is required and the tolerance is optional; the rectangle
-- and the cell size are scalars for cycle_match's reason.
--
-- `keyRemoves` states which way round the key is read, and is required for the
-- reason every other key-taking verb requires it: true counts the pixels the
-- colour does NOT take, so one colour read the two ways partitions each cell.
function ctx:cycle_census_grid(
    ticket,
    x,
    y,
    width,
    height,
    cellWidth,
    cellHeight,
    keyRed,
    keyGreen,
    keyBlue,
    tolerance,
    keyRemoves
)
    return native.cycle_census_grid(
        ticket,
        x,
        y,
        width,
        height,
        cellWidth,
        cellHeight,
        keyRed,
        keyGreen,
        keyBlue,
        tolerance,
        keyRemoves
    )
end

-- There is no ctx:cycle_click_point, no ctx:cycle_long_press and no
-- ctx:cycle_drag, and do not add one. All three ACTIVATE at a bare coordinate,
-- so all three are privileged; `ctx` is published into every project
-- environment, and section 2 rule 2 of
-- docs/plans/2026-08-01-three-layers-and-agent-operator.md requires the
-- privileged verbs to be unreachable from a business task rather than refused in
-- it. `observe.click` / `observe.long_press` / `observe.drag` /
-- `observe.drag_offset` reach the primitives through the closure upvalue after
-- checking the receipt, the hit and the interact edge, and
-- `explore.click_point` / `explore.long_press` / `explore.drag` forward them in a
-- module only the exploration environment publishes.
--
-- Activating is the operative word, and ctx:cycle_move_pointer below is the case
-- that shows it: it names a bare coordinate too and is forwarded here anyway,
-- because pressing nothing leaves the privilege nothing to protect.

function ctx:project_read(name)
    return native.project_read(name)
end

function ctx:project_write(name, blob)
    return native.project_write(name, blob)
end

-- Named `key` rather than `cycle_key` because that is the primitive's name on
-- the private surface. It takes a ticket like every cycle verb; it takes no hit.
function ctx:key(ticket, name)
    return native.key(ticket, name)
end

-- One wheel scroll of `notches` detents, positive away from the reader. It
-- consumes the cycle: the screen moved, so the frame this ticket names no longer
-- describes it. Whether a scroll should be anchored to an annotated region is
-- open question 5 of
-- docs/plans/2026-08-01-three-layers-and-agent-operator.md, so this forwards the
-- raw verb for now.
function ctx:cycle_scroll(ticket, notches)
    return native.cycle_scroll(ticket, notches)
end

-- Moves the pointer to a target pixel, pressing nothing, and consumes the cycle.
--
-- Call it before ctx:cycle_scroll over the region you mean to scroll. A posted
-- wheel does not scroll what its own message points at: the target scrolls
-- whatever it already believes is hovered, and in a background session with no
-- pointer message before it, that is somewhere else
-- (docs/pitfalls/capture-and-target-selection.md). Two cycles, because each
-- delivered input spends its frame.
function ctx:cycle_move_pointer(ticket, x, y)
    return native.cycle_move_pointer(ticket, x, y)
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

-- Taken first and recorded after, so a duration the host refused leaves no
-- record. The primitive writes its own task.native_call; the emit adds that a
-- project declared this pause rather than a retry policy taking one.
function ctx:settle(milliseconds)
    native.settle(milliseconds)
    native.emit("settled", milliseconds)
end

function ctx:random(low, high)
    return native.random(low, high)
end

-- ctx:try(fn) -> (ok, err). Pure Luau on purpose: no C closure on this path does
-- lua_pcall, so a hard cancel landing inside `fn` breaks the thread instead of
-- degrading into a catchable across-C-call error.
--
-- Both halves of the Tier B test do work. Userdata is what a project script
-- cannot produce (setmetatable and table.clone take tables, newproxy is removed
-- from both environments); the label tells a Tier B error apart from the other
-- host userdata a script can hold, a page or an element.
--
-- Anything else is the script's own failure, including the Tier C cancellation
-- sentinel -- a plain string -- and is re-raised unchanged, so try never masks a
-- bug and never swallows a cancel. Level 0 keeps the value byte-identical.
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
