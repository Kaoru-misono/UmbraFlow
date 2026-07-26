#pragma once

#include "pacing.hpp"

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>
#include <domain/space.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace uf::m0_demo
{
    inline constexpr auto k_defaultCaptureFrames = uint32{1};
    inline constexpr auto k_defaultCaptureInterval = MonotonicInstant::Duration::zero();
    inline constexpr auto k_defaultInputAgentIdleTimeout = (
        std::chrono::duration_cast<MonotonicInstant::Duration>(
            std::chrono::seconds{120}
        )
    );

    enum class Mode : uint8
    {
        Guard,
        Coexist,
    };

    struct SelectorArgs final
    {
        std::optional<uint32>      process{};
        std::optional<intptr>      windowHandle{};
        std::optional<std::string> windowClass{};
        std::optional<std::string> title{};

        auto operator==(SelectorArgs const&) const -> bool = default;
    };

    struct Args final
    {
        SelectorArgs selector{};

        std::filesystem::path homeTemplate{};
        Rect<FrameSpace>      homeRoi;
        std::filesystem::path resultTemplate{};
        Rect<FrameSpace>      resultRoi;
        std::filesystem::path resetTemplate{};
        Rect<FrameSpace>      resetRoi;

        uint64                     threshold{};
        Mode                       mode{};
        uint32                     loops{};
        MonotonicInstant::Duration maxActionFrameAge{};
        MonotonicInstant::Duration stallTimeout{};
        std::optional<ClickDelay>  clickDelay{};
        uint64                     seed{};

        std::optional<std::filesystem::path> log{};

        auto operator==(Args const&) const -> bool = default;
    };

    struct CaptureArgs final
    {
        SelectorArgs selector{};

        std::filesystem::path      output{};
        uint32                     frames{};
        MonotonicInstant::Duration interval{};

        std::optional<std::filesystem::path> log{};

        auto operator==(CaptureArgs const&) const -> bool = default;
    };

    struct InputAgentArgs final
    {
        intptr                     windowHandle{};
        std::filesystem::path      queue{};
        std::filesystem::path      results{};
        std::filesystem::path      outputDirectory{};
        MonotonicInstant::Duration idleTimeout{};

        auto operator==(InputAgentArgs const&) const -> bool = default;
    };

    [[nodiscard]]
    auto parseArguments(std::span<std::string const> raw) -> Result<Args>;

    [[nodiscard]]
    auto parseCaptureArguments(
        std::span<std::string const> raw
    ) -> Result<CaptureArgs>;

    [[nodiscard]]
    auto parseInputAgentArguments(
        std::span<std::string const> raw
    ) -> Result<InputAgentArgs>;

    [[nodiscard]] auto usageText() noexcept -> std::string_view;
    [[nodiscard]] auto captureUsageText() noexcept -> std::string_view;
    [[nodiscard]] auto inputAgentUsageText() noexcept -> std::string_view;
}
