--!strict
-- Two-stage, same-cycle RuntimeModel v2 resolver.

local model = model
local evidence = evidence

local resolution = {}

local nextStateId = 0
local nextBindingId = 0
local nextRequestId = 0
local stateLedger: { [any]: any } = setmetatable({}, { __mode = "k" }) :: any
local bindingLedger: { [any]: any } = setmetatable({}, { __mode = "k" }) :: any
local requestLedger: { [any]: any } = setmetatable({}, { __mode = "k" }) :: any

local function freezeList(values: { any }): { any }
    return table.freeze(values)
end

local function copyList(values: { any }): { any }
    local result = {}
    for index, value in values do result[index] = value end
    return freezeList(result)
end

local function sameList(left: { any }, right: { any }): boolean
    if #left ~= #right then return false end
    for index, value in left do
        if right[index] ~= value then return false end
    end
    return true
end

local function summaries(records: { any }, cycle: any): { any }
    local result = {}
    local seen: { [string]: boolean } = {}
    for _, record in records do
        local summary = evidence.summary(record, cycle)
        if summary ~= nil and not seen[summary.id] then
            seen[summary.id] = true
            table.insert(result, summary)
        end
    end
    return freezeList(result)
end

local function allEvidence(evaluations: { any }): { any }
    local result = {}
    local seen: { [string]: boolean } = {}
    for _, evaluation in evaluations do
        for _, summary in evaluation.evidence do
            if not seen[summary.id] then
                seen[summary.id] = true
                table.insert(result, summary)
            end
        end
    end
    return freezeList(result)
end

local function combine(
    allGroup: { any },
    anyGroup: { any },
    noneGroup: { any },
    outcome: (any) -> any
): any
    local absent = false
    local unknown = false
    local weakest = 1

    for _, item in allGroup do
        local value = outcome(item)
        if value.kind == "absent" then absent = true end
        if value.kind == "unknown" then unknown = true end
        if value.kind == "present" and value.confidence < weakest then weakest = value.confidence end
    end

    if #anyGroup > 0 then
        local anyPresent = false
        local anyUnknown = false
        for _, item in anyGroup do
            local value = outcome(item)
            if value.kind == "present" then
                anyPresent = true
                if value.confidence < weakest then weakest = value.confidence end
            elseif value.kind == "unknown" then
                anyUnknown = true
            end
        end
        if not anyPresent then
            if anyUnknown then unknown = true else absent = true end
        end
    end

    for _, item in noneGroup do
        local value = outcome(item)
        if value.kind == "present" then absent = true end
        if value.kind == "unknown" then unknown = true end
    end

    local kind = if absent then "absent" elseif unknown then "unknown" else "present"
    return { kind = kind, confidence = if kind == "present" then weakest else nil }
end

local function detectorRecords(detector: any): { any }
    local result = {}
    for _, group in { detector.all, detector.any, detector.none } do
        for _, predicate in group do table.insert(result, predicate) end
    end
    return result
end

local function evaluateBinding(binding: any, recordsByKey: any, cycle: any): any
    local supplied = if type(recordsByKey) == "table" then recordsByKey else {}
    local used = {}
    local recordByPredicate: { [string]: any } = {}
    local outcomeByPredicate: { [string]: any } = {}
    for _, predicate in detectorRecords(binding.detector) do
        local predicateKey = model.predicate_key(predicate)
        local measurementKey = model.measurement_key(binding.id, predicate)
        local record = if type(recordsByKey) == "function"
            then recordsByKey(binding, predicate)
            else supplied[measurementKey]
        local outcome = evidence.result_of(record, cycle)
        if outcome == nil then
            record = evidence.unknown(cycle, "not_measured", {
                kind = "measurement",
                binding = binding.id,
                predicate = predicateKey,
            })
            outcome = evidence.result_of(record, cycle)
        end
        recordByPredicate[predicateKey] = record
        outcomeByPredicate[predicateKey] = outcome
        table.insert(used, record)
    end
    local combined = combine(
        binding.detector.all,
        binding.detector.any,
        binding.detector.none,
        function(predicate: any): any return outcomeByPredicate[model.predicate_key(predicate)] end
    )
    return {
        kind = combined.kind,
        confidence = combined.confidence,
        binding = binding,
        records = used,
        record_by_predicate = recordByPredicate,
        evidence = summaries(used, cycle),
    }
end

local function evaluateSurface(
    runtimeModel: any,
    surface: any,
    recordsByKey: any,
    cycle: any
): any
    local evaluations = {}
    local byBinding: { [string]: any } = {}
    for _, group in { surface.identity.all, surface.identity.any, surface.identity.none } do
        for _, bindingId in group do
            if byBinding[bindingId] == nil then
                local evaluated = evaluateBinding(runtimeModel.binding_by_id[bindingId], recordsByKey, cycle)
                byBinding[bindingId] = evaluated
                table.insert(evaluations, evaluated)
            end
        end
    end
    local combined = combine(
        surface.identity.all,
        surface.identity.any,
        surface.identity.none,
        function(bindingId: any): any return byBinding[bindingId] end
    )
    return {
        kind = combined.kind,
        confidence = combined.confidence,
        surface = surface,
        binding_evaluations = evaluations,
        evidence = allEvidence(evaluations),
    }
end

local function unknownState(reason: string, evaluations: { any }, diagnostic: string?): any
    return table.freeze({
        kind = "unknown_state",
        reason = reason,
        evidence = allEvidence(evaluations),
        diagnostic = diagnostic,
    })
end

local function ambiguousState(candidates: { any }, reason: string, evaluations: { any }): any
    return table.freeze({
        kind = "ambiguous_state",
        candidates = freezeList(candidates),
        conflicts = freezeList({ table.freeze({ kind = reason }) }),
        evidence = allEvidence(evaluations),
    })
end

local function resolvedState(
    runtimeModel: any,
    cycle: any,
    stack: { any },
    evaluations: { any }
): any
    nextStateId += 1
    local result = table.freeze({
        kind = "resolved_state",
        id = "state-resolution-" .. tostring(nextStateId),
        ordered_surface_stack = copyList(stack),
        evidence = allEvidence(evaluations),
    })
    stateLedger[result] = {
        cycle = cycle,
        model = runtimeModel,
        stack = result.ordered_surface_stack,
    }
    return result
end

local function confirmExpected(
    runtimeModel: any,
    cycle: any,
    recordsByKey: any,
    expectedStack: { any }
): any?
    local evaluations = {}
    for _, surfaceId in expectedStack do
        local evaluated = evaluateSurface(
            runtimeModel,
            runtimeModel.surface_by_id[surfaceId],
            recordsByKey,
            cycle
        )
        table.insert(evaluations, evaluated)
        if evaluated.kind ~= "present" then return nil end
    end
    return resolvedState(runtimeModel, cycle, expectedStack, evaluations)
end

function resolution.resolve_state(
    runtimeModel: any,
    cycle: any,
    recordsByKey: any,
    expectedStack: { any }?
): any
    if not model.is_runtime_model(runtimeModel) then error("resolve_state needs RuntimeModel v2", 2) end
    if not evidence.is_cycle_live(cycle) then error("resolve_state needs a live framework cycle", 2) end
    if expectedStack ~= nil then
        if not model.valid_surface_stack(runtimeModel, expectedStack) then
            error("resolve_state expected stack must be a declared Surface stack", 2)
        end
        local confirmed = confirmExpected(runtimeModel, cycle, recordsByKey, expectedStack)
        if confirmed ~= nil then return confirmed end
    end

    local evaluations = {}
    local scenes = {}
    local presentLayers = {}
    local unknownScenes = 0
    for _, surface in runtimeModel.surfaces do
        local evaluated = evaluateSurface(runtimeModel, surface, recordsByKey, cycle)
        table.insert(evaluations, evaluated)
        if surface.kind == "scene" then
            if evaluated.kind == "present" then table.insert(scenes, surface) end
            if evaluated.kind == "unknown" then unknownScenes += 1 end
        elseif evaluated.kind == "present" then
            table.insert(presentLayers, surface)
        end
    end

    if unknownScenes > 0 then return unknownState("unknown_scene_competitor", evaluations) end
    if #scenes == 0 then
        return unknownState(
            "no_scene_candidate",
            evaluations,
            if #presentLayers == 0 then "visible_content_matched_nothing" else nil
        )
    end
    if #scenes > 1 then
        local candidates = {}
        for _, scene in scenes do
            table.insert(candidates, table.freeze({ ordered_surface_stack = freezeList({ scene.id }) }))
        end
        return ambiguousState(candidates, "multiple_scenes", evaluations)
    end

    local scene = scenes[1]
    for _, evaluated in evaluations do
        local surface = evaluated.surface
        if surface.kind ~= "scene" and evaluated.kind == "unknown"
            and model.surface_covers(runtimeModel, surface.id, scene.id)
        then
            return unknownState("unknown_layer_competitor", evaluations)
        end
    end

    for _, surface in presentLayers do
        if not model.surface_covers(runtimeModel, surface.id, scene.id) then
            return ambiguousState({}, "incompatible_present_layer", evaluations)
        end
    end
    for left = 1, #presentLayers do
        for right = left + 1, #presentLayers do
            if not model.surface_covers(runtimeModel, presentLayers[left].id, presentLayers[right].id)
                and not model.surface_covers(runtimeModel, presentLayers[right].id, presentLayers[left].id)
            then
                return ambiguousState({}, "incomparable_present_layers", evaluations)
            end
        end
    end
    table.sort(presentLayers, function(left, right)
        return model.surface_covers(runtimeModel, right.id, left.id)
    end)
    local stack = { scene.id }
    for _, surface in presentLayers do table.insert(stack, surface.id) end
    if not model.valid_surface_stack(runtimeModel, stack) then
        return ambiguousState({}, "invalid_surface_stack", evaluations)
    end

    return resolvedState(runtimeModel, cycle, stack, evaluations)
end

-- Compares an offline observation with declared policy. The observation names
-- its declaration rather than duplicating its trigger, so a replay cannot
-- become a second policy authority. Both destinations in the returned report
-- are owned frozen copies: retaining the report cannot retain a mutable caller
-- table or rewrite the RuntimeModel it was compared with.
function resolution.compare_transition(
    runtimeModel: any,
    observedTransition: any
): (any?, string?)
    if not model.is_runtime_model(runtimeModel) or type(observedTransition) ~= "table" then
        return nil, "invalid_observed_transition"
    end
    for field in observedTransition do
        if field ~= "kind" and field ~= "transition" and field ~= "observed_to_surfaces" then
            return nil, "invalid_observed_transition"
        end
    end
    if observedTransition.kind ~= "observed_transition"
        or type(observedTransition.transition) ~= "string"
    then
        return nil, "invalid_observed_transition"
    end
    local declared = runtimeModel.transition_by_id[observedTransition.transition]
    if declared == nil then return nil, "unknown_transition" end
    if not model.valid_surface_stack(runtimeModel, observedTransition.observed_to_surfaces) then
        return nil, "invalid_observed_destination"
    end

    local declaredDestination = copyList(declared.to_surfaces)
    local observedDestination = copyList(observedTransition.observed_to_surfaces)
    return table.freeze({
        kind = "transition_comparison",
        transition = declared.id,
        declared_to_surfaces = declaredDestination,
        observed_to_surfaces = observedDestination,
        matches_policy = sameList(declaredDestination, observedDestination),
    }), nil
end

local function unknownBinding(reason: string, evaluations: { any }): any
    return table.freeze({
        kind = "unknown_binding",
        reason = reason,
        evidence = allEvidence(evaluations),
    })
end

function resolution.binding_candidates(
    runtimeModel: any,
    cycle: any,
    resolvedState: any,
    uiTarget: any
): { any }?
    if not model.is_runtime_model(runtimeModel)
        or type(uiTarget) ~= "string"
        or runtimeModel.ui_target_by_id[uiTarget] == nil
    then
        return nil
    end
    local stateEntry = stateLedger[resolvedState]
    if stateEntry == nil
        or stateEntry.cycle ~= cycle
        or stateEntry.model ~= runtimeModel
        or not evidence.is_cycle_live(cycle)
    then
        return nil
    end
    local result = {}
    local top = stateEntry.stack[#stateEntry.stack]
    for _, candidate in runtimeModel.bindings_by_surface[top] or {} do
        if candidate.ui_target == uiTarget then table.insert(result, candidate) end
    end
    return freezeList(result)
end

local function collectionTarget(
    runtimeModel: any,
    cycle: any,
    resolvedState: any,
    collectionId: any
): (any?, string?)
    if not model.is_runtime_model(runtimeModel)
        or type(collectionId) ~= "string"
        or runtimeModel.collection_by_id[collectionId] == nil
    then
        return nil, "unknown_collection"
    end
    local stateEntry = stateLedger[resolvedState]
    if stateEntry == nil
        or stateEntry.cycle ~= cycle
        or stateEntry.model ~= runtimeModel
        or not evidence.is_cycle_live(cycle)
    then
        return nil, "stale_state"
    end
    local collection = runtimeModel.collection_by_id[collectionId]
    for _, surfaceId in stateEntry.stack do
        if collection.surface == surfaceId then return collection, nil end
    end
    return nil, "collection_off_surface"
end

local function orderedCollectionLines(collection: any, lines: { any }): ({ any }?, string?)
    local ordered = {}
    for index, line in lines do ordered[index] = line end
    local axis = if collection.placement.order == "left_to_right" then 1 else 2
    local extent = if axis == 1 then 3 else 4
    table.sort(ordered, function(left: any, right: any): boolean
        return left.rect[axis] < right.rect[axis]
    end)
    for index = 2, #ordered do
        local previous = ordered[index - 1].rect
        local current = ordered[index].rect
        if previous[axis] + previous[extent] > current[axis] then
            return nil, "ambiguous_order"
        end
    end
    return ordered, nil
end

-- Resolves one variable-cardinality collection against the same live state
-- that made its Surface available. A bare search rectangle contributes nothing:
-- only an absent or successful answer from the collection's trusted block
-- Reader can resolve a count. Unknown reading and overlapping spatial spans
-- refuse instead of fabricating items or inheriting adapter enumeration order.
function resolution.resolve_collection(
    runtimeModel: any,
    cycle: any,
    resolvedState: any,
    collectionId: any,
    readingsByKey: any
): (any?, string?)
    local collection, targetReason = collectionTarget(
        runtimeModel,
        cycle,
        resolvedState,
        collectionId
    )
    if collection == nil then return nil, targetReason end
    local supplied = if type(readingsByKey) == "table" then readingsByKey else {}
    local reading = supplied[model.collection_reading_key(
        collection.id,
        collection.placement.reader
    )]
    if type(reading) ~= "table" then return nil, "not_measured" end
    if reading.kind == "absent" then
        return table.freeze({ count = 0, items = freezeList({}) }), nil
    end
    if reading.kind ~= "read" or type(reading.lines) ~= "table" then
        local reason = if reading.kind == "unknown" and evidence.is_unknown_reason(reading.reason)
            then reading.reason
            else "ocr_unreadable"
        return nil, reason
    end
    local ordered, orderReason = orderedCollectionLines(collection, reading.lines)
    if ordered == nil then return nil, orderReason end
    local items = {}
    for position, line in ordered do
        items[position] = table.freeze({
            index = position - 1,
            rect = line.rect,
        })
    end
    return table.freeze({ count = #items, items = freezeList(items) }), nil
end

-- The Bindings a resolved state could report readings from: every Binding on a
-- Surface of its stack that named a Reader in `reads`.  It is the reading
-- sibling of binding_candidates and refuses on the same terms, so a state from
-- another cycle or another model reports nothing rather than reporting readings
-- taken against a world it did not resolve.
function resolution.reading_bindings(
    runtimeModel: any,
    cycle: any,
    resolvedState: any
): { any }?
    if not model.is_runtime_model(runtimeModel) then return nil end
    local stateEntry = stateLedger[resolvedState]
    if stateEntry == nil
        or stateEntry.cycle ~= cycle
        or stateEntry.model ~= runtimeModel
        or not evidence.is_cycle_live(cycle)
    then
        return nil
    end
    local result = {}
    for _, surfaceId in stateEntry.stack do
        for _, candidate in runtimeModel.bindings_by_surface[surfaceId] or {} do
            if #candidate.reads > 0 then table.insert(result, candidate) end
        end
    end
    return freezeList(result)
end

-- The Bindings a resolved state may report a reading from, out of the candidates
-- above: measured present, and the only present Binding of their UiTarget.
--
-- A Binding contributes only while its own detector measures present, because a
-- Reader reads that Binding's rectangle: reading through a Binding that is not
-- showing returns whatever is at those pixels instead.  It is a separate verb
-- rather than a step inside resolve_readings so that the caller that spends the
-- Host reads and the projection that consumes them apply ONE rule -- an absent
-- Binding must not cost a read, and a rule spelled twice would agree by test
-- rather than by construction.
--
-- The report is keyed by UiTarget rather than by Binding because a Binding is a
-- UiTarget plus a visual variant, and the variant that matches flips between two
-- captures of one unchanged instance whenever a hover or a highlight has its own
-- Binding -- which would move state_resolution_hash and every decision basis
-- under it.  The cost is that two present Bindings of one UiTarget cannot be
-- told apart, so that UiTarget reports nothing at all: which instance is on
-- screen is precisely what resolve_binding already refuses to guess.
function resolution.reading_targets(
    runtimeModel: any,
    cycle: any,
    resolvedState: any,
    recordsByKey: any
): { any }
    local candidates = resolution.reading_bindings(runtimeModel, cycle, resolvedState)
    if candidates == nil then return freezeList({}) end

    local present = {}
    local seenTarget: { [string]: boolean } = {}
    local ambiguousTarget: { [string]: boolean } = {}
    for _, binding in candidates do
        local evaluated = evaluateBinding(binding, recordsByKey, cycle)
        if evaluated.kind == "present" then
            if seenTarget[binding.ui_target] then
                ambiguousTarget[binding.ui_target] = true
            end
            seenTarget[binding.ui_target] = true
            table.insert(present, binding)
        end
    end

    local result = {}
    for _, binding in present do
        if not ambiguousTarget[binding.ui_target] then table.insert(result, binding) end
    end
    return freezeList(result)
end

-- What a resolved state reports it read: one entry per Reader every reporting
-- Binding named, sorted by UiTarget then Reader.  Each is
-- `{ ui_target, reader, kind }` plus `lines` when it read and `reason` when it
-- could not.
--
-- A reading is a LIST of lines under either Reader layout.  A `single_line`
-- Reader reports one element rather than a second shape, because a consumer
-- that has to ask which spelling it was handed has been handed two contracts.
--
-- All three outcomes travel.  A consumer told only about successful reads
-- cannot separate "nothing is written here" from "this frame was unreadable",
-- and has to fail closed on a single blurry capture; that is what dropping the
-- failures cost.  The length of the list is therefore decided by the model and
-- the resolved stack rather than by what the Host managed to answer, and a
-- reading nothing supplied is `not_measured` -- the rule evaluateBinding
-- already applies one function up to a predicate nothing measured.
--
-- The score still does not travel.  It has already been compared against the
-- Reader's confidence_floor, which is the trusted Reader's judgement and not
-- the caller's to re-take; carrying the number as well would put a value that
-- differs between two captures of one unchanged screen inside a hashed decision
-- basis.  A bounded reason is safe there, a float is not.
--
-- Each line's RECT does travel, and is hashed with the rest of this document.
-- That is the ruling and not an oversight: a block Reader's rectangle is
-- measured, so it can move by a pixel between two captures of one unchanged
-- screen exactly as the score can -- but unlike the score it has a decision
-- left in it, because it is where the consumer acts.  A basis that omitted an
-- input the consumer branches on would let a frozen plan be re-derived into a
-- different action without the basis moving, and unsoundness outranks an extra
-- approval.  Under `single_line` the rect is the Binding's declared placement
-- and cannot move at all.
function resolution.resolve_readings(
    runtimeModel: any,
    cycle: any,
    resolvedState: any,
    recordsByKey: any,
    readingsByKey: any
): { any }
    local supplied = if type(readingsByKey) == "table" then readingsByKey else {}
    local result = {}
    for _, binding in resolution.reading_targets(
        runtimeModel,
        cycle,
        resolvedState,
        recordsByKey
    ) do
        for _, readerId in binding.reads do
            local reading = supplied[model.reading_key(binding.id, readerId)]
            local kind = "unknown"
            local lines = nil
            local reason = "not_measured"
            if type(reading) == "table" then
                if reading.kind == "read" and type(reading.lines) == "table" then
                    kind = "read"
                    lines = reading.lines
                    reason = nil
                elseif reading.kind == "absent" then
                    kind = "absent"
                    reason = nil
                elseif reading.kind == "unknown"
                    and evidence.is_unknown_reason(reading.reason)
                then
                    reason = reading.reason
                end
            end
            table.insert(result, table.freeze({
                ui_target = binding.ui_target,
                reader = readerId,
                kind = kind,
                lines = lines,
                reason = reason,
            }))
        end
    end
    table.sort(result, function(left: any, right: any): boolean
        if left.ui_target ~= right.ui_target then
            return left.ui_target < right.ui_target
        end
        return left.reader < right.reader
    end)
    return freezeList(result)
end

function resolution.resolve_binding(
    runtimeModel: any,
    cycle: any,
    resolvedState: any,
    uiTarget: any,
    bindingId: any?,
    recordsByKey: any
): any
    if not model.is_runtime_model(runtimeModel) then error("resolve_binding needs RuntimeModel v2", 2) end
    if type(uiTarget) ~= "string" or runtimeModel.ui_target_by_id[uiTarget] == nil then
        error("resolve_binding needs a declared UiTarget", 2)
    end
    if bindingId ~= nil and type(bindingId) ~= "string" then error("binding_id must be a string", 2) end
    local stateEntry = stateLedger[resolvedState]
    if stateEntry == nil
        or stateEntry.cycle ~= cycle
        or stateEntry.model ~= runtimeModel
        or not evidence.is_cycle_live(cycle)
    then
        return unknownBinding("state_not_actionable", {})
    end

    local candidates = resolution.binding_candidates(runtimeModel, cycle, resolvedState, uiTarget) or {}
    if #candidates == 0 then return unknownBinding("no_binding_candidate", {}) end

    local evaluations = {}
    local present = {}
    local hasUnknown = false
    for _, candidate in candidates do
        local evaluated = evaluateBinding(candidate, recordsByKey, cycle)
        table.insert(evaluations, evaluated)
        if evaluated.kind == "unknown" then hasUnknown = true end
        if evaluated.kind == "present" then table.insert(present, evaluated) end
    end
    if hasUnknown then return unknownBinding("unknown_competitor", evaluations) end
    if #present == 0 then return unknownBinding("no_binding_candidate", evaluations) end
    if #present > 1 then
        local ids = {}
        for _, evaluated in present do table.insert(ids, evaluated.binding.id) end
        return table.freeze({
            kind = "ambiguous_binding",
            candidates = freezeList(ids),
            evidence = allEvidence(evaluations),
        })
    end
    if bindingId ~= nil and present[1].binding.id ~= bindingId then
        return unknownBinding("requested_binding_absent", evaluations)
    end

    nextBindingId += 1
    local selected = present[1]
    local result = table.freeze({
        kind = "resolved_binding",
        id = "binding-resolution-" .. tostring(nextBindingId),
        state_identity = resolvedState.id,
        surface = selected.binding.surface,
        ui_target = selected.binding.ui_target,
        binding = selected.binding.id,
        variant = selected.binding.variant,
        evidence = selected.evidence,
    })
    bindingLedger[result] = {
        cycle = cycle,
        model = runtimeModel,
        state = resolvedState,
        binding = selected.binding,
        evaluation = selected,
    }
    return result
end

function resolution.authorize(
    runtimeModel: any,
    cycle: any,
    resolvedBinding: any,
    actionId: any
): (any?, string?)
    local entry = bindingLedger[resolvedBinding]
    if entry == nil
        or entry.cycle ~= cycle
        or entry.model ~= runtimeModel
        or not evidence.is_cycle_live(cycle)
    then
        return nil, "binding_not_actionable"
    end
    if type(actionId) ~= "string" then return nil, "unknown_action" end
    local selected
    for _, action in entry.binding.actions do
        if action.id == actionId then selected = action end
    end
    if selected == nil then return nil, "unknown_action" end

    local predicateKey = model.predicate_key({
        kind = "locator_present",
        locator = selected.proof_locator,
    })
    local proofRecord = entry.evaluation.record_by_predicate[predicateKey]
    local measured = evidence.result_of(proofRecord, cycle)
    local hostProof = evidence.host_proof_of(proofRecord, cycle)
    if measured == nil
        or measured.kind ~= "present"
        or measured.source.kind ~= "template"
        or measured.source.binding ~= entry.binding.id
        or measured.source.locator ~= selected.proof_locator
        or hostProof == nil
    then
        return nil, "proof_not_actionable"
    end

    nextRequestId += 1
    local evidenceIds = {}
    local seenEvidence: { [string]: boolean } = {}
    for _, group in { entry.state.evidence, resolvedBinding.evidence } do
        for _, summary in group do
            if not seenEvidence[summary.id] then
                seenEvidence[summary.id] = true
                table.insert(evidenceIds, summary.id)
            end
        end
    end
    local request = table.freeze({
        kind = "receipt_request",
        id = "receipt-request-" .. tostring(nextRequestId),
        state_identity = resolvedBinding.state_identity,
        surface = resolvedBinding.surface,
        ui_target = resolvedBinding.ui_target,
        binding = resolvedBinding.binding,
        variant = resolvedBinding.variant,
        action = selected.id,
        action_kind = selected.kind,
        -- Absent for a click, which names a coordinate instead. The Host is
        -- told which key to deliver rather than reading it back out of a model
        -- it does not interpret.
        key = selected.key,
        evidence_ids = freezeList(evidenceIds),
    })
    requestLedger[request] = {
        cycle = cycle,
        payload = table.freeze({
            request = request,
            placement = entry.binding.placement,
            proof_locator = selected.proof_locator,
            host_proof = hostProof,
        }),
    }
    return request, nil
end

-- One-shot transfer from the pure resolver to observe's private Host call.
function resolution.take_receipt_request(request: any, cycle: any): any?
    local entry = requestLedger[request]
    requestLedger[request] = nil
    if entry == nil or entry.cycle ~= cycle or not evidence.is_cycle_live(cycle) then return nil end
    return entry.payload
end

return table.freeze(resolution)
