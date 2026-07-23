#pragma once

#include "recognition.hpp"
#include "runtime-manifest.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/frame.hpp>

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

    struct PageRecognitionAttempt final
    {
        // A control stop is a failed attempt, never a completed PageOutcome.
        std::variant<PageOutcome, PageRecognitionStop> m_result;
        std::vector<AnchorEvidence> m_completedAnchorEvidence{};
        uint64                      m_completedPixelComparisons{};
    };

    class RecognitionRuntime final
    {
        struct GrayTemplate final
        {
            ContentHash            m_hash;
            uint32                 m_width{};
            uint32                 m_height{};
            std::vector<std::byte> m_pixels{};
        };

        RuntimeManifest           m_manifest;
        std::vector<GrayTemplate> m_templates{};

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
        auto recognizePage(
            Frame const& frame,
            ProjectFingerprint liveFingerprint,
            RecognitionPolicy const& policy
        ) const -> Result<PageOutcome>;
    };
}
