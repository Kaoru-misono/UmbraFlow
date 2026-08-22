return {
    plugin_id = "fixture.alpha",
    reduce = function(input)
        for _, event in ipairs(input.journal_events) do
            if event.namespaced_event_type == "fixture.confirmed" then
                return { revision = 1 }
            end
        end
        return { revision = 0 }
    end,
}
