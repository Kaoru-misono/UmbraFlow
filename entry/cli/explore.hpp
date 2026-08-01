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
    // The three IPC paths one exploration session uses, canonicalized and
    // checked.
    //
    // They are resolved before the desktop is touched, so a mistyped path fails
    // without declaring DPI awareness, enumerating windows or opening a capture
    // resource -- the ordering `run` and `drive` both use.
    struct ExploreIpcPaths final
    {
        std::filesystem::path queue{};
        std::filesystem::path results{};
        std::filesystem::path cursor{};

        // Where the session begins reading, decided by the cursor beside the
        // queue. See resolveExploreQueueStart.
        ExploreQueuePosition start{};

        auto operator==(ExploreIpcPaths const&) const -> bool = default;
    };

    // Canonicalizes the queue and results paths, reads the cursor beside the
    // queue, and decides where this session begins.
    //
    // The results rule follows the cursor rather than being fixed, and the pairing
    // is the point. A FRESH session (no cursor, empty queue) must not find a
    // results file: a stale one from an earlier session would otherwise be
    // appended to and read as this session's answers. A RESUMED session must find
    // one, because it is the same session's answer file and the chunks already
    // recorded in the cursor were answered into it. Either rule alone would let a
    // restart lose or fabricate evidence.
    [[nodiscard]]
    auto validateExploreIpcPaths(ExploreArgs const& args) -> Result<ExploreIpcPaths>;

    // Runs one exploration session to completion over `paths`.
    //
    // Host-neutral by construction: it takes an already-bound session, so the
    // desktop binding stays in the platform composition below. `cancellation` is
    // the process's Ctrl-C token, so a stop ends the loop between chunks as well
    // as inside one.
    [[nodiscard]]
    auto exploreSession(
        task::ExplorationSession& session,
        ExploreArgs const& args,
        ExploreIpcPaths const& paths,
        std::stop_token cancellation
    ) -> Result<task::TaskRunReport>;

    // One chunk's execution: the line to append, and whether the session ends on
    // it.
    struct ExploreExecution final
    {
        std::string resultLine{};
        bool        stopSession{false};

        // The failure that ended the session, when one did. It is separate from
        // the result line because a chunk that raised is NORMAL -- the agent reads
        // the line and writes another chunk -- while a failure that ends the
        // session is what run.finished reports.
        std::optional<Error> failure{};
    };

    [[nodiscard]]
    auto executeExploreChunk(
        task::ExplorationSession& session,
        ExploreChunk const& chunk
    ) -> ExploreExecution;

    // Runs agent chunks against one bound target. Implemented per host: the
    // Windows build binds a live target and performs the full composition; other
    // hosts report the explore path as unsupported, exactly as `run` and `drive`
    // do.
    [[nodiscard]]
    auto exploreProduct(ExploreArgs const& args) -> Result<task::TaskRunReport>;
}
