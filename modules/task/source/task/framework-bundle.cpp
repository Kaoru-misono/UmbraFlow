#include "framework-bundle.hpp"

#include <script/engine.hpp>

#include <string>
#include <vector>

namespace uf::task
{
    namespace
    {
        // The only framework export reachable by project-authored source before
        // Operator exists. Explore owns its capture bracket and receives the
        // authoring-only native surface privately.
        constexpr auto k_exploreModule = "explore";
    }

    auto frameworkScriptModules() -> std::vector<script::FrameworkModule>
    {
        auto const entries = frameworkBundleEntries();

        auto modules = std::vector<script::FrameworkModule>{};
        modules.reserve(entries.size());
        for (auto const& entry : entries)
        {
            modules.emplace_back(
                script::FrameworkModule{
                    .name   = entry.name,
                    .source = entry.source,
                }
            );
        }
        return modules;
    }

    auto frameworkProjectGlobals() -> std::vector<std::string>
    {
        // Business execution is disabled until Operator can provide the trusted
        // plan, policy, approval, and delivery boundary. An empty whitelist is
        // the mechanism: loading the trusted framework publishes no Runtime or
        // Receipt closure.
        return {};
    }

    auto explorationProjectGlobals() -> std::vector<std::string>
    {
        return std::vector<std::string>{
            std::string{k_exploreModule},
        };
    }
}
