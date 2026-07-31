#include "test-helpers.hpp"

#include <annotation/resource.hpp>
#include <annotation/appearance.hpp>

#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <string_view>
#include <utility>

namespace uf::annotation
{
    namespace
    {
        constexpr auto k_sourceId = "00000000-0000-0000-0000-000000000301";
    }

    TEST_CASE("a well-formed appearance is accepted and reads back what it was given")
    {
        auto const source       = test::sourceId(k_sourceId);
        auto const templateRect = test::pixelRect(2, 3, 8, 4);
        auto const threshold    = test::threshold();
        auto const colourKey    = ColourKey::create(255, 255, 255, 12);
        REQUIRE(colourKey.has_value());

        auto result = Appearance::create(
            Appearance::Spec{
                .name         = test::resourceName("on_dark"),
                .sourceId     = source,
                .templateRect = templateRect,
                .threshold    = threshold,
                .colourKey    = *colourKey,
            }
        );
        REQUIRE(result.has_value());

        auto const appearance = *std::move(result);
        CHECK(appearance.name() == test::resourceName("on_dark"));
        CHECK(appearance.sourceId() == source);
        CHECK(appearance.templateRect() == templateRect);
        CHECK(appearance.threshold() == threshold);
        REQUIRE(appearance.colourKey().has_value());
        CHECK(*appearance.colourKey() == *colourKey);
    }

    TEST_CASE("an appearance without a colour key counts every pixel of its template")
    {
        auto result = Appearance::create(
            Appearance::Spec{
                .name         = test::resourceName("on_light"),
                .sourceId     = test::sourceId(k_sourceId),
                .templateRect = test::pixelRect(0, 0, 6, 6),
                .threshold    = test::threshold(),
            }
        );
        REQUIRE(result.has_value());
        CHECK_FALSE(result->colourKey().has_value());
    }

    TEST_CASE("an appearance whose template overflows the similarity calculation is refused")
    {
        // The one invariant an appearance's own fields can establish: the same
        // maximumSad guard the element applies to the same pair of fields.
        auto result = Appearance::create(
            Appearance::Spec{
                .name         = test::resourceName("on_dark"),
                .sourceId     = test::sourceId(k_sourceId),
                .templateRect = test::pixelRect(0, 0, 4'000'000'000, 4'000'000'000),
                .threshold    = test::threshold(),
            }
        );
        REQUIRE_FALSE(result.has_value());
        test::requireErrorKind(
            result.error(),
            AutomationErrorKind::InvalidResource
        );
        CHECK(
            result.error().message().find("overflow")
            != std::string_view::npos
        );
    }
}
