--!strict
-- Trusted live observation boundary.  It can request an opaque Host Receipt;
-- it has no input-delivery operation.

local native = ...
local model = model
local evidence = evidence

local observe = {}

local cycles: { [any]: any } = setmetatable({}, { __mode = "k" }) :: any
local templates: { [any]: { [string]: any } } = setmetatable({}, { __mode = "k" }) :: any

local function requireNative(name: string): any
    if type(native) ~= "table" or type(native[name]) ~= "function" then
        error("Host does not provide private capability '" .. name .. "'", 3)
    end
    return native[name]
end

local function normalize(value: string, mode: string): string
    if mode == "raw" then return value end
    if mode == "trim" then return (string.gsub(value, "^%s*(.-)%s*$", "%1")) end
    return (string.gsub((string.gsub(value, "%s+", " ")), "^%s*(.-)%s*$", "%1"))
end

local function templateFor(state: any, locator: any): any
    local cache = templates[state.model]
    if cache == nil then
        cache = {}
        templates[state.model] = cache
    end
    local handle = cache[locator.id]
    if handle ~= nil then return handle end
    handle = requireNative("runtime_asset")(state.host_binding, locator.asset_path)
    if handle == nil then error("Host rejected runtime asset '" .. locator.asset_path .. "'", 3) end
    cache[locator.id] = handle
    return handle
end

local function measureLocator(state: any, binding: any, locatorId: string): any
    local key = binding.id .. ":" .. locatorId
    local cached = state.locator_records[key]
    if cached ~= nil then return cached end
    local locator = state.model.locator_by_id[locatorId]
    if locator == nil then error("RuntimeModel references unknown Locator '" .. locatorId .. "'", 3) end
    local area = binding.placement.rect
    local match, hostProof = requireNative("runtime_match")(
        state.ticket,
        templateFor(state, locator),
        area[1],
        area[2],
        area[3],
        area[4]
    )
    local source = { kind = "template", binding = binding.id, locator = locator.id }
    local record
    if type(match) ~= "table" then
        record = evidence.unknown(state.evidence_cycle, "internal_error", source)
    elseif match.kind == "absent" then
        record = evidence.absent(state.evidence_cycle, source)
    elseif match.kind == "unknown" then
        record = evidence.unknown(state.evidence_cycle, match.reason, source)
    elseif match.kind ~= "present"
        or type(match.confidence) ~= "number"
        or match.confidence ~= match.confidence
        or match.confidence < 0
        or match.confidence > 1
        or type(match.rect) ~= "table"
        or hostProof == nil
    then
        record = evidence.unknown(state.evidence_cycle, "internal_error", source)
    elseif match.confidence < locator.threshold then
        record = evidence.absent(state.evidence_cycle, source)
    else
        record = evidence.present(
            state.evidence_cycle,
            source,
            { locator = locator.id, rect = match.rect },
            match.confidence,
            hostProof
        )
    end
    state.locator_records[key] = record
    return record
end

local function readText(state: any, binding: any, readerId: string): any
    local key = binding.id .. ":" .. readerId
    local cached = state.reader_values[key]
    if cached ~= nil then return cached end
    local reader = state.model.reader_by_id[readerId]
    if reader == nil then error("RuntimeModel references unknown Reader '" .. readerId .. "'", 3) end
    local area = binding.placement.rect
    local reading = requireNative("runtime_read")(
        state.ticket,
        area[1],
        area[2],
        area[3],
        area[4]
    )
    local result
    if type(reading) ~= "table" then
        result = { kind = "unknown", reason = "internal_error" }
    elseif reading.kind == "absent" then
        result = { kind = "absent" }
    elseif reading.kind == "unknown" then
        result = { kind = "unknown", reason = reading.reason }
    elseif reading.kind ~= "read"
        or type(reading.text) ~= "string"
        or type(reading.confidence) ~= "number"
        or reading.confidence ~= reading.confidence
        or reading.confidence < 0
        or reading.confidence > 1
    then
        result = { kind = "unknown", reason = "ocr_unreadable" }
    elseif reading.confidence < reader.confidence_floor then
        result = { kind = "unknown", reason = "low_confidence", confidence = reading.confidence }
    else
        result = {
            kind = "read",
            text = normalize(reading.text, reader.normalization),
            confidence = reading.confidence,
        }
    end
    state.reader_values[key] = result
    return result
end

local function measurePredicate(state: any, binding: any, predicate: any): any
    local key = model.measurement_key(binding.id, predicate)
    local cached = state.predicate_records[key]
    if cached ~= nil then return cached end
    local record
    if predicate.kind == "locator_present" then
        record = measureLocator(state, binding, predicate.locator)
    else
        local reading = readText(state, binding, predicate.reader)
        local source = { kind = "text", binding = binding.id, reader = predicate.reader }
        if reading.kind == "unknown" then
            record = evidence.unknown(
                state.evidence_cycle,
                reading.reason,
                source,
                reading.confidence
            )
        elseif reading.kind == "absent" then
            record = evidence.absent(state.evidence_cycle, source)
        elseif reading.text == predicate.value then
            record = evidence.present(
                state.evidence_cycle,
                source,
                { reader = predicate.reader, matched = true },
                reading.confidence
            )
        else
            record = evidence.absent(state.evidence_cycle, source)
        end
    end
    state.predicate_records[key] = record
    return record
end

local function measureBinding(state: any, binding: any)
    for _, group in { binding.detector.all, binding.detector.any, binding.detector.none } do
        for _, predicate in group do measurePredicate(state, binding, predicate) end
    end
end

local function requireCycle(cycle: any): any
    local state = cycles[cycle]
    if state == nil or not state.live then error("observation cycle is closed", 3) end
    return state
end

function observe.open(runtimeModel: any): any
    if not model.is_runtime_model(runtimeModel) then error("observe.open needs RuntimeModel v2", 2) end
    local hostBinding = project.host_binding(runtimeModel)
    if hostBinding == nil then error("observe.open only accepts project.load_project() output", 2) end
    local ticket = requireNative("runtime_cycle_open")(hostBinding)
    if ticket == nil then error("Host refused observation cycle", 2) end

    local state: any = {
        live = true,
        receipt_requested = false,
        model = runtimeModel,
        host_binding = hostBinding,
        ticket = ticket,
        evidence_cycle = evidence.begin_cycle(),
        locator_records = {},
        reader_values = {},
        predicate_records = {},
    }
    local cycle = {}
    cycles[cycle] = state

    function cycle:resolve_state(): any
        local current = requireCycle(self)
        local measured: { [string]: boolean } = {}
        for _, surface in current.model.surfaces do
            for _, group in { surface.identity.all, surface.identity.any, surface.identity.none } do
                for _, bindingId in group do
                    if not measured[bindingId] then
                        measured[bindingId] = true
                        measureBinding(current, current.model.binding_by_id[bindingId])
                    end
                end
            end
        end
        return resolution.resolve_state(
            current.model,
            current.evidence_cycle,
            current.predicate_records
        )
    end

    function cycle:resolve_binding(resolvedState: any, uiTarget: any, bindingId: any?): any
        local current = requireCycle(self)
        local candidates = resolution.binding_candidates(
            current.model,
            current.evidence_cycle,
            resolvedState,
            uiTarget
        )
        if candidates ~= nil then
            for _, binding in candidates do measureBinding(current, binding) end
        end
        return resolution.resolve_binding(
            current.model,
            current.evidence_cycle,
            resolvedState,
            uiTarget,
            bindingId,
            current.predicate_records
        )
    end

    function cycle:authorize(resolvedBinding: any, actionId: any): (any?, string?)
        local current = requireCycle(self)
        if current.receipt_requested then return nil, "receipt_already_requested" end
        local request, reason = resolution.authorize(
            current.model,
            current.evidence_cycle,
            resolvedBinding,
            actionId
        )
        if request == nil then return nil, reason end
        current.receipt_requested = true
        local payload = resolution.take_receipt_request(request, current.evidence_cycle)
        if payload == nil then return nil, "receipt_request_invalid" end
        local receipt = requireNative("runtime_receipt")(
            current.ticket,
            payload.host_proof,
            table.freeze({
                state_identity = payload.request.state_identity,
                surface = payload.request.surface,
                ui_target = payload.request.ui_target,
                binding = payload.request.binding,
                variant = payload.request.variant,
                action = payload.request.action,
                action_kind = payload.request.action_kind,
                placement = payload.placement,
                proof_locator = payload.proof_locator,
            })
        )
        if receipt == nil then return nil, "host_receipt_rejected" end
        return receipt, nil
    end

    function cycle:close()
        local current = cycles[self]
        if current == nil or not current.live then return end
        current.live = false
        evidence.end_cycle(current.evidence_cycle)
        requireNative("runtime_cycle_close")(current.ticket)
    end

    return table.freeze(cycle)
end

return table.freeze(observe)
