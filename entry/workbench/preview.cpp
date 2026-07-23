#include "preview.hpp"

#include <annotation/authoring-compiler.hpp>
#include <annotation/recognition.hpp>
#include <annotation/recognition-runtime.hpp>

#include <core/error/result.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <image/pixels.hpp>
#include <image/png.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace uf::workbench
{
    namespace
    {
        [[nodiscard]]
        auto toAnchorRow(annotation::AnchorEvidence const& evidence) -> PreviewAnchorRow
        {
            return PreviewAnchorRow{
                .m_recognizerId = evidence.recognizerId(),
                .m_hit          = evidence.hit(),
                .m_sadScore     = evidence.sadScore(),
                .m_maximumSad   = evidence.maximumSad(),
                .m_matchedRect  = evidence.matchedRect(),
            };
        }

        [[nodiscard]]
        auto toPageKind(annotation::PageOutcome const& outcome) noexcept -> PreviewPageKind
        {
            if (std::holds_alternative<annotation::ResolvedPage>(outcome))
            {
                return PreviewPageKind::Resolved;
            }
            if (std::holds_alternative<annotation::UnknownPage>(outcome))
            {
                return PreviewPageKind::Unknown;
            }
            return PreviewPageKind::Ambiguous;
        }

        [[nodiscard]]
        auto previewFrame(
            annotation::ProjectFingerprint fingerprint,
            std::span<std::byte const> pngBytes
        ) -> Result<Frame>
        {
            UF_TRY_VALUE(
                decoded,
                image::decodePng(pngBytes, "workbench-preview-source.png")
            );
            if (
                decoded.m_width != fingerprint.width()
                || decoded.m_height != fingerprint.height()
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "preview source geometry does not match the project fingerprint"
                );
            }
            UF_TRY_VALUE(bgra, image::rgba8ToBgra8(std::move(decoded.m_pixels)));
            UF_TRY_VALUE(
                transform,
                CoordinateTransform::create(
                    Point<DesktopSpace>{0.0F, 0.0F},
                    static_cast<float>(fingerprint.width()),
                    static_cast<float>(fingerprint.height()),
                    fingerprint.width(),
                    fingerprint.height()
                )
            );

            auto const width = checkedCast<std::size_t>(fingerprint.width());
            if (!width.has_value())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "preview source width is not addressable"
                );
            }
            auto const stride = checkedMultiply(
                *width,
                bytesPerPixel(PixelFormat::Bgra8)
            );
            if (!stride.has_value())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "preview source stride overflowed addressable memory"
                );
            }

            auto const buffer = std::shared_ptr<FrameBuffer const>{
                std::make_shared<FrameBuffer>(std::move(bgra))
            };
            return Frame::create(
                FrameId{1},
                SessionId{1},
                TargetGeneration::fromValue(1),
                MonotonicInstant::fromTimePoint(MonotonicInstant::TimePoint{}),
                fingerprint.width(),
                fingerprint.height(),
                *stride,
                PixelFormat::Bgra8,
                buffer,
                transform
            );
        }
    }

    auto runPreview(
        annotation::AuthoringDocument const& document,
        std::span<annotation::AuthoringSourceAsset const> sourceAssets,
        annotation::SourceId selectedSourceId,
        std::optional<annotation::RecognizerId> selectedRecognizerId,
        annotation::RecognitionPolicy const& policy
    ) -> Result<PreviewResult>
    {
        auto const selected = std::ranges::find(
            sourceAssets,
            selectedSourceId,
            &annotation::AuthoringSourceAsset::m_id
        );
        if (selected == sourceAssets.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "preview requires the selected source to be present in the project"
            );
        }

        UF_TRY_VALUE(
            compiled,
            annotation::compileAuthoringDocument(document, sourceAssets)
        );
        auto encodedTemplates = std::vector<annotation::EncodedRuntimeTemplate>{};
        encodedTemplates.reserve(compiled.m_templateAssets.size());
        for (auto& asset : compiled.m_templateAssets)
        {
            encodedTemplates.emplace_back(
                annotation::EncodedRuntimeTemplate{
                    .m_hash     = asset.m_hash,
                    .m_pngBytes = std::move(asset.m_pngBytes),
                }
            );
        }
        UF_TRY_VALUE(
            runtime,
            annotation::RecognitionRuntime::create(
                std::move(compiled.m_runtimeManifest),
                std::move(encodedTemplates)
            )
        );

        auto const fingerprint = document.catalog().fingerprint();
        UF_TRY_VALUE(frame, previewFrame(fingerprint, selected->m_pngBytes));

        UF_TRY_VALUE(
            pageAttempt,
            runtime.evaluatePage(frame, fingerprint, policy)
        );

        auto result = PreviewResult{};
        result.m_anchorRows.reserve(pageAttempt.m_completedAnchorEvidence.size());
        for (auto const& evidence : pageAttempt.m_completedAnchorEvidence)
        {
            result.m_anchorRows.emplace_back(toAnchorRow(evidence));
        }
        if (
            auto const* p_stop = std::get_if<annotation::PageRecognitionStop>(
                &pageAttempt.m_result
            )
        )
        {
            result.m_pageStop = PreviewStop{
                .m_recognizerId = p_stop->m_recognizerId,
                .m_reason       = p_stop->m_reason,
            };
        }
        else
        {
            auto const& outcome = std::get<annotation::PageOutcome>(
                pageAttempt.m_result
            );
            result.m_pageKind = toPageKind(outcome);
            if (
                auto const* p_resolved = std::get_if<annotation::ResolvedPage>(
                    &outcome
                )
            )
            {
                result.m_resolvedPageId = p_resolved->pageId();
            }
        }

        if (selectedRecognizerId.has_value())
        {
            auto const* p_recognizer = document.catalog().findRecognizer(
                *selectedRecognizerId
            );
            if (
                p_recognizer != nullptr
                && p_recognizer->annotationType() == annotation::AnnotationType::ActionTarget
            )
            {
                UF_TRY_VALUE(
                    actionAttempt,
                    runtime.evaluateActionTarget(
                        frame,
                        fingerprint,
                        *selectedRecognizerId,
                        policy
                    )
                );
                if (
                    auto const* p_actionStop = std::get_if<annotation::PageRecognitionStop>(
                        &actionAttempt.m_result
                    )
                )
                {
                    result.m_actionStop = PreviewStop{
                        .m_recognizerId = p_actionStop->m_recognizerId,
                        .m_reason       = p_actionStop->m_reason,
                    };
                }
                else
                {
                    result.m_actionEvidence = toAnchorRow(
                        std::get<annotation::AnchorEvidence>(actionAttempt.m_result)
                    );
                }
            }
        }

        return result;
    }
}
