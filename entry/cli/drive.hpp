#pragma once

#include "args.hpp"
#include "drive-protocol.hpp"
#include "queue-cursor.hpp"

#include <core/error/result.hpp>

#include <task/operator-session.hpp>
#include <task/task-host.hpp>

#include <filesystem>
#include <stop_token>
#include <string>

namespace uf::cli
{
    // Canonicalized and checked before the desktop is touched, so a mistyped path
    // fails without declaring DPI awareness, enumerating windows or opening a
    // capture resource -- the ordering `run` uses for a bad project path.
    struct DriveIpcPaths final
    {
        std::filesystem::path queue{};
        std::filesystem::path results{};
        std::filesystem::path cursor{};

        // Where the session begins reading. See resolveQueueStart.
        QueuePosition start{};

        auto operator==(DriveIpcPaths const&) const -> bool = default;
    };

    // Four refusals: the queue must already exist, because a session that created
    // it would race the operator appending to it; the queue, the results and the
    // queue's own cursor must be three distinct paths, because a session reading
    // its own output would re-execute it; a queue that already holds commands
    // with no cursor recording what ran is refused, because starting from byte
    // zero re-delivers every keystroke in it against a live target; and the
    // results file must agree with the cursor -- a fresh session (no cursor,
    // empty queue) must NOT find one, or a stale file would be read as this
    // session's answers, while a resumed session MUST find the file its cursor's
    // commands were answered into.
    [[nodiscard]]
    auto validateDriveIpcPaths(DriveArgs const& args) -> Result<DriveIpcPaths>;

    // The whole of the front-end above the capability surface. Host-neutral by
    // construction: it takes an already-bound session, so the desktop binding stays
    // in the platform composition below. `cancellation` is the process's Ctrl-C
    // token, so a stop ends the loop between commands as well as inside a verb.
    [[nodiscard]]
    auto driveSession(
        task::OperatorSession& session,
        DriveArgs const& args,
        DriveIpcPaths const& paths,
        std::stop_token cancellation
    ) -> Result<task::TaskRunReport>;

    // Exposed so the one-command-to-one-OperatorSession-verb mapping can be read off
    // in one place.
    struct DriveExecution final
    {
        std::string resultLine{};
        bool        stopSession{false};

        // Separate from the result line because a refused command is normal -- the
        // operator reads the line and tries something else -- while a session-ending
        // failure is what run.finished reports.
        std::optional<Error> failure{};
    };

    [[nodiscard]]
    auto executeDriveCommand(
        task::OperatorSession& session,
        DriveCommand const& command
    ) -> DriveExecution;

    // Executes operator commands against one bound target. Implemented per host: the
    // Windows build binds a live target and performs the full composition; other
    // hosts report the drive path as unsupported, as `run` does.
    [[nodiscard]]
    auto driveProduct(DriveArgs const& args) -> Result<task::TaskRunReport>;
}
