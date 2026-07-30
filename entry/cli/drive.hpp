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
    // The two IPC paths one drive session uses, canonicalized and checked.
    //
    // They are resolved before the desktop is touched, so a mistyped path fails
    // without declaring DPI awareness, enumerating windows or opening a capture
    // resource -- the same ordering `run` uses for a bad project path.
    struct DriveIpcPaths final
    {
        std::filesystem::path queue{};
        std::filesystem::path results{};

        auto operator==(DriveIpcPaths const&) const -> bool = default;
    };

    // Canonicalizes and checks the queue and results paths.
    //
    // Three refusals, and the third is the one that matters most. The queue must
    // already exist, because a session that created it would race the operator
    // appending to it. The two paths must be distinct, because a session reading its
    // own results would re-execute them. And THE RESULTS PATH MUST NOT ALREADY EXIST:
    // an operator output is always a fresh file, so a stale results file from an
    // earlier session can never be mistaken for this one's, and nothing is silently
    // appended to or clobbered. That guard is the m0-demo input agent's, carried over
    // deliberately -- it caught two real operator mistakes.
    [[nodiscard]]
    auto validateDriveIpcPaths(DriveArgs const& args) -> Result<DriveIpcPaths>;

    // Runs one operator session to completion over `paths`, and is the whole of the
    // front-end above the capability surface.
    //
    // Host-neutral by construction: it takes an already-bound session, so the desktop
    // binding stays in the platform composition below. `cancellation` is the process's
    // Ctrl-C token, so a stop ends the loop between commands as well as inside a verb.
    [[nodiscard]]
    auto driveSession(
        task::OperatorSession& session,
        DriveArgs const& args,
        DriveIpcPaths const& paths,
        std::stop_token cancellation
    ) -> Result<task::TaskRunReport>;

    // Executes one parsed command and returns the result line to append.
    //
    // Exposed for its own sake as much as for the loop's: it is where layer two is
    // shown to be built out of layer one and nothing else. Every convenience command
    // below calls only OperatorSession verbs, and every number it loops on came from
    // the command.
    struct DriveExecution final
    {
        std::string resultLine{};
        bool        stopSession{false};

        // The failure that ended the session, when one did. It is separate from the
        // result line because a refused command is normal -- the operator reads the
        // line and tries something else -- while a failure that ends the session is
        // what run.finished reports.
        std::optional<Error> failure{};
    };

    [[nodiscard]]
    auto executeDriveCommand(
        task::OperatorSession& session,
        DriveCommand const& command
    ) -> DriveExecution;

    // Executes operator commands against one bound target. Implemented per host: the
    // Windows build binds a live target and performs the full composition; other hosts
    // report the drive path as unsupported, exactly as `run` does.
    [[nodiscard]]
    auto driveProduct(DriveArgs const& args) -> Result<task::TaskRunReport>;
}
