#include <task/task-context.hpp>

#include <task/cycle-answers.hpp>
#include <task/cycle-ledger.hpp>
#include <task/pixel-probe.hpp>
#include <task/project-files.hpp>
#include <task/template-store.hpp>

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/safety/checked-access.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/time/poll-sleep.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <domain/error.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <engine/session.hpp>

#include <image/pixels.hpp>
#include <image/png.hpp>

#include <trace/event.hpp>
#include <trace/recorder.hpp>

#include <vision/bgra-image.hpp>
#include <vision/frame-analysis.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::task
{
    namespace
    {
        [[nodiscard]] auto traceSchemaHash() -> ContentHash
        {
            auto const parsed = ContentHash::parse(
                std::format("sha256:{}", trace::k_traceSchemaHash)
            );
            UF_CHECK(parsed.has_value());
            return *parsed;
        }

        // The exploration front end's own record of one act it delivered.
        //
        // The engine writes an engine.*_delivered line for the same act, and
        // this is not that line under a second name. The engine's says a
        // coordinate reached the target and carries it in CLIENT space after the
        // transform; this one says an exploration chunk chose it, and carries
        // what the chunk actually wrote, in the frame pixels it wrote them in.
        // On this stream that difference is the whole audit: an
        // engine.action_delivered line claims a Binding authorised the act, and
        // on an exploration stream nothing did -- there is no model yet, which
        // is why the chunk is running (engine/session.hpp, clickPoint).
        //
        // `verb` is the act's own spelling rather than one shared event type
        // with a kind field, so a reader grepping the stream for a drag finds
        // drags and a schema that gains an act cannot acquire a name by
        // defaulting.
        [[nodiscard]]
        auto annotationActionEvent(
            std::string_view verb,
            FrameId frame,
            std::vector<trace::TraceField> fields
        ) -> trace::TraceEventSpec
        {
            return trace::TraceEventSpec{
                .eventType = std::format("annotation.{}_delivered", verb),
                .audit     = trace::AuditMetadata{
                    .actor = "annotation",
                    .references = {
                        trace::TraceReference{
                            .type = "frame",
                            .id   = std::to_string(frame.value()),
                        },
                    },
                },
                .payload = trace::TypedTracePayload{
                    .schemaHash = traceSchemaHash(),
                    .fields     = std::move(fields),
                },
            };
        }

        // A bounded, non-negative duration as the whole milliseconds a trace
        // field carries. Every caller has already refused a negative value and
        // capped the duration far below any overflow, so the count is exact.
        [[nodiscard]]
        auto traceMillis(MonotonicInstant::Duration duration) -> uint64
        {
            return static_cast<uint64>(
                std::chrono::duration_cast<std::chrono::milliseconds>(duration)
                    .count()
            );
        }

        // The two fields every coordinate-naming exploration act opens its trace
        // line with, in the frame-pixel vocabulary engine.action_authorized
        // already uses for the same numbers.
        [[nodiscard]] auto pointFields(PixelPoint point) -> std::vector<trace::TraceField>
        {
            return {
                trace::TraceField{.name = "pixel_x", .value = uint64{point.x()}},
                trace::TraceField{.name = "pixel_y", .value = uint64{point.y()}},
            };
        }

        // One crop's mask before and after it is reduced to what the caller sees:
        // the plane that becomes the PNG's alpha channel, and the counts that go
        // back to the agent. They travel together because they are one walk over
        // one rectangle, and splitting them would be two walks that could disagree.
        struct MeasuredCropMask final
        {
            std::vector<std::byte> weights{};
            TaskContext::CropMask  reported{};
        };

        // Why `selected` of `total` looks like a mask that cannot measure
        // anything, or an empty string when it does not. Both ends and the
        // reasoning are at k_minimumUsefulMaskPixels; only the wording is here.
        //
        // `removes` moves the ceiling and nothing else: the floor is the same
        // hazard whichever way the key was read, while a large mask means a solid
        // patch of one colour in one direction and an all but unmasked template
        // in the other.
        [[nodiscard]]
        auto maskWarning(uint64 selected, uint64 total, bool removes)
            -> std::string
        {
            if (selected < k_minimumUsefulMaskPixels)
            {
                return std::format(
                    "this key selects {} of {} pixels, under the {} a mask needs "
                    "to measure anything: a handful of saturated pixels finds "
                    "some offset in any busy search region, so the element hits "
                    "every screen and scores near zero on all of them",
                    selected,
                    total,
                    k_minimumUsefulMaskPixels
                );
            }

            auto const scaled = checkedMultiply(selected, uint64{10'000});
            UF_CHECK(scaled.has_value());
            auto const shareBp = *scaled / total;
            auto const ceiling = removes
                ? k_maximumRemovedMaskShareBp
                : k_maximumUsefulMaskShareBp;
            if (shareBp >= ceiling)
            {
                return std::format(
                    "this key selects {} of {} pixels ({}.{:02} percent), at or "
                    "above the {} basis points where a mask stops distinguishing "
                    "anything: {}",
                    selected,
                    total,
                    shareBp / 100U,
                    shareBp % 100U,
                    ceiling,
                    removes
                        ? "this key names the colour to REMOVE, and it removed "
                          "almost nothing, so the template is very nearly the "
                          "unmasked rectangle it was cut to avoid being"
                        : "a key takes one colour by construction, so a mask "
                          "this large is a solid patch that any patch of the "
                          "same colour matches"
                );
            }
            return {};
        }

        // The weights `key` hands out over `region`, with the counts that go back
        // to the agent. `rect` is the rectangle in FRAME coordinates, carried only
        // so a refusal can name what the author drew rather than the crop-relative
        // box they never typed.
        [[nodiscard]]
        auto measureCropMask(
            engine::CroppedRegion const& region,
            PixelRect rect,
            ProbeColourKey const& key
        ) -> Result<MeasuredCropMask>
        {
            if (key.tolerance > k_maximumColourKeyTolerance)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "a colour tolerance of {} is beyond {}, the widest the "
                        "summed per-channel distance can express",
                        key.tolerance,
                        k_maximumColourKeyTolerance
                    )
                );
            }

            auto const widthSize = checkedCast<std::size_t>(region.width);
            if (!widthSize)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "the cropped region's width is beyond range"
                );
            }
            UF_TRY_VALUE(
                view,
                BgraImage::create(
                    region.pixels,
                    region.width,
                    region.height,
                    *widthSize * 4U
                )
            );
            UF_TRY_VALUE(
                cropRect,
                PixelRect::create(0, 0, region.width, region.height)
            );
            UF_TRY_VALUE(
                measured,
                maskColourKey(
                    view,
                    ColourProbeSpec{
                        .rect       = cropRect,
                        .keyRed     = key.red,
                        .keyGreen   = key.green,
                        .keyBlue    = key.blue,
                        .tolerance  = key.tolerance,
                        .keyRemoves = key.removes,
                    }
                )
            );

            auto const selected =
                measured.fullySelectedPixels + measured.rampSelectedPixels;
            if (selected == 0U)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "the colour key {},{},{} at tolerance {} selects no "
                        "pixel of the {}x{}+{}+{} region; the template it would "
                        "cut is fully transparent and every match of it aborts, "
                        "so key on the colour the glyph really is or widen the "
                        "tolerance",
                        key.red,
                        key.green,
                        key.blue,
                        key.tolerance,
                        rect.width(),
                        rect.height(),
                        rect.x(),
                        rect.y()
                    )
                );
            }

            return MeasuredCropMask{
                .weights  = std::move(measured.weights),
                .reported = TaskContext::CropMask{
                    .key                = key,
                    .rectPixels         = measured.rectPixels,
                    .selectedPixels     = measured.fullySelectedPixels,
                    .rampSelectedPixels = measured.rampSelectedPixels,
                    .warning            = maskWarning(
                        measured.fullySelectedPixels,
                        measured.rectPixels,
                        key.removes
                    ),
                },
            };
        }

        // What a census grid was asked for, checked against what the host can
        // answer. Every number reaching it came from a project script, so each
        // refusal is a Tier B InvalidResource the author can catch; vision
        // refuses the same three as invariants, because its own caller is host
        // C++ rather than a script.
        //
        // The cell division is repeated here rather than shared, for the reason
        // model.luau repeats the tolerance ceiling: rounding a division up is
        // arithmetic, while the CEILING it is measured against is policy and is
        // imported.
        [[nodiscard]]
        auto validateCensusGrid(
            PixelRect rect,
            uint32 cellWidth,
            uint32 cellHeight,
            ProbeColourKey const& key
        ) -> Status
        {
            if (key.tolerance > k_maximumColourKeyTolerance)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "a colour tolerance of {} is beyond {}, the widest the "
                        "summed per-channel distance can express",
                        key.tolerance,
                        k_maximumColourKeyTolerance
                    )
                );
            }
            if (cellWidth == 0U || cellHeight == 0U)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "a census grid cell of {}x{} covers no pixel; both sides "
                        "must be at least one",
                        cellWidth,
                        cellHeight
                    )
                );
            }

            auto const columns = (uint64{rect.width()} + cellWidth - 1U) / cellWidth;
            auto const rows    = (uint64{rect.height()} + cellHeight - 1U) / cellHeight;
            auto const cells   = columns * rows;
            if (cells > k_maximumColourGridCells)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "a {}x{} region in {}x{} cells is a {} by {} grid of {} "
                        "cells, beyond the {} one call reports; use larger cells "
                        "or a smaller region",
                        rect.width(),
                        rect.height(),
                        cellWidth,
                        cellHeight,
                        columns,
                        rows,
                        cells,
                        k_maximumColourGridCells
                    )
                );
            }
            return {};
        }

        // `rgba` with every pixel's alpha replaced by the weight `weights` gives
        // it, which is the whole of what makes a crop a masked template:
        // decodeTemplateImage reads that channel back as the matcher's mask plane.
        // The buffer is taken by value and handed back rather than written through
        // a reference, so nothing here is an output parameter and the caller's
        // move is visible at the call site.
        [[nodiscard]]
        auto applyMaskAlpha(
            std::vector<std::byte> rgba,
            std::span<std::byte const> weights
        ) -> Result<std::vector<std::byte>>
        {
            auto const expected = checkedMultiply(weights.size(), std::size_t{4});
            if (!expected || rgba.size() != *expected)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    std::format(
                        "a mask of {} weights does not cover {} RGBA bytes",
                        weights.size(),
                        rgba.size()
                    )
                );
            }

            for (auto index = std::size_t{0}; index < weights.size(); ++index)
            {
                checkedAt(rgba, (index * 4U) + 3U) = checkedAt(weights, index);
            }
            return rgba;
        }
    }

    TaskContext::TaskContext(
        engine::EngineSession session,
        trace::TraceRecorder& recorder,
        TaskContextConfig config
    ) noexcept
        : m_session{std::move(session)}
        , m_config{std::move(config)}
        , m_recorder{recorder}
        , m_projectFiles{m_config.projectRoot}
    {
    }

    auto TaskContext::openCycle() -> Result<CycleTicket>
    {
        // Refuse before observing. The ledger holds one cycle, and a capture
        // whose frame it could not hold would spend a whole screenshot only to
        // produce an error.
        UF_TRY(m_cycles.requireClosed());
        UF_TRY_VALUE(observation, m_session.observe());
        return m_cycles.open(std::move(observation));
    }

    auto TaskContext::closeCycle(CycleTicket ticket) noexcept -> bool
    {
        return m_cycles.close(ticket);
    }

    auto TaskContext::cycleMatch(
        CycleTicket ticket,
        TemplateTicket templateTicket,
        PixelRect searchRoi
    ) -> Result<std::optional<engine::MatchFound>>
    {
        UF_TRY(m_cycles.requireOpen(ticket));

        // Refused before the cache is consulted: a handle naming no template of
        // this generation is wrong whatever this frame was asked before.
        auto const* p_template = m_templates.find(templateTicket);
        if (p_template == nullptr)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "this template handle names no template of this generation; "
                "load it with template_load first"
            );
        }

        auto const* p_remembered = m_answers.findMatch(
            ticket.ordinal,
            templateTicket,
            searchRoi
        );
        if (p_remembered != nullptr)
        {
            return *p_remembered;
        }

        UF_TRY_VALUE(
            found,
            m_session.matchTemplate(m_cycles.observation(), *p_template, searchRoi)
        );
        m_answers.rememberMatch(ticket.ordinal, templateTicket, searchRoi, found);
        return found;
    }

    auto TaskContext::cycleRead(
        CycleTicket ticket,
        PixelRect rect
    ) -> Result<std::optional<engine::TextReading>>
    {
        UF_TRY(m_cycles.requireOpen(ticket));

        // Consulted BEFORE the budget, because an answer this frame already gave
        // costs no inference and refusing it would deny a region this cycle has
        // already read. See the header for both halves of that decision.
        auto const* p_remembered = m_answers.findText(ticket.ordinal, rect);
        if (p_remembered != nullptr)
        {
            return *p_remembered;
        }

        // Charged before the engine is reached, so an exhausted budget costs no
        // inference. RecognitionIncomplete rather than an empty optional: the
        // host stopped looking, and a miss would claim the region was inspected.
        if (m_cycles.readsCharged() >= m_config.maximumReadsPerCycle)
        {
            return fail(
                AutomationErrorKind::RecognitionIncomplete,
                std::format(
                    "this observation cycle has already spent its budget of {} "
                    "text reads; open a new cycle to read again",
                    m_config.maximumReadsPerCycle
                )
            );
        }
        m_cycles.chargeReads(1);

        UF_TRY_VALUE(reading, m_session.readText(m_cycles.observation(), rect));
        m_answers.rememberText(ticket.ordinal, rect, reading);
        return reading;
    }

    auto TaskContext::cycleReadLines(
        CycleTicket ticket,
        PixelRect rect
    ) -> Result<std::vector<engine::TextReading>>
    {
        UF_TRY(m_cycles.requireOpen(ticket));

        // cycleRead's rule and cycleRead's reasoning, over this verb's own table.
        auto const* p_remembered = m_answers.findLines(ticket.ordinal, rect);
        if (p_remembered != nullptr)
        {
            return *p_remembered;
        }

        auto const spent = m_cycles.readsCharged();
        if (spent >= m_config.maximumReadsPerCycle)
        {
            return fail(
                AutomationErrorKind::RecognitionIncomplete,
                std::format(
                    "this observation cycle has already spent its budget of {} "
                    "text reads; open a new cycle to read again",
                    m_config.maximumReadsPerCycle
                )
            );
        }

        // One read for the detection pass, charged before it runs, and one more for
        // every line it located. Locating is one inference over the region;
        // recognising is one MORE inference per line at the same 2-13 ms a
        // cycle_read costs, so a block read over a twenty-name grid is twenty-one
        // reads however it is spelled. It stays the SAME pool as cycle_read because
        // a block read's lines go through the same recogniser at the same price --
        // unlike a crop, which is an encode and does not compare.
        //
        // The remainder is handed DOWN so the engine can refuse a region holding
        // more lines than this cycle can pay for, having spent one inference
        // rather than one per line. That refusal is a failure and never a short
        // list; see engine::EngineSession::readTextLines.
        m_cycles.chargeReads(1);
        auto const remaining = m_config.maximumReadsPerCycle - spent - 1U;

        UF_TRY_VALUE(
            lines,
            m_session.readTextLines(m_cycles.observation(), rect, remaining)
        );
        m_cycles.chargeReads(static_cast<uint32>(lines.size()));
        m_answers.rememberLines(ticket.ordinal, rect, lines);
        return lines;
    }

    auto TaskContext::cycleCrop(
        CycleTicket ticket,
        PixelRect rect,
        std::optional<ProbeColourKey> key
    ) -> Result<CroppedBlob>
    {
        UF_TRY(m_cycles.requireOpen(ticket));

        // Charged before the engine is reached, so an exhausted budget costs no
        // copy and no encode. Same kind and reasoning as the read budget: the host
        // stopped looking, and an empty answer would claim the region was
        // inspected.
        if (m_cycles.cropsCharged() >= m_config.maximumCropsPerCycle)
        {
            return fail(
                AutomationErrorKind::RecognitionIncomplete,
                std::format(
                    "this observation cycle has already spent its budget of {} "
                    "crops; open a new cycle to crop again",
                    m_config.maximumCropsPerCycle
                )
            );
        }
        m_cycles.chargeCrop();

        UF_TRY_VALUE(region, m_session.cropRegion(m_cycles.observation(), rect));

        // The mask is measured BEFORE the pixels are converted, because
        // maskColourKey reads BGRA and bgra8ToRgba8 consumes the buffer it reads
        // from -- and before the encode, because an all-transparent PNG is bytes no
        // matcher can use and must never become a file an agent could save.
        auto measured = std::optional<MeasuredCropMask>{};
        if (key.has_value())
        {
            UF_TRY_VALUE(built, measureCropMask(region, rect, *key));
            measured = std::move(built);
        }

        UF_TRY_VALUE(rgba, image::bgra8ToRgba8(std::move(region.pixels)));
        if (measured.has_value())
        {
            UF_TRY_VALUE(
                masked,
                applyMaskAlpha(std::move(rgba), measured->weights)
            );
            rgba = std::move(masked);
        }
        UF_TRY_VALUE(
            png,
            image::encodeRgbaPng("cycle crop", region.width, region.height, rgba)
        );

        auto const hash = sha256(png);
        if (!hash)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "the bytes a crop encoded could not be hashed"
            );
        }

        // The crop's whole record, written AFTER the encode because the hash and
        // the byte count are what make the line worth having: a reader matching a
        // template asset against the frame it was cut from has nothing else.
        auto event = trace::TraceEventSpec{
            .eventType = "annotation.region_saved",
            .audit     = trace::AuditMetadata{
                .actor = "annotation",
                .references = {
                    trace::TraceReference{
                        .type = "frame",
                        .id   = std::to_string(region.frame.frameId().value()),
                    },
                },
            },
            .payload = trace::TypedTracePayload{
                .schemaHash = traceSchemaHash(),
                .fields = {
                    trace::TraceField{.name = "x", .value = uint64{rect.x()}},
                    trace::TraceField{.name = "y", .value = uint64{rect.y()}},
                    trace::TraceField{
                        .name  = "width",
                        .value = uint64{rect.width()},
                    },
                    trace::TraceField{
                        .name  = "height",
                        .value = uint64{rect.height()},
                    },
                    trace::TraceField{
                        .name  = "content_hash",
                        .value = hash->toString(),
                    },
                    trace::TraceField{
                        .name  = "byte_count",
                        .value = static_cast<uint64>(png.size()),
                    },
                },
            },
        };
        UF_TRY(m_recorder.emit(event));

        auto blob = CroppedBlob{
            .png  = std::move(png),
            .hash = *hash,
        };
        if (measured.has_value())
        {
            blob.mask = std::move(measured->reported);
        }
        return blob;
    }

    auto TaskContext::cycleCensusGrid(
        CycleTicket ticket,
        PixelRect rect,
        uint32 cellWidth,
        uint32 cellHeight,
        ProbeColourKey key
    ) -> Result<ColourGridReport>
    {
        UF_TRY(m_cycles.requireOpen(ticket));
        UF_TRY(validateCensusGrid(rect, cellWidth, cellHeight, key));

        // The rect's own pixels, taken through the verb that already knows how
        // to widen a Gray8 capture and how to refuse a rect the frame does not
        // contain. It is one copy of the rect and no encode, which is the whole
        // difference from cycleCrop: the copy stays in host C++ and only the
        // per-cell counts leave.
        UF_TRY_VALUE(region, m_session.cropRegion(m_cycles.observation(), rect));

        auto const widthSize = checkedCast<std::size_t>(region.width);
        if (!widthSize)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "the census grid region's width is beyond range"
            );
        }
        UF_TRY_VALUE(
            view,
            BgraImage::create(
                region.pixels,
                region.width,
                region.height,
                *widthSize * 4U
            )
        );
        UF_TRY_VALUE(
            gridRect,
            PixelRect::create(0, 0, region.width, region.height)
        );

        // censusColourGrid is a multi-frame measurement and refuses one frame,
        // because one frame is stable everywhere; the selection half asked for
        // here is read off frame zero alone. Handing it the same view twice is
        // what probePngRegion does for the same reason, and it is why every
        // cell's spread reads zero until an observation retains more than one
        // frame.
        auto const frames = std::array<BgraImage, 2>{view, view};
        return censusColourGrid(
            frames,
            ColourGridSpec{
                .rect       = gridRect,
                .cellWidth  = cellWidth,
                .cellHeight = cellHeight,
                .keyRed     = key.red,
                .keyGreen   = key.green,
                .keyBlue    = key.blue,
                .tolerance  = key.tolerance,
                .keyRemoves = key.removes,
            }
        );
    }

    auto TaskContext::sweepOpenCycle() noexcept -> bool
    {
        return m_cycles.closeOpen();
    }

    auto TaskContext::openCycleTargetGeneration() const noexcept
        -> std::optional<TargetGeneration>
    {
        if (!m_cycles.isOpen())
        {
            return std::nullopt;
        }
        return m_cycles.observation().frameIdentity().targetGeneration();
    }

    auto TaskContext::loadTemplate(
        std::span<std::byte const> pngBytes
    ) -> Result<LoadedTemplate>
    {
        UF_TRY_VALUE(ticket, m_templates.load(pngBytes));
        auto const* p_hash = m_templates.hashOf(ticket);
        UF_CHECK(p_hash != nullptr);
        return LoadedTemplate{
            .ticket = ticket,
            .hash   = *p_hash,
        };
    }

    auto TaskContext::projectRead(
        std::string_view name
    ) -> Result<std::vector<std::byte>>
    {
        return m_projectFiles.read(name);
    }

    auto TaskContext::projectWrite(
        std::string_view name,
        std::span<std::byte const> bytes
    ) -> Status
    {
        return m_projectFiles.write(name, bytes);
    }

    auto TaskContext::requireReceiptCycle(
        CycleTicket ticket,
        std::optional<uint64> evidenceCycleOrdinal
    ) const -> Status
    {
        UF_TRY(m_cycles.requireOpen(ticket));
        if (evidenceCycleOrdinal.has_value())
        {
            UF_TRY(m_cycles.requireOpenEvidence(*evidenceCycleOrdinal));
        }
        return ok();
    }

    auto TaskContext::deliverReceiptClick(
        CycleTicket ticket,
        PixelPoint point
    ) -> Result<engine::ActReceipt>
    {
        UF_TRY_VALUE(observation, m_cycles.spend(ticket));
        return m_session.clickPoint(std::move(observation), point);
    }

    auto TaskContext::deliverReceiptKey(
        CycleTicket ticket,
        KeyName key
    ) -> Result<engine::KeyReceipt>
    {
        UF_TRY_VALUE(observation, m_cycles.spend(ticket));
        return m_session.pressKey(std::move(observation), key);
    }

    // The six below share one shape and it is deliberate: spend the cycle, hand
    // the observation to the engine verb that owns the act, and write the
    // front-end's line only once the act has landed. The spend comes first on
    // every one of them, so the frame leaves the ledger BEFORE anything is
    // posted and this ticket can never deliver twice; the engine consumes the
    // observation by rvalue, so the cycle is spent whatever the outcome.
    //
    // A refused act writes no annotation line. The engine has already written
    // engine.action_rejected naming the reason, and a delivered-line for an act
    // that did not happen is the one entry an auditor must never find.

    auto TaskContext::cycleClickPoint(CycleTicket ticket, PixelPoint point) -> Status
    {
        UF_TRY_VALUE(observation, m_cycles.spend(ticket));
        UF_TRY_VALUE(
            receipt,
            m_session.clickPoint(std::move(observation), point)
        );
        return m_recorder.emit(
            annotationActionEvent("click", receipt.frameId, pointFields(point))
        );
    }

    auto TaskContext::cycleLongPress(
        CycleTicket ticket,
        PixelPoint point,
        MonotonicInstant::Duration hold
    ) -> Status
    {
        // Both refusals precede the spend, so a chunk with a sign error or a
        // mistyped hold keeps its frame and leaves no button down.
        if (hold < MonotonicInstant::Duration::zero() || hold > k_maxLongPressHold)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "a long press must hold for between 0 and {} milliseconds",
                    traceMillis(k_maxLongPressHold)
                )
            );
        }

        UF_TRY_VALUE(observation, m_cycles.spend(ticket));
        UF_TRY_VALUE(
            receipt,
            m_session.longPress(std::move(observation), point, hold)
        );
        auto fields = pointFields(point);
        fields.emplace_back(
            trace::TraceField{.name = "hold_millis", .value = traceMillis(hold)}
        );
        return m_recorder.emit(
            annotationActionEvent("long_press", receipt.frameId, std::move(fields))
        );
    }

    auto TaskContext::cycleDrag(
        CycleTicket ticket,
        PixelPoint start,
        PixelPoint end,
        MonotonicInstant::Duration travel
    ) -> Status
    {
        if (travel < MonotonicInstant::Duration::zero() || travel > k_maxDragTravel)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "a drag must travel for between 0 and {} milliseconds",
                    traceMillis(k_maxDragTravel)
                )
            );
        }

        UF_TRY_VALUE(observation, m_cycles.spend(ticket));
        UF_TRY_VALUE(
            receipt,
            m_session.drag(std::move(observation), start, end, travel)
        );

        // Both ends are on the line. The far one is the chunk's own arithmetic
        // rather than anything it measured, so a record carrying only the start
        // cannot answer where the drag ended.
        auto fields = pointFields(start);
        fields.emplace_back(
            trace::TraceField{.name = "end_pixel_x", .value = uint64{end.x()}}
        );
        fields.emplace_back(
            trace::TraceField{.name = "end_pixel_y", .value = uint64{end.y()}}
        );
        fields.emplace_back(
            trace::TraceField{
                .name  = "travel_millis",
                .value = traceMillis(travel),
            }
        );
        return m_recorder.emit(
            annotationActionEvent("drag", receipt.frameId, std::move(fields))
        );
    }

    auto TaskContext::cycleMovePointer(CycleTicket ticket, PixelPoint point) -> Status
    {
        UF_TRY_VALUE(observation, m_cycles.spend(ticket));
        UF_TRY_VALUE(
            receipt,
            m_session.movePointer(std::move(observation), point)
        );
        return m_recorder.emit(
            annotationActionEvent(
                "pointer_move",
                receipt.frameId,
                pointFields(point)
            )
        );
    }

    auto TaskContext::cycleScroll(CycleTicket ticket, int32 notches) -> Status
    {
        UF_TRY_VALUE(observation, m_cycles.spend(ticket));
        UF_TRY_VALUE(receipt, m_session.scroll(std::move(observation), notches));

        // Signed, because direction is half of what was delivered.
        return m_recorder.emit(
            annotationActionEvent(
                "scroll",
                receipt.frameId,
                {
                    trace::TraceField{
                        .name  = "wheel_notches",
                        .value = static_cast<int64>(receipt.notches),
                    },
                }
            )
        );
    }

    auto TaskContext::cycleKey(CycleTicket ticket, KeyName key) -> Status
    {
        UF_TRY_VALUE(observation, m_cycles.spend(ticket));
        UF_TRY_VALUE(receipt, m_session.pressKey(std::move(observation), key));

        // The name is read back off the RECEIPT rather than off the argument, so
        // the line records the key the delivery layer actually carried. A verb
        // that traced its own input would still say "E" if the chain below it
        // delivered something else.
        return m_recorder.emit(
            annotationActionEvent(
                "key",
                receipt.frameId,
                {
                    trace::TraceField{
                        .name  = "key",
                        .value = std::string{receipt.key.value()},
                    },
                }
            )
        );
    }

    auto TaskContext::waitUntil(
        MonotonicInstant deadline,
        MonotonicInstant::Duration interval
    ) const -> bool
    {
        // Report the expiry without sleeping when the deadline has already
        // passed: a wait whose budget is spent must not first spend an interval.
        if (MonotonicInstant::now() >= deadline)
        {
            return false;
        }

        pollSleep(interval, deadline, m_config.cancellation);
        return MonotonicInstant::now() < deadline;
    }

    auto TaskContext::settle(MonotonicInstant::Duration duration) const -> void
    {
        // The settle's own end is its deadline, so the shared poll sleep bounds
        // it by the same instant its interval would reach and no separate
        // ceiling has to be reconciled with it.
        auto const until = MonotonicInstant::now().checkedAdd(duration);
        if (!until)
        {
            // Unreachable while the binding enforces k_maxSettleDuration, which
            // is far below any monotonic overflow. Sleeping for nothing is the
            // fail-closed answer: a pause the host cannot bound is one it must
            // not take.
            return;
        }

        pollSleep(duration, *until, m_config.cancellation);
    }

    auto TaskContext::cancellationRequested() const noexcept -> bool
    {
        return m_config.cancellation.stop_requested();
    }

    auto TaskContext::hasOpenCycle() const noexcept -> bool
    {
        return m_cycles.isOpen();
    }

    void TaskContext::markTerminal(AutomationErrorKind kind) noexcept
    {
        if (!m_terminal.has_value())
        {
            m_terminal = kind;
        }
    }

    auto TaskContext::fatal() const noexcept -> bool
    {
        return m_terminal.has_value();
    }

    auto TaskContext::terminalKind() const noexcept -> std::optional<AutomationErrorKind>
    {
        return m_terminal;
    }

    void TaskContext::latchTraceFailure() noexcept
    {
        m_traceFailed = true;
    }

    auto TaskContext::traceFailed() const noexcept -> bool
    {
        return m_traceFailed;
    }

    auto TaskContext::emitTrace(trace::TraceEventSpec const& event) -> Status
    {
        return m_recorder.emit(event);
    }

}
