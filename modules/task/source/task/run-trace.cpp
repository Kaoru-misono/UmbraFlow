#include "run-trace.hpp"

#include "framework-bundle.hpp"
#include "script-validator.hpp"
#include "task-loader.hpp"

#include <core/error/error.hpp>

#include <domain/error.hpp>

#include <trace/event.hpp>

#include <string>

namespace uf::task
{
    auto runStartedEvent(RunStartSpec const& spec) -> trace::TraceEvent
    {
        return trace::TraceEvent{
            .kind = trace::TraceEventKind::RunStarted,
            .run  = trace::TraceEvent::Run{
                .projectId        = spec.projectId,
                .taskName         = spec.taskName,
                .sourceHash       = spec.sourceHash,
                .frameworkVersion = std::string{frameworkVersion()},
                .frameworkHash    = std::string{frameworkBundleHash()},
                .luauVersion      = luauRuntimeVersion(),
                .seed             = spec.seed,
            },
        };
    }

    auto runResourcesValidatedEvent(
        ScriptResourceReport const& report
    ) -> trace::TraceEvent
    {
        return trace::TraceEvent{
            .kind      = trace::TraceEventKind::RunResourcesValidated,
            .resources = trace::TraceEvent::Resources{
                .recognizers = report.recognizers,
                .pages       = report.pages,
            },
        };
    }

    auto runFinishedEvent(Error const* p_failure) -> trace::TraceEvent
    {
        if (p_failure == nullptr)
        {
            return trace::TraceEvent{
                .kind       = trace::TraceEventKind::RunFinished,
                .runOutcome = trace::RunOutcome::Completed,
            };
        }

        auto const kind = automationErrorKind(*p_failure)
            .value_or(AutomationErrorKind::InternalInvariant);
        return trace::TraceEvent{
            .kind       = trace::TraceEventKind::RunFinished,
            .runOutcome = kind == AutomationErrorKind::Cancelled
                ? trace::RunOutcome::Cancelled
                : trace::RunOutcome::Failed,
            .errorKind  = kind,
        };
    }
}
