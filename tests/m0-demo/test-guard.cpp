#include <args.hpp>
#include <guard.hpp>

#include <core/types/integer.hpp>

#include <doctest/doctest.h>

#include <array>
#include <string_view>
#include <utility>

namespace uf::m0_demo
{
    TEST_CASE("m0 integrity labels follow Windows RID bands")
    {
        struct IntegrityCase final
        {
            uint32 m_rid{};
            std::string_view m_label{};
        };

        auto const cases = std::array{
            IntegrityCase{0x0000, "untrusted"},
            IntegrityCase{0x1000, "low"},
            IntegrityCase{0x2000, "medium"},
            IntegrityCase{0x2100, "medium"},
            IntegrityCase{0x3000, "high"},
            IntegrityCase{0x4000, "system"},
            IntegrityCase{0x5000, "system"},
        };
        for (auto const& testCase : cases)
        {
            CHECK(IntegrityLevel::fromRid(testCase.m_rid).label() == testCase.m_label);
        }
    }

    TEST_CASE("m0 guard mode compares foreground and cursor")
    {
        auto const policy = GuardPolicy::forMode(Mode::Guard);
        auto const targetWindow = intptr{0x99};
        auto const baseline = GuardBaseline{0x10, {5, 6}};
        CHECK(checkGuard(policy, targetWindow, baseline, baseline).passed());

        auto const movedCursor = GuardBaseline{0x10, {7, 6}};
        auto const changedForeground = GuardBaseline{0x20, {5, 6}};
        CHECK_FALSE(
            checkGuard(
                policy,
                targetWindow,
                baseline,
                movedCursor
            ).m_cursorOk
        );
        CHECK_FALSE(
            checkGuard(
                policy,
                targetWindow,
                baseline,
                changedForeground
            ).m_foregroundOk
        );
    }

    TEST_CASE("m0 guard mode requires a non-target foreground baseline")
    {
        auto const policy = GuardPolicy::forMode(Mode::Guard);
        auto const targetWindow = intptr{0x99};
        for (auto const foreground : std::array{intptr{0}, targetWindow})
        {
            auto const baseline = GuardBaseline{foreground, {5, 6}};
            auto const check = checkGuard(
                policy,
                targetWindow,
                baseline,
                baseline
            );
            CHECK_FALSE(check.m_baselineBackgroundOk);
            CHECK_FALSE(check.passed());
        }
    }

    TEST_CASE("m0 coexist mode compares neither observation")
    {
        auto const policy = GuardPolicy::forMode(Mode::Coexist);
        auto const baseline = GuardBaseline{0x10, {5, 6}};
        auto const different = GuardBaseline{0x99, {100, 200}};
        auto const check = checkGuard(policy, 0x10, baseline, different);

        CHECK(check.passed());
        CHECK(check.m_baselineBackgroundOk);
        CHECK(check.m_foregroundOk);
        CHECK(check.m_cursorOk);
    }
}
