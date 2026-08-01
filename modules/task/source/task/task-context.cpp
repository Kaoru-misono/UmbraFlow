#include <task/task-context.hpp>

#include <task/cycle-ledger.hpp>
#include <task/project-files.hpp>
#include <task/template-store.hpp>

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
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

#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uf::task
{
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

    // D6 known-popup sweep -- landing note (deliberately not a hook here).
    //
    // The sweep has a home now, and it is neither this file nor the engine: it
    // is the framework's interrupt registry. A task declares the popups it knows
    // through task.interrupt, and the Luau wait loop offers every cycle it opens
    // to that registry before testing the target page. That is the per-cycle
    // sweep the design asked for, and it fires on the polls that matter -- the
    // ones in the middle of a long wait -- which is exactly what neither
    // candidate C++ position could do. A task-side callback would have fired
    // once at the start of a wait; the engine's own seam sat inside a poll loop
    // the registry could not reach.
    //
    // What stays true: the task side owns no part of that mechanism. This note
    // records where it went so the absence is not read as an omission.
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

        auto const* p_template = m_templates.find(templateTicket);
        if (p_template == nullptr)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "this template handle names no template of this generation; "
                "load it with template_load first"
            );
        }
        return m_session.matchTemplate(
            m_cycles.observation(),
            *p_template,
            searchRoi
        );
    }

    auto TaskContext::cycleRead(
        CycleTicket ticket,
        PixelRect rect
    ) -> Result<std::optional<engine::TextReading>>
    {
        UF_TRY(m_cycles.requireOpen(ticket));

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
        return m_session.readText(m_cycles.observation(), rect);
    }

    auto TaskContext::cycleReadLines(
        CycleTicket ticket,
        PixelRect rect
    ) -> Result<std::vector<engine::TextReading>>
    {
        UF_TRY(m_cycles.requireOpen(ticket));

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

        // ONE READ FOR THE DETECTION PASS, CHARGED BEFORE IT RUNS, AND ONE MORE
        // FOR EVERY LINE IT LOCATED. That is the whole budget rule for this
        // verb, and the reason is what the two halves cost. Locating is one
        // inference over the region; recognising is one MORE inference per line,
        // at the same 2-13 ms per line a cycle_read costs -- so a block read
        // over a twenty-name grid is twenty-one reads however it is spelled, and
        // charging it as one would leave the budget describing nothing for the
        // verb that spends the most. It stays the SAME pool as cycle_read rather
        // than a dimension of its own, which is the opposite of the choice the
        // crop budget made and for the opposite reason: a crop is an encode and
        // a read is an inference, so those two units do not compare, while a
        // block read's lines are recognised by the same recogniser at the same
        // price as a single-line read. One number covers them because it is one
        // cost.
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
        return lines;
    }

    auto TaskContext::cycleCrop(
        CycleTicket ticket,
        PixelRect rect
    ) -> Result<CroppedBlob>
    {
        UF_TRY(m_cycles.requireOpen(ticket));

        // Charged before the engine is reached, so an exhausted budget costs no
        // copy and no encode. Same kind and same reasoning as the read budget:
        // the host stopped looking, and an empty answer would claim the region
        // was inspected.
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
        UF_TRY_VALUE(rgba, image::bgra8ToRgba8(std::move(region.pixels)));
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

        // The crop's whole record. It is written AFTER the encode because the
        // hash and the byte count are what make the line worth having: a reader
        // matching a template asset in the project directory against the frame
        // it was cut from has nothing else to match on.
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

        return CroppedBlob{
            .png  = std::move(png),
            .hash = *hash,
        };
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
