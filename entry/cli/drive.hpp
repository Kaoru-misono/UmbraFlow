#pragma once

#include "args.hpp"
#include "drive-protocol.hpp"

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

        auto operator==(DriveIpcPaths const&) const -> bool = default;
    };

    // Three refusals: the queue must already exist, because a session that created
    // it would race the operator appending to it; the paths must be distinct,
    // because a session reading its own results would re-execute them; and the
    // results path must NOT already exist, so an earlier session's file can never be
    // mistaken for this one's or silently appended to.
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
