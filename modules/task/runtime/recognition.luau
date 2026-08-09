--!strict
-- recognition: the public, capture-consuming facade for the annotation-v2
-- resolver. Runtime resolution policy lives in `resolution`; this module keeps
-- the published name small and does not reintroduce Page/Element semantics.

local recognition = {}

function recognition.resolve(model: any, cycle: any, options: any?): any
    return resolution.resolve(model, cycle, options or {})
end

function recognition.authorize(
    model: any,
    cycle: any,
    resolved: any,
    request: any
): (any, string?)
    return resolution.authorize(model, cycle, resolved, request)
end

function recognition.is_resolved(result: any): boolean
    return type(result) == "table" and result.kind == "resolved"
end

function recognition.is_actionable(result: any): boolean
    return recognition.is_resolved(result)
end

function recognition.state(result: any): any
    if not recognition.is_resolved(result) then
        return nil
    end
    return result.state
end

return recognition
