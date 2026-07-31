#include "test-helpers.hpp"

#include <annotation/authorization.hpp>
#include <annotation/catalog.hpp>
#include <annotation/recognition.hpp>

#include <domain/detection.hpp>
#include <domain/error.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>
#include <domain/time.hpp>

#include <vision/sad.hpp>

#include <doctest/doctest.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uf::annotation
{
    namespace
    {
        constexpr auto k_anchorId = "00000000-0000-0000-0000-000000000001";
        constexpr auto k_actionId = "00000000-0000-0000-0000-000000000002";
        constexpr auto k_awayAnchorId = "00000000-0000-0000-0000-000000000003";
        constexpr auto k_pageId = "00000000-0000-0000-0000-000000000101";
        constexpr auto k_awayPageId = "00000000-0000-0000-0000-000000000102";

        // Pins each rejection to the branch it targets. Every guard below
        // returns ActionRejected, so the kind alone cannot tell them apart.
        auto requireActionRejected(
            Error const& error,
            std::string_view expected
        ) -> void
        {
            test::requireErrorKind(error, AutomationErrorKind::ActionRejected);
            CHECK(error.message().find(expected) != std::string_view::npos);
        }

        [[nodiscard]]
        auto anchorElement(
            ElementId id,
            std::string name,
            PixelRect templateRect
        ) -> CompiledElement
        {
            return test::element(
                test::fingerprint(),
                id,
                std::move(name),
                test::capabilities(Identify{}),
                test::pixelRect(0, 0, 4, 4),
                std::vector<CompiledAppearance>{
                    test::compiledAppearance("only", templateRect),
                }
            );
        }

        [[nodiscard]]
        auto interactiveElement(ElementId id, std::string name) -> CompiledElement
        {
            return test::element(
                test::fingerprint(),
                id,
                std::move(name),
                test::capabilities(std::nullopt, Interact{}),
                test::pixelRect(0, 0, 4, 4),
                std::vector<CompiledAppearance>{
                    test::compiledAppearance("only", test::pixelRect(1, 1, 1, 1)),
                }
            );
        }

        [[nodiscard]]
        auto hitEvaluation(CompiledElement const& element) -> AnchorEvaluation
        {
            auto result = AnchorEvaluation::fromSadOutcome(
                element,
                element.appearances().front(),
                element.searchRoi(),
                SadSearchOutcome{std::optional<SadMatch>{SadMatch{0, 0, 0}}}
            );
            REQUIRE(result.has_value());
            return *std::move(result);
        }

        [[nodiscard]]
        auto missEvaluation(CompiledElement const& element) -> AnchorEvaluation
        {
            auto result = AnchorEvaluation::fromSadOutcome(
                element,
                element.appearances().front(),
                element.searchRoi(),
                SadSearchOutcome{std::optional<SadMatch>{}}
            );
            REQUIRE(result.has_value());
            return *std::move(result);
        }

        struct AuthorizationFixture final
        {
            ProjectFingerprint fingerprint{test::fingerprint()};
            RecognitionCatalog catalog;
            ElementId actionId{test::elementId(k_actionId)};
            Frame            frame;
            ResolvedPage     resolvedPage;
            ObservationLease lease;
        };

        auto authorizationFixture() -> AuthorizationFixture
        {
            auto const projectFingerprint = test::fingerprint();
            auto const anchorId = test::elementId(k_anchorId);
            auto const actionId = test::elementId(k_actionId);
            auto const pageId = test::pageId(k_pageId);
            auto elements = std::vector<CompiledElement>{};
            elements.emplace_back(
                anchorElement(anchorId, "home_marker", test::pixelRect(0, 0, 1, 1))
            );
            elements.emplace_back(interactiveElement(actionId, "daily_button"));
            auto references = std::vector<PageReference>{};
            references.emplace_back(
                test::reference(pageId, anchorId, test::identifiesAs())
            );
            references.emplace_back(
                test::reference(pageId, actionId, test::interacts())
            );
            auto catalog = test::catalog(
                projectFingerprint,
                std::move(elements),
                {test::page(pageId, "home")},
                std::move(references)
            );
            auto const* p_anchor = catalog.findElement(anchorId);
            REQUIRE(p_anchor != nullptr);

            auto frame = test::frame(
                projectFingerprint,
                CaptureSessionId{7},
                TargetGeneration::fromValue(3),
                FrameId{11},
                test::instantAt(MonotonicInstant::Duration{100})
            );
            auto const evaluations = std::array{hitEvaluation(*p_anchor)};
            auto outcome = PageResolver::resolve(
                catalog,
                FrameIdentity::fromFrame(frame),
                evaluations
            );
            REQUIRE(outcome.has_value());
            REQUIRE(std::holds_alternative<ResolvedPage>(*outcome));
            auto lease = ObservationLease::forFrame(
                frame,
                MonotonicInstant::Duration{10}
            );
            REQUIRE(lease.has_value());
            return AuthorizationFixture{
                .fingerprint  = projectFingerprint,
                .catalog      = std::move(catalog),
                .actionId     = actionId,
                .frame        = std::move(frame),
                .resolvedPage = std::get<ResolvedPage>(std::move(*outcome)),
                .lease        = *lease,
            };
        }

        auto detection(
            AuthorizationFixture const& fixture,
            FrameId frameId,
            std::string label = "daily_button"
        ) -> Detection
        {
            auto parsedLabel = Label::create(std::move(label));
            REQUIRE(parsedLabel.has_value());
            return Detection{
                fixture.frame.sessionId(),
                fixture.frame.targetGeneration(),
                frameId,
                *std::move(parsedLabel),
                Rect<FrameSpace>{1.0F, 1.0F, 1.0F, 1.0F},
                1.0F
            };
        }

        auto delivery(
            AuthorizationFixture const& fixture,
            ProjectFingerprint liveFingerprint
        ) -> ActionDeliveryState
        {
            return ActionDeliveryState{
                .liveFingerprint  = liveFingerprint,
                .sessionId        = fixture.frame.sessionId(),
                .targetGeneration = fixture.frame.targetGeneration(),
                .frameId          = fixture.frame.id(),
                .now              = test::instantAt(MonotonicInstant::Duration{105}),
            };
        }
    }

    TEST_CASE("coordinate action requires same-frame page detection lease and fingerprint")
    {
        auto const fixture = authorizationFixture();
        auto const actionDetection = ActionDetection::create(
            fixture.catalog,
            fixture.actionId,
            detection(fixture, fixture.frame.id())
        );
        REQUIRE(actionDetection.has_value());

        auto const authorized = authorizeCoordinateAction(
            fixture.catalog,
            fixture.resolvedPage,
            *actionDetection,
            fixture.lease,
            delivery(fixture, fixture.fingerprint)
        );
        CHECK(authorized.has_value());

        auto const incompatibleFingerprint = test::fingerprint(5, 4);
        auto const incompatible = authorizeCoordinateAction(
            fixture.catalog,
            fixture.resolvedPage,
            *actionDetection,
            fixture.lease,
            delivery(fixture, incompatibleFingerprint)
        );
        REQUIRE_FALSE(incompatible.has_value());
        test::requireErrorKind(
            incompatible.error(),
            AutomationErrorKind::TargetCompatibilityUnverified
        );

        auto const staleDetection = ActionDetection::create(
            fixture.catalog,
            fixture.actionId,
            detection(fixture, FrameId{12})
        );
        REQUIRE(staleDetection.has_value());
        auto const stale = authorizeCoordinateAction(
            fixture.catalog,
            fixture.resolvedPage,
            *staleDetection,
            fixture.lease,
            delivery(fixture, fixture.fingerprint)
        );
        REQUIRE_FALSE(stale.has_value());
        test::requireErrorKind(
            stale.error(),
            AutomationErrorKind::StaleObservation
        );
    }

    TEST_CASE("action detection retains element identity instead of trusting its label")
    {
        auto const fixture = authorizationFixture();
        auto const mismatched = ActionDetection::create(
            fixture.catalog,
            fixture.actionId,
            detection(fixture, fixture.frame.id(), "other_button")
        );
        REQUIRE_FALSE(mismatched.has_value());
        requireActionRejected(
            mismatched.error(),
            "detection label does not match its bound element identity"
        );

        // The anchor is a real catalog element under a real name; what it does
        // not declare is interact, and there is nothing else to deliver to.
        auto const anchorBound = ActionDetection::create(
            fixture.catalog,
            test::elementId(k_anchorId),
            detection(fixture, fixture.frame.id(), "home_marker")
        );
        REQUIRE_FALSE(anchorBound.has_value());
        requireActionRejected(
            anchorBound.error(),
            "detection is not bound to an interactive catalog element"
        );
    }

    TEST_CASE("coordinate action refuses a page that does not exercise interact on the element")
    {
        auto const projectFingerprint = test::fingerprint();
        auto const homeAnchorId       = test::elementId(k_anchorId);
        auto const awayAnchorId       = test::elementId(k_awayAnchorId);
        auto const actionId           = test::elementId(k_actionId);
        auto const homePageId         = test::pageId(k_pageId);
        auto const awayPageId         = test::pageId(k_awayPageId);

        auto elements = std::vector<CompiledElement>{};
        elements.emplace_back(
            anchorElement(homeAnchorId, "home_marker", test::pixelRect(0, 0, 1, 1))
        );
        elements.emplace_back(
            anchorElement(awayAnchorId, "away_marker", test::pixelRect(2, 2, 1, 1))
        );
        elements.emplace_back(interactiveElement(actionId, "daily_button"));

        // Only the away page references the button for interaction, while the
        // frame resolves to home. The authorisation IS that missing reference.
        auto references = std::vector<PageReference>{};
        references.emplace_back(
            test::reference(homePageId, homeAnchorId, test::identifiesAs())
        );
        references.emplace_back(
            test::reference(awayPageId, awayAnchorId, test::identifiesAs())
        );
        references.emplace_back(
            test::reference(awayPageId, actionId, test::interacts())
        );
        auto catalog = test::catalog(
            projectFingerprint,
            std::move(elements),
            {
                test::page(homePageId, "home"),
                test::page(awayPageId, "away"),
            },
            std::move(references)
        );

        auto const* p_home = catalog.findElement(homeAnchorId);
        auto const* p_away = catalog.findElement(awayAnchorId);
        REQUIRE(p_home != nullptr);
        REQUIRE(p_away != nullptr);

        auto frame = test::frame(
            projectFingerprint,
            CaptureSessionId{7},
            TargetGeneration::fromValue(3),
            FrameId{11},
            test::instantAt(MonotonicInstant::Duration{100})
        );
        auto const evaluations = std::array{
            hitEvaluation(*p_home),
            missEvaluation(*p_away),
        };
        auto outcome = PageResolver::resolve(
            catalog,
            FrameIdentity::fromFrame(frame),
            evaluations
        );
        REQUIRE(outcome.has_value());
        REQUIRE(std::holds_alternative<ResolvedPage>(*outcome));
        auto const resolvedPage = std::get<ResolvedPage>(std::move(*outcome));
        REQUIRE(resolvedPage.pageId() == homePageId);

        auto const lease = ObservationLease::forFrame(
            frame,
            MonotonicInstant::Duration{10}
        );
        REQUIRE(lease.has_value());

        auto parsedLabel = Label::create("daily_button");
        REQUIRE(parsedLabel.has_value());
        auto const actionDetection = ActionDetection::create(
            catalog,
            actionId,
            Detection{
                frame.sessionId(),
                frame.targetGeneration(),
                frame.id(),
                *std::move(parsedLabel),
                Rect<FrameSpace>{1.0F, 1.0F, 1.0F, 1.0F},
                1.0F
            }
        );
        REQUIRE(actionDetection.has_value());

        auto const unauthorized = authorizeCoordinateAction(
            catalog,
            resolvedPage,
            *actionDetection,
            *lease,
            ActionDeliveryState{
                .liveFingerprint  = projectFingerprint,
                .sessionId        = frame.sessionId(),
                .targetGeneration = frame.targetGeneration(),
                .frameId          = frame.id(),
                .now              = test::instantAt(MonotonicInstant::Duration{105}),
            }
        );
        REQUIRE_FALSE(unauthorized.has_value());
        requireActionRejected(
            unauthorized.error(),
            "the resolved page does not exercise interact on this element"
        );
    }

    TEST_CASE("coordinate action requires the evidence to match the active catalog")
    {
        auto const fixture = authorizationFixture();
        auto const actionDetection = ActionDetection::create(
            fixture.catalog,
            fixture.actionId,
            detection(fixture, fixture.frame.id())
        );
        REQUIRE(actionDetection.has_value());

        auto const anchorId = test::elementId(k_anchorId);
        auto const pageId   = test::pageId(k_pageId);
        auto const anchorOnlyReferences = [&]
        {
            auto references = std::vector<PageReference>{};
            references.emplace_back(
                test::reference(pageId, anchorId, test::identifiesAs())
            );
            return references;
        };
        auto anchorOnly = std::vector<CompiledElement>{};
        anchorOnly.emplace_back(
            anchorElement(anchorId, "home_marker", test::pixelRect(0, 0, 1, 1))
        );

        // Same project identity, but the interactive element is gone.
        auto const withoutAction = test::catalog(
            fixture.fingerprint,
            anchorOnly,
            {test::page(pageId, "home")},
            anchorOnlyReferences()
        );
        auto const absent = authorizeCoordinateAction(
            withoutAction,
            fixture.resolvedPage,
            *actionDetection,
            fixture.lease,
            delivery(fixture, fixture.fingerprint)
        );
        REQUIRE_FALSE(absent.has_value());
        requireActionRejected(
            absent.error(),
            "page or interactive element is absent from the active catalog"
        );

        // Structurally identical catalog under a different project identity.
        auto foreign = RecognitionCatalog::create(
            test::projectId("personal.other"),
            fixture.fingerprint,
            std::move(anchorOnly),
            {test::page(pageId, "home")},
            anchorOnlyReferences()
        );
        REQUIRE(foreign.has_value());
        auto const mismatchedProject = authorizeCoordinateAction(
            *foreign,
            fixture.resolvedPage,
            *actionDetection,
            fixture.lease,
            delivery(fixture, fixture.fingerprint)
        );
        REQUIRE_FALSE(mismatchedProject.has_value());
        requireActionRejected(
            mismatchedProject.error(),
            "page and detection evidence must belong to the active annotation project"
        );
    }
}
