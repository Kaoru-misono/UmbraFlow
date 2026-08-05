#include "frame-analysis.hpp"

#include "bgra-image.hpp"
#include "sad.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/safety/checked-access.hpp>
#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace uf
{
    namespace
    {
        // A window that is still splitting after this many alternating row and
        // column cuts is noise rather than layout. The cap is what keeps region
        // building bounded: each cut costs one pass over its own window, and
        // without it a mask of alternating stable and empty rows would cost one
        // pass per row.
        constexpr auto k_maximumRegionSplitDepth = uint32{16};

        constexpr auto k_stableMaskWeight = std::byte{255};

        [[nodiscard]]
        auto validateFrames(
            std::span<BgraImage const> frames,
            PixelRect const& rect
        ) -> Status
        {
            if (frames.size() < 2)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    std::format(
                        "frame analysis needs at least two frames, got {}",
                        frames.size()
                    )
                );
            }

            for (auto const& frame : frames)
            {
                UF_TRY(rect.ensureWithinExtent(frame.width(), frame.height()));
            }

            return {};
        }

        [[nodiscard]]
        auto rectPixelCount(PixelRect const& rect) -> Result<uint64>
        {
            auto const total = checkedMultiply(
                uint64{rect.width()},
                uint64{rect.height()}
            );
            if (!total)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    std::format(
                        "rect {}x{} is too large to analyse",
                        rect.width(),
                        rect.height()
                    )
                );
            }

            return *total;
        }

        [[nodiscard]]
        auto planeOffset(
            uint32 x,
            uint32 y,
            uint32 width
        ) noexcept -> std::size_t
        {
            auto const rowStart = checkedMultiply(std::size_t{y}, std::size_t{width});
            UF_CHECK(rowStart.has_value());
            auto const offset = checkedAdd(*rowStart, std::size_t{x});
            UF_CHECK(offset.has_value());
            return *offset;
        }

        struct RectScan final
        {
            std::optional<SadSearchStopReason> stop{};
            uint64                             completedPixelVisits{};
        };

        // Walks the rect once, handing each pixel's grey spread across the
        // frames to the visitor. The budget and the poll count one visit per
        // pixel per frame, which is the term that grows with the frame count; a
        // probe's single extra colour read of the first frame rides along
        // uncounted.
        //
        // An implementation-only template with a closed instantiation set: the
        // two visitors below are the only ones.
        template <typename Visitor>
        [[nodiscard]]
        auto scanRect(
            std::span<BgraImage const> frames,
            PixelRect const& rect,
            uint64 maximumPixelVisits,
            SadSearchPoll const& poll,
            Visitor const& visitor
        ) -> RectScan
        {
            UF_CHECK(poll != nullptr);
            UF_CHECK(!frames.empty());
            auto visits = uint64{0};

            for (auto y = rect.y(); y < rect.bottom(); ++y)
            {
                for (auto x = rect.x(); x < rect.right(); ++x)
                {
                    auto lowest  = std::numeric_limits<uint32>::max();
                    auto highest = uint32{0};

                    for (auto const& frame : frames)
                    {
                        if (visits == maximumPixelVisits)
                        {
                            return RectScan{
                                .stop                 = SadSearchStopReason::ComparisonBudgetExhausted,
                                .completedPixelVisits = visits,
                            };
                        }
                        if (visits % k_sadSearchPollIntervalComparisons == 0)
                        {
                            switch (poll())
                            {
                            case SadSearchControl::Continue:
                                break;
                            case SadSearchControl::Cancelled:
                                return RectScan{
                                    .stop                 = SadSearchStopReason::Cancelled,
                                    .completedPixelVisits = visits,
                                };
                            case SadSearchControl::TimedOut:
                                return RectScan{
                                    .stop                 = SadSearchStopReason::TimedOut,
                                    .completedPixelVisits = visits,
                                };
                            default:
                                UF_UNREACHABLE_MSG("Unknown SadSearchControl value");
                            }
                        }
                        ++visits;

                        auto const gray = uint32{frame.grayAt(x, y)};
                        lowest  = std::min(lowest, gray);
                        highest = std::max(highest, gray);
                    }

                    visitor(x, y, highest - lowest);
                }
            }

            return RectScan{
                .stop                 = std::nullopt,
                .completedPixelVisits = visits,
            };
        }

        // Rect-relative, with both edges inclusive, which is what a projection
        // run naturally produces.
        struct MaskWindow final
        {
            uint32 left{};
            uint32 top{};
            uint32 right{};
            uint32 bottom{};
        };

        struct ProfileRun final
        {
            uint32 first{};
            uint32 last{};
        };

        // Maximal runs of non-empty entries, separated by runs of at least
        // minimumGap empty entries. A shorter empty run stays inside its run,
        // which is what keeps the separate strokes of one glyph together.
        [[nodiscard]]
        auto projectionRuns(
            std::span<uint32 const> profile,
            uint32 minimumGap
        ) -> std::vector<ProfileRun>
        {
            UF_CHECK(minimumGap > 0);
            auto runs  = std::vector<ProfileRun>{};
            auto index = std::size_t{0};

            while (index < profile.size())
            {
                if (checkedAt(profile, index) == 0)
                {
                    ++index;
                    continue;
                }

                auto last  = index;
                auto probe = index;
                while (probe < profile.size())
                {
                    if (checkedAt(profile, probe) != 0)
                    {
                        last = probe;
                        ++probe;
                        continue;
                    }

                    auto gapEnd = probe;
                    while (
                        gapEnd < profile.size()
                        && checkedAt(profile, gapEnd) == 0
                    )
                    {
                        ++gapEnd;
                    }
                    if (gapEnd - probe >= std::size_t{minimumGap})
                    {
                        break;
                    }
                    probe = gapEnd;
                }

                auto const firstIndex = checkedCast<uint32>(index);
                auto const lastIndex  = checkedCast<uint32>(last);
                UF_CHECK(firstIndex.has_value());
                UF_CHECK(lastIndex.has_value());
                runs.emplace_back(
                    ProfileRun{.first = *firstIndex, .last = *lastIndex}
                );
                index = last + 1U;
            }

            return runs;
        }

        // The stable mask is rect-relative, row major and tightly packed, so a
        // window's projections are plain counts over its own sub-rectangle.
        class StableMask final
        {
            std::span<std::byte const> m_mask;
            uint32                     m_width;

        public:
            constexpr StableMask(
                std::span<std::byte const> mask UF_LIFETIME_BOUND,
                uint32 width
            ) noexcept
                : m_mask{mask}
                , m_width{width}
            {
            }

            [[nodiscard]]
            auto isStable(uint32 x, uint32 y) const noexcept -> bool
            {
                return checkedAt(m_mask, planeOffset(x, y, m_width)) != std::byte{0};
            }

            [[nodiscard]]
            auto rowProfile(MaskWindow const& window) const -> std::vector<uint32>
            {
                auto counts = std::vector<uint32>(window.bottom - window.top + 1U, 0U);
                for (auto y = window.top; y <= window.bottom; ++y)
                {
                    for (auto x = window.left; x <= window.right; ++x)
                    {
                        if (isStable(x, y))
                        {
                            ++checkedAt(counts, y - window.top);
                        }
                    }
                }
                return counts;
            }

            [[nodiscard]]
            auto columnProfile(MaskWindow const& window) const -> std::vector<uint32>
            {
                auto counts = std::vector<uint32>(window.right - window.left + 1U, 0U);
                for (auto y = window.top; y <= window.bottom; ++y)
                {
                    for (auto x = window.left; x <= window.right; ++x)
                    {
                        if (isStable(x, y))
                        {
                            ++checkedAt(counts, x - window.left);
                        }
                    }
                }
                return counts;
            }

            [[nodiscard]]
            auto stableCount(MaskWindow const& window) const -> uint64
            {
                auto total = uint64{0};
                for (auto y = window.top; y <= window.bottom; ++y)
                {
                    for (auto x = window.left; x <= window.right; ++x)
                    {
                        total += isStable(x, y) ? 1U : 0U;
                    }
                }
                return total;
            }
        };

        struct SplitTask final
        {
            MaskWindow window{};

            // How many axes in a row have failed to separate anything. Two
            // means neither axis has a gap left, so the window is a region.
            uint32 settledAxes{};

            // Cuts attempted on the way here, which the cap above bounds.
            uint32 depth{};

            bool cutRows{};
        };

        // Recursive projection cut: split the window's rows on their gaps, then
        // each band's columns on theirs, then those pieces' rows again, until
        // neither axis separates anything. Tightening to the run at every step
        // is what makes the emitted bounds hug their content instead of
        // inheriting the analysed rect's edges.
        [[nodiscard]]
        auto cutWindows(
            StableMask const& mask,
            MaskWindow const& bounds,
            uint32 minimumGap
        ) -> std::vector<MaskWindow>
        {
            auto emitted = std::vector<MaskWindow>{};
            auto pending = std::vector<SplitTask>{};
            pending.emplace_back(
                SplitTask{
                    .window      = bounds,
                    .settledAxes = 0,
                    .depth       = 0,
                    .cutRows     = true,
                }
            );

            while (!pending.empty())
            {
                auto const task = pending.back();
                pending.pop_back();

                auto const profile = (
                    task.cutRows
                        ? mask.rowProfile(task.window)
                        : mask.columnProfile(task.window)
                );
                auto const runs = projectionRuns(profile, minimumGap);
                if (runs.empty())
                {
                    continue;
                }

                auto const origin      = task.cutRows ? task.window.top : task.window.left;
                auto const settledAxes = runs.size() == 1 ? task.settledAxes + 1U : uint32{0};
                auto const depth       = task.depth + 1U;
                auto const finished    = (
                    settledAxes >= 2U
                    || depth >= k_maximumRegionSplitDepth
                );

                for (auto const& run : runs)
                {
                    auto piece = task.window;
                    if (task.cutRows)
                    {
                        piece.top    = origin + run.first;
                        piece.bottom = origin + run.last;
                    }
                    else
                    {
                        piece.left  = origin + run.first;
                        piece.right = origin + run.last;
                    }

                    if (finished)
                    {
                        emitted.emplace_back(piece);
                        continue;
                    }
                    pending.emplace_back(
                        SplitTask{
                            .window      = piece,
                            .settledAxes = settledAxes,
                            .depth       = depth,
                            .cutRows     = !task.cutRows,
                        }
                    );
                }
            }

            return emitted;
        }

        [[nodiscard]]
        auto firstNonEmpty(std::span<uint32 const> profile) noexcept -> std::optional<uint32>
        {
            for (auto index = std::size_t{0}; index < profile.size(); ++index)
            {
                if (checkedAt(profile, index) != 0)
                {
                    auto const found = checkedCast<uint32>(index);
                    UF_CHECK(found.has_value());
                    return *found;
                }
            }
            return std::nullopt;
        }

        [[nodiscard]]
        auto lastNonEmpty(std::span<uint32 const> profile) noexcept -> std::optional<uint32>
        {
            auto found = std::optional<uint32>{};
            for (auto index = std::size_t{0}; index < profile.size(); ++index)
            {
                if (checkedAt(profile, index) != 0)
                {
                    auto const at = checkedCast<uint32>(index);
                    UF_CHECK(at.has_value());
                    found = *at;
                }
            }
            return found;
        }

        [[nodiscard]]
        auto buildRegions(
            StableMask const& mask,
            PixelRect const& rect,
            std::span<uint32 const> rowProfile,
            std::span<uint32 const> columnProfile,
            uint32 minimumGap
        ) -> Result<std::vector<StableRegion>>
        {
            auto const top    = firstNonEmpty(rowProfile);
            auto const bottom = lastNonEmpty(rowProfile);
            auto const left   = firstNonEmpty(columnProfile);
            auto const right  = lastNonEmpty(columnProfile);
            if (!top)
            {
                return std::vector<StableRegion>{};
            }
            UF_CHECK(bottom.has_value());
            UF_CHECK(left.has_value());
            UF_CHECK(right.has_value());

            auto const bounds = MaskWindow{
                .left   = *left,
                .top    = *top,
                .right  = *right,
                .bottom = *bottom,
            };
            auto const windows = (
                minimumGap == 0
                    ? std::vector<MaskWindow>{bounds}
                    : cutWindows(mask, bounds, minimumGap)
            );

            auto regions = std::vector<StableRegion>{};
            regions.reserve(windows.size());
            for (auto const& window : windows)
            {
                UF_TRY_VALUE(
                    frameBounds,
                    PixelRect::create(
                        rect.x() + window.left,
                        rect.y() + window.top,
                        window.right - window.left + 1U,
                        window.bottom - window.top + 1U
                    )
                );
                regions.emplace_back(
                    StableRegion{
                        .bounds       = frameBounds,
                        .stablePixels = mask.stableCount(window),
                    }
                );
            }

            std::ranges::sort(
                regions,
                [](StableRegion const& left, StableRegion const& right) noexcept -> bool
                {
                    if (left.bounds.y() != right.bounds.y())
                    {
                        return left.bounds.y() < right.bounds.y();
                    }
                    return left.bounds.x() < right.bounds.x();
                }
            );
            return regions;
        }

        [[nodiscard]]
        auto continueScanning() -> SadSearchPoll
        {
            return SadSearchPoll{
                []() noexcept -> SadSearchControl
                {
                    return SadSearchControl::Continue;
                }
            };
        }

        [[nodiscard]]
        auto meanOf(uint64 total, uint64 count) noexcept -> double
        {
            if (count == 0)
            {
                return 0.0;
            }
            return static_cast<double>(total) / static_cast<double>(count);
        }

        // The weight one pixel earns under a whole key, which is `colourKeyAlpha`
        // when the key names what to keep and its complement when the key names
        // what to remove. The probe, the mask cutter and the census grid all go
        // through here, so no two of them can come to disagree about which
        // pixels a key takes -- and `colourKeyAlpha` itself stays the one rule
        // about nearness to a colour, with no opinion about what nearness is
        // worth.
        [[nodiscard]]
        auto keyedWeight(
            Bgra8Pixel pixel,
            uint8 keyRed,
            uint8 keyGreen,
            uint8 keyBlue,
            uint32 tolerance,
            bool keyRemoves
        ) noexcept -> uint8
        {
            auto const alpha = colourKeyAlpha(
                pixel,
                keyRed,
                keyGreen,
                keyBlue,
                tolerance
            );
            return keyRemoves ? static_cast<uint8>(255U - alpha) : alpha;
        }

        // The spec-shaped spellings of the rule above. They exist so a call site
        // reads as "what this key takes here" rather than as five fields unpacked
        // by hand, which is where a transcription slip would hide.
        [[nodiscard]]
        auto keyedWeight(Bgra8Pixel pixel, ColourProbeSpec const& spec) noexcept
            -> uint8
        {
            return keyedWeight(
                pixel,
                spec.keyRed,
                spec.keyGreen,
                spec.keyBlue,
                spec.tolerance,
                spec.keyRemoves
            );
        }

        [[nodiscard]]
        auto keyedWeight(Bgra8Pixel pixel, ColourGridSpec const& spec) noexcept
            -> uint8
        {
            return keyedWeight(
                pixel,
                spec.keyRed,
                spec.keyGreen,
                spec.keyBlue,
                spec.tolerance,
                spec.keyRemoves
            );
        }

        // How many cells of `size` cover `extent`, the last one partial. The
        // rounding term is added in uint64 so an extent near the type's ceiling
        // cannot wrap it.
        [[nodiscard]]
        auto cellsCovering(uint32 extent, uint32 size) noexcept -> uint32
        {
            UF_CHECK(size > 0);
            auto const covering = (uint64{extent} + uint64{size} - 1U) / uint64{size};
            auto const narrowed = checkedCast<uint32>(covering);
            UF_CHECK(narrowed.has_value());
            return *narrowed;
        }
    }

    auto colourKeyAlpha(
        Bgra8Pixel pixel,
        uint8 keyRed,
        uint8 keyGreen,
        uint8 keyBlue,
        uint32 tolerance
    ) noexcept -> uint8
    {
        auto const spread = [](uint8 left, uint8 right) noexcept -> uint32
        {
            return left >= right
                ? uint32{left} - uint32{right}
                : uint32{right} - uint32{left};
        };
        auto const distance = (
            spread(pixel.red, keyRed)
            + spread(pixel.green, keyGreen)
            + spread(pixel.blue, keyBlue)
        );
        if (distance <= tolerance)
        {
            return uint8{255};
        }

        auto const rampEnd = tolerance * 2U;
        if (tolerance == 0U || distance >= rampEnd)
        {
            return uint8{0};
        }

        // Rounded rather than truncated, so the ramp is symmetric about its
        // midpoint. The numerator peaks at 255 * 764 and stays inside uint32.
        auto const weighted = 255U * (rampEnd - distance) + tolerance / 2U;
        auto const alpha = checkedCast<uint8>(weighted / tolerance);
        UF_CHECK(alpha.has_value());
        return *alpha;
    }

    auto analyseStability(
        std::span<BgraImage const> frames,
        StabilitySpec const& spec
    ) -> Result<StabilityReport>
    {
        UF_TRY_VALUE(
            scan,
            analyseStability(
                frames,
                spec,
                std::numeric_limits<uint64>::max(),
                continueScanning()
            )
        );
        UF_CHECK(std::holds_alternative<StabilityReport>(scan.outcome));
        return std::get<StabilityReport>(std::move(scan.outcome));
    }

    auto analyseStability(
        std::span<BgraImage const> frames,
        StabilitySpec const& spec,
        uint64 maximumPixelVisits,
        SadSearchPoll const& poll
    ) -> Result<StabilityScan>
    {
        UF_TRY(validateFrames(frames, spec.rect));
        UF_TRY_VALUE(pixels, rectPixelCount(spec.rect));

        auto const width  = spec.rect.width();
        auto const length = checkedCast<std::size_t>(pixels);
        if (!length)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format("rect of {} pixels does not fit a mask plane", pixels)
            );
        }

        auto mask          = std::vector<std::byte>(*length, std::byte{0});
        auto rowProfile    = std::vector<uint32>(spec.rect.height(), 0U);
        auto columnProfile = std::vector<uint32>(width, 0U);
        auto stablePixels  = uint64{0};
        auto spreadTotal   = uint64{0};

        auto const scan = scanRect(
            frames,
            spec.rect,
            maximumPixelVisits,
            poll,
            [&](uint32 x, uint32 y, uint32 spread) -> void
            {
                spreadTotal += spread;
                if (spread > spec.grayTolerance)
                {
                    return;
                }

                auto const column = x - spec.rect.x();
                auto const row    = y - spec.rect.y();
                checkedAt(mask, planeOffset(column, row, width)) = k_stableMaskWeight;
                ++checkedAt(rowProfile, row);
                ++checkedAt(columnProfile, column);
                ++stablePixels;
            }
        );
        if (scan.stop)
        {
            return StabilityScan{
                .outcome              = *scan.stop,
                .completedPixelVisits = scan.completedPixelVisits,
            };
        }

        UF_TRY_VALUE(
            regions,
            buildRegions(
                StableMask{mask, width},
                spec.rect,
                rowProfile,
                columnProfile,
                spec.minimumGap
            )
        );

        return StabilityScan{
            .outcome = StabilityReport{
                .stableMask    = std::move(mask),
                .rowProfile    = std::move(rowProfile),
                .columnProfile = std::move(columnProfile),
                .regions       = std::move(regions),
                .stablePixels  = stablePixels,
                .rectPixels    = pixels,

                .meanGraySpread = meanOf(spreadTotal, pixels),
            },
            .completedPixelVisits = scan.completedPixelVisits,
        };
    }

    auto probeColour(
        std::span<BgraImage const> frames,
        ColourProbeSpec const& spec
    ) -> Result<ColourProbeReport>
    {
        UF_TRY_VALUE(
            scan,
            probeColour(
                frames,
                spec,
                std::numeric_limits<uint64>::max(),
                continueScanning()
            )
        );
        UF_CHECK(std::holds_alternative<ColourProbeReport>(scan.outcome));
        return std::get<ColourProbeReport>(std::move(scan.outcome));
    }

    auto probeColour(
        std::span<BgraImage const> frames,
        ColourProbeSpec const& spec,
        uint64 maximumPixelVisits,
        SadSearchPoll const& poll
    ) -> Result<ColourProbeScan>
    {
        UF_TRY(validateFrames(frames, spec.rect));
        if (spec.tolerance > k_maximumColourKeyTolerance)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format(
                    "colour key tolerance {} exceeds {}",
                    spec.tolerance,
                    k_maximumColourKeyTolerance
                )
            );
        }
        UF_TRY_VALUE(pixels, rectPixelCount(spec.rect));

        auto const& reference = checkedAt(frames, 0);
        auto fullySelected  = uint64{0};
        auto rampSelected   = uint64{0};
        auto selectedWeight = uint64{0};
        auto weightedSpread = uint64{0};
        auto spreadTotal    = uint64{0};

        auto const scan = scanRect(
            frames,
            spec.rect,
            maximumPixelVisits,
            poll,
            [&](uint32 x, uint32 y, uint32 spread) -> void
            {
                spreadTotal += spread;

                auto const weight = keyedWeight(reference.pixelAt(x, y), spec);
                if (weight == 0)
                {
                    return;
                }

                if (weight == 255)
                {
                    ++fullySelected;
                }
                else
                {
                    ++rampSelected;
                }
                selectedWeight += weight;
                weightedSpread += uint64{weight} * uint64{spread};
            }
        );
        if (scan.stop)
        {
            return ColourProbeScan{
                .outcome              = *scan.stop,
                .completedPixelVisits = scan.completedPixelVisits,
            };
        }

        return ColourProbeScan{
            .outcome = ColourProbeReport{
                .rectPixels          = pixels,
                .fullySelectedPixels = fullySelected,
                .rampSelectedPixels  = rampSelected,
                .selectedWeight      = selectedWeight,

                .maskedMeanGraySpread = meanOf(weightedSpread, selectedWeight),
                .rectMeanGraySpread   = meanOf(spreadTotal, pixels),
            },
            .completedPixelVisits = scan.completedPixelVisits,
        };
    }

    auto censusColourGrid(
        std::span<BgraImage const> frames,
        ColourGridSpec const& spec
    ) -> Result<ColourGridReport>
    {
        UF_TRY_VALUE(
            scan,
            censusColourGrid(
                frames,
                spec,
                std::numeric_limits<uint64>::max(),
                continueScanning()
            )
        );
        UF_CHECK(std::holds_alternative<ColourGridReport>(scan.outcome));
        return std::get<ColourGridReport>(std::move(scan.outcome));
    }

    auto censusColourGrid(
        std::span<BgraImage const> frames,
        ColourGridSpec const& spec,
        uint64 maximumPixelVisits,
        SadSearchPoll const& poll
    ) -> Result<ColourGridScan>
    {
        UF_TRY(validateFrames(frames, spec.rect));
        if (spec.tolerance > k_maximumColourKeyTolerance)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format(
                    "colour key tolerance {} exceeds {}",
                    spec.tolerance,
                    k_maximumColourKeyTolerance
                )
            );
        }
        if (spec.cellWidth == 0U || spec.cellHeight == 0U)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format(
                    "a census grid cell of {}x{} covers no pixel",
                    spec.cellWidth,
                    spec.cellHeight
                )
            );
        }

        auto const columns = cellsCovering(spec.rect.width(), spec.cellWidth);
        auto const rows    = cellsCovering(spec.rect.height(), spec.cellHeight);

        // Two uint32 widened, so the product cannot overflow the wider type.
        auto const cells = uint64{columns} * uint64{rows};
        if (cells > k_maximumColourGridCells)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format(
                    "a {} by {} census grid is {} cells, beyond the {} one call "
                    "reports",
                    columns,
                    rows,
                    cells,
                    k_maximumColourGridCells
                )
            );
        }
        auto const length = checkedCast<std::size_t>(cells);
        UF_CHECK(length.has_value());

        auto const& reference = checkedAt(frames, 0);
        auto counts       = std::vector<uint64>(*length, 0U);
        auto spreadTotals = std::vector<uint64>(*length, 0U);

        auto const scan = scanRect(
            frames,
            spec.rect,
            maximumPixelVisits,
            poll,
            [&](uint32 x, uint32 y, uint32 spread) -> void
            {
                auto const column = (x - spec.rect.x()) / spec.cellWidth;
                auto const row    = (y - spec.rect.y()) / spec.cellHeight;
                auto const index  = (std::size_t{row} * columns) + column;

                checkedAt(spreadTotals, index) += spread;

                auto const weight = keyedWeight(reference.pixelAt(x, y), spec);
                if (weight == 255U)
                {
                    ++checkedAt(counts, index);
                }
            }
        );
        if (scan.stop)
        {
            return ColourGridScan{
                .outcome              = *scan.stop,
                .completedPixelVisits = scan.completedPixelVisits,
            };
        }

        // Each cell's divisor is its own pixel count, which is smaller than the
        // nominal cell on the right and bottom edges. Appending in row-major
        // order makes the size so far the index of the cell being written.
        auto means = std::vector<uint32>{};
        means.reserve(*length);
        for (auto row = uint32{0}; row < rows; ++row)
        {
            auto const top    = uint64{row} * spec.cellHeight;
            auto const height = std::min(
                uint64{spec.cellHeight},
                uint64{spec.rect.height()} - top
            );
            for (auto column = uint32{0}; column < columns; ++column)
            {
                auto const left  = uint64{column} * spec.cellWidth;
                auto const width = std::min(
                    uint64{spec.cellWidth},
                    uint64{spec.rect.width()} - left
                );

                auto const total = checkedAt(spreadTotals, means.size());
                auto const mean  = checkedCast<uint32>(total / (width * height));
                UF_CHECK(mean.has_value());
                means.emplace_back(*mean);
            }
        }

        return ColourGridScan{
            .outcome = ColourGridReport{
                .columns    = columns,
                .rows       = rows,
                .cellWidth  = spec.cellWidth,
                .cellHeight = spec.cellHeight,

                .selectedPixels = std::move(counts),
                .meanGraySpread = std::move(means),
            },
            .completedPixelVisits = scan.completedPixelVisits,
        };
    }

    auto censusColours(
        BgraImage const& frame,
        ColourCensusSpec const& spec
    ) -> Result<ColourCensusReport>
    {
        UF_TRY(spec.rect.ensureWithinExtent(frame.width(), frame.height()));
        UF_TRY_VALUE(pixels, rectPixelCount(spec.rect));

        // Keyed by the packed blue, green, red value, so the ordered traversal
        // below is exactly the tie-break the report documents.
        auto counts = std::map<uint32, uint64>{};
        for (auto y = spec.rect.y(); y < spec.rect.bottom(); ++y)
        {
            for (auto x = spec.rect.x(); x < spec.rect.right(); ++x)
            {
                auto const pixel = frame.pixelAt(x, y);
                auto const packed = (
                    (uint32{pixel.blue} << 16U)
                    | (uint32{pixel.green} << 8U)
                    | uint32{pixel.red}
                );
                ++counts[packed];
            }
        }

        auto ranked = std::vector<ColourCount>{};
        ranked.reserve(counts.size());
        for (auto const& [packed, count] : counts)
        {
            ranked.emplace_back(
                ColourCount{
                    .blue  = static_cast<uint8>((packed >> 16U) & 0xFFU),
                    .green = static_cast<uint8>((packed >> 8U) & 0xFFU),
                    .red   = static_cast<uint8>(packed & 0xFFU),

                    .count = count,
                }
            );
        }
        std::ranges::stable_sort(
            ranked,
            [](ColourCount const& left, ColourCount const& right) noexcept -> bool
            {
                return left.count > right.count;
            }
        );
        if (ranked.size() > spec.maximumEntries)
        {
            ranked.resize(spec.maximumEntries);
        }

        return ColourCensusReport{
            .rectPixels      = pixels,
            .distinctColours = counts.size(),
            .dominant        = std::move(ranked),
        };
    }

    auto maskColourKey(
        BgraImage const& frame,
        ColourProbeSpec const& spec
    ) -> Result<ColourKeyMask>
    {
        UF_TRY(spec.rect.ensureWithinExtent(frame.width(), frame.height()));
        if (spec.tolerance > k_maximumColourKeyTolerance)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format(
                    "colour key tolerance {} exceeds {}",
                    spec.tolerance,
                    k_maximumColourKeyTolerance
                )
            );
        }
        UF_TRY_VALUE(pixels, rectPixelCount(spec.rect));

        auto weights = std::vector<std::byte>{};
        auto const planeSize = checkedCast<std::size_t>(pixels);
        if (!planeSize)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format(
                    "rect {}x{} is too large to mask",
                    spec.rect.width(),
                    spec.rect.height()
                )
            );
        }
        weights.reserve(*planeSize);

        auto fullySelected = uint64{0};
        auto rampSelected  = uint64{0};
        for (auto y = spec.rect.y(); y < spec.rect.bottom(); ++y)
        {
            for (auto x = spec.rect.x(); x < spec.rect.right(); ++x)
            {
                auto const weight = keyedWeight(frame.pixelAt(x, y), spec);
                weights.emplace_back(std::byte{weight});
                if (weight == 255U)
                {
                    ++fullySelected;
                }
                else if (weight != 0U)
                {
                    ++rampSelected;
                }
            }
        }

        return ColourKeyMask{
            .weights             = std::move(weights),
            .rectPixels          = pixels,
            .fullySelectedPixels = fullySelected,
            .rampSelectedPixels  = rampSelected,
        };
    }
}
