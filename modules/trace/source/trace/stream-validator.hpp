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
    // The longest a step name or an interrupt id may be, in bytes.
    //
    // CALIBRATION: sixty-four bytes is a conservative placeholder awaiting the
    // first real daily. It is long enough for a sentence-shaped step name in any
    // script, including a CJK one (about twenty characters), and short enough
    // that no line can be dominated by one label.
    inline constexpr auto k_maxScopeLabelBytes = std::size_t{64};

    // The hard nesting ceiling for steps, and separately for interrupt matches.
    // It is a ceiling rather than a budget: a task nested deeper than this has a
    // structural problem the trace should not try to record around.
    inline constexpr auto k_maxScopeDepth = std::size_t{16};

    // The total payload ceiling: the bytes of the whole open step path, names
    // plus one separator between them. It is the budget that actually matters,
    // because the path is stamped onto every line written while those steps are
    // open, so a legal-looking name at every legal-looking depth can still cost
    // more per line than any of them does on its own. Reaching it REJECTS the
    // step that would cross it; nothing is truncated.
    inline constexpr auto k_maxStepPathBytes = std::size_t{256};

    // How many nested retry scopes the matcher tracks at once. See the note on
    // the retry rules in stream-validator.cpp for what exceeding it costs; it is
    // far beyond any retry nesting a task can plausibly write.
    inline constexpr auto k_maxTrackedRetryScopes = std::size_t{32};

    // The protocol one run's evidence stream must obey, enforced where no
    // emitter can go around it: TraceRecorder owns one and runs it on every
    // event before the stamp, exactly as it owns the sequence counter.
    //
    // What it is for. The framework.* events are the only ones the trusted Luau
    // framework REQUESTS rather than the host authoring, so `emit` is not a
    // passthrough: a framework whose step nesting, retry counting or interrupt
    // matching has drifted would otherwise write a plausible-looking audit log
    // of a run that did not happen that way. Design section 9's rule 5 applies
    // to every refusal this reports as InternalInvariant -- the caller latches
    // the generation terminal BEFORE raising, or a project pcall swallows the
    // framework bug and the run carries on.
    //
    // Two failure kinds, and the split is deliberate:
    //
    //   - InvalidResource is a REQUEST the stream refuses: a step name that is
    //     too long, not printable, or would push the open path over its budget,
    //     and a depth beyond the ceiling. Every one of those originates in a
    //     project string literal or a project's own nesting, and section 9
    //     reserves the invariant kind for failures a project cannot cause. The
    //     caller raises it as an ordinary catchable Tier B failure.
    //   - InternalInvariant is a PROTOCOL breach: a second run.started, an event
    //     after run.finished, a step finish naming other than the innermost open
    //     step, a retry attempt that continues no open scope or exceeds its
    //     declared attempts, an interrupt handled that no match opened, a
    //     framework.* event on a stream no task drove, an annotation.* event on
    //     a stream no exploration session drove, and engine.action_delivered on
    //     the one stream that DOES have the annotation vocabulary. Only the
    //     framework or the host can produce one, so it is a bug in this binary.
    //
    // The front-end is part of the protocol rather than only part of the stamp.
    // framework.* events describe the trusted Luau framework's own structure --
    // which step is open, which retry attempt this is, which interrupt matched --
    // and on any stream but the task one that framework does not exist, so such a
    // line could only ever be a host bug attributing task structure to a
    // front-end that runs no script. The rule is stated against FrontEnd::Task
    // rather than against the others by name, so a front-end added later is
    // refused by construction instead of by remembering to list it. Refusing it
    // here is what keeps the attribution worth reading: `frontEnd` does not
    // merely label a stream, it decides which events the stream may hold.
    //
    // NOT thread-safe: one recorder writes one run on one thread.
    class TraceStreamValidator final
    {
        // One retry scope the matcher is tracking: the attempt it last saw, the
        // total the policy declared, and the step depth the scope was opened at.
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

        // Refuses a framework.* event on any stream but the task one. See the
        // class comment for why the front-end is a protocol rule, and why the
        // test is against FrontEnd::Task rather than against the others.
        [[nodiscard]] auto requireFramework() const -> Status;

        // The same rule for the two annotation.* kinds, stated against the one
        // front-end that has those verbs.
        [[nodiscard]] auto requireAnnotation() const -> Status;

        // Refuses engine.action_delivered on the exploration stream. It is the
        // other half of the same decision: the annotation vocabulary exists so a
        // bare coordinate is not recorded as an action against something the
        // model recognised, and a rule that only ADMITTED the honest spelling
        // would leave the dishonest one admissible beside it.
        [[nodiscard]] auto refuseAnnotationVocabularyClash() const -> Status;

        // The fields each annotation kind must carry. A click that names no
        // point and a saved region that names no rect or no hash are events with
        // no content, and an empty line is worse evidence than a refused one.
        [[nodiscard]]
        auto requireAnnotationPayload(TraceEvent const& event) const -> Status;

        // The same "an empty line is worse evidence than a refused one" rule for
        // engine.scroll_delivered, whose whole content is the detent count. It is
        // stated where the annotation payload rules are because it is the same
        // rule; it is stated at all because a scroll, unlike a click, has no
        // coordinate on the line to fall back on.
        [[nodiscard]]
        static auto requireScrollPayload(TraceEvent const& event) -> Status;

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

        // Admits `event` into the stream, returning the framework step scope that
        // was open WHEN IT WAS WRITTEN -- the scope the recorder stamps onto it --
        // or the reason the stream refuses it. A refused event never reaches a
        // sink and leaves no state behind, which is what "rejected at the request
        // boundary, never silently truncated" means in practice.
        //
        // The scope is the one open BEFORE the event is applied, so every line
        // reports the steps it happened inside: a framework.step_started names
        // its own step in `label` and carries its parent's path, and its matching
        // framework.step_finished carries the path including that step.
        [[nodiscard]]
        auto admit(TraceEvent const& event) -> Result<std::vector<std::string>>;

        // Fails InternalInvariant when a framework scope -- a step or an
        // interrupt match -- is still open. The run owner calls it before writing
        // run.finished so the closing line can report the failure rather than
        // hide it: design section 12 makes an unclosed step at run.finished a
        // Failed(InternalInvariant) run, and a bracket that never closed would be
        // worse evidence than one that closes on a failure.
        [[nodiscard]] auto requireScopesClosed() const -> Status;
    };
}
