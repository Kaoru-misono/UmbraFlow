#pragma once

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/key.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace uf::cli
{
    // The longest command line the operator protocol accepts, matching the m0-demo
    // input agent's own ceiling. A command is a handful of scalars; anything near
    // this is a malformed line rather than a large one.
    inline constexpr auto k_maxDriveCommandBytes = std::size_t{64} * 1024U;

    // THE OPERATOR COMMAND PROTOCOL, IN TWO LAYERS.
    //
    // LAYER ONE is the private capability surface, verbatim: one command per
    // primitive, with the same name, the same arguments and the same failure modes as
    // the Luau surface has. The opaque handles a task holds become integer ids,
    // because an operator has only text; nothing else changes, nothing is composed,
    // and nothing is bypassed. Every layer-one command below maps to exactly one
    // task::OperatorSession verb, which maps to exactly one TaskContext call -- the
    // same call ctx.luau makes.
    //
    // LAYER TWO is convenience: waiting for a page, and finding-then-clicking a
    // recognizer, each in one command instead of a hand-written loop.
    //
    // THE CONSTRAINT THAT MAKES LAYER TWO SAFE, and the point of the whole design: a
    // convenience command carries NO POLICY DEFAULTS OF ITS OWN. Every timeout, poll
    // interval and retry count is a REQUIRED field. modules/task/runtime/ctx.luau
    // keeps its defaults and stays the only place task-side policy lives, so there is
    // no second copy of that policy in C++ to drift out of step with it. A command
    // that omits a required policy field is REJECTED -- never filled in. If a default
    // ever looks wanted here, that is the signal the field belongs in the command.
    //
    // Layer two composes layer-one verbs and nothing else, which is what keeps it
    // from being a second capability surface: it decides when to stop looping, and
    // the caller decides every number that decision uses.

    struct DriveCycleOpenCommand final
    {
        auto operator==(DriveCycleOpenCommand const&) const -> bool = default;
    };

    struct DriveCycleCloseCommand final
    {
        uint64 cycle{};

        auto operator==(DriveCycleCloseCommand const&) const -> bool = default;
    };

    struct DriveCyclePageCommand final
    {
        uint64 cycle{};

        auto operator==(DriveCyclePageCommand const&) const -> bool = default;
    };

    struct DriveCycleFindCommand final
    {
        uint64      cycle{};
        std::string recognizer{};

        auto operator==(DriveCycleFindCommand const&) const -> bool = default;
    };

    struct DriveCycleClickCommand final
    {
        uint64 cycle{};
        uint64 hit{};

        auto operator==(DriveCycleClickCommand const&) const -> bool = default;
    };

    // A keystroke names no position, so this command carries no coordinate and no
    // hit. It still names a cycle, because the primitive requires one: that is what
    // orders the keystroke against the observations around it and gives its trace
    // line a cycle to join on.
    struct DriveKeyCommand final
    {
        uint64  cycle{};
        KeyName key;

        auto operator==(DriveKeyCommand const&) const -> bool = default;
    };

    struct DriveSettleCommand final
    {
        MonotonicInstant::Duration duration{};

        auto operator==(DriveSettleCommand const&) const -> bool = default;
    };

    struct DriveDeadlineCommand final
    {
        MonotonicInstant::Duration duration{};

        auto operator==(DriveDeadlineCommand const&) const -> bool = default;
    };

    struct DriveWaitCommand final
    {
        uint64                     deadline{};
        MonotonicInstant::Duration pollInterval{};

        auto operator==(DriveWaitCommand const&) const -> bool = default;
    };

    // LAYER TWO. Both fields below that name a duration are REQUIRED, and the
    // constructor-free aggregate shape is not what enforces that -- the parser is.
    // See the header comment.
    struct DriveWaitPageCommand final
    {
        std::string                page{};
        MonotonicInstant::Duration timeout{};
        MonotonicInstant::Duration pollInterval{};

        auto operator==(DriveWaitPageCommand const&) const -> bool = default;
    };

    struct DriveFindClickCommand final
    {
        std::string                recognizer{};
        MonotonicInstant::Duration timeout{};
        MonotonicInstant::Duration pollInterval{};

        auto operator==(DriveFindClickCommand const&) const -> bool = default;
    };

    struct DriveQuitCommand final
    {
        auto operator==(DriveQuitCommand const&) const -> bool = default;
    };

    using DriveCommand = std::variant<
        DriveCycleOpenCommand,
        DriveCycleCloseCommand,
        DriveCyclePageCommand,
        DriveCycleFindCommand,
        DriveCycleClickCommand,
        DriveKeyCommand,
        DriveSettleCommand,
        DriveDeadlineCommand,
        DriveWaitCommand,
        DriveWaitPageCommand,
        DriveFindClickCommand,
        DriveQuitCommand
    >;

    [[nodiscard]]
    auto parseDriveCommand(std::string_view line) -> Result<DriveCommand>;

    // The `op` a command reports itself under, so a result line names the command it
    // answers even when parsing produced no command at all.
    [[nodiscard]] auto driveCommandOperation(DriveCommand const& command) -> std::string_view;

    // One command's result as its line reports it. `ok` is the distinction every
    // caller depends on, so a refused command must never report it true.
    //
    // The optional members are the values a verb can produce; each command sets only
    // the ones its own verb has. They are all optional because there is one line
    // shape rather than twelve: an operator reads results with one parser.
    struct DriveResult final
    {
        bool                  ok{};
        std::optional<uint64> cycle{};
        std::optional<uint64> hit{};
        std::optional<uint64> deadline{};
        std::optional<bool>   released{};
        std::optional<bool>   budget{};

        // Present and possibly null for a command that resolved a page: null is the
        // Unknown or Ambiguous outcome, which is a completed resolution rather than a
        // failure, so the two must not read alike.
        bool                       resolvedPage{false};
        std::optional<std::string> page{};

        // The domain's own wire spelling of the failure kind, so an operator reads
        // the same string the trace line and a task's Tier B error carry.
        std::optional<std::string> errorKind{};
        std::optional<std::string> message{};
    };

    [[nodiscard]]
    auto serializeDriveResult(
        std::string_view operation,
        DriveResult const& result
    ) -> std::string;

    // The result line for a command that could not be parsed at all. It names no
    // operation, because the line did not successfully name one.
    [[nodiscard]] auto serializeDriveParseFailure(Error const& error) -> std::string;

    // The result line for a failed verb, built from the error it failed with.
    [[nodiscard]]
    auto driveFailure(std::string_view operation, Error const& error) -> std::string;
}
