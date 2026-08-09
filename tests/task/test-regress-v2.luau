--!strict
-- Contract tests for offline Frame/Observation/Assertion/CandidateModel and
-- regress coverage/ambiguity/missing-evidence output.

local provenance = {
    run_id = "test-run",
    agent = "fixture-agent",
    created_at = "2026-08-09T00:00:00Z",
}

local function frame(id: string): any
    return oracle.Frame.new({
        id = id,
        asset = {
            path = "frames/" .. id .. ".png",
            sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            width = 1920,
            height = 1080,
        },
        captured_at = "2026-08-09T00:00:00Z",
        source = "manual_capture",
    })
end

local function observation(id: string, frame_id: string, classification: string): any
    return oracle.Observation.new({
        id = id,
        frame_id = frame_id,
        subject = { kind = "surface", surface_id = "camp" },
        measurement = { kind = "geometry", rect = { 0, 0, 100, 100 } },
        classification = classification,
        confidence = if classification == oracle.unknown then 0.20 else 0.99,
        provenance = provenance,
    })
end

local function assertion(
    id: string,
    frame_id: string,
    observation_id: string,
    outcome: string
): any
    return oracle.Assertion.new({
        id = id,
        frame_id = frame_id,
        claim = { kind = "surface_identity", surface_id = "camp" },
        outcome = outcome,
        supporting_observations = { observation_id },
        review = { status = oracle.review_accepted },
        provenance = provenance,
    })
end

local function run()
    local covered_frame = frame("frame.covered")
    local unreviewed_frame = frame("frame.unreviewed")
    local unknown_frame = frame("frame.unknown")
    local absent_frame = frame("frame.absent")
    local covered_observation = observation("observation.covered", covered_frame.id, oracle.present)
    local unknown_observation = observation("observation.unknown", unknown_frame.id, oracle.unknown)
    local absent_observation = observation("observation.absent", absent_frame.id, oracle.absent)
    local candidate = oracle.CandidateModel.new({
        id = "candidate-1",
        project_id = "fixture-project",
        revision = 1,
        status = "candidate",
        source_frame_ids = { covered_frame.id, unreviewed_frame.id, unknown_frame.id, absent_frame.id },
        targets = {},
        entities = {},
        patches = {},
        conflicts = {},
    })
    local workspace = oracle.Workspace.new({
        schema_version = 1,
        project_id = "fixture-project",
        frames = { covered_frame, unreviewed_frame, unknown_frame, absent_frame },
        observations = { covered_observation, unknown_observation, absent_observation },
        assertions = {
            assertion("assertion.covered", covered_frame.id, covered_observation.id, oracle.present),
            assertion("assertion.unknown", unknown_frame.id, unknown_observation.id, oracle.unknown),
            assertion("assertion.absent", absent_frame.id, absent_observation.id, oracle.absent),
        },
        candidates = { candidate },
    })
    assert(oracle.Workspace.is(workspace))
    assert(oracle.CandidateModel.is(candidate))
    assert(oracle.assertion_status(workspace.assertions[1]) == oracle.present)

    local report = regress.check(
        {
            surfaces = {
                { id = "camp", kind = "scene", contexts = { "camp" } },
                { id = "shop", kind = "scene", contexts = { "camp" } },
            },
        },
        workspace,
        {
            {
                frame_id = covered_frame.id,
                resolution = {
                    kind = "ambiguous",
                    candidates = {
                        { state = { context = "camp", surfaces = { "shop" } }, score = 1, evidence = { "a" } },
                        { state = { context = "camp", surfaces = { "camp" } }, score = 1, evidence = { "b" } },
                    },
                    conflicts = {
                        { kind = "multiple_candidates", surfaces = { "shop", "camp" }, message = "equal evidence" },
                    },
                    evidence = {},
                },
            },
        }
    )
    assert(not report.accepted)
    assert(#report.ambiguity == 1)
    assert(#report.coverage == 4)
    local status_by_frame: { [string]: string } = {}
    for _, coverage in report.coverage do
        status_by_frame[coverage.frame_id] = coverage.status
    end
    assert(status_by_frame[covered_frame.id] == "covered")
    assert(status_by_frame[unreviewed_frame.id] == oracle.unreviewed)
    assert(status_by_frame[unknown_frame.id] == oracle.unknown)
    assert(status_by_frame[absent_frame.id] == "uncovered")

    local has_unreviewed = false
    local has_missing_surface = false
    for _, missing in report.missing_evidence do
        if missing.frame_id == unreviewed_frame.id and missing.status == oracle.unreviewed then
            has_unreviewed = true
        end
        if missing.surface_id == "shop" then
            has_missing_surface = true
        end
    end
    assert(has_unreviewed)
    assert(has_missing_surface)

    return true
end

return { run = run }
