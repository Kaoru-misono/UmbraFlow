--!strict
-- Bounded waiting, workflow status, and the human stop.
--
-- Every call here goes through `@umbraflow/tools`, which is the SDK's one
-- spelling of a Tool call; the private capability table this module is also
-- handed as its chunk argument is deliberately unused, for the reason
-- `@umbraflow/screen` states.
--
-- Cancellation never arrives here as an error. A hard stop destroys the VM
-- without resuming the script, deliberately indistinguishable from a crash; a
-- cooperative human stop arrives as the recorded outcome of the wait Tool,
-- which `stopped` reads.

local tools = require("@umbraflow/tools")

local workflow = {}

local k_waitTool = "framework.workflow.wait"
local k_statusTool = "framework.workflow.status"

local k_durationMember = "duration_ms"
local k_maximumDurationMember = "maximum_duration_ms"
local k_completedMember = "completed"

-- The empty JSON object, which a Luau table literal cannot spell.
local emptyObject = canon.emptyObject

local function reject(message: string): never
    error("workflow " .. message, 3)
end

-- The bound comes from the pinned catalog descriptor rather than a literal
-- here, so a release that changes what the wait Tool admits changes what this
-- module admits, in one place, with the catalog identity moving under it.
local function waitCeiling(): number
    local contract = rawget(tools.describe(k_waitTool), "input_schema")
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
    if type(answer) ~= "table"
        or rawget(answer, "ok") ~= true
        or rawget(answer, "delivery") ~= "confirmed"
    then
        reject("stopped needs a confirmed wait")
    end
    local payload = rawget(answer, "result")
    if type(payload) ~= "table" then
        reject("a confirmed wait must carry an object result")
    end
    local completed = rawget(payload :: any, k_completedMember)
    if type(completed) ~= "boolean" then
        reject("a confirmed wait must state whether it completed")
    end
    return completed == false
end

return table.freeze(workflow)
