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
-- `cycle_crop` or `probe` at all. `click_point`, `long_press` and `drag` do have
-- their primitives on both surfaces, because the trusted framework needs all
-- three to act on an element the page model placed; what confines those is that
-- only this module forwards them.
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

-- The three channels, the tolerance and the direction as the scalars the private
-- surface takes, or nothing at all when there is no key.
--
-- `removes` says whether the colour names the pixels to DROP or the pixels to
-- keep. It is REQUIRED, because the two read one colour into complementary
-- masks: a caller who left it out would get whichever reading this layer
-- happened to prefer, and half of them would be measuring the complement of
-- what they meant.
local function keyScalars(verb: string, key: any, tolerance: number?)
    if key == nil then
        return
    end
    if type(key) ~= "table" then
        error(
            verb .. " takes key = { red = r, green = g, blue = b, removes = }"
                .. " or no key at all",
            3
        )
    end
    if type(key.removes) ~= "boolean" then
        error(
            verb .. " needs key.removes = true or false: true keys on the "
                .. "pixels to drop and false on the pixels to keep, and one "
                .. "colour read the two ways cuts complementary masks",
            3
        )
    end
    return key.red, key.green, key.blue, tolerance, key.removes
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
-- The host fences it as it fences click_point, and the hold has NO DEFAULT
-- here.
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

-- explore.drag(ticket, x, y, end_x, end_y, travel_ms) -> ()
--
-- `click_point` with a second coordinate and a duration: presses one bare point,
-- travels to another with the button held, releases there, and spends the cycle.
-- An agent needs it to measure the thing a business script cannot ask about
-- until it is known -- how far a map actually pans for a given drag, whether it
-- has inertia, whether it clamps at the edges.
--
-- BOTH points are bare here, which is the whole difference from `observe.drag`:
-- that one anchors the start on a hit the page authorised and derives the far
-- end from an offset. An agent is measuring the geometry that annotation would
-- later rest on, so it cannot be made to name an element that does not exist
-- yet.
--
-- The host fences it as it fences click_point, at both ends, and the travel has
-- NO DEFAULT here.
function explore.drag(
    ticket: any,
    x: number,
    y: number,
    endX: number,
    endY: number,
    travelMillis: number
)
    requirePrimitive("cycle_drag", "explore.drag")
    if type(travelMillis) ~= "number" then
        error(
            "explore.drag needs travel_ms = how long to spend travelling, in "
                .. "milliseconds; it has no default, because how slowly a target "
                .. "must be dragged before it follows is a fact about that target",
            2
        )
    end
    return (native :: any).cycle_drag(ticket, x, y, endX, endY, travelMillis)
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
-- than the whole rectangle. The key is the same optional
-- { red, green, blue, removes } table `explore.probe` takes and is unpacked
-- here because the private surface takes scalars. The third return says what the
-- key took -- `key_red`, `key_green`, `key_blue`, `tolerance` and `key_removes`
-- as the HOST applied them, so a caller that named no tolerance reads the
-- default back, then `rect_pixels`, `selected_pixels`, `ramp_selected_pixels`,
-- and a `warning` when those counts look like a mask that cannot measure
-- anything. It is nil when no key was given, and a key that selects NO pixel is
-- refused by the host -- which a `removes` key hits by naming a colour that
-- covers the whole rectangle.
function explore.crop(
    ticket: any,
    x: number,
    y: number,
    width: number,
    height: number,
    key: { red: number, green: number, blue: number, removes: boolean }?,
    tolerance: number?
): (string, string, any)
    requirePrimitive("cycle_crop", "explore.crop")
    return (native :: any).cycle_crop(
        ticket,
        x,
        y,
        width,
        height,
        keyScalars("explore.crop", key, tolerance)
    )
end

-- explore.census_grid(ticket, x, y, width, height, cellWidth, cellHeight, key,
--     tolerance?) -> grid
--
-- The colour census of one rectangle, downsampled: the rectangle is tiled into
-- cells and each cell reports how many of its pixels the key took and how far it
-- moved between the cycle's frames. The answer is a frozen table carrying
-- `columns`, `rows`, `cell_width`, `cell_height` and the flat row-major `counts`
-- and `spreads` arrays, so the cell at (column, row) is at index
-- row * columns + column + 1. Edge cells are partial where the rectangle does
-- not divide evenly, and count only their real pixels.
--
-- It is forwarded here for symmetry and NOT because it is privileged: unlike
-- `crop` and `probe`, its primitive is on both surfaces and a business task
-- reaches the same verb through `ctx:cycle_census_grid`. What it buys an agent
-- is asking one question about a whole panel instead of one question per
-- rectangle, which is the difference between measuring a screen and re-decoding
-- one for every pixel it wants to know about.
--
-- The key is REQUIRED here, where `crop` takes it or leaves it: a grid counts
-- one colour per cell, so a grid with no key is no answer rather than a weaker
-- one. It is the same { red, green, blue, removes } table the other two take,
-- direction included, and is unpacked here because the private surface takes
-- scalars: read with removes = true a cell counts the pixels the colour does
-- not take, and the two directions partition the cell.
function explore.census_grid(
    ticket: any,
    x: number,
    y: number,
    width: number,
    height: number,
    cellWidth: number,
    cellHeight: number,
    key: { red: number, green: number, blue: number, removes: boolean },
    tolerance: number?
): any
    requirePrimitive("cycle_census_grid", "explore.census_grid")
    if key == nil then
        error(
            "explore.census_grid needs key = { red = r, green = g, blue = b, "
                .. "removes = }; "
                .. "a cell reports how much of itself is one colour, and there "
                .. "is no such thing as a grid of no colour",
            2
        )
    end
    return (native :: any).cycle_census_grid(
        ticket,
        x,
        y,
        width,
        height,
        cellWidth,
        cellHeight,
        keyScalars("explore.census_grid", key, tolerance)
    )
end

-- explore.probe(blob, x, y, width, height, key?, tolerance?) -> stats
--
-- Colour statistics over one rectangle of one PNG. `key` is an optional
-- { red, green, blue, removes } table and `tolerance` an optional whole number;
-- with no key the answer carries the census fields alone, which is what an agent
-- reads to CHOOSE a key. Unpacking the table into scalars is this layer's whole
-- contribution: the numbers are the host's and the judgement is the caller's.
--
-- `removes` is what makes the census usable on a multi-coloured mark: probing
-- unkeyed names the dominant colour, and on such a mark that is the BACKDROP, so
-- the key an agent then writes is the one that drops it.
function explore.probe(
    blob: string,
    x: number,
    y: number,
    width: number,
    height: number,
    key: { red: number, green: number, blue: number, removes: boolean }?,
    tolerance: number?
): any
    requirePrimitive("probe", "explore.probe")
    return (native :: any).probe(
        blob,
        x,
        y,
        width,
        height,
        keyScalars("explore.probe", key, tolerance)
    )
end

return explore
