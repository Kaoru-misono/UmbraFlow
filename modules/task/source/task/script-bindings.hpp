#pragma once

#include <script/engine.hpp>

#include <string>
#include <vector>

namespace uf::task
{
    class TaskContext;

    // The two surfaces a task VM is booted with, and the line between them.
    //
    // The PUBLIC one is data: the frozen global `uf`, which now carries the
    // error-kind constants and nothing else. It is a global a project script may
    // name because none of it can act.
    //
    // Its `elements` and `pages` name tables are gone. They were built from the
    // C++ recognition catalog, and the catalog is no longer where a page model
    // lives: layer two reads the project file and hands a script its own element
    // and page objects (docs/plans/2026-07-31-script-owned-page-model.md 9).
    // What survives of the old surface is the pre-VM check the names existed for
    // -- see task/script-validator.hpp, which resolves a `uf.elements.<name>`
    // literal against the project file instead.
    //
    // The PRIVATE one is capability: the observation-cycle primitives, the time
    // primitives, raise and random. It is never registered as a global and never
    // becomes a key of any table either environment can reach; the boot hands it
    // to the trusted framework as a chunk argument, so it survives only as a
    // closure upvalue there.
    //
    // These are free functions rather than methods of an object because there is
    // no longer a per-project surface to be an instance of: every one of them
    // describes this binary. The installers are the whole boundary -- no Luau
    // type appears in this header, and the Luau work lives behind ffi.

    // The global names the data installer registers, in the shape
    // script::EngineConfig::projectGlobals takes. A name listed here that the
    // installer did not register fails VM creation, so the two statements cannot
    // silently disagree.
    [[nodiscard]] auto scriptProjectGlobals() -> std::vector<std::string>;

    // The decoder script::EngineConfig::classifyRaisedError takes: it reads the
    // automation kind out of a Tier B error carrier a run raised and nobody
    // caught, so the run is reported and traced under the kind that actually
    // failed rather than as a malformed script.
    //
    // It decides on the carrier's userdata tag alone. That is the whole reason
    // the carrier is host-minted userdata: a table forged by a project script
    // carries no tag, so it can name no kind here however exactly it copies a
    // real error's fields.
    [[nodiscard]] auto scriptRaisedErrorClassifier() -> script::RaisedErrorClassifier;

    // A host-table installer suitable for script::EngineConfig::installHostTables.
    // Invoked once per task VM before the sandbox freezes the globals, it builds
    // the frozen global `uf`.
    //
    // It takes no TaskContext because nothing it builds can act. That is the
    // point of the split: the data surface is the same table whether or not a
    // session is bound.
    [[nodiscard]] auto scriptHostTableInstaller() -> script::HostTableInstaller;

    // The private capability surface for a live task, suitable for
    // script::EngineConfig::installPrivateCapabilities, bound to `context`'s
    // EngineSession and its cycle ledger. It also registers the handle metatables
    // only a bound session can mint -- the cycle ticket, the match, the template,
    // the reading and the deadline -- and carries the Tier B error label to the
    // framework, which is the only piece of the surface that is data rather than
    // capability.
    //
    // The returned installer captures a raw pointer to `context`. The caller MUST
    // keep `context` alive for at least as long as the script::Engine the
    // installer configures, because the VM's host functions dereference that
    // pointer on every primitive call; the TaskContext is non-movable so the
    // address stays stable.
    [[nodiscard]]
    auto scriptPrivateCapabilities(TaskContext& context)
        -> script::PrivateCapabilityInstaller;
}
