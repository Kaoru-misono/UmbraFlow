#pragma once

#include "event.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace uf::trace
{
    // Reading a recorded run back, as the narrowest projection a checker of it
    // can be built on.
    //
    // IT IS A PROJECTION AND NOT A PARSE. A stream carries dozens of event kinds
    // and most of them are evidence about how a decision was reached; what a
    // replay check asks is what the run BELIEVED and what it DID, which is three
    // kinds. Reconstructing whole TraceEvent values would be a second authority
    // on a schema this module already owns from the writing side, and every field
    // it reconstructed would be one more thing to keep in step for no reader.
    //
    // The projection judges nothing. Whether a front end may be replayed, whether
    // a model hash still matches, whether a transition has an edge -- all of that
    // is the checker's, and the checker is Luau because the page model is
    // (docs/plans/2026-07-31-script-owned-page-model.md 1). What is here is the
    // part that carries a guarantee: these bytes were in that file, in that
    // order, under those names.

    enum struct ReplayStepKind : uint8
    {
        // The framework resolved a page: what the run believed it was standing on.
        PageResolved,

        // A click reached the target. It names NOTHING the model can attribute it
        // to -- see `ReplayStep::label`.
        ActionDelivered,

        // A keystroke reached the target, and the wire records which key.
        KeyDelivered,
    };

    struct ReplayStep final
    {
        ReplayStepKind kind{};

        // The stream sequence number, so a checker reporting a step points at a
        // line rather than at an index into this list.
        uint64 seq{};

        // The page a resolution concluded on, or the key a delivery pressed.
        //
        // EMPTY ON EVERY CLICK, and that is a fact about the stream rather than
        // about this reader: `engine.action_delivered` carries the frame identity
        // and the client point, and no event names the element a click was
        // authorised against. So a checker can attribute a keystroke to an edge
        // and cannot attribute a click, which is what the `unattributable` class
        // in the replay plan is for.
        std::string label{};
    };

    // One recorded run: who it was, and the ordered steps above.
    struct ReplayedRun final
    {
        // Read from `run.started`, which a stream opens with. The checker refuses
        // a `Check` front end by this rather than by task name: a project can
        // name a task anything, and the front end is a closed enum the host
        // stamps on every line.
        FrontEnd frontEnd{};

        std::string projectId{};
        std::string taskName{};

        // The page model the run stood on, by content. Empty on a stream recorded
        // before `modelHash` existed, which a checker must refuse rather than
        // read as agreement.
        std::string modelHash{};

        std::vector<ReplayStep> steps{};
    };

    // The largest trace this will read into memory. A menu-to-menu run of the
    // reference project is about nine megabytes over twenty thousand lines; this
    // refuses a file that is not a trace before any of it is read, on the same
    // reasoning as the page model's own cap.
    inline constexpr auto k_maximumTraceBytes = std::size_t{512} * 1024U * 1024U;

    // Projects the JSONL text of one run. Refuses text whose first line is not a
    // `run.started` this reader understands: a stream is a run bracket, and a
    // projection that started midway would report a run that began on whatever
    // page the file happened to open with.
    //
    // A line it does not project is skipped and never refused -- that is what
    // makes this a projection -- but a line that is not a JSON object at all is a
    // truncated or interleaved file and is refused, because skipping it would
    // silently shorten the run.
    [[nodiscard]]
    auto projectReplayedRun(std::string_view text) -> Result<ReplayedRun>;

    // Reads `path` and projects it. Size-capped before any bytes are read.
    [[nodiscard]]
    auto readReplayedRun(
        std::filesystem::path const& path
    ) -> Result<ReplayedRun>;
}
