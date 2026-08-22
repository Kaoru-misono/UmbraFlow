#include "framework-bundle.hpp"

#include <script/engine.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
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

        // A release-owned module that carries a reserved name but no public
        // spelling. Two populations wear this shape and they are not
        // interchangeable, so each keeps its own table: the internal pure data
        // modules below travel into a Project VM's closure with
        // projectVisible=false, while the trusted-only modules never enter one
        // at all.
        struct InternalPureModuleBinding final
        {
            std::string_view privateName;
            std::string_view reservedName;
            std::size_t      dependencyDepth{};
        };

        // The release-owned modules that never leave the trusted VM. They are
        // absent from the pure SDK closure entirely, so no Project-authored
        // module can resolve one; their reserved names exist so that one trusted
        // module can require another, which is the only route between Framework
        // modules now that loading publishes no global.
        struct TrustedModuleBinding final
        {
            std::string_view privateName;
            std::string_view reservedName;
            std::size_t      dependencyDepth{};
        };

        // A release-owned module that is publicly nameable but reaches the world
        // through one native seam, so it loads only inside a
        // script::ScopedToolProgram. It is deliberately absent from every
        // trusted Engine VM and from the pure closure: those environments build
        // no Tool Runtime capability table, and a scoped module that loaded
        // without one would either fail the whole generation or -- far worse --
        // grow a branch that pretends a Tool call can be skipped.
        struct ScopedModuleBinding final
        {
            std::string_view privateName;
            std::string_view publicName;
            std::size_t      dependencyDepth{};
        };

        // Which of the four declaration tables below admits a module, spelled
        // as one ASCII token. It is a field of the bundle identity in its own
        // right because the alias cannot stand in for it: the internal pure
        // data modules and the trusted-only modules both carry an
        // `@umbraflow/internal/` alias, so moving a module from
        // k_trustedModuleBindings to k_internalPureModuleBindings would put its
        // source inside every Project VM's closure while changing neither its
        // name, nor its alias, nor its depth, nor one byte of its source.
        constexpr auto k_pureTier         = std::string_view{"pure"};
        constexpr auto k_internalPureTier = std::string_view{"internal-pure"};
        constexpr auto k_trustedTier      = std::string_view{"trusted"};
        constexpr auto k_scopedTier       = std::string_view{"scoped"};

        // What a module embedded under modules/task/runtime/ that no table
        // declares contributes to the identity. Nothing in a passing tree is in
        // that state -- a case in tests/task/test-framework-bundle.cpp refuses
        // an embedded module that declares no reserved name -- so this token is
        // what the recipe says an undeclared module would hash as, not a branch
        // any green run takes.
        constexpr auto k_undeclaredTier = std::string_view{"undeclared"};

        struct FrameworkResolverSpec final
        {
            std::string_view name;
            std::string_view tier;
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

        constexpr auto k_trustedModuleBindings = std::array{
            TrustedModuleBinding{"evidence", "@umbraflow/internal/evidence", 0U},
            TrustedModuleBinding{"explore", "@umbraflow/internal/explore", 0U},
            TrustedModuleBinding{"model", "@umbraflow/internal/model", 0U},
            TrustedModuleBinding{"observe", "@umbraflow/internal/observe", 2U},
            TrustedModuleBinding{"project", "@umbraflow/internal/project", 1U},
            TrustedModuleBinding{
                "resolution",
                "@umbraflow/internal/resolution",
                1U,
            },
        };

        // The four scoped facades, in the exact order and under the exact names
        // script::ScopedToolProgram::scopedModuleNames() states. A name that
        // moved here without moving there is a program that cannot compile, and
        // a test binds the two lists to each other.
        //
        // Depths continue the pure tier rather than restarting: `tools` owns the
        // single call primitive and the other three reach the Tool Runtime only
        // through it, which is why nothing at depth 4 requires anything else at
        // depth 4.
        constexpr auto k_scopedModuleBindings = std::array{
            ScopedModuleBinding{"audit", "@umbraflow/audit", 4U},
            ScopedModuleBinding{"screen", "@umbraflow/screen", 4U},
            ScopedModuleBinding{"tools", "@umbraflow/tools", 3U},
            ScopedModuleBinding{"workflow", "@umbraflow/workflow", 4U},
        };

        constexpr auto k_maximumScopedDependencyDepth = std::size_t{4U};

        // The read-only JSON resource `@umbraflow/tools` reads the run's pinned
        // Tool catalog from. It is stated here as well as inside tools.luau
        // because a Luau source cannot read a C++ constant; a case in
        // tests/task/test-framework-bundle.cpp asserts this exact spelling
        // appears in the embedded module source, so the two cannot drift.
        constexpr auto k_scopedToolCatalogResource =
            std::string_view{"umbraflow.tool-catalog"};

        constexpr auto k_maximumFrameworkDependencyDepth = std::size_t{2U};

        [[nodiscard]]
        auto scopedModuleBinding(std::string_view privateName)
            -> ScopedModuleBinding const*
        {
            auto const found = std::ranges::find(
                k_scopedModuleBindings,
                privateName,
                &ScopedModuleBinding::privateName
            );
            return found == k_scopedModuleBindings.end() ? nullptr : &*found;
        }

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
                    .tier            = k_pureTier,
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
                    .tier            = k_internalPureTier,
                    .dependencyDepth = internalBinding->dependencyDepth,
                };
            }

            auto const trustedBinding = std::ranges::find(
                k_trustedModuleBindings,
                privateName,
                &TrustedModuleBinding::privateName
            );
            if (trustedBinding != k_trustedModuleBindings.end())
            {
                return FrameworkResolverSpec{
                    .name            = trustedBinding->reservedName,
                    .tier            = k_trustedTier,
                    .dependencyDepth = trustedBinding->dependencyDepth,
                };
            }

            // The scoped facades are read here too, even though no caller of
            // this function loads one: the bundle identity below must speak for
            // every embedded module, and the four scoped names and depths enter
            // no registration-level digest at all today. Every existing caller
            // filters a scoped entry out before asking, so admitting it here
            // changes no closure.
            auto const* const scoped = scopedModuleBinding(privateName);
            if (scoped != nullptr)
            {
                return FrameworkResolverSpec{
                    .name            = scoped->publicName,
                    .tier            = k_scopedTier,
                    .dependencyDepth = scoped->dependencyDepth,
                };
            }
            return std::nullopt;
        }

        // The exact byte string frameworkBundleHash() digests. It is built here
        // rather than in scripts/embed_luau.py because three of the five fields
        // it writes per entry -- the tier, the reserved alias and the dependency
        // depth -- are declared in the tables above and are invisible to a
        // reader of the .luau files. framework-bundle.hpp states the recipe; this is its one
        // implementation.
        [[nodiscard]]
        auto bundleIdentityPreimage() -> std::string
        {
            auto preimage = std::string{};
            for (auto const& entry : frameworkBundleEntries())
            {
                auto const resolver = frameworkResolverSpec(entry.name);
                preimage += entry.name;
                preimage.push_back('\0');
                preimage += resolver.has_value() ? resolver->tier : k_undeclaredTier;
                preimage.push_back('\0');
                preimage += resolver.has_value()
                    ? resolver->name
                    : std::string_view{};
                preimage.push_back('\0');
                preimage += std::to_string(
                    resolver.has_value()
                        ? resolver->dependencyDepth
                        : std::size_t{}
                );
                preimage.push_back('\0');
                preimage += entry.source;
                preimage.push_back('\0');
            }
            return preimage;
        }
    }

    auto frameworkBundleHash() -> Result<std::string>
    {
        auto const preimage = bundleIdentityPreimage();
        UF_TRY_VALUE(digest, sha256(std::as_bytes(std::span{preimage})));
        return digest.hex();
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
                // A scoped facade never loads in a trusted Engine VM: that VM
                // builds no Tool Runtime capability table, so the module would
                // refuse at load and take the whole Framework generation with
                // it. Excluding it here is what keeps the authoring and runtime
                // environments free of it, and the exclusion is asserted rather
                // than assumed.
                if (scopedModuleBinding(entry.name) != nullptr)
                {
                    continue;
                }
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
                    .dependencyDepth = entryDepth,
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
                .name            = binding.publicName,
                .source          = found->source,
                .dependencyDepth = binding.dependencyDepth,
                .projectVisible  = true,
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
                .name            = binding.reservedName,
                .source          = found->source,
                .dependencyDepth = binding.dependencyDepth,
                .projectVisible  = false,
            });
        }
        return modules;
    }

    auto scopedFrameworkScriptModules()
        -> Result<std::vector<script::FrameworkModule>>
    {
        UF_TRY_VALUE(modules, pureFrameworkScriptModules());
        auto const entries = frameworkBundleEntries();
        modules.reserve(modules.size() + k_scopedModuleBindings.size());
        for (
            auto depth = std::size_t{};
            depth <= k_maximumScopedDependencyDepth;
            ++depth
        )
        {
            for (auto const& binding : k_scopedModuleBindings)
            {
                if (binding.dependencyDepth != depth)
                {
                    continue;
                }
                auto const found = std::ranges::find(
                    entries,
                    binding.privateName,
                    &FrameworkBundleEntry::name
                );
                if (found == entries.end())
                {
                    return fail(
                        AutomationErrorKind::InternalInvariant,
                        "the embedded Framework bundle is missing scoped module "
                            + std::string{binding.privateName}
                    );
                }
                modules.emplace_back(script::FrameworkModule{
                    .name            = binding.publicName,
                    .source          = found->source,
                    .dependencyDepth = binding.dependencyDepth,
                    .projectVisible  = true,
                });
            }
        }
        return modules;
    }

    auto scopedToolCatalogResourceName() noexcept -> std::string_view
    {
        return k_scopedToolCatalogResource;
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
