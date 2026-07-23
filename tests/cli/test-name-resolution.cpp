#include <name-resolution.hpp>

#include "../annotation/test-helpers.hpp"

#include <annotation/catalog.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <string>
#include <utility>
#include <vector>

namespace uf::cli
{
    namespace
    {
        namespace test = annotation::test;

        [[nodiscard]]
        auto sampleCatalog() -> annotation::RecognitionCatalog
        {
            auto const fingerprint = test::fingerprint();
            auto const anchorId    = test::recognizerId(
                "11111111-1111-1111-1111-111111111111"
            );
            auto const actionId = test::recognizerId(
                "22222222-2222-2222-2222-222222222222"
            );
            auto const homePageId = test::pageId(
                "33333333-3333-3333-3333-333333333333"
            );

            auto recognizers = std::vector<annotation::RecognizerDefinition>{};
            recognizers.emplace_back(
                test::recognizer(
                    fingerprint,
                    anchorId,
                    "home_marker",
                    annotation::AnnotationType::PageAnchor,
                    test::pixelRect(0, 0, 1, 1),
                    test::pixelRect(0, 0, 4, 4)
                )
            );
            recognizers.emplace_back(
                test::recognizer(
                    fingerprint,
                    actionId,
                    "start_button",
                    annotation::AnnotationType::ActionTarget,
                    test::pixelRect(1, 1, 1, 1),
                    test::pixelRect(0, 0, 4, 4),
                    {homePageId}
                )
            );

            auto pages = std::vector<annotation::PageSignature>{};
            pages.emplace_back(test::page(homePageId, "home", {anchorId}));

            return test::catalog(fingerprint, std::move(recognizers), std::move(pages));
        }
    }

    TEST_CASE("resolvePageName maps a known page name to its id")
    {
        auto const catalog    = sampleCatalog();
        auto const homePageId = test::pageId(
            "33333333-3333-3333-3333-333333333333"
        );

        auto const resolved = resolvePageName(catalog, "home");
        REQUIRE(resolved.has_value());
        CHECK(*resolved == homePageId);
    }

    TEST_CASE("resolvePageName lists available pages for an unknown name")
    {
        auto const catalog = sampleCatalog();

        auto const resolved = resolvePageName(catalog, "menu");
        REQUIRE_FALSE(resolved.has_value());
        CHECK(automationErrorKind(resolved.error()) == AutomationErrorKind::InvalidResource);
        CHECK(
            resolved.error().message()
            == "unknown page name \"menu\"; available pages: \"home\""
        );
    }

    TEST_CASE("resolveActionName maps a known action target to its recognizer id")
    {
        auto const catalog   = sampleCatalog();
        auto const actionId = test::recognizerId(
            "22222222-2222-2222-2222-222222222222"
        );

        auto const resolved = resolveActionName(catalog, "start_button");
        REQUIRE(resolved.has_value());
        CHECK(*resolved == actionId);
    }

    TEST_CASE("resolveActionName rejects a page anchor name")
    {
        auto const catalog = sampleCatalog();

        auto const resolved = resolveActionName(catalog, "home_marker");
        REQUIRE_FALSE(resolved.has_value());
        CHECK(
            resolved.error().message()
            == "unknown action name \"home_marker\"; "
               "available action targets: \"start_button\""
        );
    }

    TEST_CASE("resolveActionName lists available action targets for an unknown name")
    {
        auto const catalog = sampleCatalog();

        auto const resolved = resolveActionName(catalog, "quit");
        REQUIRE_FALSE(resolved.has_value());
        CHECK(
            resolved.error().message()
            == "unknown action name \"quit\"; "
               "available action targets: \"start_button\""
        );
    }
}
