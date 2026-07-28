#pragma once

#include <task/script-validator.hpp>

#include <core/error/error.hpp>
#include <core/types/integer.hpp>

#include <trace/event.hpp>

#include <string>

namespace uf::task
{
    // What the host knows about one run before its VM exists: which project and
    // task it addresses, the bytes the task was compiled from, and the seed its
    // RNG will draw from.
    //
    // The framework version and bundle hash, and the Luau compiler version, are
    // deliberately NOT part of it. They are properties of this binary rather than
    // of the run, and runStartedEvent reads them itself, so a composition root
    // cannot ship a run.started that two different framework builds would write
    // identically -- which is the whole point of stamping them.
    struct RunStartSpec final
    {
        std::string projectId{};
        std::string taskName{};
        std::string sourceHash{};
        uint64      seed{};
    };

    // The three run-level events, which are host-authoritative: a script can
    // never request one, and they are the only place a run's identity, its
    // validated resource closure, and how it ended are written down. They live
    // here rather than in the composition root so they are reachable from a test
    // and so stage 1d's TaskHost inherits them by calling the same functions.

    [[nodiscard]]
    auto runStartedEvent(RunStartSpec const& spec) -> trace::TraceEvent;

    [[nodiscard]]
    auto runResourcesValidatedEvent(
        ScriptResourceReport const& report
    ) -> trace::TraceEvent;

    // How a run ended: `p_failure` is null for a clean script return, otherwise
    // it observes the error that ended the run. A cancellation spends the
    // generation and is Cancelled; every other failure is Failed under its own
    // automation kind, with an unclassified error counted as InternalInvariant so
    // the line always names a kind.
    [[nodiscard]]
    auto runFinishedEvent(Error const* p_failure) -> trace::TraceEvent;
}
