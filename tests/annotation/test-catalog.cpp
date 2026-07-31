#include "test-helpers.hpp"

#include <annotation/catalog.hpp>

#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
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
        constexpr auto k_anchorId = "00000000-0000-0000-0000-000000000001";
        constexpr auto k_actionId = "00000000-0000-0000-0000-000000000002";
        constexpr auto k_secondAnchorId = "00000000-0000-0000-0000-000000000003";
        constexpr auto k_unknownId = "00000000-0000-0000-0000-0000000000ff";
        constexpr auto k_pageId = "00000000-0000-0000-0000-000000000101";
        constexpr auto k_secondPageId = "00000000-0000-0000-0000-000000000102";
        constexpr auto k_unknownPageId = "00000000-0000-0000-0000-0000000001ff";

        // Deliberately bypasses test::element, which REQUIREs success and so
        // cannot observe a rejection.
        [[nodiscard]]
        auto anchorSpec() -> CompiledElementSpec
        {
            return CompiledElementSpec{
                .id           = test::elementId(k_anchorId),
                .name         = test::resourceName("home_marker"),
                .capabilities = test::capabilities(Identify{}),
                .searchRoi    = test::pixelRect(0, 0, 4, 4),
                .appearances     = std::vector<CompiledAppearance>{
                    test::compiledAppearance("only", test::pixelRect(0, 0, 2, 2)),
                },
            };
        }

        struct InvalidElement final
        {
            std::string_view    expected{};
            CompiledElementSpec spec;
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

    TEST_CASE("resource identifiers construct verbatim from bytes and round-trip")
    {
        auto const rawBytes = std::array<std::byte, 16>{
            std::byte{0x01}, std::byte{0x23}, std::byte{0x45}, std::byte{0x67},
            std::byte{0x89}, std::byte{0xab}, std::byte{0xcd}, std::byte{0xef},
            std::byte{0x01}, std::byte{0x23}, std::byte{0x45}, std::byte{0x67},
            std::byte{0x89}, std::byte{0xab}, std::byte{0xcd}, std::byte{0xef},
        };
        auto const id = ResourceId::fromBytes(rawBytes);
        CHECK(id.toString() == "01234567-89ab-cdef-0123-456789abcdef");

        auto const reparsed = ResourceId::parse(id.toString());
        REQUIRE(reparsed.has_value());
        CHECK(*reparsed == id);

        auto const zeroBytes = std::array<std::byte, 16>{};
        auto const nil       = ResourceId::fromBytes(zeroBytes);
        CHECK(nil.toString() == "00000000-0000-0000-0000-000000000000");
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

    TEST_CASE("recognition catalog publishes the anchor scan order and refuses a twin signature")
    {
        auto const projectFingerprint = test::fingerprint();
        auto const anchorId = test::elementId(k_anchorId);
        auto const actionId = test::elementId(k_actionId);
        auto const pageId   = test::pageId(k_pageId);
        auto const anchor   = [&]
        {
            return test::element(
                projectFingerprint,
                anchorId,
                "home_marker",
                test::capabilities(Identify{}),
                test::pixelRect(0, 0, 4, 4),
                std::vector<CompiledAppearance>{
                    test::compiledAppearance("only", test::pixelRect(0, 0, 1, 1)),
                }
            );
        };
        auto const action = [&]
        {
            return test::element(
                projectFingerprint,
                actionId,
                "daily_button",
                test::capabilities(std::nullopt, Interact{}),
                test::pixelRect(0, 0, 4, 4),
                std::vector<CompiledAppearance>{
                    test::compiledAppearance("only", test::pixelRect(1, 1, 1, 1)),
                }
            );
        };

        auto elements = std::vector<CompiledElement>{};
        elements.emplace_back(anchor());
        elements.emplace_back(action());
        auto references = std::vector<PageReference>{};
        references.emplace_back(
            test::reference(pageId, anchorId, test::identifiesAs())
        );
        references.emplace_back(
            test::reference(pageId, actionId, test::interacts())
        );
        auto const valid = RecognitionCatalog::create(
            test::projectId(),
            projectFingerprint,
            std::move(elements),
            {test::page(pageId, "home")},
            std::move(references)
        );
        REQUIRE(valid.has_value());
        // Only the element a reference identifies with joins the per-frame anchor
        // scan; the one this page merely clicks does not.
        REQUIRE(valid->pageAnchorOrder().size() == 1);
        CHECK(valid->pageAnchorOrder().front() == anchorId);

        // Two pages whose derived signatures agree are indistinguishable from
        // any frame, so one of them could never be resolved. The second page
        // borrows the anchor, because two pages owning it would be refused
        // first and this case would stop testing signatures.
        auto const secondPageId = test::pageId(k_secondPageId);
        auto twinElements    = std::vector<CompiledElement>{};
        twinElements.emplace_back(anchor());
        auto twinReferences = std::vector<PageReference>{};
        twinReferences.emplace_back(
            test::reference(pageId, anchorId, test::identifiesAs())
        );
        twinReferences.emplace_back(
            test::reference(
                secondPageId,
                anchorId,
                test::identifiesAs(),
                Holding::Referenced
            )
        );
        auto const duplicateSignature = RecognitionCatalog::create(
            test::projectId(),
            projectFingerprint,
            std::move(twinElements),
            {
                test::page(pageId, "home"),
                test::page(secondPageId, "home_copy"),
            },
            std::move(twinReferences)
        );
        REQUIRE_FALSE(duplicateSignature.has_value());
        requireRejection(
            duplicateSignature.error(),
            "two pages have the same required and forbidden signature"
        );
    }

    TEST_CASE("an element definition rejects malformed search, appearance, and click geometry")
    {
        auto const projectFingerprint = test::fingerprint();
        auto const outsideTemplate    = TemplateOffset::create(3, 3, 4, 4);
        auto const insideTemplate     = TemplateOffset::create(1, 1, 2, 2);
        REQUIRE(outsideTemplate.has_value());
        REQUIRE(insideTemplate.has_value());

        auto invalid = std::vector<InvalidElement>{};
        {
            auto spec      = anchorSpec();
            spec.searchRoi = test::pixelRect(0, 0, 8, 8);
            invalid.emplace_back(
                "element search_roi must fit the project resolution",
                std::move(spec)
            );
        }
        {
            auto spec     = anchorSpec();
            spec.appearances = std::vector<CompiledAppearance>{
                test::compiledAppearance("only", test::pixelRect(3, 3, 4, 4)),
            };
            invalid.emplace_back(
                "template_rect must fit the project resolution",
                std::move(spec)
            );
        }
        {
            auto spec      = anchorSpec();
            spec.searchRoi = test::pixelRect(0, 0, 2, 2);
            spec.appearances  = std::vector<CompiledAppearance>{
                test::compiledAppearance("only", test::pixelRect(0, 0, 4, 4)),
            };
            invalid.emplace_back(
                "template must fit inside the element search_roi",
                std::move(spec)
            );
        }
        {
            auto spec     = anchorSpec();
            spec.appearances = std::vector<CompiledAppearance>{
                test::compiledAppearance("only", test::pixelRect(0, 0, 2, 2)),
                test::compiledAppearance("only", test::pixelRect(2, 2, 2, 2)),
            };
            invalid.emplace_back(
                "element appearance names must be unique",
                std::move(spec)
            );
        }
        {
            // A click offset is measured inside a template, so a rectangle the
            // page locates has nothing for it to be relative to.
            auto spec         = anchorSpec();
            spec.capabilities = test::capabilities(
                std::nullopt,
                Interact{.clickOffset = *insideTemplate}
            );
            spec.appearances = {};
            invalid.emplace_back(
                "an element with no appearances cannot define a click offset",
                std::move(spec)
            );
        }
        {
            // Every appearance has to be clickable at the same template-local
            // point, or which appearance matched would move the click.
            auto spec         = anchorSpec();
            spec.capabilities = test::capabilities(
                std::nullopt,
                Interact{.clickOffset = *outsideTemplate}
            );
            invalid.emplace_back(
                "click offset must be inside every appearance template",
                std::move(spec)
            );
        }

        for (auto const& entry : invalid)
        {
            INFO(entry.expected);
            auto const rejected = CompiledElement::create(
                projectFingerprint,
                entry.spec
            );
            REQUIRE_FALSE(rejected.has_value());
            requireRejection(rejected.error(), entry.expected);
        }
    }

    TEST_CASE("one page states one thing about one element")
    {
        // What a page says about an element used to be spread over a signature
        // vector and a placement list, which could disagree: the same element
        // could be named required twice, or required and forbidden at once. One
        // reference per (page, element) makes all of that unrepresentable, and
        // this is the guard that keeps it that way.
        auto const projectFingerprint = test::fingerprint();
        auto const anchorId = test::elementId(k_anchorId);
        auto const pageId   = test::pageId(k_pageId);
        auto elements    = std::vector<CompiledElement>{};
        elements.emplace_back(
            test::element(
                projectFingerprint,
                anchorId,
                "home_marker",
                test::capabilities(Identify{}),
                test::pixelRect(0, 0, 4, 4),
                std::vector<CompiledAppearance>{
                    test::compiledAppearance("only", test::pixelRect(0, 0, 1, 1)),
                }
            )
        );
        auto references = std::vector<PageReference>{};
        references.emplace_back(
            test::reference(pageId, anchorId, test::identifiesAs(SignatureRole::Required))
        );
        references.emplace_back(
            test::reference(
                pageId,
                anchorId,
                test::identifiesAs(SignatureRole::Forbidden),
                Holding::Referenced
            )
        );

        auto const rejected = RecognitionCatalog::create(
            test::projectId(),
            projectFingerprint,
            std::move(elements),
            {test::page(pageId, "home")},
            std::move(references)
        );
        REQUIRE_FALSE(rejected.has_value());
        requireRejection(
            rejected.error(),
            "a page references the same element twice"
        );
    }

    TEST_CASE("a page recognised only by what must be absent is still a page")
    {
        auto const projectFingerprint = test::fingerprint();
        auto const anchorId = test::elementId(k_anchorId);
        auto const pageId   = test::pageId(k_pageId);
        auto elements    = std::vector<CompiledElement>{};
        elements.emplace_back(
            test::element(
                projectFingerprint,
                anchorId,
                "home_marker",
                test::capabilities(Identify{}),
                test::pixelRect(0, 0, 4, 4),
                std::vector<CompiledAppearance>{
                    test::compiledAppearance("only", test::pixelRect(0, 0, 1, 1)),
                }
            )
        );
        auto references = std::vector<PageReference>{};
        references.emplace_back(
            test::reference(
                pageId,
                anchorId,
                test::identifiesAs(SignatureRole::Forbidden)
            )
        );

        auto const catalog = RecognitionCatalog::create(
            test::projectId(),
            projectFingerprint,
            std::move(elements),
            {test::page(pageId, "home")},
            std::move(references)
        );
        REQUIRE(catalog.has_value());
        auto const* p_page = catalog->findPage(pageId);
        REQUIRE(p_page != nullptr);
        CHECK(p_page->required().empty());
        REQUIRE(p_page->forbidden().size() == 1U);
        CHECK(p_page->forbidden().front() == anchorId);
    }

    TEST_CASE("recognition catalog rejects every cross-resource inconsistency")
    {
        auto const projectFingerprint = test::fingerprint();
        auto const anchorId           = test::elementId(k_anchorId);
        auto const secondAnchorId     = test::elementId(k_secondAnchorId);
        auto const unknownId          = test::elementId(k_unknownId);
        auto const pageId             = test::pageId(k_pageId);
        auto const secondPageId       = test::pageId(k_secondPageId);
        auto const unknownPageId      = test::pageId(k_unknownPageId);

        auto const anchor = [&](ElementId id, std::string name)
        {
            return test::element(
                projectFingerprint,
                id,
                std::move(name),
                test::capabilities(Identify{}),
                test::pixelRect(0, 0, 4, 4),
                std::vector<CompiledAppearance>{
                    test::compiledAppearance("only", test::pixelRect(0, 0, 1, 1)),
                }
            );
        };
        auto const identifies = [](PageId page, ElementId element)
        {
            return test::reference(page, element, test::identifiesAs());
        };
        auto const reject = [&](
            std::string_view expected,
            std::vector<CompiledElement> elements,
            std::vector<PageSpec> pages,
            std::vector<PageReference> references
        )
        {
            INFO(expected);
            auto const rejected = RecognitionCatalog::create(
                test::projectId(),
                projectFingerprint,
                std::move(elements),
                std::move(pages),
                std::move(references)
            );
            REQUIRE_FALSE(rejected.has_value());
            requireRejection(rejected.error(), expected);
        };

        reject(
            "element IDs must be unique",
            {anchor(anchorId, "home_marker"), anchor(anchorId, "away_marker")},
            {test::page(pageId, "home")},
            {identifies(pageId, anchorId)}
        );
        reject(
            "element names must be unique",
            {
                anchor(anchorId, "home_marker"),
                anchor(secondAnchorId, "home_marker"),
            },
            {test::page(pageId, "home")},
            {identifies(pageId, anchorId)}
        );
        reject(
            "page IDs must be unique",
            {
                anchor(anchorId, "home_marker"),
                anchor(secondAnchorId, "away_marker"),
            },
            {
                test::page(pageId, "home"),
                test::page(pageId, "away"),
            },
            {identifies(pageId, anchorId)}
        );
        reject(
            "page names must be unique",
            {
                anchor(anchorId, "home_marker"),
                anchor(secondAnchorId, "away_marker"),
            },
            {
                test::page(pageId, "home"),
                test::page(secondPageId, "home"),
            },
            {
                identifies(pageId, anchorId),
                identifies(secondPageId, secondAnchorId),
            }
        );
        reject(
            "resource IDs and names must be globally unique",
            {anchor(anchorId, "home")},
            {test::page(pageId, "home")},
            {identifies(pageId, anchorId)}
        );
        reject(
            "page reference names an unknown page",
            {anchor(anchorId, "home_marker")},
            {test::page(pageId, "home")},
            {
                identifies(pageId, anchorId),
                identifies(unknownPageId, anchorId),
            }
        );
        reject(
            "page reference names an unknown element",
            {anchor(anchorId, "home_marker")},
            {test::page(pageId, "home")},
            {
                identifies(pageId, anchorId),
                identifies(pageId, unknownId),
            }
        );
    }
}
