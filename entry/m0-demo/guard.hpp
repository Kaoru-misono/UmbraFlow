#pragma once

#include "args.hpp"

#include <controller/discovery.hpp>
#include <core/error/result.hpp>
#include <domain/space.hpp>

#include <compare>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace uf::m0_demo
{
    class IntegrityLevel final
    {
        std::uint32_t m_rid;

        constexpr explicit IntegrityLevel(std::uint32_t rid) noexcept
            : m_rid{rid}
        {
        }

    public:
        auto operator<=>(IntegrityLevel const&) const = default;

        [[nodiscard]]
        static constexpr auto fromRid(std::uint32_t rid) noexcept -> IntegrityLevel
        {
            return IntegrityLevel{rid};
        }

        [[nodiscard]] constexpr auto rid() const noexcept -> std::uint32_t { return m_rid; }
        [[nodiscard]] auto label() const noexcept -> std::string_view;
    };

    struct GuardBaseline final
    {
        std::intptr_t m_foreground;
        std::pair<std::int32_t, std::int32_t> m_cursor;

        auto operator==(GuardBaseline const&) const -> bool = default;
    };

    struct GuardPolicy final
    {
        bool m_compareForeground;
        bool m_compareCursor;

        auto operator==(GuardPolicy const&) const -> bool = default;

        [[nodiscard]] static auto forMode(Mode mode) noexcept -> GuardPolicy;
    };

    struct GuardCheck final
    {
        bool m_baselineBackgroundOk;
        bool m_foregroundOk;
        bool m_cursorOk;

        auto operator==(GuardCheck const&) const -> bool = default;

        [[nodiscard]]
        constexpr auto passed() const noexcept -> bool
        {
            return m_baselineBackgroundOk && m_foregroundOk && m_cursorOk;
        }
    };

    [[nodiscard]]
    auto checkGuard(
        GuardPolicy policy,
        std::intptr_t targetWindow,
        GuardBaseline baseline,
        GuardBaseline observed
    ) noexcept -> GuardCheck;

    [[nodiscard]] auto observeGuard(GuardPolicy policy) -> Result<GuardBaseline>;

    [[nodiscard]]
    auto clientOriginDesktop(WindowHandle windowHandle) -> Result<Point<DesktopSpace>>;

    [[nodiscard]] auto currentProcessIntegrity() -> std::optional<IntegrityLevel>;
    [[nodiscard]] auto processIntegrity(ProcessId process) -> std::optional<IntegrityLevel>;
}
