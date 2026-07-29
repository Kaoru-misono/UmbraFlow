#include "framework-bundle.hpp"

#include <script/engine.hpp>

#include <string>
#include <vector>

namespace uf::task
{
    namespace
    {
        // The framework module whose frozen exports become the project global of
        // the same name. Spelled once so the bundle and the whitelist agree.
        constexpr auto k_contextModule = "ctx";
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
        return std::vector<std::string>{std::string{k_contextModule}};
    }
}
