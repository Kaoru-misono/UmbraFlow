#pragma once

#include "controller.hpp"
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

    // Whether a tool's arguments and results are stated in the project's own
    // vocabulary, or in the machine's -- coordinates, pixels, key codes,
    // receipts, fencing tokens, bindings, frames. It is a property of the Tool
    // Catalog descriptor and never of a request.
    //
    // The catalog is project-owned, so this is a declaration the project makes
    // about itself, not isolation the Operator imposes. A project that marks a
    // coordinate tool Semantic is not contained by p03; it is attributable,
    // because the catalog bytes are inside plugin_hash, which is inside
    // project_registration_hash, which pins the session. What p03 enforces is
    // that the Operator never offers or accepts a Privileged tool for an online
    // Agent. It is the same limit the Operator accepts for a plugin that
    // under-declares its own effects, and it is deliberate: a second trust
    // model beside ToolMutability's would be worse than one documented limit.
    enum class ToolSurface : uint8
    {
        Semantic,
        Privileged,
    };

    // What one Tool Catalog descriptor says about a tool. Returned by the
    // catalog validator; there is no path by which a request proposes it.
    struct ToolDescriptor final
    {
        std::string    toolVersion{};

        // Mutating is the default so that a descriptor which failed to state a
        // mutability is treated as the more restricted of the two.
        ToolMutability mutability{ToolMutability::Mutating};

        // Privileged is the default for the same reason: a descriptor that
        // failed to state a surface gets the more restricted of the two, so a
        // catalog cannot widen the Agent ceiling by omission.
        ToolSurface    surface{ToolSurface::Privileged};
    };

    // One tool call the catalog owner recognised, carrying the descriptor's own
    // version, mutability and surface. Only the owner bound to the exact
    // ProjectRegistration root and tool_catalog_hash can mint one.
    //
    // It deliberately records no controller. An invocation is what the project
    // says about one call of one of its own tools; who may present it is the
    // Operator's decision at submission, and binding the two together would
    // mean a project could only describe a tool in the presence of a session.
    class ValidatedToolInvocation final
    {
        friend class ProjectToolCatalogSchemaOwner;

        ContentHash    m_projectRegistrationHash;
        ContentHash    m_toolCatalogHash;
        std::string    m_toolName;
        std::string    m_toolVersion;
        CanonicalJson  m_canonicalArgs;
        ToolMutability m_mutability;
        ToolSurface    m_surface;

        ValidatedToolInvocation(
            ContentHash projectRegistrationHash,
            ContentHash toolCatalogHash,
            std::string toolName,
            std::string toolVersion,
            CanonicalJson canonicalArgs,
            ToolMutability mutability,
            ToolSurface surface
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
        [[nodiscard]] auto surface() const noexcept -> ToolSurface;
    };

    // The one p03 rule, spelled once. submitCommand enforces it, and a
    // deployment computing the tool names it offers a controller asks the same
    // function rather than restating the rule. Offering less would not be
    // enforcement on its own -- a controller can present an invocation it was
    // never offered -- which is why the enforcing evaluation is at the accept.
    [[nodiscard]]
    auto toolSurfaceAllowed(
        ControllerProfile profile,
        ToolSurface surface
    ) noexcept -> bool;

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
        // tool, and the exact canonical arguments -- and nothing else. It
        // deliberately does not take a ControllerBinding: what a project's
        // catalog says about its own tools is the project's authority, and a
        // foreign registration must be able to mint its own invocation with no
        // session anywhere. Who may present one is the Operator's authority and
        // is decided by toolSurfaceAllowed at submission.
        [[nodiscard]]
        auto validate(
            std::string toolName,
            CanonicalJson canonicalArgs
        ) const -> Result<ValidatedToolInvocation>;
    };
}
