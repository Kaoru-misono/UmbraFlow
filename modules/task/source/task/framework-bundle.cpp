#include "framework-bundle.hpp"

#include <script/engine.hpp>

#include <string>
#include <vector>

namespace uf::task
{
    namespace
    {
        // The framework modules whose frozen exports become project globals of
        // the same name. Spelled once so the bundle and the whitelist agree.
        //
        // `ctx` is what a task does while it runs; `task` is what it declares
        // about itself beforehand. `task` is the transitional spelling of the
        // design's `uf.task`, which cannot be a member of the frozen `uf` table
        // the host installs after the bundle has already loaded.
        constexpr auto k_contextModule     = "ctx";
        constexpr auto k_declarationModule = "task";
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
        return std::vector<std::string>{
            std::string{k_contextModule},
            std::string{k_declarationModule},
        };
    }
}
