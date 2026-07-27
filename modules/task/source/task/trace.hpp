#pragma once

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uf::task
{
    // The script layer's own trace schema, versioned so a downstream consumer can
    // reject a line it does not understand; serializeTaskTraceEvent always emits
    // it first. It is deliberately separate from engine-trace/v1, which
    // modules/engine owns and this module must never extend: engine-trace records
    // the recognition/action pipeline, while task-trace records the task's
    // identity, its resource closure, every host-API return, and how it ended
    // (roadmap landing rule 6). Two independent trace streams, each with its own
    // golden line.
    inline constexpr auto k_taskTraceSchema = std::string_view{"task-trace/v1"};

    enum class TaskTraceEventKind : uint8
    {
        TaskStarted,
        ResourcesValidated,
        HostCall,
        TaskFinished,
    };

    // The result summary of one host-API (umbra verb) call. Succeeded and Empty
    // both completed without a failure; Empty is the Tier A "completed miss"
    // (frame:find returning nil), kept distinct so a trace shows a search that ran
    // and found nothing apart from one that found its target. Failed carries the
    // AutomationErrorKind in TaskTraceEvent::errorKind.
    enum class HostCallOutcome : uint8
    {
        Succeeded,
        Empty,
        Failed,
    };

    // Why a task VM generation ended. Completed is a clean script return; Failed
    // is an uncaught Tier B automation error or the script's own error; Cancelled
    // is the single cancel source spending the generation. Failed carries the
    // AutomationErrorKind in TaskTraceEvent::errorKind.
    enum class TaskExitReason : uint8
    {
        Completed,
        Failed,
        Cancelled,
    };

    // One flat task-trace record. Every field beyond the kind is optional so a
    // single struct describes every event; serializeTaskTraceEvent omits an absent
    // field rather than emitting a null. This is a transport aggregate: construct
    // it with designated initializers and set only the fields the event carries.
    struct TaskTraceEvent final
    {
        TaskTraceEventKind kind{};

        std::optional<std::string> taskName{};
        std::optional<std::string> scriptHash{};
        std::optional<std::string> luauVersion{};
        std::optional<std::string> projectId{};
        std::optional<uint64>      seed{};

        std::optional<std::vector<std::string>> recognizers{};
        std::optional<std::vector<std::string>> pages{};

        std::optional<std::string>         verb{};
        std::optional<HostCallOutcome>     outcome{};
        std::optional<AutomationErrorKind> errorKind{};

        std::optional<TaskExitReason> exitReason{};
    };

    // Serializes one event to a single-line JSON object. Pure and I/O-free: the
    // field order is fixed and the schema version is emitted first, so the output
    // is a stable golden line. The name-list fields (recognizers, pages) are
    // sorted before emission, so an unordered container's iteration order can
    // never reach the wire -- a determinism-ledger constraint.
    [[nodiscard]]
    auto serializeTaskTraceEvent(TaskTraceEvent const& event) -> std::string;

    // A port that records one task-trace event. Traceability is a load-bearing
    // constraint (roadmap landing rule 6), so an emit failure is an error rather
    // than a best-effort side effect: the caller emits at the decision instant and
    // treats a failed emit as aborting the current operation, mirroring the
    // engine's TraceSink discipline (D4).
    class TaskTraceSink
    {
    public:
        TaskTraceSink() = default;

        TaskTraceSink(TaskTraceSink const&) = delete;
        TaskTraceSink(TaskTraceSink&&) = delete;
        auto operator=(TaskTraceSink const&) -> TaskTraceSink& = delete;
        auto operator=(TaskTraceSink&&) -> TaskTraceSink& = delete;

        virtual ~TaskTraceSink() = default;

        [[nodiscard]] virtual auto emit(TaskTraceEvent const& event) -> Status = 0;
    };
}
