--!strict
-- jsonl: one JSON object per line, which is how every offline report this
-- framework writes reaches a terminal.
--
-- It exists because two of them now write one -- `regress` for the screen
-- library and `replay` for the trace library -- and two copies of an escaping
-- rule is one copy that can be wrong. C++ has the same single copy on its own
-- side, `trace::escapeJsonString`, for the same reason.
--
-- It is NOT the trace stream. That is the host's to write, stamped and validated
-- (modules/trace); this renders a verdict a caller asked for, on standard
-- output, where the consumer is a person with a pager or a `grep`.
--
-- `jsonl` sorts after `hits` and before `mint`, and names no other module.

local jsonl = {}

local k_escapes: { [string]: string } = {
    ['"']    = '\\"',
    ["\\"]   = "\\\\",
    ["\n"]   = "\\n",
    ["\r"]   = "\\r",
    ["\t"]   = "\\t",
    ["\b"]   = "\\b",
    ["\f"]   = "\\f",
}

-- One JSON string literal, quotes included: the seven named escapes, every
-- remaining byte below 0x20 as \u00XX, and every byte above it unchanged so
-- valid UTF-8 in a page name survives.
function jsonl.text(value: string): string
    local escaped = string.gsub(value, '[%c"\\]', function(character: string): string
        local known = k_escapes[character]
        if known ~= nil then
            return known
        end
        return string.format("\\u%04x", string.byte(character))
    end)
    return '"' .. escaped .. '"'
end

function jsonl.number(value: number): string
    if value % 1 == 0 then
        return string.format("%d", value)
    end
    -- Enough digits to reproduce a double, and the same text on every host.
    return string.format("%.6g", value)
end

-- One JSON object from an ordered list of key/value pairs. The order is the
-- caller's, so the same field is in the same place on every line of a report
-- read in a terminal; sorting would put `element` after `cells`.
--
-- A nil value DROPS its field rather than writing null, which is what lets a
-- report say "not measured" and "measured, and the answer is none" differently:
-- an absent member and a zero are different bytes.
function jsonl.object(fields: { { any } }): string
    local parts: { string } = {}
    for _, field in fields do
        local key   = field[1] :: string
        local value = field[2]
        if value ~= nil then
            local rendered: string
            if type(value) == "string" then
                rendered = jsonl.text(value)
            elseif type(value) == "number" then
                rendered = jsonl.number(value)
            elseif type(value) == "boolean" then
                rendered = if value then "true" else "false"
            else
                rendered = jsonl.text(tostring(value))
            end
            table.insert(parts, jsonl.text(key) .. ":" .. rendered)
        end
    end
    return "{" .. table.concat(parts, ",") .. "}"
end

return jsonl
