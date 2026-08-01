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
        // The separator one nested step costs in the open path, so the total
        // payload budget measures what a reader actually sees rather than the
        // bare sum of the names.
        constexpr auto k_stepPathSeparatorBytes = std::size_t{1};

        // Refuses a label a project supplied: empty, over-long, not valid UTF-8,
        // or carrying a control byte.
        //
        // UTF-8 rather than ASCII is the rule on purpose. A step name is written
        // to be read by whoever wrote the task, and this project's tasks are
        // written in Chinese as often as in English; an ASCII-only character set
        // would refuse the names a real daily uses. What is refused is what
        // corrupts a line rather than what is unfamiliar: an ill-formed sequence
        // (which would make the trace file itself invalid UTF-8) and a control
        // byte (which a reader's terminal would act on rather than print).
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

        // The payload a framework event must carry. Its absence is a broken host
        // rather than a broken request: nothing outside this binary builds a
        // TraceEvent.
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
                "a framework event reached a stream no Luau framework drove"
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
            // A declared pause opens no scope: its whole content is the duration,
            // which the emitting boundary already converted and bounded.
            UF_TRY(requireFramework());
            return requirePayload(event);

        case TraceEventKind::EngineActionDelivered:
            // The mirror of the rule below, and the half that gives "never
            // engine.action_delivered" teeth. On the exploration stream a
            // delivered click is written under the annotation vocabulary, so
            // this spelling reaching that stream means an emitter chose the
            // wrong one -- which is a bug in this binary and not something a
            // reader should have to notice for themselves.
            return refuseAnnotationVocabularyClash();

        case TraceEventKind::AnnotationClickDelivered:
        case TraceEventKind::AnnotationRegionSaved:
            // Stated against the one front-end that has these verbs, exactly as
            // requireFramework is stated against FrontEnd::Task: a front-end
            // added later is refused by construction rather than by someone
            // remembering to list it.
            UF_TRY(requireAnnotation());
            return requireAnnotationPayload(event);

        case TraceEventKind::RunResourcesValidated:
        case TraceEventKind::EngineObserved:
        case TraceEventKind::EnginePageResolved:
        case TraceEventKind::EngineActionFound:
        case TraceEventKind::EngineTextRead:
        case TraceEventKind::EngineActionAuthorized:
        case TraceEventKind::EngineActionRejected:
        case TraceEventKind::EngineKeyDelivered:
        case TraceEventKind::EngineObservationInvalidated:
        case TraceEventKind::TaskNativeCall:
            // Host-authored lines. The only rule that binds them is the run
            // bracket above; the step scope they belong to is stamped rather than
            // asserted, which is what makes "a native call falls inside the step
            // scope open at the time" hold by construction.
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
            // Strict well-nestedness, stated as the only thing a finish may say:
            // naming an outer step would close it around a still-open inner one,
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
            // Retry scopes are matched rather than bracketed: section 12 gives
            // the framework no scope-open or scope-close event, so a first
            // attempt is what opens one. The stack is capped and drops its
            // OUTERMOST entry when full, which bounds the memory a long loop of
            // short retries can cost. What that costs is only matching reach for
            // an outer scope nested beyond the cap; every rule below -- an
            // attempt never exceeding its declared total, and never skipping a
            // number within a scope -- is checked before the stack is consulted.
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
            // Section 12's "that id must have no nested match": one interrupt
            // handling itself is a handler that never dismissed what it matched,
            // and the hit budget could not bound it because each nesting level
            // spends a fresh one.
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
