#pragma once

#include <task/task-host.hpp>

#include <script/engine.hpp>

#include <string>
#include <vector>

namespace uf::task
{
    class TaskContext;

    // Phase 1 has no business/native surface: scriptProjectGlobals() is empty
    // and no plugin environment carries a native.
    //
    // NEITHER PRIVATE SURFACE CARRIES A HOST VERB ANY MORE. The exploration
    // table below holds exactly one primitive -- issue one Tool call -- which is
    // the same primitive script::ScopedToolProgram's own table holds, and the
    // trusted runtime table TaskHost installs holds the RuntimeModel resolution
    // primitives a Receipt flow needs. There is no authoring surface, no input
    // primitive on either table, and therefore no trust split between an
    // exploration VM and a production one: what an exploration chunk may do is
    // the Operator policy's answer about a Tool call, exactly as it is for every
    // other caller
    // (docs/decisions/2026-08-24-there-is-no-annotation-phase.md).
    //
    // The two tables are still separate objects and neither is reachable from
    // the other environment -- a private surface is handed to framework modules
    // as a chunk argument and then dropped, so naming it requires having been
    // loaded while it stood (script/ffi/sandbox.hpp). That is module wiring
    // rather than a permission boundary.
    [[nodiscard]] auto scriptProjectGlobals() -> std::vector<std::string>;
    [[nodiscard]] auto scriptRaisedErrorClassifier() -> script::RaisedErrorClassifier;
    [[nodiscard]] auto scriptHostTableInstaller() -> script::HostTableInstaller;

    // The exploration VM's private capability table: one primitive, `invoke`,
    // over the Tool Runtime `runtime` names, driven against `context`.
    //
    // Both borrows must outlive every VM this installer builds, which
    // ExplorationSession guarantees structurally by owning all three and
    // declaring the VM last.
    [[nodiscard]]
    auto explorationToolCapabilities(
        TaskContext& context,
        script::ToolRuntimeInvoke& runtime
    ) -> script::PrivateCapabilityInstaller;
}
