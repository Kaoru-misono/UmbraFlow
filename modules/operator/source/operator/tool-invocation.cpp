#include "tool-invocation.hpp"

#include <json/value.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uf::operator_runtime
{
    namespace
    {
        constexpr auto k_frameworkNamespace = std::string_view{"framework."};
        constexpr auto k_observeTool = std::string_view{
            "framework.screen.observe"
        };
        constexpr auto k_waitTool = std::string_view{
            "framework.workflow.wait"
        };
        constexpr auto k_frameworkToolVersion = std::string_view{"1"};
        constexpr auto k_maximumObserveMillis = uint64{10'000U};
        constexpr auto k_maximumWaitMillis = uint64{60'000U};

        using FrameworkArgumentValidator = Status (*)(CanonicalJson const&);
        using FrameworkArgumentMaterial = json::Value (*)();
        using FrameworkDescriptorFactory = ToolDescriptor (*)();

        struct FrameworkToolDefinition final
        {
            std::string_view          name{};
            FrameworkDescriptorFactory descriptor{};
            FrameworkArgumentValidator validateArguments{};
            FrameworkArgumentMaterial argumentMaterial{};
        };

        [[nodiscard]]
        auto invalidFrameworkArguments(std::string message)
            -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::move(message)
            );
        }

        [[nodiscard]]
        auto observeDescriptor() -> ToolDescriptor
        {
            return ToolDescriptor{
                .toolVersion          = std::string{k_frameworkToolVersion},
                .requiredCapabilities = {},
                .effectBounds         = {},
                .uiActionBounds       = {},
                .limits               = WorkflowLimits{
                    .maximumSteps         = 1U,
                    .maximumDispatches    = 0U,
                    .maximumObservations  = 1U,
                    .maximumWaits         = 0U,
                    .maximumElapsedMillis = k_maximumObserveMillis,
                },
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = k_maximumObserveMillis,
                    .onTimeout            = TimeoutAction::Stop,
                },
                .mutability  = ToolMutability::ReadOnly,
                .surface     = ToolSurface::Semantic,
                .idempotency = ToolIdempotency::ReadSafe,
            };
        }

        [[nodiscard]]
        auto waitDescriptor() -> ToolDescriptor
        {
            return ToolDescriptor{
                .toolVersion          = std::string{k_frameworkToolVersion},
                .requiredCapabilities = {},
                .effectBounds         = {},
                .uiActionBounds       = {},
                .limits               = WorkflowLimits{
                    .maximumSteps         = 1U,
                    .maximumDispatches    = 0U,
                    .maximumObservations  = 0U,
                    .maximumWaits         = 1U,
                    .maximumElapsedMillis = k_maximumWaitMillis,
                },
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = k_maximumWaitMillis,
                    .onTimeout            = TimeoutAction::Stop,
                },
                .mutability  = ToolMutability::ReadOnly,
                .surface     = ToolSurface::Semantic,
                .idempotency = ToolIdempotency::ReadSafe,
            };
        }

        [[nodiscard]]
        auto validateObserveArguments(CanonicalJson const& arguments) -> Status
        {
            auto const& value = arguments.value();
            if (
                value.kind() != json::ValueKind::Object
                || !value.members().empty()
            )
            {
                return invalidFrameworkArguments(
                    "framework.screen.observe arguments must be exactly {}"
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto validateWaitArguments(CanonicalJson const& arguments) -> Status
        {
            auto const& value = arguments.value();
            auto const* const p_duration = value.find("duration_ms");
            if (
                value.kind() != json::ValueKind::Object
                || value.members().size() != 1U
                || p_duration == nullptr
                || !p_duration->isInteger()
                || p_duration->number() < 0.0
                || p_duration->number()
                    > static_cast<double>(k_maximumWaitMillis)
            )
            {
                return invalidFrameworkArguments(
                    "framework.workflow.wait requires only integer duration_ms "
                    "in [0, 60000]"
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto observeArgumentMaterial() -> json::Value
        {
            return json::Value::ofObject({
                {"additional_properties", json::Value::ofBoolean(false)},
                {"type", json::Value::ofString("object")},
            });
        }

        [[nodiscard]]
        auto waitArgumentMaterial() -> json::Value
        {
            return json::Value::ofObject({
                {"additional_properties", json::Value::ofBoolean(false)},
                {"maximum_duration_ms",
                 json::Value::ofNumber(
                     static_cast<double>(k_maximumWaitMillis)
                 )},
                {"required",
                 json::Value::ofArray({
                     json::Value::ofString("duration_ms"),
                 })},
                {"type", json::Value::ofString("object")},
            });
        }

        constexpr auto k_frameworkTools = std::array{
            FrameworkToolDefinition{
                k_observeTool,
                &observeDescriptor,
                &validateObserveArguments,
                &observeArgumentMaterial,
            },
            FrameworkToolDefinition{
                k_waitTool,
                &waitDescriptor,
                &validateWaitArguments,
                &waitArgumentMaterial,
            },
        };

        [[nodiscard]]
        auto frameworkDefinition(std::string_view name)
            -> FrameworkToolDefinition const*
        {
            auto const found = std::ranges::find(
                k_frameworkTools,
                name,
                &FrameworkToolDefinition::name
            );
            return found == k_frameworkTools.end() ? nullptr : &*found;
        }

        [[nodiscard]]
        auto stringArray(std::span<std::string const> values) -> json::Value
        {
            auto rendered = std::vector<json::Value>{};
            rendered.reserve(values.size());
            for (auto const& value : values)
            {
                rendered.emplace_back(json::Value::ofString(value));
            }
            return json::Value::ofArray(std::move(rendered));
        }

        [[nodiscard]]
        auto effectBoundsMaterial(std::span<EffectBound const> bounds)
            -> json::Value
        {
            auto rendered = std::vector<json::Value>{};
            rendered.reserve(bounds.size());
            for (auto const& bound : bounds)
            {
                rendered.emplace_back(json::Value::ofObject({
                    {"maximum_risk",
                     json::Value::ofString(
                         std::string{riskWireName(bound.maximumRisk)}
                     )},
                    {"namespaced_type",
                     json::Value::ofString(bound.namespacedType)},
                    {"payload_schema_hash",
                     json::Value::ofString(bound.payloadSchemaHash.hex())},
                    {"scope_kind", json::Value::ofString(bound.scopeKind)},
                }));
            }
            return json::Value::ofArray(std::move(rendered));
        }

        [[nodiscard]]
        auto descriptorMaterial(
            FrameworkToolDefinition const& definition,
            ToolDescriptor const& descriptor
        ) -> json::Value
        {
            return json::Value::ofObject({
                {"argument_contract", definition.argumentMaterial()},
                {"effect_bounds",
                 effectBoundsMaterial(descriptor.effectBounds)},
                {"idempotency",
                 json::Value::ofString(
                     std::string{
                         toolIdempotencyWireName(descriptor.idempotency)
                     }
                 )},
                {"mutability",
                 json::Value::ofString(
                     std::string{
                         toolMutabilityWireName(descriptor.mutability)
                     }
                 )},
                {"name", json::Value::ofString(std::string{definition.name})},
                {"required_capabilities",
                 stringArray(descriptor.requiredCapabilities)},
                {"surface",
                 json::Value::ofString(
                     std::string{toolSurfaceWireName(descriptor.surface)}
                 )},
                {"timeout",
                 json::Value::ofObject({
                     {"maximum_elapsed_ms",
                      json::Value::ofNumber(
                          static_cast<double>(
                              descriptor.timeout.maximumElapsedMillis
                          )
                      )},
                     {"on_timeout",
                      json::Value::ofString(
                          std::string{
                              timeoutActionWireName(
                                  descriptor.timeout.onTimeout
                              )
                          }
                      )},
                 })},
                {"tool_version",
                 json::Value::ofString(descriptor.toolVersion)},
                {"ui_action_bounds", stringArray(descriptor.uiActionBounds)},
                {"workflow_limits",
                 json::Value::ofObject({
                     {"maximum_dispatches",
                      json::Value::ofNumber(
                          static_cast<double>(
                              descriptor.limits.maximumDispatches
                          )
                      )},
                     {"maximum_elapsed_ms",
                      json::Value::ofNumber(
                          static_cast<double>(
                              descriptor.limits.maximumElapsedMillis
                          )
                      )},
                     {"maximum_observations",
                      json::Value::ofNumber(
                          static_cast<double>(
                              descriptor.limits.maximumObservations
                          )
                      )},
                     {"maximum_steps",
                      json::Value::ofNumber(
                          static_cast<double>(
                              descriptor.limits.maximumSteps
                          )
                      )},
                     {"maximum_waits",
                      json::Value::ofNumber(
                          static_cast<double>(
                              descriptor.limits.maximumWaits
                          )
                      )},
                 })},
            });
        }

        [[nodiscard]]
        auto describeTool(
            std::span<ToolCatalogEntry const> tools,
            std::string_view toolName
        ) -> Result<ToolDescriptor>
        {
            auto const found = std::ranges::find(
                tools,
                toolName,
                &ToolCatalogEntry::name
            );
            if (found == tools.end())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Tool Catalog declares no tool named "
                        + std::string{toolName}
                );
            }
            return found->descriptor;
        }

        [[nodiscard]]
        auto offerTools(
            std::span<ToolCatalogEntry const> tools,
            ControllerProfile profile,
            std::span<std::string const> heldCapabilities
        ) -> std::vector<OfferedTool>
        {
            auto offered = std::vector<OfferedTool>{};
            for (auto const& entry : tools)
            {
                if (!toolSurfaceAllowed(profile, entry.descriptor.surface))
                {
                    continue;
                }
                if (
                    missingRequiredToolCapability(
                        heldCapabilities,
                        entry.descriptor.requiredCapabilities
                    )
                )
                {
                    continue;
                }
                offered.emplace_back(OfferedTool{
                    .name    = entry.name,
                    .version = entry.descriptor.toolVersion,
                });
            }
            return offered;
        }
    }

    auto missingRequiredToolCapability(
        std::span<std::string const> heldCapabilities,
        std::span<std::string const> requiredCapabilities
    ) -> std::optional<std::string>
    {
        auto const missing = std::ranges::find_if(
            requiredCapabilities,
            [heldCapabilities](std::string const& capability)
            {
                return !std::ranges::contains(heldCapabilities, capability);
            }
        );
        if (missing == requiredCapabilities.end())
        {
            return std::nullopt;
        }
        return *missing;
    }

    ValidatedToolInvocation::ValidatedToolInvocation(
        ToolProviderIdentity provider,
        std::string toolName,
        CanonicalJson canonicalArgs,
        ToolDescriptor descriptor
    )
        : m_provider{std::move(provider)}
        , m_toolName{std::move(toolName)}
        , m_canonicalArgs{std::move(canonicalArgs)}
        , m_descriptor{std::move(descriptor)}
    {
    }

    auto ValidatedToolInvocation::provider() const noexcept
        -> ToolProviderIdentity const&
    {
        return m_provider;
    }

    auto ValidatedToolInvocation::toolName() const noexcept -> std::string const&
    {
        return m_toolName;
    }

    auto ValidatedToolInvocation::canonicalArgs() const noexcept
        -> CanonicalJson const&
    {
        return m_canonicalArgs;
    }

    auto ValidatedToolInvocation::descriptor() const noexcept
        -> ToolDescriptor const&
    {
        return m_descriptor;
    }

    auto toolSurfaceAllowed(
        ControllerProfile profile,
        ToolSurface surface
    ) noexcept -> bool
    {
        return !profile.semanticToolsOnly || surface == ToolSurface::Semantic;
    }

    ProjectToolCatalogSchemaOwner::ProjectToolCatalogSchemaOwner(
        ContentHash projectRegistrationHash,
        ContentHash toolCatalogHash,
        std::vector<ToolCatalogEntry> tools,
        ToolArgumentValidator validateArguments
    )
        : m_projectRegistrationHash{projectRegistrationHash}
        , m_toolCatalogHash{toolCatalogHash}
        , m_tools{std::move(tools)}
        , m_validateArguments{std::move(validateArguments)}
    {
    }

    auto ProjectToolCatalogSchemaOwner::create(
        VerifiedProjectRegistration const& registration,
        std::string_view exactToolCatalogBytes,
        ToolCatalogReader const& readCatalog,
        ToolArgumentValidator validateArguments
    ) -> Result<ProjectToolCatalogSchemaOwner>
    {
        if (!readCatalog || !validateArguments)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "ProjectToolCatalogSchemaOwner requires both catalog readers"
            );
        }
        UF_TRY_VALUE(
            catalogHash,
            sha256(std::as_bytes(std::span{exactToolCatalogBytes}))
        );
        if (catalogHash != registration.toolCatalogHash())
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool Catalog bytes do not match the registration's tool_catalog_hash"
            );
        }
        UF_TRY_VALUE_CONTEXT(
            tools,
            readCatalog(),
            "reading the Tool Catalog's declared tools"
        );
        if (tools.empty())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Tool Catalog declares no tool at all"
            );
        }
        for (auto const& entry : tools)
        {
            if (entry.name.empty() || entry.descriptor.toolVersion.empty())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Tool Catalog descriptor must carry a tool name and version"
                );
            }
            if (entry.name.starts_with(k_frameworkNamespace))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Project Tool Catalog cannot claim the reserved framework namespace"
                );
            }
        }
        std::ranges::sort(tools, {}, &ToolCatalogEntry::name);
        auto const repeated = std::ranges::adjacent_find(
            tools,
            {},
            &ToolCatalogEntry::name
        );
        if (repeated != tools.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Tool Catalog declares one tool name twice"
            );
        }
        return ProjectToolCatalogSchemaOwner{
            registration.hash(),
            catalogHash,
            std::move(tools),
            std::move(validateArguments),
        };
    }

    auto ProjectToolCatalogSchemaOwner::projectRegistrationHash() const
        -> ContentHash
    {
        return m_projectRegistrationHash;
    }

    auto ProjectToolCatalogSchemaOwner::toolCatalogHash() const -> ContentHash
    {
        return m_toolCatalogHash;
    }

    auto ProjectToolCatalogSchemaOwner::validate(
        std::string toolName,
        CanonicalJson canonicalArgs
    ) const -> Result<ValidatedToolInvocation>
    {
        UF_TRY_VALUE(descriptor, describe(toolName));
        UF_TRY_CONTEXT(
            m_validateArguments(toolName, canonicalArgs.bytes()),
            "validating the arguments against the schema this descriptor names"
        );
        return ValidatedToolInvocation{
            ToolProviderIdentity{ProjectToolProvider{
                .projectRegistrationHash = m_projectRegistrationHash,
                .toolCatalogHash         = m_toolCatalogHash,
            }},
            std::move(toolName),
            std::move(canonicalArgs),
            std::move(descriptor),
        };
    }

    auto ProjectToolCatalogSchemaOwner::describe(
        std::string_view toolName
    ) const -> Result<ToolDescriptor>
    {
        return withContext(
            describeTool(m_tools, toolName),
            "reading the Project Tool Catalog"
        );
    }

    auto ProjectToolCatalogSchemaOwner::offeredTools(
        ControllerProfile profile,
        std::span<std::string const> heldCapabilities
    ) const -> std::vector<OfferedTool>
    {
        return offerTools(m_tools, profile, heldCapabilities);
    }

    FrameworkToolCatalogOwner::FrameworkToolCatalogOwner(
        ContentHash toolCatalogHash,
        std::string canonicalJcs,
        std::vector<ToolCatalogEntry> tools
    )
        : m_toolCatalogHash{toolCatalogHash}
        , m_canonicalJcs{std::move(canonicalJcs)}
        , m_tools{std::move(tools)}
    {
    }

    auto FrameworkToolCatalogOwner::create()
        -> Result<FrameworkToolCatalogOwner>
    {
        auto tools    = std::vector<ToolCatalogEntry>{};
        auto material = std::vector<json::Value>{};
        tools.reserve(k_frameworkTools.size());
        material.reserve(k_frameworkTools.size());
        for (auto const& definition : k_frameworkTools)
        {
            auto descriptor = definition.descriptor();
            material.emplace_back(descriptorMaterial(definition, descriptor));
            tools.emplace_back(ToolCatalogEntry{
                .name       = std::string{definition.name},
                .descriptor = std::move(descriptor),
            });
        }
        auto canonicalJcs = json::canonicalBytes(json::Value::ofObject({
            // This material is deliberately internal until the execution
            // adapters and durable runtime can publish one atomic wire cut.
            {"internal_generation", json::Value::ofNumber(0.0)},
            {"owner", json::Value::ofString("framework")},
            {"tools", json::Value::ofArray(std::move(material))},
        }));
        UF_TRY_VALUE(
            catalogHash,
            sha256(std::as_bytes(std::span{canonicalJcs}))
        );
        return FrameworkToolCatalogOwner{
            catalogHash,
            std::move(canonicalJcs),
            std::move(tools),
        };
    }

    auto FrameworkToolCatalogOwner::toolCatalogHash() const -> ContentHash
    {
        return m_toolCatalogHash;
    }

    auto FrameworkToolCatalogOwner::canonicalJcs() const noexcept
        -> std::string const&
    {
        return m_canonicalJcs;
    }

    auto FrameworkToolCatalogOwner::validate(
        std::string toolName,
        CanonicalJson canonicalArgs
    ) const -> Result<ValidatedToolInvocation>
    {
        auto const* const p_definition = frameworkDefinition(toolName);
        if (p_definition == nullptr)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Framework Tool Catalog declares no tool named " + toolName
            );
        }
        UF_TRY(p_definition->validateArguments(canonicalArgs));
        UF_TRY_VALUE(descriptor, describe(toolName));
        return ValidatedToolInvocation{
            ToolProviderIdentity{FrameworkToolProvider{
                .toolCatalogHash = m_toolCatalogHash,
            }},
            std::move(toolName),
            std::move(canonicalArgs),
            std::move(descriptor),
        };
    }

    auto FrameworkToolCatalogOwner::describe(
        std::string_view toolName
    ) const -> Result<ToolDescriptor>
    {
        return withContext(
            describeTool(m_tools, toolName),
            "reading the Framework Tool Catalog"
        );
    }

    auto FrameworkToolCatalogOwner::offeredTools(
        ControllerProfile profile,
        std::span<std::string const> heldCapabilities
    ) const -> std::vector<OfferedTool>
    {
        return offerTools(m_tools, profile, heldCapabilities);
    }
}
