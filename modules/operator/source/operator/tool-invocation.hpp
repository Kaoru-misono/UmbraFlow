#pragma once

#include "controller.hpp"
#include "manifest.hpp"
#include "project-plugin.hpp"
#include "tool-descriptor.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>

#include <domain/content-hash.hpp>

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uf::operator_runtime
{
    class ProjectToolCatalogSchemaOwner;

    // One tool call the catalog owner recognised, carrying the descriptor the
    // catalog declared for it. Only the owner bound to the exact
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
        CanonicalJson  m_canonicalArgs;
        ToolDescriptor m_descriptor;

        ValidatedToolInvocation(
            ContentHash projectRegistrationHash,
            ContentHash toolCatalogHash,
            std::string toolName,
            CanonicalJson canonicalArgs,
            ToolDescriptor descriptor
        );

    public:
        [[nodiscard]] auto projectRegistrationHash() const -> ContentHash;
        [[nodiscard]] auto toolCatalogHash() const -> ContentHash;

        [[nodiscard]]
        auto toolName() const noexcept UF_LIFETIME_BOUND -> std::string const&;

        [[nodiscard]]
        auto canonicalArgs() const noexcept UF_LIFETIME_BOUND
            -> CanonicalJson const&;

        // The whole descriptor rather than a projection of it. Version,
        // mutability, surface, idempotency and every bound are one statement
        // the catalog made about this tool, and an accessor per member would be
        // a second spelling of each.
        [[nodiscard]]
        auto descriptor() const noexcept UF_LIFETIME_BOUND
            -> ToolDescriptor const&;
    };

    // Both p03 predicates are shared by the offer and accept sides. Neither side
    // substitutes for the other: an unoffered invocation may still be presented.
    [[nodiscard]]
    auto toolSurfaceAllowed(
        ControllerProfile profile,
        ToolSurface surface
    ) noexcept -> bool;

    [[nodiscard]]
    auto missingRequiredToolCapability(
        std::span<std::string const> heldCapabilities,
        std::span<std::string const> requiredCapabilities
    ) -> std::optional<std::string>;

    // One tool the catalog declares, under the name it declares it by.
    struct ToolCatalogEntry final
    {
        std::string    name{};
        ToolDescriptor descriptor{};
    };

    // What one controller may be handed for one tool: enough to call it, and
    // nothing about the bounds it will be judged against. Those are the
    // Operator's business at the freeze, and an online Agent that could read
    // them would learn the shape of what it is not allowed to do.
    struct OfferedTool final
    {
        std::string name{};
        std::string version{};
    };

    // Trusted deployment callbacks, and the reason they are two rather than
    // one. The catalog is read once, when the owner is built, so the offer side
    // and the accept side answer from the same stored declaration rather than
    // from two reads that could disagree. Arguments cannot be read once: they
    // arrive per call and are judged against the argument schema this
    // descriptor names. Neither is ever passed to plugin code or published in a
    // business VM.
    using ToolCatalogReader =
        std::function<Result<std::vector<ToolCatalogEntry>>()>;
    using ToolArgumentValidator = std::function<
        Status(std::string_view toolName, std::string_view exactArgsJcs)
    >;

    class ProjectToolCatalogSchemaOwner final
    {
        ContentHash                   m_projectRegistrationHash;
        ContentHash                   m_toolCatalogHash;
        std::vector<ToolCatalogEntry> m_tools;
        ToolArgumentValidator         m_validateArguments;

        ProjectToolCatalogSchemaOwner(
            ContentHash projectRegistrationHash,
            ContentHash toolCatalogHash,
            std::vector<ToolCatalogEntry> tools,
            ToolArgumentValidator validateArguments
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
            ToolCatalogReader readCatalog,
            ToolArgumentValidator validateArguments
        ) -> Result<ProjectToolCatalogSchemaOwner>;

        [[nodiscard]] auto projectRegistrationHash() const -> ContentHash;
        [[nodiscard]] auto toolCatalogHash() const -> ContentHash;

        // Takes the two things an ordinary caller is allowed to name -- which
        // tool, and the exact canonical arguments -- and nothing else. It
        // deliberately does not take a ControllerBinding: what a project's
        // catalog says about its own tools is the project's authority, and a
        // foreign registration must be able to mint its own invocation with no
        // session anywhere. Who may present one is the Operator's authority and
        // is decided from the surface and required capabilities at submission.
        [[nodiscard]]
        auto validate(
            std::string toolName,
            CanonicalJson canonicalArgs
        ) const -> Result<ValidatedToolInvocation>;

        // The declaration this catalog carries for one tool. The Operator asks
        // for it again when it freezes a plan, because the bounds a proposal is
        // judged against belong to the catalog the session pinned and not to
        // the invocation a caller once presented.
        [[nodiscard]]
        auto describe(std::string_view toolName) const -> Result<ToolDescriptor>;

        // The offer side of p03: what a controller of this profile, holding
        // these capabilities, may be told exists. A Privileged tool is absent
        // from the result for a controller restricted to semantic tools -- not
        // present and refused later -- and a tool whose required capabilities
        // this session does not hold is absent for the same reason.
        //
        // heldCapabilities is a call-scoped borrow of the session's own
        // capability set; nothing here stores it.
        [[nodiscard]]
        auto offeredTools(
            ControllerProfile profile,
            std::span<std::string const> heldCapabilities
        ) const -> std::vector<OfferedTool>;
    };
}
