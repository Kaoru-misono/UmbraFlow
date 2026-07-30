#include "test-helpers.hpp"

#include <annotation/capabilities.hpp>
#include <annotation/catalog.hpp>

#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <array>
#include <optional>
#include <string_view>

namespace uf::annotation
{
    namespace
    {
        constexpr auto k_elementId = "00000000-0000-0000-0000-000000000201";
        constexpr auto k_pageId    = "00000000-0000-0000-0000-000000000202";

        // Which of the three capabilities one case turns on. The subset rule has
        // to hold over every combination on both sides, and spelling out
        // fourteen optionals per case would bury the property being checked.
        struct CapabilitySet final
        {
            bool identify{};
            bool interact{};
            bool read{};
        };

        constexpr auto k_nonEmptySets = std::array{
            CapabilitySet{.identify = true},
            CapabilitySet{.interact = true},
            CapabilitySet{.read = true},
            CapabilitySet{.identify = true, .interact = true},
            CapabilitySet{.identify = true, .read = true},
            CapabilitySet{.interact = true, .read = true},
            CapabilitySet{.identify = true, .interact = true, .read = true},
        };

        auto declaredFrom(CapabilitySet set) -> ElementCapabilities
        {
            auto identify = std::optional<Identify>{};
            auto interact = std::optional<Interact>{};
            auto read     = std::optional<Read>{};
            if (set.identify)
            {
                identify = Identify{};
            }
            if (set.interact)
            {
                interact = Interact{};
            }
            if (set.read)
            {
                read = Read{};
            }

            auto const result = ElementCapabilities::create(identify, interact, read);
            REQUIRE(result.has_value());
            return *result;
        }

        auto exercisedFrom(CapabilitySet set) -> ExercisedCapabilities
        {
            auto identify = std::optional<ExercisedIdentify>{};
            auto interact = std::optional<ExercisedInteract>{};
            auto read     = std::optional<ExercisedRead>{};
            if (set.identify)
            {
                identify = ExercisedIdentify{};
            }
            if (set.interact)
            {
                interact = ExercisedInteract{};
            }
            if (set.read)
            {
                read = ExercisedRead{};
            }

            auto const result = ExercisedCapabilities::create(identify, interact, read);
            REQUIRE(result.has_value());
            return *result;
        }
    }

    TEST_CASE("each annotation type reads as exactly one capability")
    {
        auto const anchor = ElementCapabilities::fromAnnotationType(
            AnnotationType::PageAnchor,
            std::nullopt
        );
        CHECK(anchor.hasIdentify());
        CHECK_FALSE(anchor.hasInteract());
        CHECK_FALSE(anchor.hasRead());

        auto const clickOffset = TemplateOffset::create(1, 2, 4, 4);
        REQUIRE(clickOffset.has_value());
        auto const target = ElementCapabilities::fromAnnotationType(
            AnnotationType::ActionTarget,
            *clickOffset
        );
        CHECK_FALSE(target.hasIdentify());
        REQUIRE(target.hasInteract());
        CHECK_FALSE(target.hasRead());
        REQUIRE(target.interact()->clickOffset.has_value());
        CHECK(*target.interact()->clickOffset == *clickOffset);

        auto const info = ElementCapabilities::fromAnnotationType(
            AnnotationType::InfoRegion,
            std::nullopt
        );
        CHECK_FALSE(info.hasIdentify());
        CHECK_FALSE(info.hasInteract());
        CHECK(info.hasRead());
    }

    TEST_CASE("an element no capability can reach is refused at construction")
    {
        auto const empty = ElementCapabilities::create(
            std::nullopt,
            std::nullopt,
            std::nullopt
        );
        REQUIRE_FALSE(empty.has_value());
        test::requireErrorKind(
            empty.error(),
            AutomationErrorKind::InvalidResource
        );
        CHECK(
            empty.error().message().find("at least one of identify, interact, or read")
            != std::string_view::npos
        );
    }

    TEST_CASE("every non-empty capability combination is accepted and read back")
    {
        struct Combination final
        {
            std::optional<Identify> identify{};
            std::optional<Interact> interact{};
            std::optional<Read>     read{};
        };

        auto const combinations = std::array{
            Combination{.identify = Identify{}},
            Combination{.interact = Interact{}},
            Combination{.read = Read{}},
            Combination{.identify = Identify{}, .interact = Interact{}},
            Combination{.identify = Identify{}, .read = Read{}},
            Combination{.interact = Interact{}, .read = Read{}},
        };

        for (auto const& combination : combinations)
        {
            auto const capabilities = ElementCapabilities::create(
                combination.identify,
                combination.interact,
                combination.read
            );
            REQUIRE(capabilities.has_value());
            CHECK(capabilities->hasIdentify() == combination.identify.has_value());
            CHECK(capabilities->hasInteract() == combination.interact.has_value());
            CHECK(capabilities->hasRead() == combination.read.has_value());
        }
    }

    TEST_CASE("read parameters default to a single line with no charset restriction")
    {
        auto const defaulted = Read{};
        CHECK(defaulted.layout == ReadLayout::SingleLine);
        CHECK_FALSE(defaulted.charset.has_value());

        auto const restricted = Read{
            .layout  = ReadLayout::Block,
            .charset = CharsetRestriction::Digits,
        };
        CHECK(restricted.layout == ReadLayout::Block);
        REQUIRE(restricted.charset.has_value());
        CHECK(*restricted.charset == CharsetRestriction::Digits);
        CHECK_FALSE(restricted == defaulted);
    }

    TEST_CASE("a page reference that exercises nothing is refused at construction")
    {
        auto const empty = ExercisedCapabilities::create(
            std::nullopt,
            std::nullopt,
            std::nullopt
        );
        REQUIRE_FALSE(empty.has_value());
        test::requireErrorKind(
            empty.error(),
            AutomationErrorKind::InvalidResource
        );
        CHECK(
            empty.error().message().find("at least one of identify, interact, or read")
            != std::string_view::npos
        );
    }

    TEST_CASE("a page reference records whether it is evidence for or against the page")
    {
        auto const roles = std::array{
            SignatureRole::Required,
            SignatureRole::Forbidden,
        };

        for (auto const role : roles)
        {
            auto const exercised = ExercisedCapabilities::create(
                ExercisedIdentify{.role = role},
                std::nullopt,
                std::nullopt
            );
            REQUIRE(exercised.has_value());
            REQUIRE(exercised->hasIdentify());
            CHECK(exercised->identify()->role == role);
        }
    }

    TEST_CASE("a page reference may exercise only what its element declares")
    {
        // Spelled out rather than derived, so that at least one arm of this case
        // cannot agree with a wrong implementation by sharing its formula.
        auto const identifyOnly = exercisedFrom({.identify = true});
        CHECK(identifyOnly.isSubsetOf(declaredFrom({.identify = true})));
        CHECK_FALSE(identifyOnly.isSubsetOf(declaredFrom({.interact = true})));
        CHECK_FALSE(identifyOnly.isSubsetOf(declaredFrom({.read = true})));

        auto const interactOnly = exercisedFrom({.interact = true});
        CHECK(interactOnly.isSubsetOf(declaredFrom({.interact = true})));
        CHECK_FALSE(interactOnly.isSubsetOf(declaredFrom({.identify = true})));
        CHECK_FALSE(interactOnly.isSubsetOf(declaredFrom({.read = true})));

        auto const readOnly = exercisedFrom({.read = true});
        CHECK(readOnly.isSubsetOf(declaredFrom({.read = true})));
        CHECK_FALSE(readOnly.isSubsetOf(declaredFrom({.identify = true})));
        CHECK_FALSE(readOnly.isSubsetOf(declaredFrom({.interact = true})));

        // The pair that motivated the whole split: one patch of pixels that
        // names its page and can be clicked, used both ways on one page and
        // only clicked on another.
        auto const identifyAndInteract = exercisedFrom({
            .identify = true,
            .interact = true,
        });
        CHECK(
            identifyAndInteract.isSubsetOf(
                declaredFrom({.identify = true, .interact = true})
            )
        );
        CHECK(
            identifyAndInteract.isSubsetOf(
                declaredFrom({.identify = true, .interact = true, .read = true})
            )
        );
        CHECK_FALSE(identifyAndInteract.isSubsetOf(declaredFrom({.interact = true})));
        CHECK_FALSE(identifyAndInteract.isSubsetOf(declaredFrom({.identify = true})));

        // And then every pairing of the two sides, so no combination is left to
        // the three cases above to stand in for.
        for (auto const declaredSet : k_nonEmptySets)
        {
            auto const declared = declaredFrom(declaredSet);

            for (auto const exercisedSet : k_nonEmptySets)
            {
                auto const exercised = exercisedFrom(exercisedSet);
                auto const expected  = !(
                    (exercisedSet.identify && !declaredSet.identify)
                    || (exercisedSet.interact && !declaredSet.interact)
                    || (exercisedSet.read && !declaredSet.read)
                );

                CAPTURE(declaredSet.identify);
                CAPTURE(declaredSet.interact);
                CAPTURE(declaredSet.read);
                CAPTURE(exercisedSet.identify);
                CAPTURE(exercisedSet.interact);
                CAPTURE(exercisedSet.read);
                CHECK(exercised.isSubsetOf(declared) == expected);
            }
        }
    }

    TEST_CASE("a recognizer definition derives its capability from its annotation type")
    {
        auto const projectFingerprint = test::fingerprint();
        auto const elementId          = test::recognizerId(k_elementId);
        auto const pageId             = test::pageId(k_pageId);
        auto const templateRect       = test::pixelRect(0, 0, 2, 2);
        auto const searchRoi          = test::pixelRect(0, 0, 4, 4);

        auto const anchor = test::recognizer(
            projectFingerprint,
            elementId,
            "home_marker",
            AnnotationType::PageAnchor,
            templateRect,
            searchRoi
        );
        CHECK(anchor.capabilities().hasIdentify());
        CHECK_FALSE(anchor.capabilities().hasInteract());
        CHECK_FALSE(anchor.capabilities().hasRead());

        auto const clickOffset = TemplateOffset::create(1, 1, 2, 2);
        REQUIRE(clickOffset.has_value());
        auto const target = test::recognizer(
            projectFingerprint,
            elementId,
            "daily_button",
            AnnotationType::ActionTarget,
            templateRect,
            searchRoi,
            {pageId},
            *clickOffset
        );
        auto const targetCapabilities = target.capabilities();
        CHECK_FALSE(targetCapabilities.hasIdentify());
        REQUIRE(targetCapabilities.hasInteract());
        CHECK_FALSE(targetCapabilities.hasRead());
        REQUIRE(targetCapabilities.interact()->clickOffset.has_value());
        CHECK(*targetCapabilities.interact()->clickOffset == *clickOffset);

        auto const info = test::recognizer(
            projectFingerprint,
            elementId,
            "level_value",
            AnnotationType::InfoRegion,
            templateRect,
            searchRoi
        );
        CHECK_FALSE(info.capabilities().hasIdentify());
        CHECK_FALSE(info.capabilities().hasInteract());
        CHECK(info.capabilities().hasRead());

        // The derivation stays a reading of the stored type, not a second fact
        // alongside it.
        CHECK(
            info.capabilities()
            == ElementCapabilities::fromAnnotationType(
                info.annotationType(),
                info.defaultClick()
            )
        );
    }
}
