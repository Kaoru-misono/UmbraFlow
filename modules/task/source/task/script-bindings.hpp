#pragma once

#include <script/engine.hpp>

#include <vector>

namespace uf::task
{
    class TaskContext;

    // Phase 1 has no business/native surface: scriptProjectGlobals() is empty
    // and no plugin environment carries a native.
    //
    // The Annotation surface DOES carry input primitives, and they are its own.
    // annotationPrivateCapabilities builds one table, TaskHost's Runtime
    // installer builds another, and neither table is reachable from the other
    // environment -- a private surface is handed to framework modules as a chunk
    // argument and then dropped, so naming it requires having been loaded while
    // it stood (script/ffi/sandbox.hpp). Which module each environment then
    // publishes is the second half: `explore` is in explorationProjectGlobals()
    // and in no other whitelist.
    [[nodiscard]] auto scriptProjectGlobals() -> std::vector<std::string>;
    [[nodiscard]] auto scriptRaisedErrorClassifier() -> script::RaisedErrorClassifier;
    [[nodiscard]] auto scriptHostTableInstaller() -> script::HostTableInstaller;

    [[nodiscard]]
    auto annotationPrivateCapabilities(TaskContext& context)
        -> script::PrivateCapabilityInstaller;
}
