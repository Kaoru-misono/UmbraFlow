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

    // THE OPERATOR COMMAND PROTOCOL.
    //
    // It is the private capability surface, verbatim: one command per primitive,
    // with the same name, the same arguments and the same failure modes as the Luau
    // surface has. The opaque handles a task holds become integer ids, because an
    // operator has only text; nothing else changes, nothing is composed, and nothing
    // is bypassed. Every command below maps to exactly one task::OperatorSession
    // verb, which maps to exactly one TaskContext call -- the same call ctx.luau
    // makes.
    //
    // IT USED TO HAVE A SECOND LAYER, and this is what happened to it. `wait_page`
    // and `find_click` composed the primitives into "wait until this page resolves"
    // and "find this element, then click it". Both rode `cycle_page` and
    // `cycle_find`, which retired with the C++ page model
    // (docs/plans/2026-07-31-script-owned-page-model.md 9), and there is nothing
    // for them to compose any more: an element and a page are layer-two Luau
    // objects, and this front-end runs no Luau.
    //
    // NOTHING WAS INVENTED IN THEIR PLACE. What survives is the set of primitives
    // that never named a model -- observing, keying, and the time verbs -- and an
    // operator that needs to recognise a page reaches for `umbra-flow run` and a
    // task, until the exploration environment gives the operator its own route to
    // the layer-two model.
    //
    // The rule that governed the retired layer is kept here because it will govern
    // its successor: a convenience command carries NO POLICY DEFAULTS OF ITS OWN.
    // Every timeout and poll interval is a REQUIRED field, so there is no second
    // copy of task-side policy in C++ to drift out of step with
    // modules/task/runtime.

    struct DriveCycleOpenCommand final
    {
        auto operator==(DriveCycleOpenCommand const&) const -> bool = default;
    };

    struct DriveCycleCloseCommand final
    {
        uint64 cycle{};

        auto operator==(DriveCycleCloseCommand const&) const -> bool = default;
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

    struct DriveQuitCommand final
    {
        auto operator==(DriveQuitCommand const&) const -> bool = default;
    };

    using DriveCommand = std::variant<
        DriveCycleOpenCommand,
        DriveCycleCloseCommand,
        DriveKeyCommand,
        DriveSettleCommand,
        DriveDeadlineCommand,
        DriveWaitCommand,
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
    // shape rather than seven: an operator reads results with one parser.
    struct DriveResult final
    {
        bool                  ok{};
        std::optional<uint64> cycle{};
        std::optional<uint64> deadline{};
        std::optional<bool>   released{};
        std::optional<bool>   budget{};

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
