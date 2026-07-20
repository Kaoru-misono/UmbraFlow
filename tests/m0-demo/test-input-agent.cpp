#include "test-helpers.hpp"

#include <input-agent-protocol.hpp>
#include <input-agent.hpp>
#include <path-validation.hpp>

#include <controller/detail/input-guard.hpp>
#include <controller/input.hpp>
#include <core/utility/scope-exit.hpp>
#include <domain/error.hpp>
#include <domain/frame.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace
{
    [[nodiscard]]
    auto instantAt(
        uf::MonotonicInstant::Duration duration
    ) -> uf::MonotonicInstant
    {
        return uf::MonotonicInstant::fromTimePoint(
            uf::MonotonicInstant::TimePoint{duration}
        );
    }

    [[nodiscard]]
    auto deliveryTarget(
        uf::TargetGeneration generation = uf::TargetGeneration{}
    ) -> uf::DeliveryTarget
    {
        auto target = uf::DeliveryTarget::create(
            uf::WindowHandle{0x1234},
            uf::SessionId{1},
            generation,
            800,
            450
        );
        REQUIRE(target.has_value());
        return *std::move(target);
    }

    [[nodiscard]]
    auto observationLease(
        uf::MonotonicInstant capturedAt,
        uf::TargetGeneration generation = uf::TargetGeneration{}
    ) -> uf::ObservationLease
    {
        auto const transform = uf::CoordinateTransform::create(
            uf::Point<uf::DesktopSpace>{0.0F, 0.0F},
            1.0F,
            1.0F,
            1,
            1
        );
        REQUIRE(transform.has_value());
        auto const pixels = std::make_shared<uf::FrameBuffer const>(
            std::vector<std::byte>(4)
        );
        auto const frame = uf::Frame::create(
            uf::FrameId{1},
            uf::SessionId{1},
            generation,
            capturedAt,
            1,
            1,
            4,
            uf::PixelFormat::Bgra8,
            pixels,
            *transform
        );
        REQUIRE(frame.has_value());
        auto const lease = uf::ObservationLease::forFrame(
            *frame,
            uf::g_defaultMaxActionFrameAge
        );
        REQUIRE(lease.has_value());
        return *lease;
    }

    [[nodiscard]]
    auto parsedClick(std::string_view line) -> uf::m0_demo::InputAgentClickCommand
    {
        auto command = uf::m0_demo::parseInputAgentCommand(line);
        REQUIRE(command.has_value());
        auto const* click = std::get_if<uf::m0_demo::InputAgentClickCommand>(
            &*command
        );
        REQUIRE(click != nullptr);
        return *click;
    }
}

TEST_CASE("m0 input-agent parses every bounded command operation")
{
    auto const capture = uf::m0_demo::parseInputAgentCommand(
        R"({"op":"capture","out":"shots\/one.png"})"
    );
    REQUIRE(capture.has_value());
    auto const* captureCommand = std::get_if<uf::m0_demo::InputAgentCaptureCommand>(
        &*capture
    );
    REQUIRE(captureCommand != nullptr);
    CHECK(captureCommand->m_output == std::filesystem::path{"shots/one.png"});

    auto const click = parsedClick(
        R"({"op":"click","x":12.5,"y":4e1,"out_before":"before.png",)"
        R"("out_after":"after.png","settle_ms":250})"
    );
    CHECK(click.m_x == 12.5F);
    CHECK(click.m_y == 40.0F);
    CHECK(click.m_outputBefore == std::filesystem::path{"before.png"});
    CHECK(click.m_outputAfter == std::filesystem::path{"after.png"});
    CHECK(
        click.m_settle
        == std::chrono::duration_cast<uf::MonotonicInstant::Duration>(
            std::chrono::milliseconds{250}
        )
    );

    auto const defaultSettle = parsedClick(
        R"({"op":"click","x":1,"y":2,"out_before":"before.png","out_after":"after.png"})"
    );
    CHECK(defaultSettle.m_settle == uf::m0_demo::g_defaultInputAgentSettle);

    auto const quit = uf::m0_demo::parseInputAgentCommand(
        "  { \"op\" : \"quit\" }  "
    );
    REQUIRE(quit.has_value());
    CHECK(std::holds_alternative<uf::m0_demo::InputAgentQuitCommand>(*quit));
}

TEST_CASE("m0 input-agent rejects malformed and unrecognized commands")
{
    auto const cases = std::array<std::string_view, 11>{
        "",
        "{}",
        R"({"op":"run","out":"anything"})",
        R"({"op":"capture"})",
        R"({"op":"capture","out":""})",
        R"({"op":"capture","out":"bad\u0000path.png"})",
        R"({"op":"capture","out":"a.png","extra":1})",
        R"({"op":"quit","x":1})",
        R"({"op":"click","x":1,"y":2,"out_before":"a.png"})",
        R"({"op":"click","x":1,"x":2,"y":2,"out_before":"a.png","out_after":"b.png"})",
        R"({"op":"click","x":1,"y":2,"out_before":"a.png","out_after":"b.png","settle_ms":1.5})",
    };

    for (auto const line : cases)
    {
        auto const result = uf::m0_demo::parseInputAgentCommand(line);
        REQUIRE_FALSE(result.has_value());
        test_m0_demo::requireErrorKind(
            result.error(),
            uf::AutomationErrorKind::InvalidResource
        );
    }
}

TEST_CASE("m0 input-agent rejects settle times above the bounded wait")
{
    auto const boundary = parsedClick(
        R"({"op":"click","x":1,"y":2,"out_before":"a.png","out_after":"b.png","settle_ms":5000})"
    );
    CHECK(boundary.m_settle == uf::m0_demo::g_maximumInputAgentSettle);

    auto const result = uf::m0_demo::parseInputAgentCommand(
        R"({"op":"click","x":1,"y":2,"out_before":"a.png","out_after":"b.png","settle_ms":5001})"
    );
    REQUIRE_FALSE(result.has_value());
    test_m0_demo::requireErrorKind(
        result.error(),
        uf::AutomationErrorKind::InvalidResource
    );
    CHECK(result.error().message().contains("must not exceed 5000"));
}

TEST_CASE("m0 input-agent rejects malformed UTF-8 command strings")
{
    auto command = std::string{R"({"op":"capture","out":"bad-)"};
    command += static_cast<char>(0xC3U);
    command += static_cast<char>(0x28U);
    command += R"(.png"})";

    auto const result = uf::m0_demo::parseInputAgentCommand(command);
    REQUIRE_FALSE(result.has_value());
    test_m0_demo::requireErrorKind(
        result.error(),
        uf::AutomationErrorKind::InvalidResource
    );
    CHECK(result.error().message().contains("valid UTF-8"));
}

TEST_CASE("m0 input-agent confines output paths to its canonical directory")
{
    auto const token = std::chrono::steady_clock::now()
        .time_since_epoch()
        .count();
    auto const outputDirectory = std::filesystem::temp_directory_path()
        / std::format("umbraflow-input-agent-output-{}", token);
    auto error = std::error_code{};
    auto const created = std::filesystem::create_directory(
        outputDirectory,
        error
    );
    REQUIRE(created);
    REQUIRE_FALSE(error);
    auto const cleanupPath = std::make_shared<std::filesystem::path const>(
        outputDirectory
    );
    auto const cleanup = uf::scopeExit(
        [cleanupPath]() noexcept
        {
            auto cleanupError = std::error_code{};
            static_cast<void>(
                std::filesystem::remove(*cleanupPath, cleanupError)
            );
        }
    );

    auto const canonicalDirectory = uf::m0_demo::canonicalizeOutputDirectory(
        outputDirectory
    );
    REQUIRE(canonicalDirectory.has_value());
    auto const inside = uf::m0_demo::resolveConfinedOutputPath(
        *canonicalDirectory,
        "inside.png",
        "test output"
    );
    REQUIRE(inside.has_value());
    CHECK(inside->parent_path() == *canonicalDirectory);

    for (auto const& escaped : std::array{
        std::filesystem::path{"../escape.png"},
        outputDirectory.parent_path() / "absolute-escape.png",
    })
    {
        auto const result = uf::m0_demo::resolveConfinedOutputPath(
            *canonicalDirectory,
            escaped,
            "test output"
        );
        REQUIRE_FALSE(result.has_value());
        test_m0_demo::requireErrorKind(
            result.error(),
            uf::AutomationErrorKind::InvalidResource
        );
    }
}

TEST_CASE("m0 input-agent clears per-command audit state across many clicks")
{
    auto audit = uf::AuditLog{};
    auto maximumRecords = std::size_t{};
    for (auto command = std::size_t{}; command < 10'000U; ++command)
    {
        for (auto message = std::uint32_t{}; message < 3U; ++message)
        {
            uf::controller_detail::AuditLogAccess::record(
                audit,
                uf::WindowHandle{0x1234},
                message,
                0U,
                0
            );
        }
        maximumRecords = std::max(maximumRecords, audit.size());
        uf::m0_demo::clearInputAgentCommandAudit(audit);
    }

    CHECK(maximumRecords == 3U);
    CHECK(audit.empty());
}

TEST_CASE("m0 input-agent click coordinates fail closed at client bounds")
{
    auto const target = deliveryTarget();
    auto const now = instantAt(uf::MonotonicInstant::Duration{10});
    auto const lease = observationLease(now);
    auto const valid = parsedClick(
        R"({"op":"click","x":799.9,"y":449.9,"out_before":"a.png","out_after":"b.png"})"
    );
    CHECK(
        uf::m0_demo::validateInputAgentClick(
            target,
            lease,
            uf::Point<uf::ClientSpace>{valid.m_x, valid.m_y},
            now
        )
    );

    auto const cases = std::array<std::string_view, 4>{
        R"({"op":"click","x":-0.1,"y":1,"out_before":"a.png","out_after":"b.png"})",
        R"({"op":"click","x":1,"y":-0.1,"out_before":"a.png","out_after":"b.png"})",
        R"({"op":"click","x":800,"y":1,"out_before":"a.png","out_after":"b.png"})",
        R"({"op":"click","x":1,"y":450,"out_before":"a.png","out_after":"b.png"})",
    };
    for (auto const line : cases)
    {
        auto const click = parsedClick(line);
        auto const result = uf::m0_demo::validateInputAgentClick(
            target,
            lease,
            uf::Point<uf::ClientSpace>{click.m_x, click.m_y},
            now
        );

        REQUIRE_FALSE(result.has_value());
        test_m0_demo::requireErrorKind(
            result.error(),
            uf::AutomationErrorKind::ActionRejected
        );
    }
}

TEST_CASE("m0 input-agent click validation rejects a stale generation")
{
    auto const next = uf::TargetGeneration{}.next();
    REQUIRE(next.has_value());
    auto const now = instantAt(uf::MonotonicInstant::Duration{10});
    auto const result = uf::m0_demo::validateInputAgentClick(
        deliveryTarget(*next),
        observationLease(now),
        uf::Point<uf::ClientSpace>{1.0F, 1.0F},
        now
    );

    REQUIRE_FALSE(result.has_value());
    test_m0_demo::requireErrorKind(
        result.error(),
        uf::AutomationErrorKind::StaleObservation
    );
}
