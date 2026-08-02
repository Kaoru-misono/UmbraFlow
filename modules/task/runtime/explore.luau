--!strict
-- explore: the verbs an agent has and a business script does not.
--
-- It is a module rather than more `ctx` methods because `ctx` is published into
-- EVERY project environment. Which module names an environment publishes is this
-- framework's isolation mechanism
-- (docs/plans/2026-07-29-three-layer-task-system.md 7), and `explore` is in the
-- exploration environment's publish list and in no other.
--
-- The isolation is structural rather than checked. A project environment is a
-- whitelist with no metatable, and a run VM's private surface never binds
-- `cycle_crop` or `probe` at all. `click_point` and `long_press` do have their
-- primitives on both surfaces, because the trusted framework needs both to act
-- on an element the page model placed; what confines those is that only this
-- module forwards them.
--
-- Frame differencing is deliberately absent
-- (docs/plans/2026-08-01-agent-front-end-and-exploration.md 1). Nothing here
-- decides anything: every function forwards its arguments and returns what the
-- host returned, and the judgement is the agent's or `scribe`'s.
--
-- Framework modules load in the sorted order of their file names. `explore`
-- sorts after `ctx` and before every other module, which is fine because it
-- names only the private surface and the standard library.

local native = ...

local explore = {}

-- Whether the host bound `name` on this VM's private capability surface. It
-- exists so the environment split can be falsified from outside: a test that
-- observed it only through a raised error would pass equally against a surface
-- that carried the key and refused the call, which is the shape section 2 rule 2
-- rejects. It confers nothing -- the answer is a boolean, and a caller that can
-- ask could have called the verb.
function explore.has(name: string): boolean
    if type(name) ~= "string" then
        error("explore.has needs the name of a primitive", 2)
    end
    if native == nil then
        return false
    end
    return (native :: any)[name] ~= nil
end

local function requirePrimitive(name: string, verb: string)
    if not explore.has(name) then
        error(
            verb
                .. " needs the '"
                .. name
                .. "' primitive, which this environment does not load; it is an "
                .. "exploration verb and this VM is not an exploration VM",
            3
        )
    end
end

-- explore.click_point(ticket, x, y) -> ()
--
-- Clicks a bare coordinate and spends the cycle. A business script clicks an
-- element its page authorised; an agent has no model yet and clicks where it
-- decided to look (docs/plans/2026-08-01-three-layers-and-agent-operator.md 2).
--
-- The host still fences it: the frame must be this ticket's, its observation
-- lease must still be fresh, the live geometry must match the project's
-- fingerprint, and the cycle is spent so one frame delivers one input. Only the
-- part that needed a model is gone.
--
-- It is recorded as `annotation.click_delivered`, never as
-- `engine.action_delivered`: no element and no page stand behind this
-- coordinate.
function explore.click_point(ticket: any, x: number, y: number)
    requirePrimitive("cycle_click_point", "explore.click_point")
    return (native :: any).cycle_click_point(ticket, x, y)
end

-- explore.long_press(ticket, x, y, hold_ms) -> ()
--
-- `click_point` with a duration: presses a bare coordinate, holds it for
-- `hold_ms`, releases it, and spends the cycle. An agent needs it because a long
-- press is how a target volunteers what it will not show otherwise, which is a
-- thing to be measured.
--
-- The host fences it as it fences click_point. The hold is bounded by the host
-- and has NO DEFAULT here: how long this target wants a press held is the fact
-- being measured, so a number supplied here would be a measurement nobody took.
function explore.long_press(ticket: any, x: number, y: number, holdMillis: number)
    requirePrimitive("cycle_long_press", "explore.long_press")
    if type(holdMillis) ~= "number" then
        error(
            "explore.long_press needs hold_ms = how long to hold the button "
                .. "down, in milliseconds; it has no default, because how long "
                .. "a target wants a press held is a fact about that target",
            2
        )
    end
    return (native :: any).cycle_long_press(ticket, x, y, holdMillis)
end

-- explore.crop(ticket, x, y, width, height, key?, tolerance?)
--     -> (blob, hash, mask?)
--
-- The pixels of one rectangle as a PNG, and the lowercase hex SHA-256 of exactly
-- those bytes. The hash comes back because a template asset is NAMED by it and
-- this layer has no hash function of its own. The blob is the same kind of value
-- `project_read` hands back, so it goes straight into `project_write`.
--
-- With a key the crop is a masked template: the host bakes the weights into the
-- PNG's alpha channel, which is what makes the matcher compare the glyph rather
-- than the whole rectangle. The key is the same optional { red, green, blue }
-- table `explore.probe` takes and is unpacked here because the private surface
-- takes scalars. The third return says what the key took -- `key_red`,
-- `key_green`, `key_blue` and `tolerance` as the HOST applied them, so a caller
-- that named no tolerance reads the default back, then `rect_pixels`,
-- `selected_pixels`, `ramp_selected_pixels`, and a `warning` when those counts
-- look like a mask that cannot measure anything. It is nil when no key was
-- given, and a key that selects NO pixel is refused by the host.
function explore.crop(
    ticket: any,
    x: number,
    y: number,
    width: number,
    height: number,
    key: { red: number, green: number, blue: number }?,
    tolerance: number?
): (string, string, any)
    requirePrimitive("cycle_crop", "explore.crop")
    if key == nil then
        return (native :: any).cycle_crop(ticket, x, y, width, height)
    end
    if type(key) ~= "table" then
        error(
            "explore.crop takes key = { red = r, green = g, blue = b } or no "
                .. "key at all",
            2
        )
    end
    return (native :: any).cycle_crop(
        ticket,
        x,
        y,
        width,
        height,
        key.red,
        key.green,
        key.blue,
        tolerance
    )
end

-- explore.probe(blob, x, y, width, height, key?, tolerance?) -> stats
--
-- Colour statistics over one rectangle of one PNG. `key` is an optional
-- { red, green, blue } table and `tolerance` an optional whole number; with no
-- key the answer carries the census fields alone, which is what an agent reads
-- to CHOOSE a key. Unpacking the table into scalars is this layer's whole
-- contribution: the numbers are the host's and the judgement is the caller's.
function explore.probe(
    blob: string,
    x: number,
    y: number,
    width: number,
    height: number,
    key: { red: number, green: number, blue: number }?,
    tolerance: number?
): any
    requirePrimitive("probe", "explore.probe")
    if key == nil then
        return (native :: any).probe(blob, x, y, width, height)
    end
    if type(key) ~= "table" then
        error(
            "explore.probe takes key = { red = r, green = g, blue = b } or no "
                .. "key at all",
            2
        )
    end
    return (native :: any).probe(
        blob,
        x,
        y,
        width,
        height,
        key.red,
        key.green,
        key.blue,
        tolerance
    )
end

return explore
