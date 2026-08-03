#pragma once

#include "sad.hpp"

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/space.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

// Searching one frame for one template, and nothing about what the template
// means.
namespace uf
{
    // The single classification of a stopped search. Recognition failures and
    // regression control flow both read it instead of deciding per stop reason
    // at each site.
    //
    // No reason maps to a kind whose failureResponse is StepFailed, and each
    // reason has a kind of its own. That is the load-bearing property, not an
    // accident of the three current reasons: a stop means the search never
    // decided, so a caller must not read it as the step having been ruled out. A
    // completed search that matched nothing is not routed through here at all.
    [[nodiscard]]
    auto searchStopKind(SadSearchStopReason reason) noexcept -> AutomationErrorKind;

    // The returned view is backed by static string literals, so it outlives
    // every caller and needs no owner to be kept alive.
    [[nodiscard]]
    auto searchStopDescription(SadSearchStopReason reason) noexcept -> std::string_view;

    // One template PNG after decoding: the Gray8 plane the matcher compares
    // against a frame, plus the alpha plane it weights each pixel by.
    //
    // `identity` is the name the caller knows this template by -- both callers
    // pass the hex content hash of the bytes it was decoded from -- and it is
    // carried rather than derived because this module hashes nothing. It reaches
    // the decoder's own diagnostics and the trace line of a raw match, which is
    // the only name a template that belongs to no catalog element ever has.
    struct GrayTemplateImage final
    {
        std::string identity{};

        uint32                 width{};
        uint32                 height{};
        std::vector<std::byte> pixels{};

        // The decoded template's alpha channel, Gray8 shaped like pixels. Empty
        // when every pixel is opaque, which selects the unmasked matcher and the
        // behaviour projects authored before masks had.
        std::vector<std::byte> mask{};
    };

    // Decodes one template PNG into the planes the matcher consumes, and is the
    // single definition of that decoding: every template the script layer loads
    // through template_load goes through it, so the same bytes always become the
    // same pixels whichever verb asked for them.
    [[nodiscard]]
    auto decodeTemplateImage(
        std::span<std::byte const> pngBytes,
        std::string identity
    ) -> Result<GrayTemplateImage>;

    // What bounds one search: how many pixel comparisons it may spend, when it
    // must be finished, and the stop token that can end it early. Every entry
    // point below takes one, so no search can outrun the bound its caller set.
    struct RecognitionPolicy final
    {
        uint64                          maximumPixelComparisons{};
        std::optional<MonotonicInstant> deadline{};
        std::stop_token                 cancellation{};
    };

    // The poll `policy` implies, ready to hand to the matcher.
    //
    // The returned callable captures the token and the deadline BY VALUE,
    // because the matcher stores the poll for the duration of the search and a
    // stored callback must not borrow caller state.
    [[nodiscard]]
    auto makeSadSearchPoll(RecognitionPolicy const& policy) -> SadSearchPoll;

    // Converts the frame to a Gray8 view once and hands it to the continuation
    // while the backing storage is still alive. The Bgra8 path's gray buffer
    // lives on this stack frame for the whole synchronous call, so the view the
    // continuation receives never outlives its owner. That is the whole reason
    // this is a continuation rather than a function returning a view.
    template <typename Continuation>
    [[nodiscard]]
    auto withGrayFrame(
        Frame const& frame,
        Continuation const& continuation
    ) -> std::invoke_result_t<Continuation const&, GrayImage const&>
    {
        auto const p_pixels = frame.pixels();
        UF_CHECK(p_pixels != nullptr);
        switch (frame.pixelFormat())
        {
        case PixelFormat::Gray8:
        {
            UF_TRY_VALUE(
                grayFrame,
                GrayImage::create(
                    p_pixels->bytes(),
                    frame.width(),
                    frame.height(),
                    frame.stride()
                )
            );
            return std::invoke(continuation, grayFrame);
        }
        case PixelFormat::Bgra8:
        {
            UF_TRY_VALUE(
                grayPixels,
                bgra8ToGray8(
                    p_pixels->bytes(),
                    frame.width(),
                    frame.height(),
                    frame.stride()
                )
            );
            auto const grayStride = checkedCast<std::size_t>(frame.width());
            UF_CHECK(grayStride.has_value());
            UF_TRY_VALUE(
                grayFrame,
                GrayImage::create(
                    grayPixels,
                    frame.width(),
                    frame.height(),
                    *grayStride
                )
            );
            return std::invoke(continuation, grayFrame);
        }
        }

        UF_UNREACHABLE_MSG("Unknown PixelFormat value");
    }

    // Runs one decoded template against one gray frame, picking the masked or
    // the unmasked matcher from whether the template carries a mask.
    //
    // It is public because a second copy of this choice is how a script-loaded
    // template would quietly stop honouring an alpha channel.
    [[nodiscard]]
    auto matchGrayTemplateImage(
        GrayImage const& grayFrame,
        GrayTemplateImage const& grayTemplate,
        PixelRect roi,
        uint64 maximumPixelComparisons,
        SadSearchPoll const& poll
    ) -> Result<SadSearchReport>;

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
    // The budget and the stop token come from `policy`, so a raw match cannot
    // outrun the bound a page resolution honours.
    [[nodiscard]]
    auto matchTemplateOnFrame(
        Frame const& frame,
        GrayTemplateImage const& templateImage,
        PixelRect searchRoi,
        RecognitionPolicy const& policy
    ) -> Result<TemplateMatchAttempt>;

    // Whether `frame` may be compared against a model authored at
    // `projectFingerprint` at all: the live geometry has to be that fingerprint,
    // and the frame has to have the extent it names.
    //
    // Both halves are here rather than at each caller because skipping either
    // one is invisible -- a search on a frame of the wrong size still returns a
    // number.
    [[nodiscard]]
    auto ensureCompatibleFrame(
        Frame const& frame,
        ProjectFingerprint liveFingerprint,
        ProjectFingerprint projectFingerprint
    ) -> Status;
}
