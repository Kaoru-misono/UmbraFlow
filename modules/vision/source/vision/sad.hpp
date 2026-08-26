#pragma once

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace uf
{
    enum class SadSearchControl : uint8
    {
        Continue,
        Cancelled,
        TimedOut,
    };

    enum class SadSearchStopReason : uint8
    {
        Cancelled,
        TimedOut,
        ComparisonBudgetExhausted,
    };

    inline constexpr auto k_sadSearchPollIntervalComparisons = uint64{4096};

    // Invoked synchronously during matching and never retained by the matcher.
    using SadSearchPoll = std::function<SadSearchControl()>;

    class SadMatch final
    {
        uint32 m_x;
        uint32 m_y;
        uint64 m_score;

    public:
        constexpr SadMatch(
            uint32 x,
            uint32 y,
            uint64 score
        ) noexcept
            : m_x{x}
            , m_y{y}
            , m_score{score}
        {
        }

        auto operator==(SadMatch const&) const -> bool = default;

        [[nodiscard]] constexpr auto x() const noexcept -> uint32 { return m_x; }
        [[nodiscard]] constexpr auto y() const noexcept -> uint32 { return m_y; }
        [[nodiscard]] constexpr auto score() const noexcept -> uint64 { return m_score; }
    };

    using SadSearchOutcome = std::variant<
        std::optional<SadMatch>,
        SadSearchStopReason
    >;

    struct SadSearchReport final
    {
        SadSearchOutcome outcome{};

        // Counts comparisons actually executed across every candidate, including
        // the comparisons that trigger pruning or an exact-match return, and the
        // ones a masked candidate's contrast probe spends before its score. A
        // budget or poll stop excludes the comparison that was not executed.
        // Valid for every outcome and starts at zero for each matcher call.
        uint64 completedPixelComparisons{};

        // How many candidate positions the masked matcher refused for want of
        // contrast outside the mask instead of scoring them. It is what tells a
        // reader that a search missed because nothing in the region contrasted
        // with the glyph, rather than because everything in it scored badly.
        // Always zero for the unmasked matcher, which excludes no pixel and so
        // carries no such evidence to weigh.
        uint64 contrastRefusedCandidates{};
    };

    class GrayImage;

    [[nodiscard]]
    auto matchTemplateSad(
        GrayImage const& haystack,
        GrayImage const& templateImage,
        PixelRect roi
    ) -> Result<std::optional<SadMatch>>;

    [[nodiscard]]
    auto matchTemplateSad(
        GrayImage const& haystack,
        GrayImage const& templateImage,
        PixelRect roi,
        uint64 maximumPixelComparisons,
        SadSearchPoll const& poll
    ) -> Result<SadSearchReport>;

    // Matches templateImage while weighting each of its pixels by the parallel
    // templateMask plane, which must have the template's exact extent. A mask
    // byte of 255 counts its pixel in full, 0 excludes it, and an intermediate
    // value is a partial weight, which is what an antialiased glyph edge needs.
    // A template PNG's alpha channel is that plane.
    //
    // The reported score normalizes by the weight actually summed:
    //
    //     score = templatePixels * sum(weight * |haystack - template|)
    //             / sum(weight)
    //
    // where templatePixels is the template's full rectangle. A mask covering a
    // tenth of its rectangle therefore stays on the same scale as one covering
    // all of it, and both stay on the scale existing unmasked thresholds use. A
    // fully opaque mask reproduces the unmasked match, score and comparison
    // count exactly, because the constant 255 cancels out of the quotient and
    // out of every pruning comparison. Truncating division rounds the quotient
    // down. A mask whose weights sum to zero selects nothing and is rejected.
    //
    // A candidate must also CONTRAST with the glyph outside the mask before it
    // is scored at all; see GrayImage::MaskContrastProbe for the rule and why a
    // masked score alone is not enough to call a template present.
    [[nodiscard]]
    auto matchTemplateSad(
        GrayImage const& haystack,
        GrayImage const& templateImage,
        GrayImage const& templateMask,
        PixelRect roi
    ) -> Result<std::optional<SadMatch>>;

    [[nodiscard]]
    auto matchTemplateSad(
        GrayImage const& haystack,
        GrayImage const& templateImage,
        GrayImage const& templateMask,
        PixelRect roi,
        uint64 maximumPixelComparisons,
        SadSearchPoll const& poll
    ) -> Result<SadSearchReport>;

    // A read-only Gray8 view. The backing storage must outlive this object and
    // every matcher call that uses it.
    class GrayImage final
    {
        // A candidate whose surround carried no negative evidence, refused
        // before it was scored. It is an outcome rather than a score because a
        // refused candidate has no score: the comparison that would have
        // produced one was never run.
        struct ContrastRefused final
        {
        };

        // The negative evidence a mask carries, taken once per search.
        //
        // A masked template states which of its pixels are the glyph and has
        // never stated anything about the rest of its rectangle. So a mask that
        // keeps a white tick and drops the button around it scores perfectly
        // against any large enough field of white: every pixel the score reads
        // is white on both sides. The mask that makes the template clean throws
        // away the one piece of evidence that would have told the two apart.
        // This is that evidence, sampled back.
        //
        // THE RULE. Let w be the mask weight at a pixel and c = 255 - w the
        // weight the mask left there, and let `ink` be the mask-weighted mean
        // grey of the template, which is the glyph the mask selected. At a fixed
        // set of positions where c is non-zero, a candidate must sit at least
        // half as far from `ink`, weighted by c, as the template's own pixels
        // there do:
        //
        //     sum(c * |haystack - ink|) >= sum(c * |template - ink|) / 2
        //
        // A candidate that does not is refused rather than scored, because its
        // mask separated nothing. The right-hand side is the template's own
        // measurement of how much its crop's surround differed from its glyph,
        // so the requirement is scaled by evidence the project supplied rather
        // than by a contrast level this module invented; a template whose own
        // surround looks like its glyph demands correspondingly little.
        //
        // Half is measured, not picked. Over the four colour-keyed templates and
        // eight 1600x900 frames of the consuming project's battle corpus, a true
        // hit reproduced 0.980 to 1.147 of its template's own contrast, while
        // the plain field of the glyph's own colour that scores best against
        // each template reproduced 0.011 to 0.082 of it. One half is a factor of
        // two below every true hit and a factor of six above every plain field.
        //
        // Both sides are integer sums over the same sample set, so the
        // comparison needs no division and no floating point at the candidate
        // and reproduces byte for byte on every host. That matters because a
        // match reaches durable records.
        class MaskContrastProbe final
        {
            // One sampled position, as an offset inside the template rectangle,
            // with the weight the mask left there and how far the template's own
            // pixel sits from the glyph.
            struct Sample final
            {
                std::size_t offsetX{};
                std::size_t offsetY{};
                uint64      complementWeight{};
            };

            std::vector<Sample> m_samples{};
            uint64              m_ink{};
            uint64              m_requiredContrast{};

        public:
            // An empty probe: no sampled position, and nothing required. It is
            // what an unmasked search carries, because a template that excludes
            // no pixel has no complement to sample and so nothing to refuse.
            MaskContrastProbe() = default;

            // `templateMask` must have `templateImage`'s exact extent and a
            // non-zero weight sum; the masked search checks both before it gets
            // here. Neither view is retained: every byte the probe needs is
            // copied into it.
            [[nodiscard]]
            static auto create(
                GrayImage const& templateImage,
                GrayImage const& templateMask
            ) -> MaskContrastProbe;

            [[nodiscard]]
            auto samples() const noexcept UF_LIFETIME_BOUND
                -> std::span<Sample const>
            {
                return m_samples;
            }

            [[nodiscard]] auto ink() const noexcept -> uint64 { return m_ink; }

            [[nodiscard]]
            auto requiredContrast() const noexcept -> uint64
            {
                return m_requiredContrast;
            }
        };

        using CandidateOutcome = std::variant<
            uint64,
            ContrastRefused,
            SadSearchStopReason
        >;

        struct CandidateReport final
        {
            CandidateOutcome outcome{};
            uint64           completedPixelComparisons{};
        };

        friend auto matchTemplateSad(
            GrayImage const& haystack,
            GrayImage const& templateImage,
            PixelRect roi,
            uint64 maximumPixelComparisons,
            SadSearchPoll const& poll
        ) -> Result<SadSearchReport>;

        friend auto matchTemplateSad(
            GrayImage const& haystack,
            GrayImage const& templateImage,
            GrayImage const& templateMask,
            PixelRect roi,
            uint64 maximumPixelComparisons,
            SadSearchPoll const& poll
        ) -> Result<SadSearchReport>;

        std::span<std::byte const> m_data;
        uint32                     m_width;
        uint32                     m_height;
        std::size_t                m_stride;

        constexpr GrayImage(
            std::span<std::byte const> data,
            uint32 width,
            uint32 height,
            std::size_t stride
        ) noexcept
            : m_data{data}
            , m_width{width}
            , m_height{height}
            , m_stride{stride}
        {
        }

        [[nodiscard]]
        auto rowSegment(
            std::size_t y,
            std::size_t x,
            std::size_t width
        ) const noexcept UF_LIFETIME_BOUND -> std::optional<std::span<std::byte const>>;

        // Sums every pixel of this plane. A mask plane's sum is the weight its
        // score must be normalized by.
        [[nodiscard]]
        auto weightSum() const noexcept -> uint64;

        // p_templateMask is an optional observation of a plane with the
        // template's extent; without one every pixel carries weight one, which
        // makes the accumulated sum the plain SAD.
        //
        // `probe` is walked BEFORE the score, so a candidate whose surround
        // carries no contrast costs its sample count and nothing more, and
        // `best` keeps meaning the best score a candidate was actually allowed
        // to hold -- a refused candidate must not tighten the pruning bound, or
        // it would prune the weaker true hit it was standing in front of.
        [[nodiscard]]
        auto candidateSad(
            GrayImage const& templateImage,
            GrayImage const* p_templateMask,
            MaskContrastProbe const& probe,
            std::size_t candidateX,
            std::size_t candidateY,
            uint64 best,
            uint64 maximumPixelComparisons,
            uint64 completedPixelComparisons,
            SadSearchPoll const& poll
        ) const -> CandidateReport;

        [[nodiscard]]
        auto search(
            GrayImage const& templateImage,
            GrayImage const* p_templateMask,
            PixelRect roi,
            uint64 maximumPixelComparisons,
            SadSearchPoll const& poll
        ) const -> Result<SadSearchReport>;

    public:
        [[nodiscard]]
        static auto create(
            std::span<std::byte const> data UF_LIFETIME_BOUND,
            uint32 width,
            uint32 height,
            std::size_t stride
        ) -> Result<GrayImage>;

        [[nodiscard]] constexpr auto width() const noexcept -> uint32 { return m_width; }
        [[nodiscard]] constexpr auto height() const noexcept -> uint32 { return m_height; }
        [[nodiscard]] constexpr auto stride() const noexcept -> std::size_t { return m_stride; }
    };

    [[nodiscard]]
    auto bgra8ToGray8(
        std::span<std::byte const> bgra,
        uint32 width,
        uint32 height,
        std::size_t stride
    ) -> Result<std::vector<std::byte>>;

    // Extracts the alpha channel as a tightly packed Gray8 shaped plane, which
    // is the mask the masked matcher consumes.
    [[nodiscard]]
    auto bgra8ToAlpha8(
        std::span<std::byte const> bgra,
        uint32 width,
        uint32 height,
        std::size_t stride
    ) -> Result<std::vector<std::byte>>;
}
