#pragma once

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <optional>
#include <string_view>

namespace uf::m0_demo
{
    class JsonlLog;

    inline constexpr auto g_defaultPacingSeed = uint64{0x5EED'C10C'2B1D'1A7E};

    class ClickDelay final
    {
        uint64 m_minimumMilliseconds;
        uint64 m_maximumMilliseconds;

        constexpr ClickDelay(
            uint64 minimumMilliseconds,
            uint64 maximumMilliseconds
        ) noexcept
            : m_minimumMilliseconds{minimumMilliseconds}
            , m_maximumMilliseconds{maximumMilliseconds}
        {
        }

    public:
        auto operator==(ClickDelay const&) const -> bool = default;

        [[nodiscard]]
        static auto create(
            uint64 minimumMilliseconds,
            uint64 maximumMilliseconds
        ) -> Result<ClickDelay>;

        [[nodiscard]]
        constexpr auto minimumMilliseconds() const noexcept -> uint64
        {
            return m_minimumMilliseconds;
        }

        [[nodiscard]]
        constexpr auto maximumMilliseconds() const noexcept -> uint64
        {
            return m_maximumMilliseconds;
        }

        [[nodiscard]]
        constexpr auto pickMilliseconds(uint64 raw) const noexcept -> uint64
        {
            auto const width = m_maximumMilliseconds - m_minimumMilliseconds + 1U;
            return m_minimumMilliseconds + raw % width;
        }
    };

    class SplitMix64 final
    {
        uint64 m_state;

    public:
        constexpr explicit SplitMix64(uint64 seed) noexcept
            : m_state{seed}
        {
        }

        [[nodiscard]] auto next() noexcept -> uint64;
    };

    enum class PaceOutcome : uint8
    {
        Elapsed,
        Stopped,
    };

    class ClickPacer final
    {
        std::optional<ClickDelay> m_delay;
        SplitMix64 m_random;

    public:
        ClickPacer(std::optional<ClickDelay> delay, uint64 seed) noexcept;

        [[nodiscard]]
        auto pauseBeforeClick(
            std::string_view label,
            uint32 loopIndex,
            JsonlLog& log
        ) -> Result<PaceOutcome>;
    };
}
