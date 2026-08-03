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
    // A command is a handful of scalars; anything near this ceiling is a malformed
    // line rather than a large one.
    inline constexpr auto k_maxDriveCommandBytes = std::size_t{64} * 1024U;

    // The operator command protocol: the private capability surface verbatim, one
    // command per primitive, with the same name, arguments and failure modes as the
    // Luau surface has. Opaque handles become integer ids because an operator has
    // only text; nothing is composed and nothing is bypassed. Every command below
    // maps to exactly one task::OperatorSession verb, and so to exactly one
    // TaskContext call -- the same call ctx.luau makes.
    //
    // Only the primitives that never named a model survive. The composing
    // `wait_page` and `find_click` rode `cycle_page` and `cycle_find`, which retired
    // with the C++ page model (docs/plans/2026-07-31-script-owned-page-model.md 9);
    // an operator that needs to recognise a page reaches for `umbra-flow run`. The
    // rule that governed them governs their successor: a convenience command carries
    // no policy defaults of its own -- every timeout and poll interval is a REQUIRED
    // field, so no second copy of task-side policy can drift out of step with
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

    // A keystroke names no position, so no coordinate and no hit. It still names a
    // cycle, which is what orders the keystroke against the observations around it
    // and gives its trace line a cycle to join on.
    struct DriveKeyCommand final
    {
        uint64  cycle{};
        KeyName key;

        auto operator==(DriveKeyCommand const&) const -> bool = default;
    };

    // A wheel scroll names no position either, for the same reason a keystroke
    // does not: the target scrolls whatever it believes is hovered. `notches`
    // is signed because the two directions are one verb, and it is the only
    // signed field in this protocol.
    struct DriveScrollCommand final
    {
        uint64 cycle{};
        int32  notches{};

        auto operator==(DriveScrollCommand const&) const -> bool = default;
    };

    // The one command here that names a screen position. It presses nothing, so
    // it activates nothing, which is why an operator may name the coordinate
    // directly where no click command exists to do so. It is what makes the
    // scroll above land where it was aimed
    // (docs/pitfalls/capture-and-target-selection.md).
    struct DriveMovePointerCommand final
    {
        uint64 cycle{};
        uint32 x{};
        uint32 y{};

        auto operator==(DriveMovePointerCommand const&) const -> bool = default;
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
        DriveScrollCommand,
        DriveMovePointerCommand,
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

    // A refused command must never report `ok` true. The optional members are the
    // values a verb can produce, each command setting only its own verb's; they are
    // optional because there is one line shape rather than seven, so an operator
    // reads results with one parser.
    struct DriveResult final
    {
        bool                  ok{};
        std::optional<uint64> cycle{};
        std::optional<uint64> deadline{};
        std::optional<bool>   released{};
        std::optional<bool>   budget{};

        // The domain's own wire spelling, so an operator reads the same string the
        // trace line and a task's Tier B error carry.
        std::optional<std::string> errorKind{};
        std::optional<std::string> message{};
    };

    [[nodiscard]]
    auto serializeDriveResult(
        std::string_view operation,
        DriveResult const& result
    ) -> std::string;

    // For a command that could not be parsed at all. It names no operation, because
    // the line did not successfully name one.
    [[nodiscard]] auto serializeDriveParseFailure(Error const& error) -> std::string;

    [[nodiscard]]
    auto driveFailure(std::string_view operation, Error const& error) -> std::string;
}
