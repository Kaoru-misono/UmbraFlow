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
#include <span>
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

    struct PageRecognitionStop final
    {
        ElementId           elementId;
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
        // Searching one element across its appearances: either the folded
        // evidence, or the stop that ended the search. A stop in any appearance
        // stops the whole element -- taking the best of the appearances already
        // searched would make the answer a function of the comparison budget,
        // which is a configuration value.
        struct ElementMatchAttempt final
        {
            std::variant<AnchorEvidence, SadSearchStopReason> result;

            uint64 completedPixelComparisons{};
        };

        // One decoded template beside the hash it was published under. The
        // hash is here rather than on the decoded image because vision, which
        // owns the decoding, hashes nothing and must not name a project's
        // content-address type; the closure below is what pairs the two.
        struct StoredTemplate final
        {
            ContentHash       hash;
            GrayTemplateImage image;
        };

        RuntimeManifest             m_manifest;
        std::vector<StoredTemplate> m_templates;

        RecognitionRuntime(
            RuntimeManifest manifest,
            std::vector<StoredTemplate> templates
        ) noexcept;

        [[nodiscard]]
        auto findTemplate(
            ContentHash const& hash
        ) const noexcept UF_LIFETIME_BOUND -> GrayTemplateImage const*;

        // Every declared appearance is searched and the results are folded into
        // one piece of evidence before anything downstream sees them. The fold
        // has to happen here: PageResolver ANDs over a page's required anchors,
        // so V appearances compiled into V required anchors would turn "any
        // appearance matches" into "all of them must".
        [[nodiscard]]
        auto matchElement(
            GrayImage const& grayFrame,
            CompiledElement const& element,
            PixelRect searchRoi,
            std::optional<ResourceName> const& pinnedAppearance,
            uint64 maximumPixelComparisons,
            SadSearchPoll const& poll
        ) const -> Result<ElementMatchAttempt>;

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

        // Whether `frame` may be compared against this project at all,
        // answered by vision::ensureCompatibleFrame against this catalog's own
        // fingerprint. It stays a member so the manifest's fingerprint is read
        // in one place; the rule itself is shared with every other caller of
        // that free function.
        [[nodiscard]]
        auto ensureCompatibleFrame(
            Frame const& frame,
            ProjectFingerprint liveFingerprint
        ) const -> Status;

        [[nodiscard]]
        auto evaluatePage(
            Frame const& frame,
            ProjectFingerprint liveFingerprint,
            RecognitionPolicy const& policy
        ) const -> Result<PageRecognitionAttempt>;

        // Locating one element on one already-resolved page. The page is a
        // parameter because the per-page facts now live on the reference: a
        // refined search region and a pinned appearance are both read from it,
        // and an element the page does not exercise for interaction has no
        // action to be located for.
        [[nodiscard]]
        auto evaluateActionTarget(
            Frame const& frame,
            ProjectFingerprint liveFingerprint,
            PageId pageId,
            ElementId elementId,
            RecognitionPolicy const& policy
        ) const -> Result<ActionTargetAttempt>;

        [[nodiscard]]
        auto recognizePage(
            Frame const& frame,
            ProjectFingerprint liveFingerprint,
            RecognitionPolicy const& policy
        ) const -> Result<PageOutcome>;

        // One declared appearance of one element, searched alone in one region.
        //
        // No product path does this: the anchor pass and the action path both
        // fold across the appearances, which is the whole point of the fold.
        // The falsification matrix needs the opposite -- one appearance
        // measured on a screen NO page claims -- because an appearance that
        // matches where it does not belong is invisible once folded with one
        // that matches where it does. Neither page-scoped entry point can
        // reach that cell, so it is exposed here rather than reimplemented at
        // the authoring edge, where it would drift from what the runtime does.
        //
        // The region is the caller's because the two page paths derive it
        // differently -- the anchor pass reads the element's, the action path
        // the reference's refinement -- and a per-appearance measurement is
        // only comparable with the folded one when both searched the same
        // pixels.
        [[nodiscard]]
        auto evaluateAppearance(
            Frame const& frame,
            ProjectFingerprint liveFingerprint,
            ElementId elementId,
            ResourceName const& appearance,
            PixelRect searchRoi,
            RecognitionPolicy const& policy
        ) const -> Result<ActionTargetAttempt>;
    };

    // Derives the single deterministic click pixel for an interactive element
    // from the rectangle it matched. A template-local click offset is added to
    // the matched origin with checked arithmetic; without one, the click is the
    // truncating integer center of the matched rectangle. An element located by
    // its page rather than by its own pixels has no offset to carry, so it
    // takes the center of the region the page put it in.
    [[nodiscard]]
    auto resolveClickPixel(
        CompiledElement const& element,
        PixelRect const& matchedRect
    ) -> Result<PixelPoint>;
}
