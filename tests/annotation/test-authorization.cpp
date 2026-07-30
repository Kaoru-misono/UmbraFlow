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
            auto catalog = test::catalog(
                projectFingerprint,
                std::move(recognizers),
                {test::page(pageId, "home", {anchorId})}
            );
            auto const* p_anchor = catalog.findRecognizer(anchorId);
            REQUIRE(p_anchor != nullptr);
            auto const sadOutcome = SadSearchOutcome{
                std::optional<SadMatch>{SadMatch{0, 0, 0}}
            };
            auto const evaluation = AnchorEvaluation::fromSadOutcome(
                *p_anchor,
                sadOutcome
            );
            REQUIRE(evaluation.has_value());

            auto frame = test::frame(
                projectFingerprint,
                CaptureSessionId{7},
                TargetGeneration::fromValue(3),
                FrameId{11},
                test::instantAt(MonotonicInstant::Duration{100})
            );
            auto const evaluations = std::array{*evaluation};
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

    TEST_CASE("action detection retains recognizer identity instead of trusting its label")
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
            "detection label does not match its bound recognizer identity"
        );

        auto const anchorBound = ActionDetection::create(
            fixture.catalog,
            test::elementId(k_anchorId),
            detection(fixture, fixture.frame.id(), "home_marker")
        );
        REQUIRE_FALSE(anchorBound.has_value());
        requireActionRejected(
            anchorBound.error(),
            "detection is not bound to a catalog action_target"
        );
    }

    TEST_CASE("coordinate action refuses a page the recognizer does not authorize")
    {
        auto const projectFingerprint = test::fingerprint();
        auto const homeAnchorId       = test::elementId(k_anchorId);
        auto const awayAnchorId       = test::elementId(k_awayAnchorId);
        auto const actionId           = test::elementId(k_actionId);
        auto const homePageId         = test::pageId(k_pageId);
        auto const awayPageId         = test::pageId(k_awayPageId);

        auto recognizers = std::vector<RecognizerDefinition>{};
        recognizers.emplace_back(
            test::recognizer(
                projectFingerprint,
                homeAnchorId,
                "home_marker",
                AnnotationType::PageAnchor,
                test::pixelRect(0, 0, 1, 1),
                test::pixelRect(0, 0, 4, 4)
            )
        );
        recognizers.emplace_back(
            test::recognizer(
                projectFingerprint,
                awayAnchorId,
                "away_marker",
                AnnotationType::PageAnchor,
                test::pixelRect(2, 2, 1, 1),
                test::pixelRect(0, 0, 4, 4)
            )
        );
        // Authorizes the away page only, while the frame resolves to home.
        recognizers.emplace_back(
            test::recognizer(
                projectFingerprint,
                actionId,
                "daily_button",
                AnnotationType::ActionTarget,
                test::pixelRect(1, 1, 1, 1),
                test::pixelRect(0, 0, 4, 4),
                {awayPageId}
            )
        );
        auto catalog = test::catalog(
            projectFingerprint,
            std::move(recognizers),
            {
                test::page(homePageId, "home", {homeAnchorId}),
                test::page(awayPageId, "away", {awayAnchorId}),
            }
        );

        auto const* p_home = catalog.findRecognizer(homeAnchorId);
        auto const* p_away = catalog.findRecognizer(awayAnchorId);
        REQUIRE(p_home != nullptr);
        REQUIRE(p_away != nullptr);
        auto const present = AnchorEvaluation::fromSadOutcome(
            *p_home,
            SadSearchOutcome{std::optional<SadMatch>{SadMatch{0, 0, 0}}}
        );
        auto const absent = AnchorEvaluation::fromSadOutcome(
            *p_away,
            SadSearchOutcome{std::optional<SadMatch>{}}
        );
        REQUIRE(present.has_value());
        REQUIRE(absent.has_value());

        auto frame = test::frame(
            projectFingerprint,
            CaptureSessionId{7},
            TargetGeneration::fromValue(3),
            FrameId{11},
            test::instantAt(MonotonicInstant::Duration{100})
        );
        auto const evaluations = std::array{*present, *absent};
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
            "action recognizer does not authorize the resolved page"
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
        auto anchorOnly     = std::vector<RecognizerDefinition>{};
        anchorOnly.emplace_back(
            test::recognizer(
                fixture.fingerprint,
                anchorId,
                "home_marker",
                AnnotationType::PageAnchor,
                test::pixelRect(0, 0, 1, 1),
                test::pixelRect(0, 0, 4, 4)
            )
        );

        // Same project identity, but the action recognizer is gone.
        auto const withoutAction = test::catalog(
            fixture.fingerprint,
            anchorOnly,
            {test::page(pageId, "home", {anchorId})}
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
            "page or action recognizer is absent from the active catalog"
        );

        // Structurally identical catalog under a different project identity.
        auto foreign = RecognitionCatalog::create(
            test::projectId("personal.other"),
            fixture.fingerprint,
            std::move(anchorOnly),
            {test::page(pageId, "home", {anchorId})}
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
