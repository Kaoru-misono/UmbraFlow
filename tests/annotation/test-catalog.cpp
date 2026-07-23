#include "test-helpers.hpp"

#include <annotation/catalog.hpp>

#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <limits>
#include <utility>
#include <vector>

namespace uf::annotation
{
    namespace
    {
        constexpr auto g_anchorId = "00000000-0000-0000-0000-000000000001";
        constexpr auto g_actionId = "00000000-0000-0000-0000-000000000002";
        constexpr auto g_pageId = "00000000-0000-0000-0000-000000000101";
        constexpr auto g_secondPageId = "00000000-0000-0000-0000-000000000102";
    }

    TEST_CASE("annotation resource identifiers and names use canonical closed forms")
    {
        auto const uppercase = ResourceId::parse(
            "A5B4C3D2-1111-2222-3333-ABCDEF123456"
        );
        REQUIRE(uppercase.has_value());
        CHECK(
            uppercase->toString()
            == "a5b4c3d2-1111-2222-3333-abcdef123456"
        );

        auto const badUuid = ResourceId::parse("not-a-uuid");
        REQUIRE_FALSE(badUuid.has_value());
        test::requireErrorKind(
            badUuid.error(),
            AutomationErrorKind::InvalidResource
        );

        CHECK(ResourceName::create("home_marker").has_value());
        for (
            auto const* value : {
                "",
                "9marker",
                "home-marker",
                "页面",
                "end",
                "local",
                "true",
            }
        )
        {
            auto const result = ResourceName::create(value);
            REQUIRE_FALSE(result.has_value());
            test::requireErrorKind(
                result.error(),
                AutomationErrorKind::InvalidResource
            );
        }
    }

    TEST_CASE("similarity threshold uses checked inclusive integer SAD boundaries")
    {
        auto const minimum = SimilarityThreshold::create(0);
        auto const ninetyPercent = SimilarityThreshold::create(9'000);
        auto const maximum = SimilarityThreshold::create(10'000);
        REQUIRE(minimum.has_value());
        REQUIRE(ninetyPercent.has_value());
        REQUIRE(maximum.has_value());

        CHECK(minimum->maximumSad(2, 2) == 1'020);
        CHECK(ninetyPercent->maximumSad(2, 2) == 102);
        CHECK(maximum->maximumSad(2, 2) == 0);

        auto const outOfRange = SimilarityThreshold::create(10'001);
        REQUIRE_FALSE(outOfRange.has_value());
        test::requireErrorKind(
            outOfRange.error(),
            AutomationErrorKind::InvalidResource
        );

        auto const overflow = ninetyPercent->maximumSad(
            std::numeric_limits<uint32>::max(),
            std::numeric_limits<uint32>::max()
        );
        REQUIRE_FALSE(overflow.has_value());
        test::requireErrorKind(
            overflow.error(),
            AutomationErrorKind::InvalidResource
        );
    }

    TEST_CASE("recognition catalog closes page references and rejects duplicate signatures")
    {
        auto const projectFingerprint = test::fingerprint();
        auto const anchorId = test::recognizerId(g_anchorId);
        auto const actionId = test::recognizerId(g_actionId);
        auto const pageId = test::pageId(g_pageId);
        auto recognizers = std::vector<RecognizerDefinition>{};
        recognizers.emplace_back(
            test::recognizer(
                projectFingerprint,
                anchorId,
                "home_marker",
                AnnotationType::PageAnchor,
                test::pixelRect(0, 0, 1, 1),
                test::pixelRect(0, 0, 4, 4)
            )
        );
        recognizers.emplace_back(
            test::recognizer(
                projectFingerprint,
                actionId,
                "daily_button",
                AnnotationType::ActionTarget,
                test::pixelRect(1, 1, 1, 1),
                test::pixelRect(0, 0, 4, 4),
                {pageId}
            )
        );

        auto valid = RecognitionCatalog::create(
            test::projectId(),
            projectFingerprint,
            recognizers,
            {test::page(pageId, "home", {anchorId})}
        );
        REQUIRE(valid.has_value());
        REQUIRE(valid->pageAnchorOrder().size() == 1);
        CHECK(valid->pageAnchorOrder().front() == anchorId);

        auto duplicateSignature = RecognitionCatalog::create(
            test::projectId(),
            projectFingerprint,
            std::move(recognizers),
            {
                test::page(pageId, "home", {anchorId}),
                test::page(
                    test::pageId(g_secondPageId),
                    "home_copy",
                    {anchorId}
                ),
            }
        );
        REQUIRE_FALSE(duplicateSignature.has_value());
        test::requireErrorKind(
            duplicateSignature.error(),
            AutomationErrorKind::InvalidResource
        );
    }
}
