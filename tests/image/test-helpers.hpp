#pragma once

#include <domain/error.hpp>

#include <doctest/doctest.h>

namespace test_image
{
    inline auto requireErrorKind(
        uf::Error const& error,
        uf::AutomationErrorKind expected
    ) -> void
    {
        auto const kind = uf::automationErrorKind(error);
        REQUIRE(kind.has_value());
        CHECK(kind == expected);
    }
}
