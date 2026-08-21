#include "framework-bundle.hpp"

#include <script/engine.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace uf::task
{
    namespace
    {
        // The only framework export reachable by project-authored source before
        // Operator exists. Explore owns its capture bracket and receives the
        // authoring-only native surface privately.
        constexpr auto k_exploreModule = "explore";

        struct PureModuleBinding final
        {
            std::string_view privateName;
            std::string_view publicName;
        };

        constexpr auto k_pureModuleBindings = std::array{
            PureModuleBinding{"collections", "@umbraflow/collections"},
            PureModuleBinding{"jcs", "@umbraflow/jcs"},
            PureModuleBinding{"result", "@umbraflow/result"},
        };
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

    auto pureFrameworkScriptModules()
        -> Result<std::vector<script::FrameworkModule>>
    {
        auto const entries = frameworkBundleEntries();
        auto modules = std::vector<script::FrameworkModule>{};
        modules.reserve(k_pureModuleBindings.size());
        for (auto const& binding : k_pureModuleBindings)
        {
            auto const found = std::ranges::find(
                entries,
                binding.privateName,
                &FrameworkBundleEntry::name
            );
            if (found == entries.end())
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "the embedded Framework bundle is missing pure module "
                        + std::string{binding.privateName}
                );
            }
            modules.emplace_back(script::FrameworkModule{
                .name   = binding.publicName,
                .source = found->source,
            });
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

    auto runtimeProjectGlobals() -> std::vector<std::string>
    {
        return std::vector<std::string>{
            std::string{"jcs"},
            std::string{"observe"},
            std::string{"project"},
        };
    }
}
