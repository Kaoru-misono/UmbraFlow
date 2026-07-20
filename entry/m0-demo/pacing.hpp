#pragma once

#include <core/error/result.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace uf::m0_demo
{
    class JsonlLog;

    inline constexpr auto g_defaultPacingSeed = std::uint64_t{0x5EED'C10C'2B1D'1A7E};

    class ClickDelay final
    {
        std::uint64_t m_minimumMilliseconds;
        std::uint64_t m_maximumMilliseconds;

        constexpr ClickDelay(
            std::uint64_t minimumMilliseconds,
            std::uint64_t maximumMilliseconds
        ) noexcept
            : m_minimumMilliseconds{minimumMilliseconds}
            , m_maximumMilliseconds{maximumMilliseconds}
        {
        }

    public:
        auto operator==(ClickDelay const&) const -> bool = default;

        [[nodiscard]]
        static auto create(
            std::uint64_t minimumMilliseconds,
            std::uint64_t maximumMilliseconds
        ) -> Result<ClickDelay>;

        [[nodiscard]]
        constexpr auto minimumMilliseconds() const noexcept -> std::uint64_t
        {
            return m_minimumMilliseconds;
        }

        [[nodiscard]]
        constexpr auto maximumMilliseconds() const noexcept -> std::uint64_t
        {
            return m_maximumMilliseconds;
        }

        [[nodiscard]]
        constexpr auto pickMilliseconds(std::uint64_t raw) const noexcept -> std::uint64_t
        {
            auto const width = m_maximumMilliseconds - m_minimumMilliseconds + 1U;
            return m_minimumMilliseconds + raw % width;
        }
    };

    class SplitMix64 final
    {
        std::uint64_t m_state;

    public:
        constexpr explicit SplitMix64(std::uint64_t seed) noexcept
            : m_state{seed}
        {
        }

        [[nodiscard]] auto next() noexcept -> std::uint64_t;
    };

    enum class PaceOutcome
    {
        Elapsed,
        Stopped,
    };

    class ClickPacer final
    {
        std::optional<ClickDelay> m_delay;
        SplitMix64 m_random;

    public:
        ClickPacer(std::optional<ClickDelay> delay, std::uint64_t seed) noexcept;

        [[nodiscard]]
        auto pauseBeforeClick(
            std::string_view label,
            std::uint32_t loopIndex,
            JsonlLog& log
        ) -> Result<PaceOutcome>;
    };
}
