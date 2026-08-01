#include "template-match.hpp"

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/space.hpp>

#include <image/pixels.hpp>
#include <image/png.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uf
{
    namespace
    {
        [[nodiscard]]
        auto invalidTemplate(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        [[nodiscard]]
        auto incompatibleFrame(std::string message) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::TargetCompatibilityUnverified,
                std::move(message)
            );
        }
    }

    auto searchStopKind(SadSearchStopReason reason) noexcept -> AutomationErrorKind
    {
        switch (reason)
        {
        case SadSearchStopReason::Cancelled: return AutomationErrorKind::Cancelled;
        case SadSearchStopReason::TimedOut: return AutomationErrorKind::Timeout;
        case SadSearchStopReason::ComparisonBudgetExhausted:
            return AutomationErrorKind::RecognitionIncomplete;
        }

        UF_UNREACHABLE_MSG("Unknown SadSearchStopReason value");
    }

    auto searchStopDescription(SadSearchStopReason reason) noexcept -> std::string_view
    {
        switch (reason)
        {
        case SadSearchStopReason::Cancelled: return "cancelled";
        case SadSearchStopReason::TimedOut: return "timed out";
        case SadSearchStopReason::ComparisonBudgetExhausted: return "budget exhausted";
        }

        UF_UNREACHABLE_MSG("Unknown SadSearchStopReason value");
    }

    auto decodeTemplateImage(
        std::span<std::byte const> pngBytes,
        std::string identity
    ) -> Result<GrayTemplateImage>
    {
        UF_TRY_VALUE(decoded, image::decodePng(pngBytes, identity));
        auto const width  = decoded.width;
        auto const height = decoded.height;
        UF_TRY_VALUE(bgra, image::rgba8ToBgra8(std::move(decoded.pixels)));
        auto const widthSize = checkedCast<std::size_t>(width);
        if (!widthSize)
        {
            return invalidTemplate("template width is not addressable");
        }
        auto const stride = checkedMultiply(*widthSize, std::size_t{4});
        if (!stride)
        {
            return invalidTemplate("template stride overflowed");
        }
        UF_TRY_VALUE(gray, bgra8ToGray8(bgra, width, height, *stride));
        UF_TRY_VALUE(alpha, bgra8ToAlpha8(bgra, width, height, *stride));

        // A template that excludes nothing keeps an empty mask, so it takes the
        // same matcher call it took before templates carried one.
        auto const opaque = std::ranges::all_of(
            alpha,
            [](std::byte weight) noexcept -> bool
            {
                return weight == std::byte{255};
            }
        );
        return GrayTemplateImage{
            .identity = std::move(identity),
            .width    = width,
            .height   = height,
            .pixels   = std::move(gray),
            .mask     = opaque ? std::vector<std::byte>{} : std::move(alpha),
        };
    }

    auto makeSadSearchPoll(RecognitionPolicy const& policy) -> SadSearchPoll
    {
        auto const cancellation = policy.cancellation;
        auto const deadline     = policy.deadline;
        return SadSearchPoll{
            [cancellation, deadline]() noexcept -> SadSearchControl
            {
                if (cancellation.stop_requested())
                {
                    return SadSearchControl::Cancelled;
                }
                if (deadline && MonotonicInstant::now() >= *deadline)
                {
                    return SadSearchControl::TimedOut;
                }
                return SadSearchControl::Continue;
            }
        };
    }

    auto matchGrayTemplateImage(
        GrayImage const& grayFrame,
        GrayTemplateImage const& grayTemplate,
        PixelRect roi,
        uint64 maximumPixelComparisons,
        SadSearchPoll const& poll
    ) -> Result<SadSearchReport>
    {
        auto const templateStride = checkedCast<std::size_t>(grayTemplate.width);
        UF_CHECK(templateStride.has_value());
        UF_TRY_VALUE(
            templateImage,
            GrayImage::create(
                grayTemplate.pixels,
                grayTemplate.width,
                grayTemplate.height,
                *templateStride
            )
        );
        if (grayTemplate.mask.empty())
        {
            return matchTemplateSad(
                grayFrame,
                templateImage,
                roi,
                maximumPixelComparisons,
                poll
            );
        }

        UF_TRY_VALUE(
            maskImage,
            GrayImage::create(
                grayTemplate.mask,
                grayTemplate.width,
                grayTemplate.height,
                *templateStride
            )
        );
        return matchTemplateSad(
            grayFrame,
            templateImage,
            maskImage,
            roi,
            maximumPixelComparisons,
            poll
        );
    }

    auto matchTemplateOnFrame(
        Frame const& frame,
        GrayTemplateImage const& templateImage,
        PixelRect searchRoi,
        RecognitionPolicy const& policy
    ) -> Result<TemplateMatchAttempt>
    {
        // The ceiling a raw score is read against. A masked template normalizes
        // its weighted sum back to the full template rectangle (see
        // vision/sad.hpp), so the ceiling is the same product for both matchers
        // and a mask does not change the scale a caller compares on.
        auto const pixels = checkedMultiply(
            static_cast<uint64>(templateImage.width),
            static_cast<uint64>(templateImage.height)
        );
        auto const maximumSad = pixels
            ? checkedMultiply(*pixels, uint64{255})
            : std::optional<uint64>{};
        if (!maximumSad)
        {
            return invalidTemplate("template dimensions overflow the score ceiling");
        }

        auto const poll = makeSadSearchPoll(policy);
        return withGrayFrame(
            frame,
            [&templateImage, searchRoi, &policy, &poll, ceiling = *maximumSad](
                GrayImage const& grayFrame
            ) -> Result<TemplateMatchAttempt>
            {
                UF_TRY_VALUE(
                    report,
                    matchGrayTemplateImage(
                        grayFrame,
                        templateImage,
                        searchRoi,
                        policy.maximumPixelComparisons,
                        poll
                    )
                );
                if (
                    auto const* p_stop = std::get_if<SadSearchStopReason>(
                        &report.outcome
                    )
                )
                {
                    return TemplateMatchAttempt{
                        .result                    = *p_stop,
                        .completedPixelComparisons = report.completedPixelComparisons,
                    };
                }

                auto const& match = std::get<std::optional<SadMatch>>(report.outcome);
                if (!match)
                {
                    return TemplateMatchAttempt{
                        .result                    = std::optional<TemplateMatch>{},
                        .completedPixelComparisons = report.completedPixelComparisons,
                    };
                }

                UF_TRY_VALUE(
                    matchedRect,
                    PixelRect::create(
                        match->x(),
                        match->y(),
                        templateImage.width,
                        templateImage.height
                    )
                );
                return TemplateMatchAttempt{
                    .result = std::optional<TemplateMatch>{
                        TemplateMatch{
                            .matchedRect = matchedRect,
                            .sadScore    = match->score(),
                            .maximumSad  = ceiling,
                        },
                    },
                    .completedPixelComparisons = report.completedPixelComparisons,
                };
            }
        );
    }

    auto ensureCompatibleFrame(
        Frame const& frame,
        ProjectFingerprint liveFingerprint,
        ProjectFingerprint projectFingerprint
    ) -> Status
    {
        if (liveFingerprint != projectFingerprint)
        {
            return incompatibleFrame(
                std::format(
                    "live fingerprint {}x{} @ {}x{} DPI does not match project {}x{} @ {}x{} DPI",
                    liveFingerprint.width(),
                    liveFingerprint.height(),
                    liveFingerprint.dpiX(),
                    liveFingerprint.dpiY(),
                    projectFingerprint.width(),
                    projectFingerprint.height(),
                    projectFingerprint.dpiX(),
                    projectFingerprint.dpiY()
                )
            );
        }
        if (
            frame.width() != projectFingerprint.width()
            || frame.height() != projectFingerprint.height()
        )
        {
            return incompatibleFrame(
                std::format(
                    "frame extent {}x{} does not match project {}x{}",
                    frame.width(),
                    frame.height(),
                    projectFingerprint.width(),
                    projectFingerprint.height()
                )
            );
        }

        return ok();
    }
}
