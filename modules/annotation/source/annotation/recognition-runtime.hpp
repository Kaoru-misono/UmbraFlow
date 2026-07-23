#pragma once

#include "recognition.hpp"
#include "runtime-manifest.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/frame.hpp>
#include <domain/space.hpp>

#include <vision/sad.hpp>

#include <cstddef>
#include <optional>
#include <stop_token>
#include <variant>
#include <vector>

namespace uf::annotation
{
    struct EncodedRuntimeTemplate final
    {
        ContentHash            m_hash;
        std::vector<std::byte> m_pngBytes{};
    };

    struct RecognitionPolicy final
    {
        uint64                          m_maximumPixelComparisons{};
        std::optional<MonotonicInstant> m_deadline{};
        std::stop_token                 m_cancellation{};
    };

    struct PageRecognitionStop final
    {
        RecognizerId        m_recognizerId;
        SadSearchStopReason m_reason{};
    };

    using PageAttemptResult = std::variant<PageOutcome, PageRecognitionStop>;

    struct PageRecognitionAttempt final
    {
        // A control stop is a failed attempt, never a completed PageOutcome.
        PageAttemptResult m_result;

        std::vector<AnchorEvidence> m_completedAnchorEvidence{};
        uint64                      m_completedPixelComparisons{};
    };

    using ActionAttemptResult = std::variant<AnchorEvidence, PageRecognitionStop>;

    struct ActionTargetAttempt final
    {
        // A control stop is a failed attempt, never a completed evaluation.
        ActionAttemptResult m_result;
        uint64              m_completedPixelComparisons{};
    };

    class RecognitionRuntime final
    {
        struct GrayTemplate final
        {
            ContentHash m_hash;

            uint32                 m_width{};
            uint32                 m_height{};
            std::vector<std::byte> m_pixels{};
        };

        RuntimeManifest           m_manifest;
        std::vector<GrayTemplate> m_templates;

        RecognitionRuntime(
            RuntimeManifest manifest,
            std::vector<GrayTemplate> templates
        ) noexcept;

        [[nodiscard]]
        auto findTemplate(
            ContentHash const& hash
        ) const noexcept UF_LIFETIME_BOUND -> GrayTemplate const*;

        [[nodiscard]]
        auto evaluateGrayPage(
            Frame const& frame,
            GrayImage const& grayFrame,
            RecognitionPolicy const& policy,
            SadSearchPoll const& poll
        ) const -> Result<PageRecognitionAttempt>;

        [[nodiscard]]
        auto ensureCompatibleFrame(
            Frame const& frame,
            ProjectFingerprint liveFingerprint
        ) const -> Status;

        [[nodiscard]]
        auto evaluateGrayActionTarget(
            GrayImage const& grayFrame,
            RecognizerDefinition const& recognizer,
            GrayTemplate const& grayTemplate,
            RecognitionPolicy const& policy,
            SadSearchPoll const& poll
        ) const -> Result<ActionTargetAttempt>;

    public:
        [[nodiscard]]
        static auto create(
            RuntimeManifest manifest,
            std::vector<EncodedRuntimeTemplate> encodedTemplates
        ) -> Result<RecognitionRuntime>;

        [[nodiscard]]
        auto manifest() const noexcept UF_LIFETIME_BOUND -> RuntimeManifest const&;

        [[nodiscard]]
        auto evaluatePage(
            Frame const& frame,
            ProjectFingerprint liveFingerprint,
            RecognitionPolicy const& policy
        ) const -> Result<PageRecognitionAttempt>;

        [[nodiscard]]
        auto evaluateActionTarget(
            Frame const& frame,
            ProjectFingerprint liveFingerprint,
            RecognizerId recognizerId,
            RecognitionPolicy const& policy
        ) const -> Result<ActionTargetAttempt>;

        [[nodiscard]]
        auto recognizePage(
            Frame const& frame,
            ProjectFingerprint liveFingerprint,
            RecognitionPolicy const& policy
        ) const -> Result<PageOutcome>;
    };

    // Derives the single deterministic click pixel for an action target from the
    // rectangle its template matched. A template-local click offset is added to
    // the matched origin with checked arithmetic; without one, the click is the
    // truncating integer center of the matched rectangle.
    [[nodiscard]]
    auto resolveClickPixel(
        RecognizerDefinition const& recognizer,
        PixelRect const& matchedRect
    ) -> Result<PixelPoint>;
}
