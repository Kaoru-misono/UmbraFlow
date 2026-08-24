--!strict
-- Luau may construct one same-cycle request.  The opaque Host Receipt stays
-- entirely outside this module contract.

local function detector(locator: string): any
    return { all = { { kind = "locator_present", locator = locator } }, any = {}, none = {} }
end

local function variants(name: string, value: any): any
    return { { name = name, detector = value } }
end

local runtime = model.compile({
    schema_version = 3,
    base_resolution = { 800, 600 },
    base_dpi = { 96, 96 },
    ui_targets = {
        { id = "title", kind = "region" },
        { id = "confirm", kind = "control" },
        { id = "commit", kind = "control" },
        { id = "card", kind = "control" },
        { id = "list", kind = "control" },
    },
    locators = {
        { id = "title.locator", kind = "template", asset_path = "assets/title.png", threshold = 0.9 },
        { id = "confirm.locator", kind = "template", asset_path = "assets/confirm.png", threshold = 0.9 },
    },
    readers = {},
    bindings = {
        {
            id = "screen.identity",
            surface = "screen",
            ui_target = "title",
            placement = { kind = "fixed", rect = { 10, 10, 200, 50 } },
            variants = variants("default", detector("title.locator")),
            actions = {},
        },
        {
            id = "confirm.enabled",
            surface = "screen",
            ui_target = "confirm",
            placement = { kind = "fixed", rect = { 550, 480, 180, 70 }, action_point = { 640, 515 } },
            variants = variants("enabled", detector("confirm.locator")),
            actions = { { id = "activate", kind = "click", proof_locator = "confirm.locator" } },
        },
        -- The same Surface and the same proof, granting a keystroke instead:
        -- what separates the two requests below is the kind and nothing else.
        {
            id = "commit.enabled",
            surface = "screen",
            ui_target = "commit",
            placement = { kind = "fixed", rect = { 300, 480, 180, 70 } },
            variants = variants("enabled", detector("confirm.locator")),
            actions = {
                { id = "commit", kind = "key", key = "ENTER", proof_locator = "confirm.locator" },
            },
        },
        -- The three kinds the vocabulary gained. A hold and a move aim at the
        -- same measured point a click does; a scroll names none, so its
        -- placement drops the point exactly as a keystroke's does.
        {
            id = "card.present",
            surface = "screen",
            ui_target = "card",
            placement = { kind = "fixed", rect = { 80, 200, 120, 180 }, action_point = { 140, 290 } },
            variants = variants("face_up", detector("confirm.locator")),
            actions = {
                { id = "inspect", kind = "hold", hold_ms = 350, proof_locator = "confirm.locator" },
                { id = "hover", kind = "move", proof_locator = "confirm.locator" },
            },
        },
        {
            id = "list.present",
            surface = "screen",
            ui_target = "list",
            placement = { kind = "fixed", rect = { 400, 200, 120, 180 } },
            variants = variants("open", detector("confirm.locator")),
            actions = {
                { id = "wheel", kind = "scroll", notches = -3, proof_locator = "confirm.locator" },
            },
        },
    },
    collections = {},
    surfaces = {
        { id = "screen", kind = "scene", covers = {}, identity = { "screen.identity" } },
    },
    transitions = {},
})

local function present(cycle: any, binding: string, locator: string, hostProof: any?): any
    return evidence.present(
        cycle,
        { kind = "template", binding = binding, locator = locator },
        { locator = locator },
        0.99,
        hostProof
    )
end

local function key(binding: string, locator: string): string
    return model.measurement_key(binding, { kind = "locator_present", locator = locator })
end

local cycle = evidence.begin_cycle()
local hostProof = table.freeze({ host_owned = true })
local title = present(cycle, "screen.identity", "title.locator")
local confirm = present(cycle, "confirm.enabled", "confirm.locator", hostProof)
local commit = present(cycle, "commit.enabled", "confirm.locator", hostProof)
local card = present(cycle, "card.present", "confirm.locator", hostProof)
local list = present(cycle, "list.present", "confirm.locator", hostProof)
local records = {
    [key("screen.identity", "title.locator")] = title,
    [key("confirm.enabled", "confirm.locator")] = confirm,
    [key("commit.enabled", "confirm.locator")] = commit,
    [key("card.present", "confirm.locator")] = card,
    [key("list.present", "confirm.locator")] = list,
}
local state = resolution.resolve_state(runtime, cycle, records)
local binding = resolution.resolve_binding(runtime, cycle, state, "confirm", nil, records)
local request, reason = resolution.authorize(runtime, cycle, binding, "activate")
assert(request ~= nil, reason)
assert(request.kind == "receipt_request")
assert(request.binding == "confirm.enabled")
assert(request.variant == "enabled")
assert(request.action == "activate")
assert(request.receipt == nil)

-- A click names a coordinate and carries no key.
assert(request.action_kind == "click")
assert(request.key == nil)

local payload = resolution.take_receipt_request(request, cycle)
assert(payload ~= nil)
assert(payload.host_proof == hostProof)
assert(payload.placement.action_point[1] == 640)
assert(resolution.take_receipt_request(request, cycle) == nil)
assert(resolution.take_receipt_request({ id = request.id }, cycle) == nil)

-- A keystroke is the mirror: the key travels so the Host is told what to
-- deliver without reading the model, and the placement it authorizes against
-- carries no point at all.
local keyBinding = resolution.resolve_binding(runtime, cycle, state, "commit", nil, records)
local keyRequest, keyReason = resolution.authorize(runtime, cycle, keyBinding, "commit")
assert(keyRequest ~= nil, keyReason)
assert(keyRequest.action_kind == "key")
assert(keyRequest.key == "ENTER")
assert(keyRequest.action == "commit")

local keyPayload = resolution.take_receipt_request(keyRequest, cycle)
assert(keyPayload ~= nil)
assert(keyPayload.placement.action_point == nil)
assert(keyPayload.proof_locator == "confirm.locator")

-- Each of the three new kinds carries exactly the fields its own act needs and
-- none of the others, so the Host is told what to deliver without reading a
-- model it does not interpret.
local cardBinding = resolution.resolve_binding(runtime, cycle, state, "card", nil, records)
local holdRequest, holdReason = resolution.authorize(runtime, cycle, cardBinding, "inspect")
assert(holdRequest ~= nil, holdReason)
assert(holdRequest.action_kind == "hold")
assert(holdRequest.hold_ms == 350)
assert(holdRequest.key == nil)
assert(holdRequest.notches == nil)
assert(holdRequest.duration_ms == nil)

local holdPayload = resolution.take_receipt_request(holdRequest, cycle)
assert(holdPayload ~= nil)
assert(holdPayload.placement.action_point[1] == 140)

-- One receipt per cycle, so the move below needs a Binding resolved on a cycle
-- that has not already handed one out. It is the same rule the click above
-- proves and is not restated per kind.
local moveCycle = evidence.begin_cycle()
local moveRecords = {
    [key("screen.identity", "title.locator")] = present(moveCycle, "screen.identity", "title.locator"),
    [key("confirm.enabled", "confirm.locator")] = present(moveCycle, "confirm.enabled", "confirm.locator", hostProof),
    [key("commit.enabled", "confirm.locator")] = present(moveCycle, "commit.enabled", "confirm.locator", hostProof),
    [key("card.present", "confirm.locator")] = present(moveCycle, "card.present", "confirm.locator", hostProof),
    [key("list.present", "confirm.locator")] = present(moveCycle, "list.present", "confirm.locator", hostProof),
}
local moveState = resolution.resolve_state(runtime, moveCycle, moveRecords)
local moveBinding = resolution.resolve_binding(runtime, moveCycle, moveState, "card", nil, moveRecords)
local moveRequest, moveReason = resolution.authorize(runtime, moveCycle, moveBinding, "hover")
assert(moveRequest ~= nil, moveReason)
assert(moveRequest.action_kind == "move")
assert(moveRequest.hold_ms == nil)
assert(moveRequest.notches == nil)
assert(resolution.take_receipt_request(moveRequest, moveCycle).placement.action_point[2] == 290)

local listBinding = resolution.resolve_binding(runtime, moveCycle, moveState, "list", nil, moveRecords)
local scrollRequest, scrollReason = resolution.authorize(runtime, moveCycle, listBinding, "wheel")
assert(scrollRequest ~= nil, scrollReason)
assert(scrollRequest.action_kind == "scroll")
assert(scrollRequest.notches == -3)
assert(scrollRequest.hold_ms == nil)
assert(resolution.take_receipt_request(scrollRequest, moveCycle).placement.action_point == nil)
evidence.end_cycle(moveCycle)

-- Copying a resolved value loses the private ledger identity.
local copied = {}
for field, value in binding do copied[field] = value end
local denied, deniedReason = resolution.authorize(runtime, cycle, copied, "activate")
assert(denied == nil)
assert(deniedReason == "binding_not_actionable")

-- A state or Binding cannot cross cycle boundaries.
local otherCycle = evidence.begin_cycle()
local stale, staleReason = resolution.authorize(runtime, otherCycle, binding, "activate")
assert(stale == nil)
assert(staleReason == "binding_not_actionable")
evidence.end_cycle(otherCycle)

-- Even a valid request becomes unusable when its evidence cycle closes.
local closingRequest = resolution.authorize(runtime, cycle, binding, "activate")
assert(closingRequest ~= nil)
evidence.end_cycle(cycle)
assert(resolution.take_receipt_request(closingRequest, cycle) == nil)

-- There is no Luau Host-delivery or direct production input surface.
assert(observe.deliver == nil)
assert(observe.click == nil)
assert(observe.drag == nil)
assert(observe.key == nil)

return 1
