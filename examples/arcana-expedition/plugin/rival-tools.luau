-- The rival's tool closure. It declares the same four entries as the
-- expedition's because both catalogs are built from the same local names; the
-- eight Tools stay distinct because each name sits under its own plugin id.
--
-- Every Tool of this catalog declares `MarchResult`, and only `move` marches,
-- so it is the one entry that reads its arguments.
local plugin_id = "arcana.rival"

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
