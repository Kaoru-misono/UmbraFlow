--!strict
-- observe: the runtime boundary around one annotation-v2 Observation Cycle.
--
-- The resolver is pure with respect to the host.  This module owns the cycle
-- lifetime, turns the host's template/OCR answers into tri-state evidence, and
-- delivers an action only after a framework-minted receipt has been checked
-- against the same cycle.  A ticket, a frame ID, or a receipt-shaped table
-- supplied by a project is never enough to authorize input.

local native = ...
local observe = {}

local nextCycleId = 0

local function records(value: any): { any }
    if type(value) ~= "table" then
        return {}
    end
    if #value > 0 then
        return value
    end
    local result = {}
    for _, item in value do
        table.insert(result, item)
    end
    table.sort(result, function(left, right)
        return tostring(left.id) < tostring(right.id)
    end)
    return result
end

local function findRecord(model: any, name: string, id: string): any
    local direct = model[name .. "_by_id"]
    if type(direct) == "table" and direct[id] ~= nil then
        return direct[id]
    end
    for _, item in records(model[name]) do
        if item.id == id then
            return item
        end
    end
    return nil
end

local function validHash(value: any): boolean
    return type(value) == "string"
        and #value == 64
        and string.match(value, "^[0-9a-f]+$") ~= nil
end

local function modelHash(runtimeModel: any, options: any): string
    local value = options.model_hash
        or runtimeModel.model_hash
        or runtimeModel.content_hash
        or runtimeModel.hash
    if value == nil and type(model) == "table" and type(model.model_hash) == "function" then
        value = model.model_hash(runtimeModel)
    end
    if not validHash(value) then
        error(
            "observe.open_cycle needs model_hash = the lowercase SHA-256 of the "
                .. "accepted runtime model",
            3
        )
    end
    return value
end

local function normalizeWhitespace(value: string, mode: string?): string
    if mode == "raw" or mode == nil then
        return value
    end
    if mode == "trim" then
        return (string.gsub(value, "^%s*(.-)%s*$", "%1"))
    end
    if mode == "collapse_whitespace" then
        return (string.gsub(value, "%s+", " ")):gsub("^%s*(.-)%s*$", "%1")
    end
    return value
end

local function rectFrom(value: any): any
    if type(value) ~= "table" then
        return nil
    end
    if value.x ~= nil then
        return {
            x = value.x,
            y = value.y,
            width = value.width,
            height = value.height,
        }
    end
    if #value == 4 then
        return {
            x = value[1],
            y = value[2],
            width = value[3],
            height = value[4],
        }
    end
    return nil
end

local function targetRect(target: any): any
    if type(target) ~= "table" or type(target.geometry) ~= "table" then
        return nil
    end
    if target.geometry.kind == "fixed" then
        return rectFrom(target.geometry.rect)
    elseif target.geometry.kind == "collection" then
        return rectFrom(target.geometry.rect)
    end
    return nil
end

local function placementRect(model: any, spec: any): any
    local binding = spec.binding
    if type(binding) == "table" and type(binding.placement) == "table" then
        if binding.placement.kind == "rect" then
            return rectFrom(binding.placement.rect)
        end
    end
    local locator = spec.locator
    if type(locator) == "table" and locator.search_rect ~= nil then
        return rectFrom(locator.search_rect)
    end
    if type(locator) == "table" and locator.locator ~= nil then
        local locatorRecord = findRecord(model, "locators", locator.locator)
        if locatorRecord ~= nil and locatorRecord.search_rect ~= nil then
            return rectFrom(locatorRecord.search_rect)
        end
        if locatorRecord ~= nil then
            local target = findRecord(model, "targets", locatorRecord.target)
            return targetRect(target)
        end
    end
    if type(binding) == "table" then
        local target = findRecord(model, "targets", binding.target)
        return targetRect(target)
    end
    return nil
end

local function clickPoint(value: any, rect: any): any
    if type(value) == "table" and value.click_x ~= nil then
        return { x = value.click_x, y = value.click_y }
    end
    if type(value) == "table" and value.x ~= nil and value.y ~= nil then
        return { x = value.x, y = value.y }
    end
    if rect ~= nil then
        return {
            x = rect.x + math.floor(rect.width / 2),
            y = rect.y + math.floor(rect.height / 2),
        }
    end
    return nil
end

local function presentResult(value: any, confidence: number, proof: any): any
    return evidence.present(value, confidence, { proof })
end

local function absentResult(proof: any): any
    return evidence.absent({ proof })
end

local function unknownResult(reason: string, confidence: number?, proof: any?): any
    return evidence.unknown(reason, confidence, if proof == nil then nil else { proof })
end

local function runtimeProbe(state: any, subject: string, spec: any): any
    local model = state.model
    local ctx = state.ctx
    local ticket = state.ticket
    if ctx == nil then
        return unknownResult("not_measured", nil, { kind = "geometry", target = subject })
    end
    local locatorRef = spec.locator
    local locator = nil
    if type(locatorRef) == "table" then
        locator = findRecord(model, "locators", locatorRef.locator)
    elseif type(locatorRef) == "string" then
        locator = findRecord(model, "locators", locatorRef)
    end

    if spec.kind == "text_match" then
        local reader = findRecord(model, "readers", spec.reader)
        if reader == nil then
            return unknownResult("internal_error", nil, { kind = "reader", target = subject })
        end
        local rect = placementRect(model, spec)
        if rect == nil then
            return unknownResult("incompatible_geometry", nil, { kind = "reader", target = subject })
        end

        local reading
        if reader.kind == "block" and type(ctx.cycle_read_lines) == "function" then
            reading = ctx:cycle_read_lines(
                ticket,
                rect.x,
                rect.y,
                rect.width,
                rect.height
            )
        elseif type(ctx.cycle_read) == "function" then
            reading = ctx:cycle_read(ticket, rect.x, rect.y, rect.width, rect.height)
        end
        if reading == nil then
            return unknownResult("ocr_unreadable", nil, { kind = "reader", target = subject })
        end

        local confidence = reading.confidence
        if type(confidence) ~= "number" then
            return unknownResult("internal_error", nil, { kind = "reader", target = subject })
        end
        if confidence < (reader.confidence_floor or 0) then
            return unknownResult(
                "low_confidence",
                confidence,
                { kind = "reader", target = subject, confidence = confidence }
            )
        end

        local text = reading.text
        if text == nil and type(reading.lines) == "table" then
            local parts = {}
            local weakest = confidence
            for _, line in reading.lines do
                table.insert(parts, line.text or "")
                if type(line.confidence) == "number" and line.confidence < weakest then
                    weakest = line.confidence
                end
            end
            local joiner = if reader.kind == "block" and reader.join == "space"
                then " "
                else "\n"
            text = table.concat(parts, joiner)
            confidence = weakest
        end
        if type(text) ~= "string" then
            return unknownResult("ocr_unreadable", confidence, { kind = "reader", target = subject })
        end
        text = normalizeWhitespace(text, reader.normalization)
        return presentResult({
            text = text,
            rect = rect,
            click_point = clickPoint(nil, rect),
        }, confidence, {
            kind = "reader",
            target = subject,
            reader = reader.id,
            confidence = confidence,
            rect = rect,
        })
    end

    if locator == nil then
        return unknownResult("locator_failed", nil, { kind = "locator", target = subject })
    end
    local rect = placementRect(model, spec)
    if rect == nil then
        return unknownResult("incompatible_geometry", nil, { kind = "locator", target = subject })
    end

    if locator.kind == "geometry" then
        return presentResult(
            { rect = rect, click_point = clickPoint(nil, rect) },
            1,
            { kind = "geometry", target = subject, rect = rect }
        )
    elseif locator.kind == "collection" then
        return unknownResult("not_measured", nil, { kind = "locator", target = subject })
    elseif locator.kind == "template" then
        local template = locator.template or locator.handle or locator.runtime_asset
        if template == nil and type(locatorRef) == "table" then
            local variant = findRecord(model, "locator_variants", locatorRef.variant or "")
            if variant ~= nil then
                template = variant.template or variant.handle or variant.runtime_asset
            end
        end
        if template == nil or type(ctx.cycle_match) ~= "function" then
            return unknownResult("locator_failed", nil, { kind = "locator", target = subject })
        end
        local match = ctx:cycle_match(ticket, template, rect.x, rect.y, rect.width, rect.height)
        if match == nil then
            return absentResult({ kind = "locator", target = subject, rect = rect })
        end
        local confidence = 1
        if type(match.score) == "number" and type(match.maximum) == "number"
            and match.maximum > 0
        then
            confidence = 1 - match.score / match.maximum
        end
        local threshold = locator.threshold or 0
        if type(locatorRef) == "table" and locatorRef.variant ~= nil then
            local variant = findRecord(model, "locator_variants", locatorRef.variant)
            if variant ~= nil and variant.threshold ~= nil then
                threshold = variant.threshold
            end
        end
        if confidence < threshold then
            return absentResult({
                kind = "locator",
                target = subject,
                score = confidence,
                rect = rect,
            })
        end
        local matchedRect = if type(match) == "table" and match.x ~= nil
            then {
                x = match.x,
                y = match.y,
                width = match.width,
                height = match.height,
            }
            else rect
        return presentResult(
            {
                match = match,
                rect = matchedRect,
                click_point = clickPoint(match, matchedRect),
            },
            confidence,
            {
                kind = "locator",
                target = subject,
                locator = locatorRef,
                score = confidence,
                rect = matchedRect,
            }
        )
    end
    return unknownResult("locator_failed", nil, { kind = "locator", target = subject })
end

local function deliveryFromResult(raw: any): any
    local result = raw
    if type(result) == "table" and result.result ~= nil then
        result = result.result
    end
    if type(result) ~= "table" or result.kind ~= "present" then
        return nil
    end
    if type(result.value) ~= "table" then
        return nil
    end
    return {
        match = result.value.match,
        click_point = result.value.click_point,
        rect = result.value.rect,
    }
end

local function makeCycle(ctx: any, model: any, options: any): any
    nextCycleId += 1
    if ctx == nil or type(ctx.cycle_open) ~= "function" then
        error("observe.open_cycle needs the trusted ctx cycle_open primitive", 3)
    end
    local ticket = ctx:cycle_open()
    local state: any = {
        ctx = ctx,
        model = model,
        ticket = ticket,
        ticket_id = options.ticket_id
            or (if type(ticket) == "string"
                then ticket
                else "ticket-" .. tostring(nextCycleId)),
        cycle_id = options.cycle_id or "cycle-" .. tostring(nextCycleId),
        frame_id = options.frame_id or "frame-" .. tostring(nextCycleId),
        model_hash = modelHash(model, options),
        token = {},
        measurements = options.measurements,
        measure = options.measure,
        closed = false,
        consumed = false,
    }

    local cycle = {}
    state.cycle = cycle
    cycle.ticket_id = state.ticket_id
    cycle.cycle_id = state.cycle_id
    cycle.frame_id = state.frame_id
    cycle.model_hash = state.model_hash
    cycle.cycle_token = state.token

    function cycle:measurePredicate(
        predicate: any,
        binding: any,
        surface: any,
        role: string,
        index: number
    ): (any, any)
        if state.closed or state.consumed then
            local stale = unknownResult(
                "stale_frame",
                nil,
                { kind = "geometry", target = binding.target }
            )
            return stale, nil
        end
        if type(state.measure) == "function" then
            local measured, delivery = state.measure(
                predicate,
                binding,
                surface,
                role,
                index
            )
            if measured ~= nil then
                return measured, delivery or deliveryFromResult(measured)
            end
        end
        local effectivePredicate = table.clone(predicate)
        effectivePredicate.binding = binding
        local subject = predicate.probe
        if type(subject) ~= "string" or subject == "" then
            if predicate.kind == "text_match" then
                subject = predicate.reader
            elseif type(predicate.locator) == "table" then
                subject = predicate.locator.locator
            else
                subject = binding.target
            end
        end
        if type(state.measurements) == "table" then
            local measured = state.measurements[subject]
            if measured ~= nil then
                return measured, deliveryFromResult(measured)
            end
        end
        local measured, delivery = runtimeProbe(state, subject, effectivePredicate)
        return measured, delivery or deliveryFromResult(measured)
    end

    function cycle:probe(
        subject: string,
        spec: any,
        surface: any?,
        role: string?,
        index: number?
    ): any
        -- resolution.resolve accepts a cycle directly as a measurement seam.
        -- It supplies the binding target as the first argument, while the
        -- actual probe key lives on the LocatorRef/reader predicate.  Route
        -- both paths through the same adapter so fixture probes and live
        -- geometry do not diverge.
        local binding = if type(spec.binding) == "table"
            then spec.binding
            else { target = subject }
        local measured = cycle:measurePredicate(
            spec,
            binding,
            surface or { id = subject },
            role or "identity",
            index or 1
        )
        return measured
    end

    function cycle:resolve(optionsForResolution: any?): any
        if state.closed or state.consumed then
            return {
                kind = "unknown",
                reason = "invalid_stack",
                evidence = {},
                diagnostic = "the observation cycle is no longer live",
            }
        end
        local settings = optionsForResolution or {}
        settings.context = settings.context or options.context
        settings.margin = settings.margin or options.margin
        settings.cycle_token = state.token
        local result = resolution.resolve(
            model,
            function(predicate, binding, surface, role, index)
                return cycle:measurePredicate(predicate, binding, surface, role, index)
            end,
            settings
        )
        state.resolution = result
        return result
    end

    function cycle:authorize(resolved: any, request: any): (any, string?)
        if state.closed or state.consumed then
            return nil, "the observation cycle is no longer live"
        end
        local authorization, why = resolution.authorize(
            model,
            resolved,
            request,
            function(predicate, binding, surface, role, index)
                return cycle:measurePredicate(predicate, binding, surface, role, index)
            end,
            state.token
        )
        if authorization == nil or authorization.authorized ~= true then
            return nil, if authorization == nil then why else authorization.reason
        end

        local firstEvidence = authorization.evidence[1]
        local delivery = if firstEvidence ~= nil
            then authorization.deliveries[firstEvidence.id]
            else nil
        local action = authorization.action
        local receipt = evidence.mint_receipt(
            {
                ticket_id = state.ticket_id,
                cycle_id = state.cycle_id,
                frame_id = state.frame_id,
                model_hash = state.model_hash,
                state = resolved.state,
                surface = resolved.state.surfaces[#resolved.state.surfaces],
                binding = authorization.binding.id,
                action = action.id,
                evidence_ids = (function()
                    local ids = {}
                    for _, record in resolved.evidence do
                        table.insert(ids, record.id)
                    end
                    for _, record in authorization.evidence do
                        local duplicate = false
                        for _, existing in ids do
                            if existing == record.id then
                                duplicate = true
                                break
                            end
                        end
                        if not duplicate then
                            table.insert(ids, record.id)
                        end
                    end
                    return ids
                end)(),
            },
            state.token,
            {
                ticket_id = state.ticket_id,
                cycle_id = state.cycle_id,
                target = authorization.target,
                action = action.id,
                kind = action.kind,
                action = action,
                binding = authorization.binding,
                match = if delivery ~= nil then delivery.match else nil,
                click_point = if delivery ~= nil then delivery.click_point else nil,
            }
        )
        return receipt, nil
    end

    function cycle:close(): ()
        if state.closed then
            return
        end
        state.closed = true
        if ctx ~= nil and not state.consumed and type(ctx.cycle_close) == "function" then
            ctx:cycle_close(ticket)
        end
    end

    function cycle:click(receipt: any): ()
        if state.closed or state.consumed then
            error("observe.click cannot use a closed or consumed observation cycle", 2)
        end
        if not evidence.is_receipt_for(receipt, state.token) then
            error(
                "observe.click needs a framework receipt minted for this exact "
                    .. "observation cycle; Unknown and Ambiguous resolutions never mint one",
                2
            )
        end
        if receipt.model_hash ~= state.model_hash
            or receipt.ticket_id ~= state.ticket_id
            or receipt.cycle_id ~= state.cycle_id
            or receipt.frame_id ~= state.frame_id
        then
            error("observe.click refused a stale receipt", 2)
        end
        local authorization = evidence.receipt_authorization(receipt, state.token)
        if authorization == nil or authorization.kind ~= "click" then
            error("observe.click receipt does not authorize a click", 2)
        end

        if authorization.match ~= nil and ctx ~= nil and type(ctx.cycle_click) == "function" then
            ctx:cycle_click(ticket, authorization.match)
        elseif authorization.click_point ~= nil and native ~= nil
            and type(native.cycle_click_point) == "function"
        then
            native.cycle_click_point(
                ticket,
                authorization.click_point.x,
                authorization.click_point.y
            )
        elseif authorization.click_point ~= nil and ctx ~= nil
            and type(ctx.cycle_click_point) == "function"
        then
            ctx:cycle_click_point(
                ticket,
                authorization.click_point.x,
                authorization.click_point.y
            )
        else
            error("observe.click has no host delivery primitive for this action", 2)
        end

        if not evidence.consume_receipt(receipt, state.token) then
            error("observe.click receipt was consumed concurrently or already used", 2)
        end
        state.consumed = true
    end

    return table.freeze(cycle)
end

function observe.open_cycle(ctx: any, model: any, options: any?): any
    if type(model) ~= "table" then
        error("observe.open_cycle needs the accepted runtime model", 2)
    end
    return makeCycle(ctx, model, options or {})
end

observe.open = observe.open_cycle

function observe.with_cycle(
    ctx: any,
    model: any,
    options: any,
    callback: (any) -> any
): any
    if type(callback) ~= "function" then
        error("observe.with_cycle needs a callback", 2)
    end
    local cycle = observe.open_cycle(ctx, model, options)
    local ok, result = pcall(callback, cycle)
    local closed, closeError = pcall(cycle.close, cycle)
    if not ok then
        error(result, 0)
    end
    if not closed then
        error(closeError, 0)
    end
    return result
end

function observe.resolve(
    cycle: any,
    model: any,
    options: any?
): any
    if type(cycle) ~= "table" or type(cycle.resolve) ~= "function" then
        error("observe.resolve needs an observation cycle returned by open_cycle", 2)
    end
    return cycle:resolve(options)
end

observe.resolve_state = observe.resolve

function observe.authorize(
    cycle: any,
    resolved: any,
    request: any
): (any, string?)
    if type(cycle) ~= "table" or type(cycle.authorize) ~= "function" then
        error("observe.authorize needs an observation cycle returned by open_cycle", 2)
    end
    return cycle:authorize(resolved, request)
end

function observe.click(cycle: any, receipt: any): ()
    if type(cycle) ~= "table" or type(cycle.click) ~= "function" then
        error("observe.click needs an observation cycle returned by open_cycle", 2)
    end
    cycle:click(receipt)
end

function observe.resolve_and_authorize(
    cycle: any,
    model: any,
    request: any,
    options: any?
): (any, any, string?)
    local resolved = cycle:resolve(options)
    local receipt, why = cycle:authorize(resolved, request)
    return resolved, receipt, why
end

return observe
