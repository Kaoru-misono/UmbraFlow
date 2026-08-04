#include "stream-validator.hpp"

#include "event.hpp"

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/text/utf8.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::trace
{
    namespace
    {
        // The separator one nested step costs in the open path, so the budget
        // measures what a reader sees rather than the bare sum of the names.
        constexpr auto k_stepPathSeparatorBytes = std::size_t{1};

        // Refuses a label a project supplied. UTF-8 rather than ASCII because this
        // project's tasks are written in Chinese as often as in English; what is
        // refused is what corrupts a line rather than what is unfamiliar -- an
        // ill-formed sequence makes the trace file itself invalid UTF-8, and a
        // control byte is acted on by a reader's terminal rather than printed.
        [[nodiscard]]
        auto checkLabel(std::string const& label, std::string_view what) -> Status
        {
            if (label.empty())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::string{what} + " must not be empty"
                );
            }
            if (label.size() > k_maxScopeLabelBytes)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::string{what} + " is longer than the host's label ceiling"
                );
            }
            if (!isValidUtf8(label))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::string{what} + " must be valid UTF-8"
                );
            }
            for (auto const character : label)
            {
                auto const byte = static_cast<uint8>(character);
                if (byte < 0x20U || byte == 0x7FU)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::string{what} + " must not contain a control character"
                    );
                }
            }
            return ok();
        }

        // Its absence is a broken host rather than a broken request: nothing
        // outside this binary builds a TraceEvent.
        [[nodiscard]]
        auto requirePayload(TraceEvent const& event) -> Status
        {
            if (!event.framework.has_value())
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "a framework event reached the trace with no payload"
                );
            }
            return ok();
        }
    }

    TraceStreamValidator::TraceStreamValidator(FrontEnd frontEnd) noexcept
        : m_frontEnd{frontEnd}
    {
    }

    auto TraceStreamValidator::requireFramework() const -> Status
    {
        if (m_frontEnd != FrontEnd::Task)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "a framework event describing task orchestration -- step "
                "nesting, retry counting, interrupt matching -- reached a "
                "stream that runs none"
            );
        }
        return ok();
    }

    auto TraceStreamValidator::requireAnnotation() const -> Status
    {
        if (m_frontEnd != FrontEnd::Annotation)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "an annotation event reached a stream no exploration session drove"
            );
        }
        return ok();
    }

    auto TraceStreamValidator::refuseAnnotationVocabularyClash() const -> Status
    {
        if (m_frontEnd == FrontEnd::Annotation)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "engine.action_delivered reached the exploration stream, where a "
                "delivered click is annotation.click_delivered"
            );
        }
        return ok();
    }

    auto TraceStreamValidator::requireAnnotationPayload(
        TraceEvent const& event
    ) const -> Status
    {
        if (!event.annotation.has_value())
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "an annotation event carries no annotation payload"
            );
        }
        if (event.kind == TraceEventKind::AnnotationClickDelivered)
        {
            if (!event.annotation->point.has_value())
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "annotation.click_delivered carries no point, which is the "
                    "whole of what it records"
                );
            }
            return ok();
        }
        if (
            !event.annotation->rect.has_value()
            || !event.annotation->contentHash.has_value()
        )
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "annotation.region_saved needs both the rect it copied and the "
                "hash of the bytes it produced; either alone names no evidence"
            );
        }
        return ok();
    }

    auto TraceStreamValidator::requireScrollPayload(
        TraceEvent const& event
    ) -> Status
    {
        if (!event.wheelNotches.has_value())
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "engine.scroll_delivered carries no wheel delta, which is the "
                "whole of what it records"
            );
        }
        return ok();
    }

    auto TraceStreamValidator::requireCaptureRetryPayload(
        TraceEvent const& event
    ) -> Status
    {
        if (!event.captureAttempt.has_value())
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "engine.capture_retried carries no attempt number, which is the "
                "whole of what it records"
            );
        }
        return ok();
    }

    auto TraceStreamValidator::requireLongPressPayload(
        TraceEvent const& event
    ) -> Status
    {
        if (!event.clickClient.has_value())
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "engine.long_press_delivered carries no point, so nothing says "
                "where the button went down"
            );
        }
        if (!event.holdMillis.has_value())
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "engine.long_press_delivered carries no hold, which is the whole "
                "of what separates it from the click at the same point"
            );
        }
        return ok();
    }

    auto TraceStreamValidator::requireMovePayload(
        TraceEvent const& event
    ) -> Status
    {
        if (!event.clickClient.has_value())
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "engine.pointer_move_delivered carries no point, which is the "
                "whole of what it records"
            );
        }
        return ok();
    }

    auto TraceStreamValidator::admit(
        TraceEvent const& event
    ) -> Result<std::vector<std::string>>
    {
        // Read the scope before applying, so the returned stamp is the scope the
        // event happened INSIDE rather than the one it leaves behind.
        auto scope = m_steps;
        UF_TRY(apply(event));
        return scope;
    }

    auto TraceStreamValidator::apply(TraceEvent const& event) -> Status
    {
        if (m_runFinished)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "the run bracket is closed; no event is accepted after run.finished"
            );
        }

        switch (event.kind)
        {
        case TraceEventKind::RunStarted:
            if (m_runStarted)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "a run starts exactly once; run.started is already recorded"
                );
            }
            m_runStarted = true;
            return ok();

        case TraceEventKind::RunFinished:
            m_runFinished = true;
            return ok();

        case TraceEventKind::FrameworkStepStarted:
            UF_TRY(requireFramework());
            UF_TRY(requirePayload(event));
            return startStep(*event.framework);

        case TraceEventKind::FrameworkStepFinished:
            UF_TRY(requireFramework());
            UF_TRY(requirePayload(event));
            return finishStep(*event.framework);

        case TraceEventKind::FrameworkRetryAttempt:
            UF_TRY(requireFramework());
            UF_TRY(requirePayload(event));
            return retryAttempt(*event.framework);

        case TraceEventKind::FrameworkInterruptMatched:
            UF_TRY(requireFramework());
            UF_TRY(requirePayload(event));
            return matchInterrupt(*event.framework);

        case TraceEventKind::FrameworkInterruptHandled:
            UF_TRY(requireFramework());
            UF_TRY(requirePayload(event));
            return closeInterrupt(*event.framework, "interrupt_handled");

        case TraceEventKind::FrameworkInterruptExhausted:
            UF_TRY(requireFramework());
            UF_TRY(requirePayload(event));
            return closeInterrupt(*event.framework, "interrupt_exhausted");

        case TraceEventKind::FrameworkRetryBackoff:
        case TraceEventKind::FrameworkSettled:
            // A declared pause opens no scope: its whole content is the duration.
            UF_TRY(requireFramework());
            return requirePayload(event);

        case TraceEventKind::FrameworkPageResolved:
            // Opens no scope, and admitted on EVERY stream -- the long press's
            // precedent rather than step_started's. The front-end rule guards
            // framework STRUCTURE, which a stream running no task cannot have;
            // this claims none, and a run that only measures resolves against
            // the same page model a task does -- regress.check is that sweep,
            // and it runs on the check stream and on the exploration one -- so
            // refusing it there would fail the sweep rather than catch a bug.
            //
            // Its whole content is a NAME, so the label carries the requirement
            // a step name does: a line saying a page resolved without saying
            // which one names no page at all, and a reader cannot subtract it
            // from the sequence later.
            UF_TRY(requirePayload(event));
            return checkLabel(event.framework->label, "a resolved page name");

        case TraceEventKind::EngineActionDelivered:
            // On the exploration stream a delivered click is written under the
            // annotation vocabulary, so this spelling reaching that stream means
            // an emitter chose the wrong one.
            return refuseAnnotationVocabularyClash();

        case TraceEventKind::EngineScrollDelivered:
            // Host-authored, so the run bracket is the only structural rule that
            // binds it -- but the delta is the whole of what it records.
            return requireScrollPayload(event);

        case TraceEventKind::EngineCaptureRetried:
            return requireCaptureRetryPayload(event);

        case TraceEventKind::EngineLongPressDelivered:
            // Host-authored, and admitted on EVERY stream including the exploration
            // one -- the scroll's precedent rather than the click's, because
            // `long_press_delivered` claims no recognition, only what happened.
            return requireLongPressPayload(event);

        case TraceEventKind::EnginePointerMoveDelivered:
            // Admitted on every stream, the long press's precedent: a move claims
            // no recognition, and the exploration vocabulary has no spelling of
            // its own for it to have chosen instead.
            return requireMovePayload(event);

        case TraceEventKind::AnnotationClickDelivered:
        case TraceEventKind::AnnotationRegionSaved:
            UF_TRY(requireAnnotation());
            return requireAnnotationPayload(event);

        case TraceEventKind::RunResourcesValidated:
        case TraceEventKind::EngineObserved:
        case TraceEventKind::EngineActionFound:
        case TraceEventKind::EngineTextRead:
        case TraceEventKind::EngineActionAuthorized:
        case TraceEventKind::EngineActionRejected:
        case TraceEventKind::EngineKeyDelivered:
        case TraceEventKind::EngineObservationInvalidated:
        case TraceEventKind::TaskNativeCall:
            // Host-authored, bound only by the run bracket above. Their step scope
            // is stamped rather than asserted, which is what makes "a native call
            // falls inside the step scope open at the time" hold by construction.
            return ok();
        }

        UF_UNREACHABLE_MSG("Unknown TraceEventKind value");
    }

    auto TraceStreamValidator::startStep(
        TraceEvent::Framework const& payload
    ) -> Status
    {
        UF_TRY(checkLabel(payload.label, "a step name"));

        if (m_steps.size() >= k_maxScopeDepth)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "steps are nested deeper than the host's ceiling"
            );
        }

        auto const separator =
            m_steps.empty() ? std::size_t{0} : k_stepPathSeparatorBytes;
        auto const pathBytes = m_stepPathBytes + separator + payload.label.size();
        if (pathBytes > k_maxStepPathBytes)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "the open step path exceeds the host's total payload budget"
            );
        }

        m_stepPathBytes = pathBytes;
        m_steps.emplace_back(payload.label);
        return ok();
    }

    auto TraceStreamValidator::finishStep(
        TraceEvent::Framework const& payload
    ) -> Status
    {
        if (m_steps.empty())
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "step_finished with no step open"
            );
        }
        if (m_steps.back() != payload.label)
        {
            // Naming an outer step would close it around a still-open inner one,
            // and the path stamped on every line since would have been a lie.
            return fail(
                AutomationErrorKind::InternalInvariant,
                "step_finished must name the innermost open step"
            );
        }

        auto const separator =
            m_steps.size() > 1U ? k_stepPathSeparatorBytes : std::size_t{0};
        m_stepPathBytes -= separator + payload.label.size();
        m_steps.pop_back();

        // A retry scope opened inside the step that just closed cannot be
        // continued, so it stops competing for a later attempt's match.
        while (!m_retries.empty() && m_retries.back().stepDepth > m_steps.size())
        {
            m_retries.pop_back();
        }
        return ok();
    }

    auto TraceStreamValidator::retryAttempt(
        TraceEvent::Framework const& payload
    ) -> Status
    {
        if (!payload.attempt.has_value() || !payload.attempts.has_value())
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "retry_attempt carries neither an attempt nor a declared total"
            );
        }

        auto const attempt  = *payload.attempt;
        auto const attempts = *payload.attempts;
        if (attempt == 0U || attempts == 0U)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "a retry attempt and its declared total both start at one"
            );
        }
        if (attempt > attempts)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "retry_attempt exceeds the attempts its policy declared"
            );
        }

        if (attempt == 1U)
        {
            // Retry scopes are matched rather than bracketed: section 12 gives the
            // framework no scope-open event, so a first attempt opens one. The stack
            // drops its OUTERMOST entry when full, bounding what a long loop of
            // short retries costs; all that loses is matching reach for a scope
            // nested beyond the cap, since every rule above is checked first.
            if (m_retries.size() >= k_maxTrackedRetryScopes)
            {
                m_retries.erase(m_retries.begin());
            }
            m_retries.emplace_back(
                RetryScope{
                    .attempt   = attempt,
                    .attempts  = attempts,
                    .stepDepth = m_steps.size(),
                }
            );
            return ok();
        }

        // Unwind to the scope this attempt continues: an inner retry that gave up
        // leaves its scope on the stack, and the outer attempt that follows is
        // the next number of ITS scope.
        while (!m_retries.empty())
        {
            auto& top = m_retries.back();
            if (top.attempts == attempts && top.attempt + 1U == attempt)
            {
                top.attempt = attempt;
                return ok();
            }
            m_retries.pop_back();
        }

        return fail(
            AutomationErrorKind::InternalInvariant,
            "retry_attempt continues no open retry scope monotonically"
        );
    }

    auto TraceStreamValidator::matchInterrupt(
        TraceEvent::Framework const& payload
    ) -> Status
    {
        UF_TRY(checkLabel(payload.label, "an interrupt id"));

        if (m_interrupts.size() >= k_maxScopeDepth)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "interrupt matches are nested deeper than the host's ceiling"
            );
        }
        if (std::ranges::contains(m_interrupts, payload.label))
        {
            // Section 12's "that id must have no nested match": an interrupt
            // handling itself never dismissed what it matched, and the hit budget
            // cannot bound it because each nesting level spends a fresh one.
            return fail(
                AutomationErrorKind::InternalInvariant,
                "interrupt_matched nests a match of an id already matched"
            );
        }

        m_interrupts.emplace_back(payload.label);
        return ok();
    }

    auto TraceStreamValidator::closeInterrupt(
        TraceEvent::Framework const& payload,
        std::string_view verb
    ) -> Status
    {
        if (m_interrupts.empty())
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::string{verb} + " follows no interrupt_matched"
            );
        }
        if (m_interrupts.back() != payload.label)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::string{verb} + " must name the innermost matched interrupt"
            );
        }

        m_interrupts.pop_back();
        return ok();
    }

    auto TraceStreamValidator::requireScopesClosed() const -> Status
    {
        if (!m_steps.empty())
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "the run ended with the step '" + m_steps.back() + "' still open"
            );
        }
        if (!m_interrupts.empty())
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "the run ended while the interrupt '" + m_interrupts.back()
                    + "' was still being handled"
            );
        }
        return ok();
    }
}
