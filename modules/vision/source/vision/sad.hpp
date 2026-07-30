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
        // the comparisons that trigger pruning or an exact-match return. A budget
        // or poll stop excludes the comparison that was not executed. Valid for
        // every outcome and starts at zero for each matcher call.
        uint64 completedPixelComparisons{};
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
        using CandidateOutcome = std::variant<
            uint64,
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
        [[nodiscard]]
        auto candidateSad(
            GrayImage const& templateImage,
            GrayImage const* p_templateMask,
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
