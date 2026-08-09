#pragma once

#include <script/engine.hpp>

#include <vector>

namespace uf::task
{
    class TaskContext;

    // Phase 1 has no business/native surface. The only private capabilities are
    // installed for privileged Annotation and contain no input primitive.
    [[nodiscard]] auto scriptProjectGlobals() -> std::vector<std::string>;
    [[nodiscard]] auto scriptRaisedErrorClassifier() -> script::RaisedErrorClassifier;
    [[nodiscard]] auto scriptHostTableInstaller() -> script::HostTableInstaller;

    [[nodiscard]]
    auto annotationPrivateCapabilities(TaskContext& context)
        -> script::PrivateCapabilityInstaller;
}
