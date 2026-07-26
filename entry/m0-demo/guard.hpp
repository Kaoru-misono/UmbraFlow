#pragma once

#include "args.hpp"

#include <controller/discovery.hpp>
#include <core/error/result.hpp>
#include <core/types/integer.hpp>
#include <domain/space.hpp>

#include <compare>
#include <optional>
#include <string_view>
#include <utility>

namespace uf::m0_demo
{
    class IntegrityLevel final
    {
        uint32 m_rid;

        constexpr explicit IntegrityLevel(uint32 rid) noexcept
            : m_rid{rid}
        {
        }

    public:
        auto operator<=>(IntegrityLevel const&) const = default;

        [[nodiscard]]
        static constexpr auto fromRid(uint32 rid) noexcept -> IntegrityLevel
        {
            return IntegrityLevel{rid};
        }

        [[nodiscard]] constexpr auto rid() const noexcept -> uint32 { return m_rid; }
        [[nodiscard]] auto label() const noexcept -> std::string_view;
    };

    struct GuardBaseline final
    {
        intptr foreground{};
        std::pair<int32, int32> cursor{};

        auto operator==(GuardBaseline const&) const -> bool = default;
    };

    struct GuardPolicy final
    {
        bool compareForeground{};
        bool compareCursor{};

        auto operator==(GuardPolicy const&) const -> bool = default;

        [[nodiscard]] static auto forMode(Mode mode) noexcept -> GuardPolicy;
    };

    struct GuardCheck final
    {
        bool baselineBackgroundOk{};
        bool foregroundOk{};
        bool cursorOk{};

        auto operator==(GuardCheck const&) const -> bool = default;

        [[nodiscard]]
        constexpr auto passed() const noexcept -> bool
        {
            return baselineBackgroundOk && foregroundOk && cursorOk;
        }
    };

    [[nodiscard]]
    auto checkGuard(
        GuardPolicy policy,
        intptr targetWindow,
        GuardBaseline baseline,
        GuardBaseline observed
    ) noexcept -> GuardCheck;

    [[nodiscard]] auto observeGuard(GuardPolicy policy) -> Result<GuardBaseline>;

    [[nodiscard]]
    auto clientOriginDesktop(WindowHandle windowHandle) -> Result<Point<DesktopSpace>>;

    [[nodiscard]] auto currentProcessIntegrity() -> std::optional<IntegrityLevel>;
    [[nodiscard]] auto processIntegrity(ProcessId process) -> std::optional<IntegrityLevel>;
}
