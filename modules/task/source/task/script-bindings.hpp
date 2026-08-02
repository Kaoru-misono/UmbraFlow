#pragma once

#include <core/types/integer.hpp>

#include <script/engine.hpp>

#include <string>
#include <vector>

namespace uf::task
{
    class TaskContext;

    // Which of the two Luau environments a VM is being booted for, and therefore
    // which primitives its private capability surface carries. This is the
    // enforcement, not a label: the privileged verbs must not EXIST in the
    // business environment rather than be refused there call by call
    // (docs/plans/2026-08-01-three-layers-and-agent-operator.md section 2 rule 2),
    // so a Run surface is built without those keys and there is nothing for a
    // refusal to guard. The difference is observable in buildPrivateSurface and
    // nowhere else.
    //
    // Run is first so a default-initialised value is the narrow surface.
    enum class ScriptTrustMode : uint8
    {
        // A business task or a host routine: the primitives every operator of
        // this system may reach.
        Run,

        // An agent measuring a target it has no model of yet: the Run surface
        // plus the bare-coordinate click, the crop and the probe.
        Exploration,
    };

    // The two surfaces a task VM is booted with, and the line between them.
    //
    // The PUBLIC one is data: the frozen global `uf`, carrying the error-kind
    // constants and nothing else. A project script may name it because none of it
    // can act. Its `elements` and `pages` name tables are gone -- layer two reads
    // the project file and hands a script its own element and page objects
    // (docs/plans/2026-07-31-script-owned-page-model.md 9) -- and what survives of
    // them is the pre-VM check in task/script-validator.hpp, which resolves a
    // `uf.elements.<name>` literal against the project file instead.
    //
    // The PRIVATE one is capability: the observation-cycle primitives, the time
    // primitives, raise and random. It is never registered as a global and never
    // becomes a key of any table either environment can reach; the boot hands it
    // to the trusted framework as a chunk argument, so it survives only as a
    // closure upvalue there.
    //
    // These are free functions rather than methods of an object because there is
    // no per-project surface to be an instance of: every one of them describes
    // this binary. No Luau type appears in this header; the Luau work is behind
    // ffi.

    // The global names the data installer registers, in the shape
    // script::EngineConfig::projectGlobals takes. A name listed here that the
    // installer did not register fails VM creation, so the two cannot silently
    // disagree.
    [[nodiscard]] auto scriptProjectGlobals() -> std::vector<std::string>;

    // The decoder script::EngineConfig::classifyRaisedError takes: it reads the
    // automation kind out of an uncaught Tier B error carrier, so a run is
    // reported and traced under the kind that actually failed rather than as a
    // malformed script. It decides on the carrier's userdata tag alone, which is
    // the whole reason the carrier is host-minted userdata: a table forged by a
    // project script carries no tag, however exactly it copies a real error's
    // fields.
    [[nodiscard]] auto scriptRaisedErrorClassifier() -> script::RaisedErrorClassifier;

    // A host-table installer suitable for script::EngineConfig::installHostTables.
    // Invoked once per task VM before the sandbox freezes the globals, it builds
    // the frozen global `uf`. It takes no TaskContext because nothing it builds
    // can act: the data surface is the same table whether or not a session is
    // bound.
    [[nodiscard]] auto scriptHostTableInstaller() -> script::HostTableInstaller;

    // The private capability surface for a live task, suitable for
    // script::EngineConfig::installPrivateCapabilities, bound to `context`'s
    // EngineSession and its cycle ledger. It also registers the handle metatables
    // only a bound session can mint -- the cycle ticket, the match, the template,
    // the reading and the deadline -- and carries the Tier B error label to the
    // framework.
    //
    // The returned installer captures a raw pointer to `context`. The caller MUST
    // keep `context` alive for at least as long as the script::Engine the
    // installer configures, because the VM's host functions dereference that
    // pointer on every primitive call; the TaskContext is non-movable so the
    // address stays stable.
    //
    // `mode` decides which primitives are on the surface at all: an Exploration
    // surface carries three keys a Run surface does not have, and a Run surface
    // has no way to reach them.
    [[nodiscard]]
    auto scriptPrivateCapabilities(TaskContext& context, ScriptTrustMode mode)
        -> script::PrivateCapabilityInstaller;
}
