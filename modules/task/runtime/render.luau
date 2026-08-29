--!strict
-- The Tool face: one Luau function per Tool, rendered from the pinned catalog.
--
-- A caller never sees this module. It sees the modules the host GENERATES from
-- the catalog, one per Tool namespace, each of which is a single line that asks
-- this one to render it:
--
--     local screen = require("@umbraflow/screen")
--     local input  = require("@umbraflow/input")
--
--     local shot  = screen.capture{}.screenshot_sha256
--     local lines = screen.read_lines{
--         screenshot_sha256 = shot, x = 0, y = 640, width = 1600, height = 60,
--     }
--     input.key{ screenshot_sha256 = shot, key = "1" }
--
-- The catalog is the single authority and this module is a RENDERER of it. One
-- mechanical rule maps each catalog entry onto Luau's native call form: a Tool
-- named `<namespace>.<member>` becomes the member `<member>` of the module the
-- host generated for `<namespace>`, taking one argument table and answering
-- with the Tool's own result. NOTHING PER-TOOL EXISTS ANYWHERE -- not a
-- wrapper, not a name, not a source file. A Tool added to the catalog is
-- callable with no edit to any source, and a Tool removed from it stops being
-- callable the same way. This is what an MCP client does with `tools/list`.
--
-- Project Tools and framework Tools are rendered by the same rule, because the
-- catalog holds them identically.
--
-- ARGUMENTS ARE THE CATALOG'S OWN NAMES, IN ONE TABLE. Nothing here reorders,
-- renames or positionally binds anything: a call passes exactly the object the
-- Tool's input schema declares, so what a chunk writes and what an agent or an
-- MCP client sends are the same keys. That is also why adding a member to a
-- Tool cannot break a call that already exists. A member whose name is not a
-- Luau identifier is reached the way Luau reaches any such key --
-- `alpha["observe-1"]{...}` -- which is the language's own index sugar for one
-- expression and not a second spelling of the call.
--
-- A CALL READS AS A CALL. On success the Tool's own result is returned
-- directly, so a call site is `screen.capture{}.screenshot_sha256` and not an
-- envelope to unwrap. On any outcome that is not `confirmed` the whole answer
-- envelope is RAISED as a table -- `ok`, `call_identity`, `delivery` and
-- `error`, exactly as the Tool Runtime answered it.
--
-- Raising discards nothing, which is the property
-- `docs/decisions/2026-08-26-a-tool-is-a-flat-call-over-an-explicit-reference.md`
-- clause Z5 protects: `possible` must stay data because an exception must not
-- destroy the fact that an input may have landed. It does not. A caller writes
-- `local ok, raised = pcall(input.key, arguments)` and reads the identical
-- `delivery` and `error` the envelope carried, so whether to retry a `possible`
-- delivery stays the Project's decision and never the framework's. What raising
-- removes is the ability to FORGET the check -- the exact failure Z5 was
-- written from, where three failed observations were read as "the screen has no
-- text on it" because an ignored envelope looks like a success.
--
-- The chunk argument is the private capability table an interactive scoped Tool
-- session builds for one run and then drops. This is the ONLY module handed it,
-- and it is the only module Project code cannot resolve -- it carries no public
-- name, so the sole route to the Tool Runtime is a generated module the host
-- built from the pinned catalog. Registered handlers receive the same capability
-- through their own child issuing context. It carries exactly one primitive,
-- `invoke(toolName, argumentsObject)`, which runs the Tool to completion and
-- answers one frozen decoded JSON value. Two facts about it shape everything
-- below. It is synchronous, so nothing here yields, polls in the background, or
-- takes a completion callback. And a REFUSAL from it is terminal: the run is
-- recorded as failed and the VM is destroyed without resuming this script, so
-- no `pcall` in this module or above it can observe one. A Tool that ran and
-- failed is not a refusal -- it answers with its terminal delivery value and
-- the provider's error, and that is what is raised.
--
-- The call's POSITION is assigned by the seam and is never a parameter here.
-- The issuing context is the interactive request itself, and its call is a
-- parentless position under that request. A script that could name its ordinal
-- could name another call's position and inherit that call's recorded outcome
-- without authorisation, so a Tool takes its argument object and nothing else.

local native = ...

local catalog = require("@umbraflow/catalog")

local render = {}

local k_identityHexLength = 64

-- Only terminal delivery values cross a synchronous call boundary. Proposed,
-- admitted and dispatching remain ledger states and are not answers. Exactly
-- one of them, `confirmed`, is a success; the Tool Runtime itself sets
-- `ok = true` for that state and for no other.
local k_deliveries = table.freeze({
    confirmed = true,
    proven_absent = true,
    possible = true,
    terminal_failure = true,
    terminally_unresolved = true,
})

local function reject(message: string): never
    error("tool " .. message, 3)
end

-- A load-time refusal, reported without a caller frame because there is no
-- script frame to blame: it fires while a generated module is being rendered,
-- or while this module is loaded by a program type that publishes no Tool
-- Runtime seam at all.
local function refuseLoad(message: string): never
    error("tool " .. message, 0)
end

if type(native) ~= "table" then
    refuseLoad("requires the scoped Tool Runtime capability table")
end
local invoke = rawget(native :: any, "invoke")
if type(invoke) ~= "function" then
    refuseLoad("requires the scoped Tool Runtime invoke primitive")
end

local function contentHash(value: any, where: string): string
    if type(value) ~= "string" or #value ~= k_identityHexLength then
        reject(where .. " must be a 64 character content hash")
    end
    if string.find(value :: string, "^[0-9a-f]+$") == nil then
        reject(where .. " must be lowercase hexadecimal")
    end
    return value :: string
end

-- The answer the Tool Runtime handed back, proved to be one of the two
-- published shapes before anything is read out of it. A violation here is a
-- protocol failure rather than a Tool outcome, so it raises a diagnostic
-- string; a Tool that ran and failed raises the envelope instead.
local function checkedAnswer(answer: any): any
    if type(answer) ~= "table" then
        reject("answer must be the object the Tool Runtime returned")
    end
    local ok = rawget(answer, "ok")
    if type(ok) ~= "boolean" then
        reject("answer must carry an ok boolean")
    end
    local delivery = rawget(answer, "delivery")
    if type(delivery) ~= "string" or k_deliveries[delivery] ~= true then
        reject("answer must carry a terminal delivery value")
    end
    local _ = contentHash(rawget(answer, "call_identity"), "answer call_identity")
    if ok then
        if delivery ~= "confirmed" or rawget(answer, "result") == nil then
            reject("an ok answer must be confirmed and carry result")
        end
        if rawget(answer, "error") ~= nil then
            reject("an ok answer must not carry error")
        end
    else
        if delivery == "confirmed" or rawget(answer, "result") ~= nil then
            reject("a failed answer must be unconfirmed and carry no result")
        end
        local recorded = rawget(answer, "error")
        if type(recorded) ~= "table"
            or type(rawget(recorded, "code")) ~= "string"
            or type(rawget(recorded, "message")) ~= "string"
            or type(rawget(recorded, "retryable")) ~= "boolean"
        then
            reject("a failed answer must carry a code, message, retryable error")
        end
    end
    return answer
end

-- One closure per Tool, closing over its own name. The name is never an
-- argument, so a caller cannot compute one, and a Tool the run did not pin has
-- no closure at all.
--
-- Nothing validates the arguments before the seam does. A Tool's top-level
-- arguments are one JSON OBJECT by contract, the seam enforces exactly that
-- (an empty table crosses as `{}` rather than as an empty array), and the
-- Operator judges the members against the Tool's own input schema. A second
-- check here would be a second authority that can only ever disagree.
local function calls(name: string): any
    return function(arguments: any): any
        local answer = checkedAnswer(invoke(name, arguments))
        if rawget(answer, "ok") ~= true then
            error(answer)
        end
        return rawget(answer, "result")
    end
end

-- Render one Tool namespace: every pinned Tool named `<namespace>.<member>`,
-- keyed by `<member>`. A name with a further dot after the namespace belongs to
-- a DIFFERENT namespace and its own generated module, which is why a Tool name
-- being a dotted prefix of another's is not a collision here at all.
function render.module(namespace: any): any
    if type(namespace) ~= "string" or namespace == "" then
        refuseLoad("a generated module must name the Tool namespace it renders")
    end
    local prefix = (namespace :: string) .. "."
    local members: { [string]: any } = {}
    local rendered = 0
    for _, name in ipairs(catalog.names()) do
        if string.sub(name, 1, #prefix) == prefix then
            local member = string.sub(name, #prefix + 1)
            if string.find(member, ".", 1, true) == nil then
                members[member] = calls(name)
                rendered += 1
            end
        end
    end
    if rendered == 0 then
        refuseLoad(
            "the pinned catalog names no Tool in the namespace "
                .. (namespace :: string)
        )
    end
    return table.freeze(members)
end

return table.freeze(render)
