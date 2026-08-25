#pragma once

#include <task/task-host.hpp>

#include <script/engine.hpp>

#include <string>
#include <vector>

namespace uf::task
{
    // The trusted RuntimeModel resolver's Engine surface. It can parse and
    // measure a pinned model and cannot issue a Tool call. Interactive code does
    // not boot this environment; ScopedToolSession gives it the same scoped
    // module closure and Tool Runtime primitive registered handlers use.
    [[nodiscard]] auto scriptProjectGlobals() -> std::vector<std::string>;
    [[nodiscard]] auto scriptRaisedErrorClassifier() -> script::RaisedErrorClassifier;
    [[nodiscard]] auto scriptHostTableInstaller() -> script::HostTableInstaller;

}
