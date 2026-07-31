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

    // One template PNG after decoding: the Gray8 plane the matcher compares
    // against a frame, plus the alpha plane it weights each pixel by.
    //
    // It is at namespace scope rather than nested in RecognitionRuntime because
    // it now has a second consumer: the script-owned page model loads templates
    // that belong to no catalog element, so the engine holds them outside any
    // recognition runtime and matches them through matchTemplateOnFrame.
    struct GrayTemplateImage final
    {
        ContentHash hash;

        uint32                 width{};
        uint32                 height{};
        std::vector<std::byte> pixels{};

        // The decoded template's alpha channel, Gray8 shaped like pixels. Empty
        // when every pixel is opaque, which selects the unmasked matcher and the
        // behaviour projects authored before masks had.
        std::vector<std::byte> mask{};
    };

    // Decodes one template PNG into the planes the matcher consumes, and is the
    // single definition of that decoding: the recognition runtime's own template
    // closure goes through it, so a template the script layer loads is the same
    // pixels the runtime would have loaded from the same bytes.
    //
    // `hash` is the content hash the caller already computed over `pngBytes`; it
    // is carried into the result rather than recomputed, because the two callers
    // reach it differently -- the runtime reads it off the manifest, the script
    // layer hashes the blob it was handed.
    [[nodiscard]]
    auto decodeTemplateImage(
        std::span<std::byte const> pngBytes,
        ContentHash const& hash
    ) -> Result<GrayTemplateImage>;

    struct RecognitionPolicy final
    {
        uint64                          maximumPixelComparisons{};
        std::optional<MonotonicInstant> deadline{};
        std::stop_token                 cancellation{};
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

    // Where one template matched and how far off it was. There is no threshold
    // here on purpose: a raw match reports the distance and the ceiling it is
    // measured against, and deciding whether that counts as a hit is the script
    // layer's, which is what "scores stay in layer one, judging moves to layer
    // two" means.
    struct TemplateMatch final
    {
        PixelRect matchedRect;

        uint64 sadScore{};

        // The largest score the comparison could have produced: the template's
        // pixel count times the maximum per-pixel distance. It is what makes two
        // scores from differently sized templates comparable at all.
        uint64 maximumSad{};
    };

    struct TemplateMatchAttempt final
    {
        // A control stop is a failed attempt, never a completed match. An empty
        // optional inside the first alternative is a completed search that had
        // no candidate position at all, which is the region being smaller than
        // the template.
        std::variant<std::optional<TemplateMatch>, SadSearchStopReason> result;

        uint64 completedPixelComparisons{};
    };

    // Searches `searchRoi` of `frame` for `templateImage` and reports the best
    // position with its score, or the control stop that ended the search.
    //
    // It knows nothing about elements, pages or appearances, which is the whole
    // point: it is the primitive the script-owned page model rebuilds those on
    // top of. The budget and the stop token come from `policy`, exactly as they
    // do for the catalog-driven paths, so a raw match cannot outrun the bound a
    // page resolution honours.
    [[nodiscard]]
    auto matchTemplateOnFrame(
        Frame const& frame,
        GrayTemplateImage const& templateImage,
        PixelRect searchRoi,
        RecognitionPolicy const& policy
    ) -> Result<TemplateMatchAttempt>;

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

        RuntimeManifest                m_manifest;
        std::vector<GrayTemplateImage> m_templates;

        RecognitionRuntime(
            RuntimeManifest manifest,
            std::vector<GrayTemplateImage> templates
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

        // Whether `frame` may be compared against this project at all: the live
        // geometry has to be the fingerprint the project was authored at, and
        // the frame has to have the extent that fingerprint names.
        //
        // Public because a raw template match runs on the same frames without
        // going through any catalog entry point, and that path must not be the
        // one place the compatibility gate is skipped. Every entry point below
        // still calls it for itself.
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
