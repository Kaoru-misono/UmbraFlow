#include "tool-invocation.hpp"

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace uf::operator_runtime
{
    ValidatedToolInvocation::ValidatedToolInvocation(
        ContentHash projectRegistrationHash,
        ContentHash toolCatalogHash,
        std::string toolName,
        std::string toolVersion,
        CanonicalJson canonicalArgs,
        ToolMutability mutability,
        ToolSurface surface
    )
        : m_projectRegistrationHash{projectRegistrationHash}
        , m_toolCatalogHash{toolCatalogHash}
        , m_toolName{std::move(toolName)}
        , m_toolVersion{std::move(toolVersion)}
        , m_canonicalArgs{std::move(canonicalArgs)}
        , m_mutability{mutability}
        , m_surface{surface}
    {
    }

    auto ValidatedToolInvocation::projectRegistrationHash() const -> ContentHash
    {
        return m_projectRegistrationHash;
    }

    auto ValidatedToolInvocation::toolCatalogHash() const -> ContentHash
    {
        return m_toolCatalogHash;
    }

    auto ValidatedToolInvocation::toolName() const noexcept -> std::string const&
    {
        return m_toolName;
    }

    auto ValidatedToolInvocation::toolVersion() const noexcept
        -> std::string const&
    {
        return m_toolVersion;
    }

    auto ValidatedToolInvocation::canonicalArgs() const noexcept
        -> CanonicalJson const&
    {
        return m_canonicalArgs;
    }

    auto ValidatedToolInvocation::mutability() const noexcept -> ToolMutability
    {
        return m_mutability;
    }

    auto ValidatedToolInvocation::surface() const noexcept -> ToolSurface
    {
        return m_surface;
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
        ToolCatalogValidator validateInvocation
    )
        : m_projectRegistrationHash{projectRegistrationHash}
        , m_toolCatalogHash{toolCatalogHash}
        , m_validateInvocation{std::move(validateInvocation)}
    {
    }

    auto ProjectToolCatalogSchemaOwner::create(
        VerifiedProjectRegistration const& registration,
        std::string_view exactToolCatalogBytes,
        ToolCatalogValidator validateInvocation
    ) -> Result<ProjectToolCatalogSchemaOwner>
    {
        if (!validateInvocation)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "ProjectToolCatalogSchemaOwner requires a catalog validator"
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
        return ProjectToolCatalogSchemaOwner{
            registration.hash(),
            catalogHash,
            std::move(validateInvocation),
        };
    }

    auto ProjectToolCatalogSchemaOwner::validate(
        std::string toolName,
        CanonicalJson canonicalArgs
    ) const -> Result<ValidatedToolInvocation>
    {
        if (toolName.empty())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "tool_name must be non-empty"
            );
        }
        UF_TRY_VALUE_CONTEXT(
            descriptor,
            m_validateInvocation(toolName, canonicalArgs.bytes()),
            "validating the Tool Catalog descriptor and its argument schema"
        );
        if (descriptor.toolVersion.empty())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Tool Catalog descriptor must carry a tool version"
            );
        }
        return ValidatedToolInvocation{
            m_projectRegistrationHash,
            m_toolCatalogHash,
            std::move(toolName),
            std::move(descriptor.toolVersion),
            std::move(canonicalArgs),
            descriptor.mutability,
            descriptor.surface,
        };
    }
}
