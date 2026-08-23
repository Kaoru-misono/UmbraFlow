return {
    plugin_id = "fixture.foreign",
    reduce = function(input)
        for _, event in ipairs(input.prospective_journal_batch) do
            if event.namespaced_event_type == "fixture.confirmed" then
                return { revision = 1 }
            end
        end
        return { revision = 0 }
    end,
}
