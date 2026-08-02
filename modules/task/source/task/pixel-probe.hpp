#pragma once

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <cstddef>
#include <optional>
#include <span>

namespace uf::task
{
    // The largest PNG blob one probe may decode.
    //
    // A probe measures a crop, and a crop of one screen is a few hundred
    // kilobytes at most, so this is the same ceiling template_load uses and for
    // the same reason: a primitive has to be bounded, and without a bound the
    // memory one call costs would be a property of whatever the caller happened
    // to hand it.
    inline constexpr auto k_maximumProbeBytes = std::size_t{4} * 1024U * 1024U;

    // The colour key one probe measures a region against: an RGB triple and how
    // far a pixel may sit from it and still count.
    //
    // It is optional at the call site (see probePngRegion) because the first
    // probe an agent runs is the one that has no key yet -- "what colours are in
    // this rectangle" is how a key gets chosen, and demanding one up front would
    // make the verb useless for exactly the question it exists to answer.
    //
    // IT IS THE CROP'S KEY TOO. TaskContext::cycleCrop takes one of these and
    // bakes the weights it implies into the PNG's alpha channel, which is what
    // makes an authored template a masked template. One type rather than two
    // because a key an agent probed with and a key it then cut with have to be
    // the same key -- two spellings of it would be two things that could come to
    // mean different tolerances, and the counts the two verbs report would stop
    // being about one measurement.
    struct ProbeColourKey final
    {
        uint8 red{};
        uint8 green{};
        uint8 blue{};

        // Rejected above vision's k_maximumColourKeyTolerance, which is the
        // widest distance the summed per-channel rule can express.
        uint32 tolerance{};
    };

    // What one probe measured.
    //
    // WHICH FIELDS THIS CARRIES, AND WHICH THE v4 `frames probe` HAD THAT IT DOES
    // NOT. The v4 command took two or more frames and reported six numbers; four
    // of them are here, and the two that are not are `masked_mean_gray_spread`
    // and `rect_mean_gray_spread`. Both are the mean, over a rect, of how far a
    // pixel's grey moved BETWEEN FRAMES -- a property of a frame SET. This verb
    // takes one blob, so both would be zero on every call, and a field that
    // always reads zero is not a measurement, it is a fixture. An agent that
    // wants them crops the same rect twice and compares.
    //
    // The census half is not v4's `frames probe`, it is v4's `frames census`,
    // and it is folded in because the plan's loop is "crop, then probe to fix
    // the key AND the threshold" (docs/plans/2026-08-01-agent-front-end-and-
    // exploration.md 1): an agent with no key yet has to be able to ask what is
    // there. It costs one pass over the same pixels the selection walk already
    // reads.
    struct PixelProbeReport final
    {
        uint32 imageWidth{};
        uint32 imageHeight{};

        uint64 rectPixels{};

        // How many distinct colours the rect holds, alpha ignored, and the most
        // frequent of them with its count. Ties are broken by the packed blue,
        // green, red value ascending, so the answer never depends on traversal
        // order.
        uint64 distinctColours{};

        uint8  dominantRed{};
        uint8  dominantGreen{};
        uint8  dominantBlue{};
        uint64 dominantPixels{};

        // Absent when the caller supplied no key. Absent rather than zero
        // because "nothing was selected" and "no selection was asked for" are
        // different answers, and a caller that cannot tell them apart would read
        // a key it never passed as a key that matched nothing.
        std::optional<uint64> fullySelectedPixels{};
        std::optional<uint64> rampSelectedPixels{};
        std::optional<uint64> selectedWeight{};
    };

    // Decodes `png` and measures `rect` of it.
    //
    // Pure: no frame, no ticket, no observation, no clock. That is what makes it
    // safe to be a primitive of its own rather than a cycle verb -- the bytes are
    // already in the caller's hand, and measuring them establishes nothing new
    // about the live target.
    //
    // A rect the decoded image does not contain is refused rather than clamped:
    // a probe of a region that is partly off the image would report counts a
    // caller would read as being about the region they asked for.
    [[nodiscard]]
    auto probePngRegion(
        std::span<std::byte const> png,
        PixelRect rect,
        std::optional<ProbeColourKey> key
    ) -> Result<PixelProbeReport>;
}
