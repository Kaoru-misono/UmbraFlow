#pragma once

#include <annotation/authoring-document.hpp>
#include <annotation/capabilities.hpp>
#include <annotation/catalog.hpp>
#include <annotation/resource.hpp>
#include <annotation/appearance.hpp>

#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/time.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::annotation::test
{
    inline auto resourceId(std::string_view value) -> ResourceId
    {
        auto const result = ResourceId::parse(value);
        REQUIRE(result.has_value());
        return *result;
    }

    inline auto elementId(std::string_view value) -> ElementId
    {
        return ElementId{resourceId(value)};
    }

    inline auto sourceId(std::string_view value) -> SourceId
    {
        return SourceId{resourceId(value)};
    }

    inline auto pageId(std::string_view value) -> PageId
    {
        return PageId{resourceId(value)};
    }

    inline auto regressionId(std::string_view value) -> RegressionId
    {
        return RegressionId{resourceId(value)};
    }

    inline auto projectId(std::string value = "personal.test") -> ProjectId
    {
        auto result = ProjectId::create(std::move(value));
        REQUIRE(result.has_value());
        return *std::move(result);
    }

    inline auto resourceName(std::string value) -> ResourceName
    {
        auto result = ResourceName::create(std::move(value));
        REQUIRE(result.has_value());
        return *std::move(result);
    }

    inline auto fingerprint(
        uint32 width  = 4,
        uint32 height = 4,
        uint32 dpiX   = 96,
        uint32 dpiY   = 96
    ) -> ProjectFingerprint
    {
        auto const result = ProjectFingerprint::create(width, height, dpiX, dpiY);
        REQUIRE(result.has_value());
        return *result;
    }

    inline auto pixelRect(
        uint32 x,
        uint32 y,
        uint32 width,
        uint32 height
    ) -> PixelRect
    {
        auto const result = PixelRect::create(x, y, width, height);
        REQUIRE(result.has_value());
        return *result;
    }

    inline auto threshold(uint32 basisPoints = 9'000) -> SimilarityThreshold
    {
        auto const result = SimilarityThreshold::create(basisPoints);
        REQUIRE(result.has_value());
        return *result;
    }

    inline auto templateOffset(
        uint32 x,
        uint32 y,
        uint32 templateWidth,
        uint32 templateHeight
    ) -> TemplateOffset
    {
        auto const result = TemplateOffset::create(
            x,
            y,
            templateWidth,
            templateHeight
        );
        REQUIRE(result.has_value());
        return *result;
    }

    // What an element declares it can be used for.
    inline auto capabilities(
        std::optional<Identify> identify = std::nullopt,
        std::optional<Interact> interact = std::nullopt,
        std::optional<Read> read         = std::nullopt
    ) -> ElementCapabilities
    {
        auto const result = ElementCapabilities::create(identify, interact, read);
        REQUIRE(result.has_value());
        return *result;
    }

    // What one page's reference does with it. The subset of the above that this
    // page actually uses.
    inline auto exercised(
        std::optional<ExercisedIdentify> identify = std::nullopt,
        std::optional<ExercisedInteract> interact = std::nullopt,
        std::optional<ExercisedRead> read         = std::nullopt
    ) -> ExercisedCapabilities
    {
        auto const result = ExercisedCapabilities::create(identify, interact, read);
        REQUIRE(result.has_value());
        return *result;
    }

    inline auto identifiesAs(
        SignatureRole role = SignatureRole::Required
    ) -> ExercisedCapabilities
    {
        return exercised(ExercisedIdentify{.role = role});
    }

    inline auto interacts() -> ExercisedCapabilities
    {
        return exercised(std::nullopt, ExercisedInteract{});
    }

    inline auto appearance(
        std::string name,
        SourceId appearanceSourceId,
        PixelRect templateRect,
        SimilarityThreshold similarityThreshold = threshold(),
        std::optional<ColourKey> colourKey      = std::nullopt
    ) -> Appearance
    {
        auto result = Appearance::create(
            Appearance::Spec{
                .name         = resourceName(std::move(name)),
                .sourceId     = appearanceSourceId,
                .templateRect = templateRect,
                .threshold    = similarityThreshold,
                .colourKey    = colourKey,
            }
        );
        REQUIRE(result.has_value());
        return *std::move(result);
    }

    inline auto compiledAppearance(
        std::string name,
        PixelRect templateRect,
        SimilarityThreshold similarityThreshold = threshold()
    ) -> CompiledAppearance
    {
        return CompiledAppearance{
            .name         = resourceName(std::move(name)),
            .templateRect = templateRect,
            .threshold    = similarityThreshold,
        };
    }

    inline auto element(
        ProjectFingerprint projectFingerprint,
        ElementId id,
        std::string name,
        ElementCapabilities elementCapabilities,
        PixelRect searchRoi,
        std::vector<Appearance> appearances = {}
    ) -> Element
    {
        auto result = Element::create(
            projectFingerprint,
            Element::Spec{
                .id           = id,
                .name         = resourceName(std::move(name)),
                .capabilities = std::move(elementCapabilities),
                .searchRoi    = searchRoi,
                .appearances  = std::move(appearances),
            }
        );
        REQUIRE(result.has_value());
        return *std::move(result);
    }

    inline auto element(
        ProjectFingerprint projectFingerprint,
        ElementId id,
        std::string name,
        ElementCapabilities elementCapabilities,
        PixelRect searchRoi,
        std::vector<CompiledAppearance> appearances = {}
    ) -> CompiledElement
    {
        auto result = CompiledElement::create(
            projectFingerprint,
            CompiledElementSpec{
                .id           = id,
                .name         = resourceName(std::move(name)),
                .capabilities = std::move(elementCapabilities),
                .searchRoi    = searchRoi,
                .appearances  = std::move(appearances),
            }
        );
        REQUIRE(result.has_value());
        return *std::move(result);
    }

    inline auto page(PageId id, std::string name) -> PageSpec
    {
        return PageSpec{
            .id   = id,
            .name = resourceName(std::move(name)),
        };
    }

    inline auto reference(
        PageId referencedPageId,
        ElementId referencedElementId,
        ExercisedCapabilities exercisedCapabilities,
        Holding holding                          = Holding::Owned,
        std::optional<PixelRect> searchRoi        = std::nullopt,
        std::optional<ResourceName> pinnedAppearance = std::nullopt
    ) -> PageReference
    {
        return PageReference{
            .pageId     = referencedPageId,
            .elementId  = referencedElementId,
            .holding    = holding,
            .exercised  = std::move(exercisedCapabilities),
            .searchRoi  = searchRoi,
            .appearance = std::move(pinnedAppearance),
        };
    }

    inline auto catalog(
        ProjectFingerprint projectFingerprint,
        std::vector<CompiledElement> elements,
        std::vector<PageSpec> pages,
        std::vector<PageReference> references
    ) -> RecognitionCatalog
    {
        auto result = RecognitionCatalog::create(
            projectId(),
            projectFingerprint,
            std::move(elements),
            std::move(pages),
            std::move(references)
        );
        REQUIRE(result.has_value());
        return *std::move(result);
    }

    inline auto requireErrorKind(
        Error const& error,
        AutomationErrorKind expected
    ) -> void
    {
        auto const kind = automationErrorKind(error);
        REQUIRE(kind.has_value());
        CHECK(*kind == expected);
    }

    inline auto instantAt(MonotonicInstant::Duration duration) -> MonotonicInstant
    {
        return MonotonicInstant::fromTimePoint(
            MonotonicInstant::TimePoint{duration}
        );
    }

    inline auto frame(
        ProjectFingerprint projectFingerprint,
        CaptureSessionId sessionId,
        TargetGeneration generation,
        FrameId frameId,
        MonotonicInstant capturedAt
    ) -> Frame
    {
        auto const transform = CoordinateTransform::create(
            Point<DesktopSpace>{0.0F, 0.0F},
            static_cast<float>(projectFingerprint.width()),
            static_cast<float>(projectFingerprint.height()),
            projectFingerprint.width(),
            projectFingerprint.height()
        );
        REQUIRE(transform.has_value());

        auto const width  = checkedCast<std::size_t>(projectFingerprint.width());
        auto const height = checkedCast<std::size_t>(projectFingerprint.height());
        REQUIRE(width.has_value());
        REQUIRE(height.has_value());
        auto const stride = checkedMultiply(
            width.value_or(std::size_t{0}),
            std::size_t{4}
        );
        REQUIRE(stride.has_value());
        auto const length = checkedMultiply(
            stride.value_or(std::size_t{0}),
            height.value_or(std::size_t{0})
        );
        REQUIRE(length.has_value());
        auto const pixels = std::shared_ptr<FrameBuffer const>{
            std::make_shared<FrameBuffer>(
                std::vector<std::byte>(length.value_or(std::size_t{0}))
            )
        };
        auto result = Frame::create(
            frameId,
            sessionId,
            generation,
            capturedAt,
            projectFingerprint.width(),
            projectFingerprint.height(),
            *stride,
            PixelFormat::Bgra8,
            pixels,
            *transform
        );
        REQUIRE(result.has_value());
        return *std::move(result);
    }
}
