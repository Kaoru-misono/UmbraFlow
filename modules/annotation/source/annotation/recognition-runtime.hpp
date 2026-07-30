#pragma once

#include "content-hash.hpp"
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
        ContentHash            hash;
        std::vector<std::byte> pngBytes{};
    };

    struct RecognitionPolicy final
    {
        uint64                          maximumPixelComparisons{};
        std::optional<MonotonicInstant> deadline{};
        std::stop_token                 cancellation{};
    };

    struct PageRecognitionStop final
    {
        RecognizerId        recognizerId;
        SadSearchStopReason reason{};
    };

    using PageAttemptResult = std::variant<PageOutcome, PageRecognitionStop>;

    struct PageRecognitionAttempt final
    {
        // A control stop is a failed attempt, never a completed PageOutcome.
        PageAttemptResult result;

        std::vector<AnchorEvidence> completedAnchorEvidence{};
        uint64                      completedPixelComparisons{};
    };

    using ActionAttemptResult = std::variant<AnchorEvidence, PageRecognitionStop>;

    struct ActionTargetAttempt final
    {
        // A control stop is a failed attempt, never a completed evaluation.
        ActionAttemptResult result;
        uint64              completedPixelComparisons{};
    };

    class RecognitionRuntime final
    {
        struct GrayTemplate final
        {
            ContentHash hash;

            uint32                 width{};
            uint32                 height{};
            std::vector<std::byte> pixels{};

            // The decoded template's alpha channel, Gray8 shaped like pixels.
            // Empty when every pixel is opaque, which selects the unmasked
            // matcher and the behaviour projects authored before masks had.
            std::vector<std::byte> mask{};
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
        static auto matchGrayTemplate(
            GrayImage const& grayFrame,
            GrayTemplate const& grayTemplate,
            PixelRect roi,
            uint64 maximumPixelComparisons,
            SadSearchPoll const& poll
        ) -> Result<SadSearchReport>;

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
