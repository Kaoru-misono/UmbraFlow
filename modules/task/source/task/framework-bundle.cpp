#include "framework-bundle.hpp"

#include <script/engine.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <optional>
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
            std::size_t      dependencyDepth{};
        };

        struct InternalPureModuleBinding final
        {
            std::string_view privateName;
            std::string_view reservedName;
            std::size_t      dependencyDepth{};
        };

        struct FrameworkResolverSpec final
        {
            std::string_view name;
            std::size_t      dependencyDepth{};
        };

        constexpr auto k_pureModuleBindings = std::array{
            PureModuleBinding{"collections", "@umbraflow/collections", 0U},
            PureModuleBinding{"jcs", "@umbraflow/jcs", 0U},
            PureModuleBinding{"json", "@umbraflow/json", 1U},
            PureModuleBinding{"result", "@umbraflow/result", 0U},
            PureModuleBinding{"text", "@umbraflow/text", 2U},
            PureModuleBinding{"utf8", "@umbraflow/utf8", 1U},
        };

        constexpr auto k_internalPureModuleBindings = std::array{
            InternalPureModuleBinding{
                "unicode-text-data",
                "@umbraflow/internal/unicode-text-data",
                0U,
            },
            InternalPureModuleBinding{
                "unicode-utf8-data",
                "@umbraflow/internal/unicode-utf8-data",
                0U,
            },
        };

        constexpr auto k_maximumFrameworkDependencyDepth = std::size_t{2U};

        [[nodiscard]]
        auto frameworkResolverSpec(std::string_view privateName)
            -> std::optional<FrameworkResolverSpec>
        {
            auto const publicBinding = std::ranges::find(
                k_pureModuleBindings,
                privateName,
                &PureModuleBinding::privateName
            );
            if (publicBinding != k_pureModuleBindings.end())
            {
                return FrameworkResolverSpec{
                    .name            = publicBinding->publicName,
                    .dependencyDepth = publicBinding->dependencyDepth,
                };
            }

            auto const internalBinding = std::ranges::find(
                k_internalPureModuleBindings,
                privateName,
                &InternalPureModuleBinding::privateName
            );
            if (internalBinding != k_internalPureModuleBindings.end())
            {
                return FrameworkResolverSpec{
                    .name            = internalBinding->reservedName,
                    .dependencyDepth = internalBinding->dependencyDepth,
                };
            }
            return std::nullopt;
        }
    }

    auto frameworkScriptModules() -> std::vector<script::FrameworkModule>
    {
        auto const entries = frameworkBundleEntries();

        auto modules = std::vector<script::FrameworkModule>{};
        modules.reserve(entries.size());
        for (
            auto depth = std::size_t{};
            depth <= k_maximumFrameworkDependencyDepth;
            ++depth
        )
        {
            for (auto const& entry : entries)
            {
                auto const resolver = frameworkResolverSpec(entry.name);
                auto const entryDepth = resolver.has_value()
                    ? resolver->dependencyDepth
                    : std::size_t{};
                if (entryDepth != depth)
                {
                    continue;
                }
                modules.emplace_back(script::FrameworkModule{
                    .name   = entry.name,
                    .source = entry.source,
                    .resolverName = resolver.has_value()
                        ? resolver->name
                        : std::string_view{},
                });
            }
        }
        return modules;
    }

    auto pureFrameworkScriptModules()
        -> Result<std::vector<script::FrameworkModule>>
    {
        auto const entries = frameworkBundleEntries();
        auto modules = std::vector<script::FrameworkModule>{};
        modules.reserve(
            k_pureModuleBindings.size() + k_internalPureModuleBindings.size()
        );
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
                .name           = binding.publicName,
                .source         = found->source,
                .projectVisible = true,
            });
        }
        for (auto const& binding : k_internalPureModuleBindings)
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
                    "the embedded Framework bundle is missing internal pure module "
                        + std::string{binding.privateName}
                );
            }
            modules.emplace_back(script::FrameworkModule{
                .name           = binding.reservedName,
                .source         = found->source,
                .projectVisible = false,
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
