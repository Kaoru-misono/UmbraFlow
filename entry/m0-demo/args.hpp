#pragma once

#include "pacing.hpp"

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>
#include <domain/space.hpp>

#include <target-setup.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace uf::m0_demo
{
    inline constexpr auto k_defaultCaptureFrames = uint32{1};
    inline constexpr auto k_defaultCaptureInterval = MonotonicInstant::Duration::zero();

    enum class Mode : uint8
    {
        Guard,
        Coexist,
    };

    // The selector this demo shares with the input agent, which owns it beside
    // the buildSelector call that consumes it.
    using SelectorArgs = input_agent::SelectorArgs;

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

    [[nodiscard]]
    auto parseArguments(std::span<std::string const> raw) -> Result<Args>;

    [[nodiscard]]
    auto parseCaptureArguments(
        std::span<std::string const> raw
    ) -> Result<CaptureArgs>;

    [[nodiscard]] auto usageText() noexcept -> std::string_view;
    [[nodiscard]] auto captureUsageText() noexcept -> std::string_view;
}
