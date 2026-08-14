#include "tool-invocation.hpp"

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::operator_runtime
{
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
        ContentHash projectRegistrationHash,
        std::string toolName,
        CanonicalJson canonicalArgs,
        ToolDescriptor descriptor
    )
        : m_projectRegistrationHash{projectRegistrationHash}
        , m_toolName{std::move(toolName)}
        , m_canonicalArgs{std::move(canonicalArgs)}
        , m_descriptor{std::move(descriptor)}
    {
    }

    auto ValidatedToolInvocation::projectRegistrationHash() const -> ContentHash
    {
        return m_projectRegistrationHash;
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
            m_projectRegistrationHash,
            std::move(toolName),
            std::move(canonicalArgs),
            std::move(descriptor),
        };
    }

    auto ProjectToolCatalogSchemaOwner::describe(
        std::string_view toolName
    ) const -> Result<ToolDescriptor>
    {
        auto const found = std::ranges::find(
            m_tools,
            toolName,
            &ToolCatalogEntry::name
        );
        if (found == m_tools.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "This registration's Tool Catalog declares no tool named "
                    + std::string{toolName}
            );
        }
        return found->descriptor;
    }

    auto ProjectToolCatalogSchemaOwner::offeredTools(
        ControllerProfile profile,
        std::span<std::string const> heldCapabilities
    ) const -> std::vector<OfferedTool>
    {
        auto offered = std::vector<OfferedTool>{};
        for (auto const& entry : m_tools)
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
