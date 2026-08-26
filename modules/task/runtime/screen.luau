--!strict
-- Ergonomic wrappers over the Framework screen Tools, plus the lifetime and
-- result helpers a snapshot handle needs.
--
-- Every call here goes through `@umbraflow/tools`, which is the SDK's one
-- spelling of a Tool call. The private capability table this module is also
-- handed as its chunk argument is deliberately unused: a second direct use of
-- the native primitive would be a second place where argument shaping, catalog
-- checking and answer validation could drift.
--
-- Two ergonomics here earn their place and neither is cosmetic. First, capture
-- takes exactly the empty JSON OBJECT, and a Luau `{}` decodes as an empty ARRAY
-- at the value boundary -- so every script that spelled the arguments itself
-- would spell them wrong. `canon.emptyObject` is the one value that crosses as
-- `{}`, and it is written here once. Second, the observation
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

local k_captureTool = "framework.screen.capture"
local k_observeTool = "framework.screen.observe"
local k_cropTool = "framework.screen.crop"
local k_referenceMember = "observation_reference"
local k_uiActionsMember = "ui_actions"

-- The empty JSON object, which a Luau table literal cannot spell.
local emptyObject = canon.emptyObject

-- The handles this run has already spent, by handle identity. Per-VM and
-- therefore per-run by construction: the module cache never crosses a VM, so
-- nothing here can leak a spent mark into another run or be consulted from one.
local usedHandles: { [any]: boolean } = {}

local function reject(message: string): never
    error("screen " .. message, 3)
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

local function uiActionList(reference: any): any
    local values = rawget(reference, k_uiActionsMember)
    if type(values) ~= "table" then
        reject("observation reference must carry a ui_actions list")
    end
    local count = rawlen(values :: any)
    local copied: { any } = {}
    for index = 1, count do
        local value = rawget(values :: any, index)
        if type(value) ~= "table" then
            reject("ui_actions must hold only action objects")
        end
        local uiTarget = rawget(value, "ui_target")
        local binding = rawget(value, "binding")
        local action = rawget(value, "action")
        local kind = rawget(value, "kind")
        if type(uiTarget) ~= "string" or type(binding) ~= "string"
            or type(action) ~= "string" or type(kind) ~= "string"
        then
            reject("each ui_actions entry must name ui_target, binding, action and kind")
        end
        copied[index] = table.freeze({
            ui_target = uiTarget,
            binding = binding,
            action = action,
            kind = kind,
        })
    end
    return table.freeze(copied)
end

-- Capture one frame as durable evidence. The returned receipt names its bytes;
-- observation and measurement take that name explicitly.
function screen.capture(): any
    return tools.call(k_captureTool, emptyObject)
end

-- Resolve one already-captured screenshot into an observation handle. This
-- performs no capture; the Tool reconstructs only the retained frame named by
-- the required hash and closes that cycle before it answers.
function screen.observe(screenshotSha256: string): any
    return tools.call(k_observeTool, {
        screenshot_sha256 = screenshotSha256,
    })
end

-- Derive a new screenshot artifact without writing to the Project.
function screen.crop(
    screenshotSha256: string,
    x: number,
    y: number,
    width: number,
    height: number
): any
    return tools.call(k_cropTool, {
        screenshot_sha256 = screenshotSha256,
        x = x,
        y = y,
        width = width,
        height = height,
    })
end

-- The handle a completed observation carries, as a `@umbraflow/result`
-- envelope. An observation that did not confirm has no handle at all, and its
-- delivery classification is the error code, so a caller that ignores the
-- envelope cannot mistake an unconfirmed observation for a frame it saw.
function screen.observation(answer: any): any
    if type(answer) ~= "table" then
        reject("observation needs a Tool answer")
    end
    local ok = rawget(answer, "ok")
    local delivery = rawget(answer, "delivery")
    if ok ~= true then
        -- THE RECORDED REASON REACHES THE READER, VERBATIM. An outcome that did
        -- not confirm carries the framework's own account of why -- the
        -- provider's exact error -- and the Operator persists it, replays it,
        -- and hands it back on this very
        -- answer. Answering with a constant sentence here threw all of that
        -- away one step before the only reader who has no other way to see it:
        -- a chunk author is not an operator with the ledger open.
        --
        -- Measured 2026-08-25: every observation of a live session failed with
        -- "Operator database write failed: CHECK constraint failed:
        -- target_generation > 0", that sentence arrived here three times, and
        -- this function replaced it with "the observation did not confirm".
        -- Three probes were then read as "the screen has no text on it".
        --
        -- The code keeps classifying by delivery, because a caller matching on
        -- a category should not have to parse prose.
        local recorded = rawget(answer, "error")
        if type(delivery) ~= "string" or type(recorded) ~= "table" then
            reject("an unconfirmed observation must carry delivery and error")
        end
        local reason = rawget(recorded, "message")
        if type(reason) ~= "string" or reason == "" then
            reject("an unconfirmed observation error must carry its message")
        end
        return resultModule.err(
            "umbraflow.screen." .. delivery,
            reason,
            recorded
        )
    end
    if delivery ~= "confirmed" then
        reject("a successful observation must be confirmed")
    end
    local payload = rawget(answer, "result")
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

-- The exact target/binding/action tuples this observation resolved.
function screen.ui_actions(handle: any): any
    return uiActionList(checkedHandle(handle))
end

return table.freeze(screen)
