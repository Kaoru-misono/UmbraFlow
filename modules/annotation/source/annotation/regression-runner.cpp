#include "regression-runner.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/safety/checked-access.hpp>
#include <core/types/integer.hpp>

#include <domain/frame.hpp>

#include <image/pixels.hpp>
#include <image/png.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <memory>
#include <ranges>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace uf::annotation
{
    namespace
    {
        [[nodiscard]]
        auto matchesExpectation(
            RegressionExpectation const& expectation,
            PageRecognitionAttempt const& attempt
        ) noexcept -> bool
        {
            auto const* p_outcome = std::get_if<PageOutcome>(&attempt.result);
            if (p_outcome == nullptr)
            {
                return false;
            }

            if (auto const* p_expected = std::get_if<ResolvedRegression>(&expectation))
            {
                auto const* p_actual = std::get_if<ResolvedPage>(p_outcome);
                return p_actual != nullptr && p_actual->pageId() == p_expected->pageId;
            }
            if (std::holds_alternative<UnknownRegression>(expectation))
            {
                return std::holds_alternative<UnknownPage>(*p_outcome);
            }
            UF_CHECK(std::holds_alternative<AmbiguousRegression>(expectation));
            return std::holds_alternative<AmbiguousPages>(*p_outcome);
        }

        // A stop interrupts the suite exactly when its failure would end a run;
        // a per-page budget stop is recorded as a regression and the suite
        // continues.
        [[nodiscard]]
        auto isSuiteInterruption(PageRecognitionAttempt const& attempt) noexcept -> bool
        {
            auto const* p_stop = std::get_if<PageRecognitionStop>(&attempt.result);
            if (p_stop == nullptr)
            {
                return false;
            }

            switch (failureResponse(searchStopKind(p_stop->reason)))
            {
            case FailureResponse::Abort:
            case FailureResponse::Cancelled: return true;
            case FailureResponse::Retry:
            case FailureResponse::StepFailed: return false;
            }

            UF_UNREACHABLE_MSG("Unknown FailureResponse value");
        }
    }

    auto runAuthoringRegressions(
        AuthoringDocument const& document,
        std::span<AuthoringSourceAsset const> sourceAssets,
        RecognitionPolicy const& policy
    ) -> Result<RegressionSuiteReport>
    {
        UF_TRY_VALUE(compiled, compileAuthoringDocument(document, sourceAssets));

        auto encodedTemplates = std::vector<EncodedRuntimeTemplate>{};
        encodedTemplates.reserve(compiled.templateAssets.size());
        for (auto& asset : compiled.templateAssets)
        {
            encodedTemplates.emplace_back(
                EncodedRuntimeTemplate{
                    .hash     = asset.hash,
                    .pngBytes = std::move(asset.pngBytes),
                }
            );
        }
        UF_TRY_VALUE(
            runtime,
            RecognitionRuntime::create(
                std::move(compiled.runtimeManifest),
                std::move(encodedTemplates)
            )
        );

        auto reports = std::vector<RegressionCaseReport>{};
        reports.reserve(document.regressions().size());
        auto suiteInterrupted = false;
        for (auto index = std::size_t{0}; index < document.regressions().size(); ++index)
        {
            auto const& regression = checkedAt(document.regressions(), index);
            auto const* p_source   = document.findSource(regression.sourceId());
            UF_CHECK(p_source != nullptr);
            auto const asset = std::ranges::find(
                sourceAssets,
                regression.sourceId(),
                &AuthoringSourceAsset::id
            );
            UF_CHECK(asset != sourceAssets.end());
            UF_TRY_VALUE_CONTEXT(
                decoded,
                image::decodePng(asset->pngBytes, p_source->relativePath()),
                std::format("decoding regression source {}", p_source->relativePath())
            );
            UF_TRY_VALUE_CONTEXT(
                bgra,
                image::rgba8ToBgra8(std::move(decoded.pixels)),
                std::format("converting regression source {}", p_source->relativePath())
            );
            auto const width = checkedCast<std::size_t>(decoded.width);
            UF_CHECK(width.has_value());
            auto const stride = checkedMultiply(*width, std::size_t{4});
            UF_CHECK(stride.has_value());
            UF_TRY_VALUE(
                transform,
                CoordinateTransform::create(
                    Point<DesktopSpace>{0.0F, 0.0F},
                    static_cast<float>(decoded.width),
                    static_cast<float>(decoded.height),
                    decoded.width,
                    decoded.height
                )
            );
            auto const nextIndex = checkedAdd(index, std::size_t{1});
            UF_CHECK(nextIndex.has_value());
            auto const frameId = checkedCast<uint64>(*nextIndex);
            UF_CHECK(frameId.has_value());

            auto generation = TargetGeneration::initial();
            if (
                auto const* p_wgc = std::get_if<WgcSourceProvenance>(
                    &p_source->provenance()
                )
            )
            {
                generation = p_wgc->targetGeneration;
            }
            auto const pixels = std::shared_ptr<FrameBuffer const>{
                std::make_shared<FrameBuffer>(std::move(bgra))
            };
            UF_TRY_VALUE(
                frame,
                Frame::create(
                    FrameId{*frameId},
                    SessionId{1},
                    generation,
                    MonotonicInstant::fromTimePoint(MonotonicInstant::TimePoint{}),
                    decoded.width,
                    decoded.height,
                    *stride,
                    PixelFormat::Bgra8,
                    pixels,
                    transform
                )
            );
            UF_TRY_VALUE_CONTEXT(
                attempt,
                runtime.evaluatePage(
                    frame,
                    document.catalog().fingerprint(),
                    policy
                ),
                std::format(
                    "evaluating regression {}",
                    regression.id().value().toString()
                )
            );
            auto const matches = matchesExpectation(regression.expectation(), attempt);
            suiteInterrupted   = isSuiteInterruption(attempt);
            reports.emplace_back(
                RegressionCaseReport{
                    .id                 = regression.id(),
                    .sourceId           = regression.sourceId(),
                    .classification     = regression.classification(),
                    .expectation        = regression.expectation(),
                    .attempt            = std::move(attempt),
                    .matchesExpectation = matches,
                }
            );
            if (suiteInterrupted)
            {
                break;
            }
        }

        auto const completedAllCases = (
            !suiteInterrupted
            && reports.size() == document.regressions().size()
        );
        return RegressionSuiteReport{
            .cases             = std::move(reports),
            .completedAllCases = completedAllCases,
        };
    }
}
