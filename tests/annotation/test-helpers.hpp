#pragma once

#include <annotation/authoring-document.hpp>

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

    inline auto recognizerId(std::string_view value) -> RecognizerId
    {
        return RecognizerId{resourceId(value)};
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

    inline auto recognizer(
        ProjectFingerprint projectFingerprint,
        RecognizerId id,
        std::string name,
        AnnotationType annotationType,
        PixelRect templateRect,
        PixelRect searchRoi,
        std::vector<PageId> allowedPageIds         = {},
        std::optional<TemplateOffset> defaultClick = std::nullopt,
        SimilarityThreshold similarityThreshold    = threshold()
    ) -> RecognizerDefinition
    {
        auto result = RecognizerDefinition::create(
            projectFingerprint,
            RecognizerSpec{
                .m_id             = id,
                .m_name           = resourceName(std::move(name)),
                .m_annotationType = annotationType,
                .m_templateRect   = templateRect,
                .m_searchRoi      = searchRoi,
                .m_threshold      = similarityThreshold,
                .m_defaultClick   = defaultClick,
                .m_allowedPageIds = std::move(allowedPageIds),
            }
        );
        REQUIRE(result.has_value());
        return *std::move(result);
    }

    inline auto page(
        PageId id,
        std::string name,
        std::vector<RecognizerId> required,
        std::vector<RecognizerId> forbidden = {}
    ) -> PageSignature
    {
        auto result = PageSignature::create(
            PageSpec{
                .m_id        = id,
                .m_name      = resourceName(std::move(name)),
                .m_required  = std::move(required),
                .m_forbidden = std::move(forbidden),
            }
        );
        REQUIRE(result.has_value());
        return *std::move(result);
    }

    inline auto catalog(
        ProjectFingerprint projectFingerprint,
        std::vector<RecognizerDefinition> recognizers,
        std::vector<PageSignature> pages
    ) -> RecognitionCatalog
    {
        auto result = RecognitionCatalog::create(
            projectId(),
            projectFingerprint,
            std::move(recognizers),
            std::move(pages)
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
        SessionId sessionId,
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
        auto const stride = checkedMultiply(*width, std::size_t{4});
        REQUIRE(stride.has_value());
        auto const length = checkedMultiply(*stride, *height);
        REQUIRE(length.has_value());
        auto const pixels = std::shared_ptr<FrameBuffer const>{
            std::make_shared<FrameBuffer>(
                std::vector<std::byte>(*length)
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
