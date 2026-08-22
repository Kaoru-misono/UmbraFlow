--!strict
-- Ergonomic wrappers over the Framework observation Tool, plus the lifetime and
-- result helpers a snapshot handle needs.
--
-- Every call here goes through `@umbraflow/tools`, which is the SDK's one
-- spelling of a Tool call. The private capability table this module is also
-- handed as its chunk argument is deliberately unused: a second direct use of
-- the native primitive would be a second place where argument shaping, catalog
-- checking and answer validation could drift.
--
-- Two ergonomics here earn their place and neither is cosmetic. First, the
-- observation Tool takes exactly the empty JSON OBJECT, and a Luau `{}` decodes
-- as an empty ARRAY at the value boundary -- so every script that spelled the
-- arguments itself would spell them wrong. `canon.emptyObject` is the one value
-- that crosses as `{}`, and it is written here once. Second, the observation
-- reference is single-use authority: `use` is the only route from a handle to
-- the reference an input Tool consumes, and it refuses a second use in this run
-- BEFORE a Tool-call budget is spent on a reference the Operator would refuse
-- anyway. That is an ergonomic and not a boundary; single use is enforced by
-- the Operator's own spent set, which is the only place it can be enforced
-- across restarts.
--
-- A handle is plain JSON data: the frozen object `framework.screen.observe`
-- answered with. There is no userdata and no metatable, so a handle can be
-- held, returned, recorded and compared as bytes, and replay equality is byte
-- equality of the recorded JSON.

local tools = require("@umbraflow/tools")
local resultModule = require("@umbraflow/result")

local screen = {}

local k_observeTool = "framework.screen.observe"
local k_referenceMember = "observation_reference"
local k_targetsMember = "local_semantic_targets"
local k_actionsMember = "authorized_ui_actions"

-- The empty JSON object, which a Luau table literal cannot spell.
local emptyObject = canon.emptyObject

-- The handles this run has already spent, by handle identity. Per-VM and
-- therefore per-run by construction: the module cache never crosses a VM, so
-- nothing here can leak a spent mark into another run or be consulted from one.
local usedHandles: { [any]: boolean } = {}

local function reject(message: string): never
    error("screen " .. message, 3)
end

local function fromTool(answer: any, expected: string, where: string): any
    if tools.tool_name(answer) ~= expected then
        reject(where .. " needs the answer of " .. expected)
    end
    return answer
end

local function checkedHandle(handle: any): any
    if type(handle) ~= "table" then
        reject("handle must be the object an observation answered with")
    end
    local reference = rawget(handle, k_referenceMember)
    if type(reference) ~= "table" then
        reject("handle must carry an " .. k_referenceMember .. " object")
    end
    return reference
end

local function stringList(reference: any, member: string): any
    local values = rawget(reference, member)
    if type(values) ~= "table" then
        reject("observation reference must carry a " .. member .. " list")
    end
    local count = rawlen(values :: any)
    local copied: { string } = {}
    for index = 1, count do
        local value = rawget(values :: any, index)
        if type(value) ~= "string" then
            reject(member .. " must hold only strings")
        end
        copied[index] = value :: string
    end
    return table.freeze(copied)
end

-- Read one frame. Read-only: it persists a durable observation reference but
-- performs no external-world effect, so it needs no plan authority.
function screen.observe(): any
    return tools.call(k_observeTool, emptyObject)
end

-- The handle a completed observation carries, as a `@umbraflow/result`
-- envelope. An observation that did not confirm has no handle at all, and its
-- delivery classification is the error code, so a caller that ignores the
-- envelope cannot mistake an unconfirmed observation for a frame it saw.
function screen.observation(answer: any): any
    local observed = fromTool(answer, k_observeTool, "observation")
    local state = tools.state(observed)
    if state ~= "confirmed" then
        return resultModule.err(
            "umbraflow.screen." .. state,
            "the observation did not confirm"
        )
    end
    local payload = resultModule.unwrap_or(tools.result(observed), nil)
    if type(payload) ~= "table" then
        reject("a confirmed observation must carry an object result")
    end
    local _ = checkedHandle(payload)
    return resultModule.ok(payload)
end

-- Spend the handle and answer with the reference an input Tool consumes. A
-- second use in this run is refused here rather than at the Operator, which is
-- the whole point: the refusal costs nothing and names the mistake.
function screen.use(handle: any): any
    local reference = checkedHandle(handle)
    if usedHandles[handle] == true then
        reject("handle was already spent in this run")
    end
    usedHandles[handle] = true
    return reference
end

function screen.used(handle: any): boolean
    local _ = checkedHandle(handle)
    return usedHandles[handle] == true
end

-- The snapshot-local semantic targets Project interpretation may name against
-- this observation.
function screen.targets(handle: any): any
    return stringList(checkedHandle(handle), k_targetsMember)
end

-- The UI actions this observation's evidence supports.
function screen.actions(handle: any): any
    return stringList(checkedHandle(handle), k_actionsMember)
end

return table.freeze(screen)
