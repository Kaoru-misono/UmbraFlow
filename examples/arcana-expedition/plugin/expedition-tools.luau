-- The expedition's tool closure: one entry per Tool its Tool Catalog declares,
-- joined to that Tool by the deployment's `tool_bindings`. Every Tool here has
-- a single-word local name, so each entry is spelled exactly as its Tool's
-- local half.
--
-- Every Tool of this catalog declares `MarchResult`, whose whole content is the
-- leagues covered. Only `move` marches, so it is the one entry that reads its
-- arguments; the other three answer a halted march rather than a made-up one.
local plugin_id = "arcana.expedition"

local function halted(_arguments)
    return { leagues = 0 }
end

return {
    plugin_id = plugin_id,
    approval = halted,
    move = function(arguments)
        return { leagues = arguments.steps }
    end,
    survey = halted,
    trade = halted,
}
