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
    inline constexpr auto g_defaultCaptureFrames = uint32{1};
    inline constexpr auto g_defaultCaptureInterval = MonotonicInstant::Duration::zero();
    inline constexpr auto g_defaultInputAgentIdleTimeout = (
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
        std::optional<uint32>      m_process{};
        std::optional<intptr>      m_windowHandle{};
        std::optional<std::string> m_windowClass{};
        std::optional<std::string> m_title{};

        auto operator==(SelectorArgs const&) const -> bool = default;
    };

    struct Args final
    {
        SelectorArgs m_selector{};

        std::filesystem::path m_homeTemplate{};
        Rect<FrameSpace>      m_homeRoi;
        std::filesystem::path m_resultTemplate{};
        Rect<FrameSpace>      m_resultRoi;
        std::filesystem::path m_resetTemplate{};
        Rect<FrameSpace>      m_resetRoi;

        uint64                     m_threshold{};
        Mode                       m_mode{};
        uint32                     m_loops{};
        MonotonicInstant::Duration m_maxActionFrameAge{};
        MonotonicInstant::Duration m_stallTimeout{};
        std::optional<ClickDelay>  m_clickDelay{};
        uint64                     m_seed{};

        std::optional<std::filesystem::path> m_log{};

        auto operator==(Args const&) const -> bool = default;
    };

    struct CaptureArgs final
    {
        SelectorArgs m_selector{};

        std::filesystem::path      m_output{};
        uint32                     m_frames{};
        MonotonicInstant::Duration m_interval{};

        std::optional<std::filesystem::path> m_log{};

        auto operator==(CaptureArgs const&) const -> bool = default;
    };

    struct InputAgentArgs final
    {
        intptr                     m_windowHandle{};
        std::filesystem::path      m_queue{};
        std::filesystem::path      m_results{};
        std::filesystem::path      m_outputDirectory{};
        MonotonicInstant::Duration m_idleTimeout{};

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
