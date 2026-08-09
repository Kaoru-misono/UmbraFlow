#pragma once

#include "manifest.hpp"
#include "project-plugin.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <functional>
#include <string>
#include <string_view>

namespace uf::operator_runtime
{
    class ProjectToolCatalogSchemaOwner;

    // Whether a tool changes anything outside the Operator. It is a property of
    // the Tool Catalog descriptor and never of a request, because the whole
    // point of the mutation chain is that it cannot be opted out of.
    enum class ToolMutability : uint8
    {
        ReadOnly,
        Mutating,
    };

    // What one Tool Catalog descriptor says about a tool. Returned by the
    // catalog validator; there is no path by which a request proposes it.
    struct ToolDescriptor final
    {
        std::string    toolVersion{};

        // Mutating is the default so that a descriptor which failed to state a
        // mutability is treated as the more restricted of the two.
        ToolMutability mutability{ToolMutability::Mutating};
    };

    // One tool call the catalog owner recognised, carrying the descriptor's own
    // version and mutability. Only the owner bound to the exact
    // ProjectRegistration root and tool_catalog_hash can mint one.
    class ValidatedToolInvocation final
    {
        friend class ProjectToolCatalogSchemaOwner;

        ContentHash    m_projectRegistrationHash;
        ContentHash    m_toolCatalogHash;
        std::string    m_toolName;
        std::string    m_toolVersion;
        CanonicalJson  m_canonicalArgs;
        ToolMutability m_mutability;

        ValidatedToolInvocation(
            ContentHash projectRegistrationHash,
            ContentHash toolCatalogHash,
            std::string toolName,
            std::string toolVersion,
            CanonicalJson canonicalArgs,
            ToolMutability mutability
        );

    public:
        [[nodiscard]] auto projectRegistrationHash() const -> ContentHash;
        [[nodiscard]] auto toolCatalogHash() const -> ContentHash;

        [[nodiscard]]
        auto toolName() const noexcept UF_LIFETIME_BOUND -> std::string const&;

        [[nodiscard]]
        auto toolVersion() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        [[nodiscard]]
        auto canonicalArgs() const noexcept UF_LIFETIME_BOUND
            -> CanonicalJson const&;

        [[nodiscard]] auto mutability() const noexcept -> ToolMutability;
    };

    // Trusted deployment callback. It selects the descriptor by tool name,
    // validates the complete argument schema that descriptor names, and returns
    // the descriptor's own version and mutability. It is never passed to plugin
    // code or published in a business VM.
    using ToolCatalogValidator = std::function<
        Result<ToolDescriptor>(
            std::string_view toolName,
            std::string_view exactArgsJcs
        )
    >;

    class ProjectToolCatalogSchemaOwner final
    {
        ContentHash          m_projectRegistrationHash;
        ContentHash          m_toolCatalogHash;
        ToolCatalogValidator m_validateInvocation;

        ProjectToolCatalogSchemaOwner(
            ContentHash projectRegistrationHash,
            ContentHash toolCatalogHash,
            ToolCatalogValidator validateInvocation
        );

    public:
        // The exact Tool Catalog bytes are required, not merely referenced:
        // without them an owner is bound to a registration whose
        // tool_catalog_hash it never has to satisfy, and any validator at all
        // could answer for that catalog.
        [[nodiscard]]
        static auto create(
            VerifiedProjectRegistration const& registration,
            std::string_view exactToolCatalogBytes,
            ToolCatalogValidator validateInvocation
        ) -> Result<ProjectToolCatalogSchemaOwner>;

        // Takes the two things an ordinary caller is allowed to name -- which
        // tool, and the exact canonical arguments -- and nothing else.
        [[nodiscard]]
        auto validate(
            std::string toolName,
            CanonicalJson canonicalArgs
        ) const -> Result<ValidatedToolInvocation>;
    };
}
