#include "test-helpers.hpp"

#include <annotation/catalog.hpp>

#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::annotation
{
    namespace
    {
        constexpr auto g_anchorId = "00000000-0000-0000-0000-000000000001";
        constexpr auto g_actionId = "00000000-0000-0000-0000-000000000002";
        constexpr auto g_secondAnchorId = "00000000-0000-0000-0000-000000000003";
        constexpr auto g_unknownId = "00000000-0000-0000-0000-0000000000ff";
        constexpr auto g_pageId = "00000000-0000-0000-0000-000000000101";
        constexpr auto g_secondPageId = "00000000-0000-0000-0000-000000000102";
        constexpr auto g_unknownPageId = "00000000-0000-0000-0000-0000000001ff";

        // Deliberately bypasses test::recognizer, which REQUIREs success and so
        // cannot observe a rejection.
        [[nodiscard]]
        auto anchorSpec() -> RecognizerSpec
        {
            return RecognizerSpec{
                .m_id             = test::recognizerId(g_anchorId),
                .m_name           = test::resourceName("home_marker"),
                .m_annotationType = AnnotationType::PageAnchor,
                .m_templateRect   = test::pixelRect(0, 0, 2, 2),
                .m_searchRoi      = test::pixelRect(0, 0, 4, 4),
                .m_threshold      = test::threshold(),
                .m_defaultClick   = std::nullopt,
                .m_allowedPageIds = {},
            };
        }

        struct InvalidRecognizer final
        {
            std::string_view m_expected;
            RecognizerSpec   m_spec;
        };

        // Pins each case to the branch it targets. Asserting only the error kind
        // would still pass if an earlier guard rejected the input first.
        auto requireRejection(
            Error const& error,
            std::string_view expected
        ) -> void
        {
            test::requireErrorKind(error, AutomationErrorKind::InvalidResource);
            CHECK(error.message().find(expected) != std::string_view::npos);
        }
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

    TEST_CASE("recognizer definition rejects malformed geometry and page membership")
    {
        auto const projectFingerprint = test::fingerprint();
        auto const pageId             = test::pageId(g_pageId);
        auto const outsideTemplate    = TemplateOffset::create(3, 3, 4, 4);
        REQUIRE(outsideTemplate.has_value());

        auto invalid = std::vector<InvalidRecognizer>{};
        {
            auto spec        = anchorSpec();
            spec.m_searchRoi = test::pixelRect(0, 0, 8, 8);
            invalid.emplace_back(
                "recognizer template_rect and search_roi must fit the project resolution",
                std::move(spec)
            );
        }
        {
            auto spec           = anchorSpec();
            spec.m_templateRect = test::pixelRect(0, 0, 4, 4);
            spec.m_searchRoi    = test::pixelRect(0, 0, 2, 2);
            invalid.emplace_back(
                "recognizer template dimensions must fit inside search_roi",
                std::move(spec)
            );
        }
        {
            auto spec           = anchorSpec();
            spec.m_defaultClick = *outsideTemplate;
            invalid.emplace_back(
                "only action_target recognizers may define a default click",
                std::move(spec)
            );
        }
        {
            auto spec             = anchorSpec();
            spec.m_annotationType = AnnotationType::ActionTarget;
            spec.m_allowedPageIds = {pageId};
            spec.m_defaultClick   = *outsideTemplate;
            invalid.emplace_back(
                "default click must be inside the recognizer template",
                std::move(spec)
            );
        }
        {
            auto spec             = anchorSpec();
            spec.m_allowedPageIds = {pageId};
            invalid.emplace_back(
                "page_anchor membership must be expressed by page signatures",
                std::move(spec)
            );
        }
        {
            auto spec             = anchorSpec();
            spec.m_annotationType = AnnotationType::ActionTarget;
            invalid.emplace_back(
                "action_target recognizer must authorize at least one page",
                std::move(spec)
            );
        }
        {
            auto spec             = anchorSpec();
            spec.m_annotationType = AnnotationType::ActionTarget;
            spec.m_allowedPageIds = {pageId, pageId};
            invalid.emplace_back(
                "recognizer contains duplicate allowed page IDs",
                std::move(spec)
            );
        }

        for (auto const& entry : invalid)
        {
            INFO(entry.m_expected);
            auto const rejected = RecognizerDefinition::create(
                projectFingerprint,
                entry.m_spec
            );
            REQUIRE_FALSE(rejected.has_value());
            requireRejection(rejected.error(), entry.m_expected);
        }
    }

    TEST_CASE("page signature rejects duplicate and contradictory recognizer sets")
    {
        auto const anchorId = test::recognizerId(g_anchorId);
        auto const pageId   = test::pageId(g_pageId);

        auto const duplicateRequired = PageSignature::create(
            PageSpec{
                .m_id        = pageId,
                .m_name      = test::resourceName("home"),
                .m_required  = {anchorId, anchorId},
                .m_forbidden = {},
            }
        );
        REQUIRE_FALSE(duplicateRequired.has_value());
        requireRejection(
            duplicateRequired.error(),
            "page signature contains duplicate recognizer IDs"
        );

        auto const overlapping = PageSignature::create(
            PageSpec{
                .m_id        = pageId,
                .m_name      = test::resourceName("home"),
                .m_required  = {anchorId},
                .m_forbidden = {anchorId},
            }
        );
        REQUIRE_FALSE(overlapping.has_value());
        requireRejection(
            overlapping.error(),
            "page required and forbidden recognizer sets overlap"
        );
    }

    TEST_CASE("page signature rejects an empty evidence signature")
    {
        auto const rejected = PageSignature::create(
            PageSpec{
                .m_id        = test::pageId(g_pageId),
                .m_name      = test::resourceName("home"),
                .m_required  = {},
                .m_forbidden = {},
            }
        );

        REQUIRE_FALSE(rejected.has_value());
        requireRejection(
            rejected.error(),
            "page signature must contain at least one required or forbidden recognizer"
        );
    }

    TEST_CASE("page signature accepts forbidden-only evidence")
    {
        auto const signature = PageSignature::create(
            PageSpec{
                .m_id        = test::pageId(g_pageId),
                .m_name      = test::resourceName("home"),
                .m_required  = {},
                .m_forbidden = {test::recognizerId(g_anchorId)},
            }
        );

        REQUIRE(signature.has_value());
    }

    TEST_CASE("recognition catalog rejects every cross-resource inconsistency")
    {
        auto const projectFingerprint = test::fingerprint();
        auto const anchorId           = test::recognizerId(g_anchorId);
        auto const secondAnchorId     = test::recognizerId(g_secondAnchorId);
        auto const actionId           = test::recognizerId(g_actionId);
        auto const unknownId          = test::recognizerId(g_unknownId);
        auto const pageId             = test::pageId(g_pageId);
        auto const secondPageId       = test::pageId(g_secondPageId);
        auto const unknownPageId      = test::pageId(g_unknownPageId);

        auto const anchor = [&](RecognizerId id, std::string name)
        {
            return test::recognizer(
                projectFingerprint,
                id,
                std::move(name),
                AnnotationType::PageAnchor,
                test::pixelRect(0, 0, 1, 1),
                test::pixelRect(0, 0, 4, 4)
            );
        };
        auto const action = [&](std::vector<PageId> allowedPageIds)
        {
            return test::recognizer(
                projectFingerprint,
                actionId,
                "daily_button",
                AnnotationType::ActionTarget,
                test::pixelRect(1, 1, 1, 1),
                test::pixelRect(0, 0, 4, 4),
                std::move(allowedPageIds)
            );
        };
        auto const reject = [&](
            std::string_view expected,
            std::vector<RecognizerDefinition> recognizers,
            std::vector<PageSignature> pages
        )
        {
            INFO(expected);
            auto const rejected = RecognitionCatalog::create(
                test::projectId(),
                projectFingerprint,
                std::move(recognizers),
                std::move(pages)
            );
            REQUIRE_FALSE(rejected.has_value());
            requireRejection(rejected.error(), expected);
        };

        reject(
            "recognizer IDs must be unique",
            {anchor(anchorId, "home_marker"), anchor(anchorId, "away_marker")},
            {test::page(pageId, "home", {anchorId})}
        );
        reject(
            "recognizer names must be unique",
            {
                anchor(anchorId, "home_marker"),
                anchor(secondAnchorId, "home_marker"),
            },
            {test::page(pageId, "home", {anchorId})}
        );
        reject(
            "page IDs must be unique",
            {
                anchor(anchorId, "home_marker"),
                anchor(secondAnchorId, "away_marker"),
            },
            {
                test::page(pageId, "home", {anchorId}),
                test::page(pageId, "away", {secondAnchorId}),
            }
        );
        reject(
            "page names must be unique",
            {
                anchor(anchorId, "home_marker"),
                anchor(secondAnchorId, "away_marker"),
            },
            {
                test::page(pageId, "home", {anchorId}),
                test::page(secondPageId, "home", {secondAnchorId}),
            }
        );
        reject(
            "resource IDs and names must be globally unique",
            {anchor(anchorId, "home")},
            {test::page(pageId, "home", {anchorId})}
        );
        reject(
            "page signatures may reference only existing page_anchor recognizers",
            {anchor(anchorId, "home_marker")},
            {test::page(pageId, "home", {anchorId, unknownId})}
        );
        reject(
            "page signatures may reference only existing page_anchor recognizers",
            {anchor(anchorId, "home_marker"), action({pageId})},
            {test::page(pageId, "home", {anchorId}, {actionId})}
        );
        reject(
            "recognizer allowed_page_ids contains an unknown page",
            {anchor(anchorId, "home_marker"), action({unknownPageId})},
            {test::page(pageId, "home", {anchorId})}
        );
    }
}
