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

-- What one Host read produced, narrowed by the Reader that asked for it.
--
-- A reading is the WHOLE set of lines, so the floor judges the set: one line
-- below it makes the reading low_confidence rather than making that line
-- disappear.  Dropping it would hand a consumer a shorter block than the frame
-- holds, with nothing in the answer to say so, which is the fail-open shape
-- this path refuses everywhere else.
local function readLines(reader: any, reading: any): any
    if type(reading.lines) ~= "table" or #reading.lines == 0 then
        return { kind = "unknown", reason = "ocr_unreadable" }
    end
    local lines = {}
    local lowest = 1
    for index, line in reading.lines do
        if type(line) ~= "table"
            or type(line.text) ~= "string"
            or type(line.rect) ~= "table"
            or #line.rect ~= 4
            or type(line.confidence) ~= "number"
            or line.confidence ~= line.confidence
            or line.confidence < 0
            or line.confidence > 1
        then
            return { kind = "unknown", reason = "ocr_unreadable" }
        end
        if line.confidence < lowest then lowest = line.confidence end
        lines[index] = table.freeze({
            text = normalize(line.text, reader.normalization),
            rect = table.freeze({ line.rect[1], line.rect[2], line.rect[3], line.rect[4] }),
        })
    end
    if lowest < reader.confidence_floor then
        return { kind = "unknown", reason = "low_confidence", confidence = lowest }
    end
    return { kind = "read", lines = table.freeze(lines), confidence = lowest }
end

local function readText(
    state: any,
    key: string,
    readerId: string,
    area: { number }
): any
    local cached = state.reader_values[key]
    if cached ~= nil then return cached end
    local reader = state.model.reader_by_id[readerId]
    if reader == nil then error("RuntimeModel references unknown Reader '" .. readerId .. "'", 3) end
    -- The Reader's own layout travels to the Host, which runs a different
    -- pipeline for each: the model states how many lines its rectangle holds
    -- and the Host never guesses.
    local reading = requireNative("runtime_read")(
        state.ticket,
        area[1],
        area[2],
        area[3],
        area[4],
        reader.layout
    )
    local result
    if type(reading) ~= "table" then
        result = { kind = "unknown", reason = "internal_error" }
    elseif reading.kind == "absent" then
        result = { kind = "absent" }
    elseif reading.kind == "unknown" then
        result = { kind = "unknown", reason = reading.reason }
    elseif reading.kind ~= "read" then
        result = { kind = "unknown", reason = "ocr_unreadable" }
    else
        result = readLines(reader, reading)
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
        local reading = readText(
            state,
            model.reading_key(binding.id, predicate.reader),
            predicate.reader,
            binding.placement.rect
        )
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
        -- One line, equal to the value.  `text_equals` asks whether the text in
        -- this rectangle IS the value, and a set of several lines is not equal
        -- to one string under any reading that does not invent a separator.  A
        -- single_line Reader always answers one line, so this is that Reader's
        -- behaviour unchanged; a block Reader over a rectangle holding two
        -- lines measures absent, which is the conservative half.
        elseif #reading.lines == 1 and reading.lines[1].text == predicate.value then
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
    for _, variant in binding.variants do
        for _, group in { variant.detector.all, variant.detector.any, variant.detector.none } do
            for _, predicate in group do measurePredicate(state, binding, predicate) end
        end
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

    function cycle:resolve_state(expectedStack: { any }?): any
        local current = requireCycle(self)
        return resolution.resolve_state(
            current.model,
            current.evidence_cycle,
            function(binding: any, predicate: any): any
                return measurePredicate(current, binding, predicate)
            end,
            expectedStack
        )
    end

    -- Every reading the resolved state reports, measured on demand rather than
    -- during resolve_state: a Reader costs one Host read out of the cycle's
    -- pool, and a state that resolved no Surface has nothing to attribute one
    -- to.  Exhausting that pool leaves the reading Unknown, which resolve_readings
    -- drops, so a model that declares more reads than a cycle can pay for
    -- reports fewer readings and never a wrong one.
    function cycle:resolve_readings(resolvedState: any): { any }
        local current = requireCycle(self)
        local candidates = resolution.reading_bindings(
            current.model,
            current.evidence_cycle,
            resolvedState
        )
        if candidates ~= nil then
            for _, binding in candidates do measureBinding(current, binding) end
            -- Second pass, and the order is the point: a Reader costs a Host read
            -- and reads the Binding's own rectangle, so only a Binding the first
            -- pass measured present may spend one.
            for _, binding in resolution.reading_targets(
                current.model,
                current.evidence_cycle,
                resolvedState,
                current.predicate_records
            ) do
                for _, readerId in binding.reads do
                    readText(
                        current,
                        model.reading_key(binding.id, readerId),
                        readerId,
                        binding.placement.rect
                    )
                end
            end
        end
        return resolution.resolve_readings(
            current.model,
            current.evidence_cycle,
            resolvedState,
            current.predicate_records,
            current.reader_values
        )
    end

    -- A collection is measured only after its Surface resolved on this cycle.
    -- Its declared search rectangle bounds the block read but creates no item;
    -- the trusted Reader's detected lines are the sole item-existence evidence.
    function cycle:resolve_collection(resolvedState: any, collectionId: any): (any?, string?)
        local current = requireCycle(self)
        local resolved, reason = resolution.resolve_collection(
            current.model,
            current.evidence_cycle,
            resolvedState,
            collectionId,
            current.reader_values
        )
        if resolved ~= nil or reason ~= "not_measured" then return resolved, reason end
        local collection = current.model.collection_by_id[collectionId]
        readText(
            current,
            model.collection_reading_key(collection.id, collection.placement.reader),
            collection.placement.reader,
            collection.placement.search_rect
        )
        local targets = resolution.collection_read_targets(
            current.model,
            current.evidence_cycle,
            resolvedState,
            collectionId,
            current.reader_values
        )
        if targets ~= nil then
            for _, target in targets do
                readText(current, target.key, target.reader, target.rect)
            end
        end
        return resolution.resolve_collection(
            current.model,
            current.evidence_cycle,
            resolvedState,
            collectionId,
            current.reader_values
        )
    end

    function cycle:stitched_map_connectivity_targets(
        mapId: any,
        collection: any
    ): (any?, string?)
        local current = requireCycle(self)
        return resolution.stitched_map_connectivity_targets(
            current.model,
            mapId,
            collection
        )
    end

    function cycle:evaluate_stitched_map(
        mapId: any,
        collection: any,
        readings: any
    ): (any?, string?)
        local current = requireCycle(self)
        return resolution.evaluate_stitched_map(
            current.model,
            mapId,
            collection,
            readings
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
                key = payload.request.key,
                offset = payload.request.offset,
                duration_ms = payload.request.duration_ms,
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
