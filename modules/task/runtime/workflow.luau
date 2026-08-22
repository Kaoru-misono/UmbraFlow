--!strict
-- Bounded waiting, delivery classification, the human stop, child-flow
-- composition, and recovery -- with no blind retry anywhere in the module.
--
-- Every call here goes through `@umbraflow/tools`, which is the SDK's one
-- spelling of a Tool call; the private capability table this module is also
-- handed as its chunk argument is deliberately unused, for the reason
-- `@umbraflow/screen` states.
--
-- ## Why nothing here can turn a `possible` outcome into a retry
--
-- `possible` means the Operator durably crossed the dispatch boundary and never
-- learned whether the effect landed. Re-issuing such a call is how a bounded
-- mutation becomes two, so the SDK must not offer it. That is enforced by the
-- SHAPE of this module rather than by a warning in a comment:
--
--  * No function here takes a prior answer together with a tool name or an
--    argument value. An answer carries the classification, the position
--    identity and the Tool's own result -- and deliberately NOT the arguments
--    the call was made with -- so re-issuing a recorded call is not expressible
--    from anything this module hands a script.
--  * Exactly one function accepts an answer AND issues a Tool call:
--    `reconcile`. The Tool it issues is the literal
--    `framework.workflow.reconcile` compiled into this module, it is read-only,
--    and its whole job is to REPLACE the uncertain classification with a
--    proven one. It refuses any answer that is not `possible`, so it can
--    neither retry a settled call nor be aimed at a different Tool.
--  * `recover` dispatches on the classification and has NO default arm. A
--    script that did not write a `possible` arm gets a refusal naming the
--    classification it failed to handle, rather than falling through into
--    whatever branch it wrote for the ordinary case.
--
-- Cancellation never arrives here as an error. A hard stop destroys the VM
-- without resuming the script, deliberately indistinguishable from a crash; a
-- cooperative human stop arrives as the recorded outcome of the wait Tool,
-- which `stopped` reads.

local tools = require("@umbraflow/tools")

local workflow = {}

local k_waitTool = "framework.workflow.wait"
local k_statusTool = "framework.workflow.status"
local k_reconcileTool = "framework.workflow.reconcile"

local k_durationMember = "duration_ms"
local k_maximumDurationMember = "maximum_duration_ms"
local k_completedMember = "completed"

-- The empty JSON object, which a Luau table literal cannot spell.
local emptyObject = canon.emptyObject

-- The three-way partition of the nine delivery classifications. Every
-- classification is in exactly one of them, so a script that branches on all
-- three has branched on all nine.
local k_pendingStates = table.freeze({
    proposed = true,
    admitted = true,
    dispatching = true,
})
local k_settledStates = table.freeze({
    confirmed = true,
    proven_absent = true,
    rejected = true,
    terminal_failure = true,
})
local k_uncertainStates = table.freeze({
    possible = true,
    terminally_unresolved = true,
})

local function reject(message: string): never
    error("workflow " .. message, 3)
end

local function fromTool(answer: any, expected: string, where: string): any
    if tools.tool_name(answer) ~= expected then
        reject(where .. " needs the answer of " .. expected)
    end
    return answer
end

-- The bound comes from the pinned catalog descriptor rather than a literal
-- here, so a release that changes what the wait Tool admits changes what this
-- module admits, in one place, with the catalog identity moving under it.
local function waitCeiling(): number
    local contract = rawget(tools.describe(k_waitTool), "argument_contract")
    if type(contract) ~= "table" then
        reject("the catalog descriptor for " .. k_waitTool .. " states no argument contract")
    end
    local ceiling = rawget(contract, k_maximumDurationMember)
    if type(ceiling) ~= "number" then
        reject("the catalog descriptor for " .. k_waitTool .. " states no " .. k_maximumDurationMember)
    end
    return ceiling :: number
end

-- Wait for a bounded duration. The blocking is bounded by construction: this is
-- a Tool with a validated maximum, not a sleep the script chose.
function workflow.wait(durationMillis: any): any
    if type(durationMillis) ~= "number" or durationMillis % 1 ~= 0 then
        reject("wait needs a whole number of milliseconds")
    end
    local ceiling = waitCeiling()
    if durationMillis < 0 or durationMillis > ceiling then
        reject("wait duration must lie in [0, " .. tostring(ceiling) .. "]")
    end
    return tools.call(k_waitTool, { [k_durationMember] = durationMillis })
end

-- The run's own workflow status.
function workflow.status(): any
    return tools.call(k_statusTool, emptyObject)
end

-- Whether a completed wait ran its full duration. A cooperative human stop ends
-- the wait early, so `stopped` is the branch a script takes when a person asked
-- it to stand down -- it is a recorded fact about a Tool call and not an error
-- raised into the script.
function workflow.stopped(answer: any): boolean
    local waited = fromTool(answer, k_waitTool, "stopped")
    if tools.state(waited) ~= "confirmed" then
        reject("stopped needs a confirmed wait")
    end
    local payload = rawget(waited, "result")
    if type(payload) ~= "table" then
        reject("a confirmed wait must carry an object result")
    end
    local completed = rawget(payload :: any, k_completedMember)
    if type(completed) ~= "boolean" then
        reject("a confirmed wait must state whether it completed")
    end
    return completed == false
end

function workflow.pending(answer: any): boolean
    return k_pendingStates[tools.state(answer)] == true
end

function workflow.settled(answer: any): boolean
    return k_settledStates[tools.state(answer)] == true
end

function workflow.uncertain(answer: any): boolean
    return k_uncertainStates[tools.state(answer)] == true
end

function workflow.delivered(answer: any): boolean
    return tools.state(answer) == "confirmed"
end

function workflow.absent(answer: any): boolean
    return tools.state(answer) == "proven_absent"
end

function workflow.refused(answer: any): boolean
    local state = tools.state(answer)
    return state == "rejected" or state == "terminal_failure"
end

-- Replace one uncertain classification with a proven one. This is the only
-- function in the module that takes an answer and issues a Tool call, and the
-- Tool it issues is fixed: it cannot be aimed at the call's own Tool, so it
-- cannot be a retry.
function workflow.reconcile(answer: any): any
    local state = tools.state(answer)
    if state ~= "possible" then
        reject("reconcile applies only to a possible outcome, not " .. state)
    end
    return tools.call(k_reconcileTool, {
        call_identity = tools.call_identity(answer),
    })
end

-- Compose a child flow: call a Tool whose own handler may issue Tool calls, so
-- the call opens a new issuing context numbered under this call's position. A
-- Tool the catalog admits no child call for is a leaf, and calling it is
-- `tools.call`; naming it here is a mistake worth a refusal rather than a
-- silent synonym.
function workflow.child_flow(name: any, arguments: any): any
    local children = rawget(tools.describe(name), "child_effects")
    if type(children) ~= "table" then
        reject("the catalog descriptor for " .. tostring(name) .. " states no child effects")
    end
    local maximum = rawget(children :: any, "maximum_child_calls")
    if type(maximum) ~= "number" or maximum <= 0 then
        reject("child_flow names a tool the catalog admits no child call for: " .. tostring(name))
    end
    return tools.call(name, arguments)
end

-- Branch on the delivery classification with no default arm. `handlers` is a
-- table keyed by classification; the arm for THIS answer's classification must
-- be a function, and a missing arm is a refusal naming the classification. That
-- is what keeps `possible` from being swept into the branch a script wrote for
-- the ordinary case.
function workflow.recover(answer: any, handlers: any): any
    local state = tools.state(answer)
    if type(handlers) ~= "table" then
        reject("recover needs a table of handlers keyed by classification")
    end
    local arm = rawget(handlers, state)
    if type(arm) ~= "function" then
        reject("recover has no handler for the " .. state .. " outcome")
    end
    return arm(answer)
end

return table.freeze(workflow)
