#pragma once

#include "args.hpp"
#include "explore-cursor.hpp"
#include "explore-protocol.hpp"

#include <core/error/error.hpp>
#include <core/error/result.hpp>

#include <task/exploration-session.hpp>
#include <task/task-host.hpp>

#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>

namespace uf::cli
{
    // Canonicalized and checked before the desktop is touched, so a mistyped path
    // fails without declaring DPI awareness, enumerating windows or opening a
    // capture resource -- the ordering `run` and `drive` both use.
    struct ExploreIpcPaths final
    {
        std::filesystem::path queue{};
        std::filesystem::path results{};
        std::filesystem::path cursor{};

        // Where the session begins reading. See resolveExploreQueueStart.
        ExploreQueuePosition start{};

        auto operator==(ExploreIpcPaths const&) const -> bool = default;
    };

    // The results rule follows the cursor: a fresh session (no cursor, empty queue)
    // must NOT find a results file, or a stale one would be appended to and read as
    // this session's answers; a resumed session MUST find one, because the chunks
    // its cursor records were answered into it. Either rule alone would let a
    // restart lose or fabricate evidence.
    [[nodiscard]]
    auto validateExploreIpcPaths(ExploreArgs const& args) -> Result<ExploreIpcPaths>;

    // Host-neutral by construction: it takes an already-bound session, so the
    // desktop binding stays in the platform composition below. `cancellation` is the
    // process's Ctrl-C token, so a stop ends the loop between chunks as well as
    // inside one.
    [[nodiscard]]
    auto exploreSession(
        task::ExplorationSession& session,
        ExploreArgs const& args,
        ExploreIpcPaths const& paths,
        std::stop_token cancellation
    ) -> Result<task::TaskRunReport>;

    struct ExploreExecution final
    {
        std::string resultLine{};
        bool        stopSession{false};

        // Separate from the result line because a chunk that raised is NORMAL -- the
        // agent reads the line and writes another chunk -- while a session-ending
        // failure is what run.finished reports.
        std::optional<Error> failure{};
    };

    [[nodiscard]]
    auto executeExploreChunk(
        task::ExplorationSession& session,
        ExploreChunk const& chunk
    ) -> ExploreExecution;

    // Runs agent chunks against one bound target. Implemented per host: the Windows
    // build binds a live target and performs the full composition; other hosts
    // report the explore path as unsupported, as `run` and `drive` do.
    [[nodiscard]]
    auto exploreProduct(ExploreArgs const& args) -> Result<task::TaskRunReport>;
}
