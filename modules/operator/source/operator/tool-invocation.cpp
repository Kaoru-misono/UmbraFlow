#include "tool-invocation.hpp"

#include <domain/error.hpp>

#include <string>
#include <utility>

namespace uf::operator_runtime
{
    ValidatedToolInvocation::ValidatedToolInvocation(
        ContentHash projectRegistrationHash,
        ContentHash toolCatalogHash,
        std::string toolName,
        std::string toolVersion,
        CanonicalJson canonicalArgs,
        ToolMutability mutability
    )
        : m_projectRegistrationHash{projectRegistrationHash}
        , m_toolCatalogHash{toolCatalogHash}
        , m_toolName{std::move(toolName)}
        , m_toolVersion{std::move(toolVersion)}
        , m_canonicalArgs{std::move(canonicalArgs)}
        , m_mutability{mutability}
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
        return ProjectToolCatalogSchemaOwner{
            registration.hash(),
            registration.toolCatalogHash(),
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
        };
    }
}
