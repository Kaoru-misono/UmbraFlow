#pragma once

#include "pacing.hpp"

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <domain/space.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace uf::m0_demo
{
    inline constexpr auto g_defaultCaptureFrames = std::uint32_t{1};
    inline constexpr auto g_defaultCaptureInterval = MonotonicInstant::Duration::zero();
    inline constexpr auto g_defaultInputAgentIdleTimeout = (
        std::chrono::duration_cast<MonotonicInstant::Duration>(
            std::chrono::seconds{120}
        )
    );

    enum class Mode
    {
        Guard,
        Coexist,
    };

    struct SelectorArgs final
    {
        std::optional<std::uint32_t> m_process{};
        std::optional<std::intptr_t> m_windowHandle{};
        std::optional<std::string> m_windowClass{};
        std::optional<std::string> m_title{};

        auto operator==(SelectorArgs const&) const -> bool = default;
    };

    struct Args final
    {
        SelectorArgs m_selector;
        std::filesystem::path m_homeTemplate;
        Rect<FrameSpace> m_homeRoi;
        std::filesystem::path m_resultTemplate;
        Rect<FrameSpace> m_resultRoi;
        std::filesystem::path m_resetTemplate;
        Rect<FrameSpace> m_resetRoi;
        std::uint64_t m_threshold;
        Mode m_mode;
        std::uint32_t m_loops;
        MonotonicInstant::Duration m_maxActionFrameAge;
        MonotonicInstant::Duration m_stallTimeout;
        std::optional<ClickDelay> m_clickDelay;
        std::uint64_t m_seed;
        std::optional<std::filesystem::path> m_log;

        auto operator==(Args const&) const -> bool = default;
    };

    struct CaptureArgs final
    {
        SelectorArgs m_selector;
        std::filesystem::path m_output;
        std::uint32_t m_frames;
        MonotonicInstant::Duration m_interval;
        std::optional<std::filesystem::path> m_log;

        auto operator==(CaptureArgs const&) const -> bool = default;
    };

    struct InputAgentArgs final
    {
        std::intptr_t m_windowHandle;
        std::filesystem::path m_queue;
        std::filesystem::path m_results;
        std::filesystem::path m_outputDirectory;
        MonotonicInstant::Duration m_idleTimeout;

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
