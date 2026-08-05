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
#include <domain/key.hpp>
#include <domain/space.hpp>

#include <engine/session.hpp>

#include <image/pixels.hpp>
#include <image/png.hpp>

#include <trace/event.hpp>
#include <trace/recorder.hpp>

#include <vision/bgra-image.hpp>
#include <vision/frame-analysis.hpp>

#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uf::task
{
    namespace
    {
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
        [[nodiscard]]
        auto maskWarning(uint64 selected, uint64 total) -> std::string
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
            if (shareBp >= k_maximumUsefulMaskShareBp)
            {
                return std::format(
                    "this key selects {} of {} pixels ({}.{:02} percent), at or "
                    "above the {} basis points where a mask stops distinguishing "
                    "anything: a key takes one colour by construction, so a mask "
                    "this large is a solid patch that any patch of the same "
                    "colour matches",
                    selected,
                    total,
                    shareBp / 100U,
                    shareBp % 100U,
                    k_maximumUsefulMaskShareBp
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
                        measured.rectPixels
                    ),
                },
            };
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
        , m_rng{m_config.randomSeed}
        , m_recorder{recorder}
        , m_projectFiles{m_config.projectRoot}
    {
    }

    // D6 known-popup sweep: it lives in the framework's interrupt registry, not
    // here and not in the engine. A task declares the popups it knows through
    // task.interrupt, and the Luau wait loop offers every cycle it opens to that
    // registry before testing the target page -- so the sweep fires on the polls
    // in the middle of a long wait, which neither candidate C++ position could do.
    // This note records where it went so the absence is not read as an omission.
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
        auto event = trace::TraceEvent{
            .kind  = trace::TraceEventKind::AnnotationRegionSaved,
            .frame = region.frame,
            .annotation = trace::TraceEvent::Annotation{
                .rect        = rect,
                .contentHash = hash->toString(),
                .byteCount   = static_cast<uint64>(png.size()),
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

    auto TaskContext::sweepOpenCycle() noexcept -> bool
    {
        return m_cycles.closeOpen();
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

    auto TaskContext::cycleClickPoint(
        CycleTicket ticket,
        std::optional<uint64> hitCycleOrdinal,
        PixelPoint point
    ) -> Result<engine::ActReceipt>
    {
        // Both the ticket and, when one was supplied, the match's ordinal are
        // checked against the one open cycle before it is spent, so a stale match
        // leaves the cycle open for the framework to close rather than destroying
        // a frame the script still has a live ticket for.
        UF_TRY(m_cycles.requireOpen(ticket));
        if (hitCycleOrdinal.has_value())
        {
            UF_TRY(m_cycles.requireOpenOrdinal(*hitCycleOrdinal));
        }

        // The rest of the fence -- fingerprint, lease, single delivery -- is the
        // engine's; what the ledger contributes is that the frame leaves it here,
        // so this ticket delivers nothing else.
        UF_TRY_VALUE(observation, m_cycles.spend(ticket));
        return m_session.clickPoint(std::move(observation), point);
    }

    auto TaskContext::cycleKey(CycleTicket ticket, KeyName key) -> Status
    {
        // The ticket is checked against the one open cycle first, so a stale
        // ticket leaves the cycle open for the framework to close rather than
        // destroying a frame a live ticket still names.
        UF_TRY_VALUE(observation, m_cycles.spend(ticket));

        // pressKey consumes the frame by rvalue, so the cycle is spent whatever
        // the outcome; spend already dropped the ledger entry, which is what
        // makes every later use of this ticket fail StaleObservation.
        UF_TRY(m_session.pressKey(std::move(observation), key));
        return ok();
    }

    auto TaskContext::cycleScroll(CycleTicket ticket, int32 notches) -> Status
    {
        // The ledger's half of the fence is cycleKey's: the ticket is checked
        // against the one open cycle, and the frame leaves the ledger here so a
        // stale ticket is refused before anything is delivered.
        UF_TRY_VALUE(observation, m_cycles.spend(ticket));

        // scroll consumes the frame by rvalue, so the cycle is spent whatever the
        // outcome; spend already dropped the ledger entry, which is what makes
        // every later use of this ticket fail StaleObservation.
        UF_TRY(m_session.scroll(std::move(observation), notches));
        return ok();
    }

    auto TaskContext::cycleLongPress(
        CycleTicket ticket,
        PixelPoint point,
        MonotonicInstant::Duration hold
    ) -> Result<engine::LongPressReceipt>
    {
        // The ledger's half of the fence: the ticket is checked against the one
        // open cycle, and the frame leaves the ledger here so a stale ticket is
        // refused before anything is delivered. There is no ordinal to check first
        // -- see the header -- so the spend is the whole of it.
        UF_TRY_VALUE(observation, m_cycles.spend(ticket));

        // longPress consumes the frame by rvalue, so the cycle is spent whatever
        // the outcome; spend already dropped the ledger entry, which is what
        // makes every later use of this ticket fail StaleObservation.
        return m_session.longPress(std::move(observation), point, hold);
    }

    auto TaskContext::cycleMovePointer(
        CycleTicket ticket,
        PixelPoint point
    ) -> Result<engine::PointerMoveReceipt>
    {
        // The ledger's half of the fence, cycleLongPress's exactly: the ticket is
        // checked against the one open cycle, and the frame leaves the ledger here
        // so a stale ticket is refused before anything is delivered.
        UF_TRY_VALUE(observation, m_cycles.spend(ticket));

        // movePointer consumes the frame by rvalue, so the cycle is spent whatever
        // the outcome; spend already dropped the ledger entry, which is what makes
        // every later use of this ticket fail StaleObservation.
        return m_session.movePointer(std::move(observation), point);
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

    auto TaskContext::emitTrace(trace::TraceEvent const& event) -> Status
    {
        return m_recorder.emit(event);
    }

    auto TaskContext::nextRandomUnitDouble() noexcept -> double
    {
        return m_rng.nextUnitDouble();
    }

    auto TaskContext::nextRandomInRange(
        int64 lowInclusive,
        int64 highInclusive
    ) noexcept -> int64
    {
        // Precondition, guaranteed by the binding that reads the script arguments:
        // lowInclusive <= highInclusive and both within +/-2^53. Under it the span
        // fits in uint64, the offset is far below 2^63 so the cast back to int64 is
        // exact, and low + offset stays within [low, high] with no signed overflow.
        UF_ASSERT(lowInclusive <= highInclusive);
        auto const span   = static_cast<uint64>(highInclusive - lowInclusive) + uint64{1};
        auto const offset = m_rng.boundedUint64(span);
        return lowInclusive + static_cast<int64>(offset);
    }
}
