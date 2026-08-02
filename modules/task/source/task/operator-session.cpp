#include "operator-session.hpp"

#include "native-call-trace.hpp"

#include <task/cycle-ledger.hpp>
#include <task/task-context.hpp>
#include <task/task-host.hpp>

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/ids.hpp>
#include <domain/key.hpp>

#include <engine/session.hpp>

#include <trace/event.hpp>
#include <trace/file-sink.hpp>
#include <trace/recorder.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace uf::task
{
    namespace
    {
        // The opening line of an operator session's run bracket.
        //
        // Four of the seven Run members are deliberately empty, and the emptiness
        // is accurate rather than missing. There is no task, so no taskName and no
        // sourceHash; no trusted Luau framework ran, so no frameworkVersion,
        // frameworkHash or luauVersion -- naming this binary's framework build
        // would claim a framework that took no part in the run. The seed is zero
        // for the same reason: the operator surface exposes no randomness, so a
        // drawn-but-unused seed would be a false one. The `frontEnd` member the
        // recorder stamps on this same line tells a reader which absences to
        // expect.
        [[nodiscard]]
        auto operatorRunStartedEvent(std::string const& projectId) -> trace::TraceEvent
        {
            return trace::TraceEvent{
                .kind = trace::TraceEventKind::RunStarted,
                .run  = trace::TraceEvent::Run{
                    .projectId = projectId,
                },
            };
        }

        [[nodiscard]]
        auto operatorRunFinishedEvent(
            TaskRunReport const& report
        ) -> trace::TraceEvent
        {
            auto wireOutcome = trace::RunOutcome::Completed;
            switch (report.outcome())
            {
            case TaskRunOutcome::Completed:
                wireOutcome = trace::RunOutcome::Completed;
                break;
            case TaskRunOutcome::Cancelled:
                wireOutcome = trace::RunOutcome::Cancelled;
                break;
            case TaskRunOutcome::Failed:
                wireOutcome = trace::RunOutcome::Failed;
                break;
            }

            auto errorKind = std::optional<AutomationErrorKind>{};
            if (report.failure)
            {
                errorKind = automationErrorKind(*report.failure)
                    .value_or(AutomationErrorKind::InternalInvariant);
            }

            return trace::TraceEvent{
                .kind       = trace::TraceEventKind::RunFinished,
                .runOutcome = wireOutcome,
                .errorKind  = errorKind,
            };
        }

        [[nodiscard]]
        auto invalid(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        // The whole millisecond count of a duration the caller already bounded, for
        // the one native call that records a duration.
        [[nodiscard]]
        auto wholeMillis(MonotonicInstant::Duration duration) noexcept -> uint64
        {
            return static_cast<uint64>(
                std::chrono::duration_cast<std::chrono::milliseconds>(duration).count()
            );
        }
    }

    OperatorSession::OperatorSession(
        CreateTag,
        std::unique_ptr<trace::TraceRecorder> recorder,
        engine::EngineSession session,
        TaskContextConfig contextConfig,
        std::filesystem::path tracePath,
        uint64 seed
    ) noexcept
        : m_recorder{std::move(recorder)}
        , m_context{std::move(session), *m_recorder, std::move(contextConfig)}
        , m_tracePath{std::move(tracePath)}
        , m_seed{seed}
    {
    }

    auto OperatorSession::create(
        TaskRunConfig config,
        Spec spec,
        TaskRunId runId,
        GenerationId generationId
    ) -> Result<std::unique_ptr<OperatorSession>>
    {
        UF_TRY_VALUE(traceSink, trace::FileTraceSink::create(config.tracePath));
        auto recorder = std::make_unique<trace::TraceRecorder>(
            std::move(traceSink),
            runId,
            generationId,
            trace::FrontEnd::Operator
        );

        UF_TRY(recorder->emit(operatorRunStartedEvent(spec.projectId)));

        // No run.resources_validated line. That event records the closure of uf
        // references a task SOURCE was validated against before its VM existed;
        // there is no source here, so an empty one would misreport a validation
        // that never happened.
        UF_TRY_VALUE(
            session,
            engine::EngineSession::create(
                std::move(config.frameSource),
                std::move(config.actionSink),
                *recorder,
                engine::EngineSessionConfig{
                    .liveFingerprint         = config.liveFingerprint,
                    .projectFingerprint      = spec.projectFingerprint,
                    .maximumPixelComparisons = config.maximumPixelComparisons,
                    .recognitionTimeout      = config.recognitionTimeout,
                    .maxActionFrameAge       = config.maxActionFrameAge,
                    .cancellation            = spec.cancellation,
                }
            )
        );

        return std::make_unique<OperatorSession>(
            CreateTag{},
            std::move(recorder),
            std::move(session),
            TaskContextConfig{
                .cancellation = spec.cancellation,
            },
            config.tracePath,
            uint64{0}
        );
    }

    auto OperatorSession::finish(std::optional<Error> failure) -> TaskRunReport
    {
        auto report = TaskRunReport{
            .seed      = m_seed,
            .tracePath = m_tracePath,
            .failure   = std::move(failure),
        };

        // A generation the host spent terminally ended there whatever the operator
        // did afterwards, exactly as for a task run: the latch is read back here so a
        // cancelled session cannot report itself completed.
        if (!report.failure)
        {
            if (auto const terminal = m_context.terminalKind(); terminal.has_value())
            {
                report.failure = fail(
                    *terminal,
                    "the operator session's generation was spent before it ended"
                ).error();
            }
        }

        auto finishStatus = m_recorder->emit(operatorRunFinishedEvent(report));
        if (!report.failure && !finishStatus)
        {
            report.failure = std::move(finishStatus).error();
        }
        return report;
    }

    auto OperatorSession::ticketFor(uint64 ordinal) const noexcept -> CycleTicket
    {
        if (!m_ticket.has_value())
        {
            return CycleTicket{};
        }
        return CycleTicket{.generation = m_ticket->generation, .ordinal = ordinal};
    }

    auto OperatorSession::cycleOpen() -> Result<uint64>
    {
        UF_TRY(requireLiveGeneration(m_context));

        auto const call = NativeCallIdentity{.verb = "cycle_open"};

        auto result = m_context.openCycle();
        if (!result)
        {
            recordNativeCallFailure(m_context, call, result.error());
            return std::unexpected{std::move(result).error()};
        }
        // Remembered before the line is recorded, so a lost trace line still leaves
        // the operator able to close the cycle it just opened. The Luau primitive
        // cannot do this -- its raise skips the return -- and the failure is the
        // same IoFailure either way, only the recovery is better here.
        m_ticket = *result;

        UF_TRY(recordNativeCall(m_context, call, trace::NativeCallOutcome::Succeeded));
        return result->ordinal;
    }

    auto OperatorSession::cycleClose(uint64 cycleOrdinal) -> Result<bool>
    {
        UF_TRY(requireLiveGeneration(m_context));

        auto const call = NativeCallIdentity{
            .verb         = "cycle_close",
            .cycleOrdinal = cycleOrdinal,
        };

        bool const released = m_context.closeCycle(ticketFor(cycleOrdinal));
        UF_TRY(
            recordNativeCall(
                m_context,
                call,
                released ? trace::NativeCallOutcome::Succeeded
                         : trace::NativeCallOutcome::Empty
            )
        );
        return released;
    }

    auto OperatorSession::key(uint64 cycleOrdinal, KeyName keyName) -> Status
    {
        UF_TRY(requireLiveGeneration(m_context));

        auto const call = NativeCallIdentity{
            .verb         = "key",
            .cycleOrdinal = cycleOrdinal,
            .key          = keyName,
        };

        auto result = m_context.cycleKey(ticketFor(cycleOrdinal), keyName);
        if (!result)
        {
            recordNativeCallFailure(m_context, call, result.error());
            return std::unexpected{std::move(result).error()};
        }
        return recordNativeCall(m_context, call, trace::NativeCallOutcome::Succeeded);
    }

    auto OperatorSession::settle(MonotonicInstant::Duration duration) -> Status
    {
        UF_TRY(requireLiveGeneration(m_context));

        if (duration < MonotonicInstant::Duration::zero())
        {
            return invalid("settle needs a non-negative millisecond count");
        }
        if (duration > k_maxSettleDuration)
        {
            // The same ceiling the Luau primitive enforces, from the same constant.
            // Only the wording differs, because the wording names what the caller
            // wrote to get here.
            return invalid(
                "settle exceeds the host's settle ceiling; wait against a deadline "
                "instead of sleeping"
            );
        }

        auto const call = NativeCallIdentity{
            .verb           = "settle",
            .durationMillis = wholeMillis(duration),
        };

        UF_TRY(requireNotCancelled(m_context));
        m_context.settle(duration);
        if (auto live = requireNotCancelled(m_context); !live)
        {
            // Record the abandoned settle before reporting the stop, so the trace
            // shows a pause that was cut short rather than a verb that reported
            // nothing.
            recordNativeCallFailure(m_context, call, live.error());
            return std::unexpected{std::move(live).error()};
        }
        return recordNativeCall(m_context, call, trace::NativeCallOutcome::Succeeded);
    }

    auto OperatorSession::deadline(
        MonotonicInstant::Duration duration
    ) -> Result<uint64>
    {
        UF_TRY(requireLiveGeneration(m_context));
        UF_TRY(requireNotCancelled(m_context));

        if (duration < MonotonicInstant::Duration::zero())
        {
            return invalid("deadline needs a non-negative millisecond count");
        }
        if (m_deadlines.size() >= k_maxOperatorDeadlines)
        {
            return invalid(
                "this session has minted its ceiling of deadline handles"
            );
        }

        auto const instant = MonotonicInstant::now().checkedAdd(duration);
        if (!instant)
        {
            return invalid("deadline overflows the monotonic clock");
        }

        m_deadlines.emplace_back(*instant);
        return static_cast<uint64>(m_deadlines.size());
    }

    auto OperatorSession::wait(
        uint64 deadlineId,
        MonotonicInstant::Duration interval
    ) -> Result<bool>
    {
        UF_TRY(requireLiveGeneration(m_context));

        if (deadlineId == 0U || deadlineId > m_deadlines.size())
        {
            return invalid(
                std::format("this session minted no deadline {}", deadlineId)
            );
        }
        if (interval < MonotonicInstant::Duration::zero())
        {
            return invalid("wait needs a non-negative poll interval");
        }

        // Clamped up from the same floor the Luau primitive uses, so a caller asking
        // for a zero interval cannot turn the observation cycle into a busy wait
        // whichever front-end asked.
        auto const clamped = std::max(interval, k_minWaitPollInterval);
        auto const until   = m_deadlines[static_cast<std::size_t>(deadlineId - 1U)];

        UF_TRY(requireNotCancelled(m_context));
        bool const budgetRemains = m_context.waitUntil(until, clamped);
        UF_TRY(requireNotCancelled(m_context));
        return budgetRemains;
    }

    auto OperatorSession::tracePath() const noexcept -> std::filesystem::path const&
    {
        return m_tracePath;
    }
}
