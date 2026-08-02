#pragma once

#include "event.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace uf::trace
{
    // The longest a step name or an interrupt id may be, in bytes. A placeholder
    // until the first real daily: enough for a sentence-shaped step name, CJK
    // included (about twenty characters), without one label dominating a line.
    inline constexpr auto k_maxScopeLabelBytes = std::size_t{64};

    // The nesting ceiling for steps, and separately for interrupt matches. Deeper
    // nesting is a structural problem the trace should not record around.
    inline constexpr auto k_maxScopeDepth = std::size_t{16};

    // The total payload ceiling: the whole open step path, names plus one separator
    // each. It binds hardest because the path is stamped onto every line written
    // while those steps are open; a step crossing it is REJECTED, never truncated.
    inline constexpr auto k_maxStepPathBytes = std::size_t{256};

    // How many nested retry scopes the matcher tracks at once, far beyond any
    // nesting a task can plausibly write. retryAttempt in stream-validator.cpp
    // says what exceeding it costs.
    inline constexpr auto k_maxTrackedRetryScopes = std::size_t{32};

    // The protocol one run's evidence stream must obey, enforced where no emitter
    // can go around it: TraceRecorder owns one and runs it on every event before
    // the stamp. It is not a passthrough because framework.* events are the only
    // ones the trusted Luau framework REQUESTS rather than the host authoring, and
    // a framework whose step nesting, retry counting or interrupt matching has
    // drifted would otherwise write a plausible-looking audit log of a run that did
    // not happen that way. The front-end is part of that protocol and not only of
    // the stamp: framework.* describes structure a stream running no script cannot
    // have, and the annotation.* verbs exist on one front-end only. Each such rule
    // is stated against the front-end that HAS the vocabulary, so one added later
    // is refused by construction rather than by someone remembering to list it.
    //
    // The two failure kinds split by who can cause one. InvalidResource is a
    // request the stream refuses -- an over-budget or unprintable step name, a
    // depth past a ceiling -- each originating in a project's own literal or
    // nesting, so the caller raises it as an ordinary catchable Tier B failure.
    // InternalInvariant is a protocol breach only the framework or the host can
    // produce, so it is a bug in this binary; design section 9's rule 5 applies to
    // every one -- the caller latches the generation terminal BEFORE raising, or a
    // project pcall swallows the framework bug and the run carries on.
    //
    // NOT thread-safe: one recorder writes one run on one thread.
    class TraceStreamValidator final
    {
        // The step depth is where the scope opened, so closing that step drops it.
        struct RetryScope final
        {
            uint64      attempt{};
            uint64      attempts{};
            std::size_t stepDepth{};
        };

        FrontEnd m_frontEnd;

        std::vector<std::string> m_steps{};
        std::vector<std::string> m_interrupts{};
        std::vector<RetryScope>  m_retries{};

        std::size_t m_stepPathBytes{0};
        bool        m_runStarted{false};
        bool        m_runFinished{false};

        [[nodiscard]] auto apply(TraceEvent const& event) -> Status;

        // Refuses a framework.* event on any stream but the task one.
        [[nodiscard]] auto requireFramework() const -> Status;

        // The same rule for the two annotation.* kinds.
        [[nodiscard]] auto requireAnnotation() const -> Status;

        // Refuses engine.action_delivered on the exploration stream. Admitting only
        // the honest annotation spelling would leave the dishonest one -- a bare
        // coordinate recorded as an action against a recognised element -- beside it.
        [[nodiscard]] auto refuseAnnotationVocabularyClash() const -> Status;

        // The fields each annotation kind must carry: an empty line is worse
        // evidence than a refused one.
        [[nodiscard]]
        auto requireAnnotationPayload(TraceEvent const& event) const -> Status;

        // The same rule for engine.scroll_delivered, whose whole content is the
        // detent count: unlike a click it has no coordinate to fall back on.
        [[nodiscard]]
        static auto requireScrollPayload(TraceEvent const& event) -> Status;

        // Both halves are required for engine.long_press_delivered: the point says
        // where the button went down, and the hold is what makes it a long press
        // rather than the click at the same coordinate.
        [[nodiscard]]
        static auto requireLongPressPayload(TraceEvent const& event) -> Status;

        [[nodiscard]] auto startStep(TraceEvent::Framework const& payload) -> Status;
        [[nodiscard]] auto finishStep(TraceEvent::Framework const& payload) -> Status;
        [[nodiscard]] auto retryAttempt(TraceEvent::Framework const& payload) -> Status;
        [[nodiscard]] auto matchInterrupt(TraceEvent::Framework const& payload) -> Status;
        [[nodiscard]]
        auto closeInterrupt(
            TraceEvent::Framework const& payload,
            std::string_view verb
        ) -> Status;

    public:
        explicit TraceStreamValidator(FrontEnd frontEnd) noexcept;

        // Admits `event` into the stream and returns the framework step scope open
        // WHEN IT WAS WRITTEN -- what the recorder stamps onto it -- or the reason
        // the stream refuses it; a refused event reaches no sink and leaves no
        // state behind. The scope is read BEFORE the event is applied, so a
        // step_started carries its parent's path and its step_finished its own.
        [[nodiscard]]
        auto admit(TraceEvent const& event) -> Result<std::vector<std::string>>;

        // Fails InternalInvariant when a framework step or interrupt match is still
        // open. The run owner calls it before writing run.finished, so an unclosed
        // scope becomes the run's Failed(InternalInvariant) (design section 12).
        [[nodiscard]] auto requireScopesClosed() const -> Status;
    };
}
