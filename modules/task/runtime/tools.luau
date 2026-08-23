--!strict
-- Tool discovery, description, calling, and answer access for scoped Project
-- code.
--
-- This module owns the ONE spelling of "make a Tool call" in the Framework SDK.
-- `@umbraflow/screen`, `@umbraflow/workflow` and `@umbraflow/audit` are handed
-- the same private capability table and deliberately ignore it; they reach the
-- Tool Runtime through `tools.call`, so argument shaping, catalog checking and
-- answer validation exist once rather than four times.
--
-- The chunk argument is the private capability table ScopedToolProgram builds
-- for one run and then drops. It carries exactly one primitive,
-- `invoke(toolName, argumentsValue)`, which runs the Tool to completion and
-- answers one frozen decoded JSON value. Two facts about it shape everything
-- below. It is synchronous, so nothing here yields, polls in the background, or
-- takes a completion callback. And a REFUSAL from it is terminal: the run is
-- recorded as failed and the VM is destroyed without resuming this script, so
-- no `pcall` in this module or above it can observe one. A Tool that ran and
-- failed is not a refusal -- its classification travels in the answer's `state`
-- member, which is what `@umbraflow/workflow` reads.
--
-- The call's POSITION is assigned by the seam and is never a parameter here.
-- The issuing context is the run itself: a root automation script's calls are
-- parentless positions under the run's own root request, and a Tool handler's
-- calls are parented under that handler's recorded position. Both are numbered
-- from 1 by the C++ seam. A script that could name its ordinal could name
-- another call's position and inherit that call's recorded outcome without
-- authorisation, so `call` takes a tool name and one value and nothing else.
--
-- Discovery and description are frozen DATA, not a call: the run's pinned Tool
-- catalog arrives as the read-only JSON resource named below, is validated once
-- while this module loads, and is answered from there forever after. Reading it
-- spends no Tool-call budget and creates no ToolInvocation. It is ADVISORY: the
-- Operator validates the tool name, the arguments and the authority again on
-- every call, so a catalog that disagreed with the Operator can only make this
-- module refuse early, never make the Operator accept.
--
-- One answer has exactly this shape, and every accessor below rejects anything
-- else:
--
--     {
--         tool          = "<the tool name the seam ran>",
--         call_identity = "<64 lowercase hex, the recorded position>",
--         state         = "<one of the eight delivery classifications>",
--         result        = <the Tool's own answer, absent when it carries none>,
--         evidence      = <canonical evidence, absent when there is none>,
--     }

local native = ...
local collections = require("@umbraflow/collections")
local resultModule = require("@umbraflow/result")

local tools = {}

-- The read-only resource the host bakes the run's pinned Tool catalog into. It
-- is a resource rather than a native call because discovery must cost nothing
-- and must be identical on replay: the bytes are pinned at program
-- construction, so a description cannot change under a script mid-run.
local k_catalogResource = "umbraflow.tool-catalog"

local k_identityHexLength = 64

-- The delivery vocabulary, spelled exactly as the Operator's ToolCallState wire
-- names. It is published so a script can enumerate what it must be prepared
-- for, and it is the set `workflow.recover` requires an arm from.
local k_states = table.freeze({
    "proposed",
    "admitted",
    "dispatching",
    "confirmed",
    "proven_absent",
    "possible",
    "terminal_failure",
    "terminally_unresolved",
})

local function reject(message: string): never
    error("tools " .. message, 3)
end

-- A load-time refusal, reported without a caller frame because there is no
-- script frame to blame: it fires when this module is loaded by a program type
-- that publishes no Tool Runtime seam at all.
local function refuseLoad(message: string): never
    error("tools " .. message, 0)
end

if type(native) ~= "table" then
    refuseLoad("requires the scoped Tool Runtime capability table")
end
local invoke = rawget(native :: any, "invoke")
if type(invoke) ~= "function" then
    refuseLoad("requires the scoped Tool Runtime invoke primitive")
end

local stateSet = collections.set(k_states)

local function contentHash(value: any, where: string): string
    if type(value) ~= "string" or #value ~= k_identityHexLength then
        reject(where .. " must be a 64 character content hash")
    end
    if string.find(value :: string, "^[0-9a-f]+$") == nil then
        reject(where .. " must be lowercase hexadecimal")
    end
    return value :: string
end

local function checkedAnswer(answer: any): any
    if type(answer) ~= "table" then
        reject("answer must be the object the Tool Runtime returned")
    end
    local name = rawget(answer, "tool")
    if type(name) ~= "string" or name == "" then
        reject("answer must name the tool it came from")
    end
    local state = rawget(answer, "state")
    if type(state) ~= "string" or not collections.has(stateSet, state) then
        reject("answer must carry a known delivery classification")
    end
    local _ = contentHash(rawget(answer, "call_identity"), "answer call_identity")
    return answer
end

-- The catalog, validated once. Every refusal here fails the whole scoped
-- generation rather than one call, which is correct: a catalog a script cannot
-- read is a program that cannot be reviewed.
local function loadedCatalog(): ({ [string]: any }, { string }, string)
    local document = resource.readJson(k_catalogResource)
    if type(document) ~= "table" then
        refuseLoad("catalog resource must be a JSON object")
    end
    local digest = rawget(document, "catalog_hash")
    if
        type(digest) ~= "string"
        or #digest ~= k_identityHexLength
        or string.find(digest :: string, "^[0-9a-f]+$") == nil
    then
        refuseLoad("catalog resource must carry a lowercase hex catalog_hash")
    end
    local entries = rawget(document, "tools")
    if type(entries) ~= "table" then
        refuseLoad("catalog resource must carry a tools list")
    end

    local byName: { [string]: any } = {}
    local names: { string } = {}
    local count = rawlen(entries :: any)
    for index = 1, count do
        local entry = rawget(entries :: any, index)
        if type(entry) ~= "table" then
            refuseLoad("catalog tools must all be descriptor objects")
        end
        local name = rawget(entry, "name")
        if type(name) ~= "string" or name == "" then
            refuseLoad("catalog descriptor must name its tool")
        end
        -- Strictly increasing, which refuses an unsorted catalog and a
        -- duplicated tool in one comparison. The catalog owner renders the
        -- array in UTF-8 order of the names, so a catalog that arrives out of
        -- order is a catalog that was rewritten by something else.
        if index > 1 and not (names[index - 1] < (name :: string)) then
            refuseLoad("catalog tools must be sorted by name and unique")
        end
        names[index] = name :: string
        byName[name :: string] = entry
    end
    if count == 0 then
        refuseLoad("catalog resource declares no tool at all")
    end
    return byName, table.freeze(names), digest :: string
end

local descriptorByName, catalogNames, catalogHash = loadedCatalog()

-- The identity of the exact catalog bytes this run was pinned to. A script that
-- records it has recorded which descriptions it acted on.
tools.catalog_hash = catalogHash

-- The eight delivery classifications, in the Operator's own order.
tools.states = k_states

function tools.names(): any
    return catalogNames
end

function tools.knows(name: any): boolean
    if type(name) ~= "string" then
        reject("knows needs a tool name string")
    end
    return descriptorByName[name] ~= nil
end

function tools.describe(name: any): any
    if type(name) ~= "string" then
        reject("describe needs a tool name string")
    end
    local descriptor = descriptorByName[name]
    if descriptor == nil then
        reject("describe names a tool the pinned catalog does not declare: " .. name)
    end
    return descriptor
end

-- Issue one Tool call at this run's next position. `arguments` is the value the
-- Tool's own argument contract states; it is a table because every Tool
-- contract in the catalog takes an object, and an empty object is spelled
-- `canon.emptyObject` rather than `{}`, which is an empty ARRAY at this
-- boundary.
--
-- The catalog check refuses a name no descriptor covers before a Tool-call
-- budget is spent on it. It is not an authority check: the Operator judges the
-- name, the arguments, the policy and the grants again, and only its verdict
-- decides anything.
function tools.call(name: any, arguments: any): any
    if type(name) ~= "string" then
        reject("call needs a tool name string")
    end
    if descriptorByName[name] == nil then
        reject("call names a tool the pinned catalog does not declare: " .. name)
    end
    if type(arguments) ~= "table" then
        reject("call needs an object value for arguments")
    end
    return checkedAnswer(invoke(name, arguments))
end

function tools.tool_name(answer: any): string
    return rawget(checkedAnswer(answer), "tool")
end

function tools.call_identity(answer: any): string
    return rawget(checkedAnswer(answer), "call_identity")
end

function tools.state(answer: any): string
    return rawget(checkedAnswer(answer), "state")
end

-- The Tool's own answer, as a `@umbraflow/result` envelope: an outcome that
-- carries no payload is a failure of the request "give me the payload" and not
-- a value a caller can mistake for one.
function tools.result(answer: any): any
    local payload = rawget(checkedAnswer(answer), "result")
    if payload == nil then
        return resultModule.err(
            "umbraflow.tools.no_result",
            "the recorded outcome carries no result"
        )
    end
    return resultModule.ok(payload)
end

function tools.evidence(answer: any): any
    local recorded = rawget(checkedAnswer(answer), "evidence")
    if recorded == nil then
        return resultModule.err(
            "umbraflow.tools.no_evidence",
            "the recorded outcome carries no evidence"
        )
    end
    return resultModule.ok(recorded)
end

return table.freeze(tools)
