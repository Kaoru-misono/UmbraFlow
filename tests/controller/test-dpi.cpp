#include <controller/detail/dpi-classification.hpp>
#include <controller/dpi.hpp>

#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <array>
#include <optional>

namespace uf
{
    namespace
    {
        [[nodiscard]]
        auto automationKind(Error const& error) -> AutomationErrorKind
        {
            auto const kind = automationErrorKind(error);
            if (!kind.has_value())
            {
                FAIL("The error did not contain an automation error kind");
                return AutomationErrorKind::InternalInvariant;
            }
            return *kind;
        }
    }

    TEST_CASE("successful DPI declaration reports declared")
    {
        auto const result = controller_detail::classifyDpiResult(std::nullopt, true);

        REQUIRE(result.has_value());
        CHECK(*result == DpiDeclaration::Declared);
    }

    TEST_CASE("access denied is tolerated only when V2 is already active")
    {
        auto const result = controller_detail::classifyDpiResult(
            controller_detail::k_accessDeniedError,
            true
        );

        REQUIRE(result.has_value());
        CHECK(*result == DpiDeclaration::AlreadyDeclared);
    }

    TEST_CASE("DPI HRESULT extraction keeps only the low Win32 bits")
    {
        for (auto const hresult : std::array{0x8007'0005U, 0x0001'0005U})
        {
            CHECK(
                controller_detail::win32Code(hresult)
                == controller_detail::k_accessDeniedError
            );
        }
    }

    TEST_CASE("success and access denied reject the wrong actual DPI context")
    {
        auto const outcomes = std::array<std::optional<uint32>, 2>{
            std::nullopt,
            controller_detail::k_accessDeniedError,
        };
        for (auto const outcome : outcomes)
        {
            auto const result = controller_detail::classifyDpiResult(outcome, false);
            REQUIRE_FALSE(result.has_value());
            CHECK(automationKind(result.error()) == AutomationErrorKind::InternalInvariant);
        }
    }

    TEST_CASE("other DPI declaration failures fail closed")
    {
        for (auto const code : std::array{87U, 1U, 0x0000'FFFFU})
        {
            auto const result = controller_detail::classifyDpiResult(code, true);
            REQUIRE_FALSE(result.has_value());
            CHECK(automationKind(result.error()) == AutomationErrorKind::InternalInvariant);
        }
    }
}
