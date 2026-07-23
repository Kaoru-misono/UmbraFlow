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
#include <utility>
#include <variant>
#include <vector>

namespace uf::annotation
{
    namespace
    {
        constexpr auto g_anchorId = "00000000-0000-0000-0000-000000000001";
        constexpr auto g_actionId = "00000000-0000-0000-0000-000000000002";
        constexpr auto g_pageId = "00000000-0000-0000-0000-000000000101";

        struct AuthorizationFixture final
        {
            ProjectFingerprint m_fingerprint;
            RecognitionCatalog m_catalog;
            RecognizerId m_actionId;
            Frame m_frame;
            ResolvedPage m_resolvedPage;
            ObservationLease m_lease;
        };

        auto authorizationFixture() -> AuthorizationFixture
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
                SessionId{7},
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
                .m_fingerprint = projectFingerprint,
                .m_catalog = std::move(catalog),
                .m_actionId = actionId,
                .m_frame = std::move(frame),
                .m_resolvedPage = std::get<ResolvedPage>(std::move(*outcome)),
                .m_lease = *lease,
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
                fixture.m_frame.sessionId(),
                fixture.m_frame.targetGeneration(),
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
                .m_liveFingerprint = liveFingerprint,
                .m_sessionId = fixture.m_frame.sessionId(),
                .m_targetGeneration = fixture.m_frame.targetGeneration(),
                .m_frameId = fixture.m_frame.id(),
                .m_now = test::instantAt(MonotonicInstant::Duration{105}),
            };
        }
    }

    TEST_CASE("coordinate action requires same-frame page detection lease and fingerprint")
    {
        auto const fixture = authorizationFixture();
        auto const actionDetection = ActionDetection::create(
            fixture.m_catalog,
            fixture.m_actionId,
            detection(fixture, fixture.m_frame.id())
        );
        REQUIRE(actionDetection.has_value());

        auto const authorized = authorizeCoordinateAction(
            fixture.m_catalog,
            fixture.m_resolvedPage,
            *actionDetection,
            fixture.m_lease,
            delivery(fixture, fixture.m_fingerprint)
        );
        CHECK(authorized.has_value());

        auto const incompatibleFingerprint = test::fingerprint(5, 4);
        auto const incompatible = authorizeCoordinateAction(
            fixture.m_catalog,
            fixture.m_resolvedPage,
            *actionDetection,
            fixture.m_lease,
            delivery(fixture, incompatibleFingerprint)
        );
        REQUIRE_FALSE(incompatible.has_value());
        test::requireErrorKind(
            incompatible.error(),
            AutomationErrorKind::TargetCompatibilityUnverified
        );

        auto const staleDetection = ActionDetection::create(
            fixture.m_catalog,
            fixture.m_actionId,
            detection(fixture, FrameId{12})
        );
        REQUIRE(staleDetection.has_value());
        auto const stale = authorizeCoordinateAction(
            fixture.m_catalog,
            fixture.m_resolvedPage,
            *staleDetection,
            fixture.m_lease,
            delivery(fixture, fixture.m_fingerprint)
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
            fixture.m_catalog,
            fixture.m_actionId,
            detection(fixture, fixture.m_frame.id(), "other_button")
        );
        REQUIRE_FALSE(mismatched.has_value());
        test::requireErrorKind(
            mismatched.error(),
            AutomationErrorKind::ActionRejected
        );
    }
}
