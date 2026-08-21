#pragma once

#include "controller.hpp"
#include "manifest.hpp"
#include "project-plugin.hpp"
#include "tool-descriptor.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <compare>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uf::operator_runtime
{
    class ProjectToolCatalogSchemaOwner;
    class FrameworkToolCatalogOwner;

    // No in-class initializers for hashes: ContentHash has no default state.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct FrameworkToolProvider final
    {
        ContentHash toolCatalogHash;
    };

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct ProjectToolProvider final
    {
        ContentHash projectRegistrationHash;
        ContentHash toolCatalogHash;
    };

    using ToolProviderIdentity = std::variant<
        FrameworkToolProvider,
        ProjectToolProvider>;

    class CallerIdempotencyNamespace final
    {
        std::string m_value;

        explicit CallerIdempotencyNamespace(std::string value);

    public:
        [[nodiscard]]
        static auto create(std::string value)
            -> Result<CallerIdempotencyNamespace>;

        [[nodiscard]]
        auto value() const noexcept UF_LIFETIME_BOUND -> std::string const&;

        auto operator<=>(CallerIdempotencyNamespace const&) const = default;
    };

    class RootRequestKey final
    {
        std::string m_value;

        explicit RootRequestKey(std::string value);

    public:
        [[nodiscard]]
        static auto create(std::string value) -> Result<RootRequestKey>;

        [[nodiscard]]
        auto value() const noexcept UF_LIFETIME_BOUND -> std::string const&;

        auto operator<=>(RootRequestKey const&) const = default;
    };

    enum class RootRequestRelation : uint8
    {
        SameRequest,
        Conflict,
        Distinct,
    };

    class ToolRootRequestIdentity final
    {
        CallerIdempotencyNamespace m_callerNamespace;
        RootRequestKey             m_requestKey;
        CanonicalJson              m_requestPreimage;
        ContentHash                m_identity;

        ToolRootRequestIdentity(
            CallerIdempotencyNamespace callerNamespace,
            RootRequestKey requestKey,
            CanonicalJson requestPreimage,
            ContentHash identity
        );

    public:
        [[nodiscard]]
        static auto create(
            std::string callerNamespace,
            std::string requestKey,
            CanonicalJson requestPreimage
        ) -> Result<ToolRootRequestIdentity>;

        [[nodiscard]] auto identity() const -> ContentHash;

        [[nodiscard]]
        auto callerNamespace() const noexcept UF_LIFETIME_BOUND
            -> CallerIdempotencyNamespace const&;

        [[nodiscard]]
        auto requestKey() const noexcept UF_LIFETIME_BOUND
            -> RootRequestKey const&;

        [[nodiscard]]
        auto requestPreimage() const noexcept UF_LIFETIME_BOUND
            -> CanonicalJson const&;

        [[nodiscard]]
        auto relationTo(ToolRootRequestIdentity const& other) const noexcept
            -> RootRequestRelation;
    };

    // These identities are fixed by the caller/runtime before dispatch. The
    // origin and executing principals, policy, approval, lease, fence and
    // admitted budget belong to admission attempts and deliberately cannot be
    // supplied here.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct ToolExecutionIdentity final
    {
        ContentHash runIdentity;
        ContentHash frameworkReleaseIdentity;
        ContentHash toolRuntimeProtocolIdentity;
        ContentHash environmentIdentity;
    };

    class ToolCallPositionIdentity;

    class ToolCallParent final
    {
        friend class ToolCallPositionIdentity;

        ContentHash m_rootIdentity;
        ContentHash m_callIdentity;

        ToolCallParent(ContentHash rootIdentity, ContentHash callIdentity);

    public:
        [[nodiscard]] auto rootIdentity() const -> ContentHash;
        [[nodiscard]] auto callIdentity() const -> ContentHash;
    };

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
        friend class FrameworkToolCatalogOwner;

        ToolProviderIdentity m_provider;
        std::string          m_toolName;
        CanonicalJson        m_canonicalArgs;
        ToolDescriptor       m_descriptor;

        ValidatedToolInvocation(
            ToolProviderIdentity provider,
            std::string toolName,
            CanonicalJson canonicalArgs,
            ToolDescriptor descriptor
        );

    public:
        [[nodiscard]]
        auto provider() const noexcept UF_LIFETIME_BOUND
            -> ToolProviderIdentity const&;

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

    // One immutable call-position fingerprint. Provider result, delivery
    // classification and Operator-selected admission material have no input in
    // this factory, so none can change or be smuggled into the call identity.
    class ToolCallPositionIdentity final
    {
        ContentHash                m_identity;
        ContentHash                m_rootIdentity;
        std::optional<ContentHash> m_parentIdentity;
        uint32                     m_sequence;
        ToolExecutionIdentity      m_executionIdentity;
        ToolProviderIdentity       m_provider;
        std::string                m_toolName;
        std::string                m_toolVersion;
        ContentHash                m_canonicalArgsHash;

        ToolCallPositionIdentity(
            ContentHash identity,
            ContentHash rootIdentity,
            std::optional<ContentHash> parentIdentity,
            uint32 sequence,
            ToolExecutionIdentity executionIdentity,
            ToolProviderIdentity provider,
            std::string toolName,
            std::string toolVersion,
            ContentHash canonicalArgsHash
        );

    public:
        [[nodiscard]]
        static auto create(
            ToolRootRequestIdentity const& root,
            std::optional<ToolCallParent> const& parent,
            uint64 sequence,
            ToolExecutionIdentity executionIdentity,
            ValidatedToolInvocation const& invocation
        ) -> Result<ToolCallPositionIdentity>;

        [[nodiscard]] auto identity() const -> ContentHash;
        [[nodiscard]] auto rootIdentity() const -> ContentHash;

        [[nodiscard]]
        auto parentIdentity() const noexcept UF_LIFETIME_BOUND
            -> std::optional<ContentHash> const&;

        [[nodiscard]] auto sequence() const noexcept -> uint32;

        [[nodiscard]]
        auto executionIdentity() const noexcept UF_LIFETIME_BOUND
            -> ToolExecutionIdentity const&;

        [[nodiscard]]
        auto provider() const noexcept UF_LIFETIME_BOUND
            -> ToolProviderIdentity const&;

        [[nodiscard]]
        auto toolName() const noexcept UF_LIFETIME_BOUND -> std::string const&;

        [[nodiscard]]
        auto toolVersion() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        [[nodiscard]] auto canonicalArgsHash() const -> ContentHash;
        [[nodiscard]] auto asParent() const -> ToolCallParent;
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
            ToolCatalogReader const& readCatalog,
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

    // Framework owns this catalog and derives its identity from exact material
    // rendered from its built-in definitions. Callers can select a name and
    // canonical arguments, but cannot inject a descriptor, validator, digest,
    // or namespace entry.
    class FrameworkToolCatalogOwner final
    {
        ContentHash                   m_toolCatalogHash;
        std::string                   m_canonicalJcs;
        std::vector<ToolCatalogEntry> m_tools;

        FrameworkToolCatalogOwner(
            ContentHash toolCatalogHash,
            std::string canonicalJcs,
            std::vector<ToolCatalogEntry> tools
        );

    public:
        [[nodiscard]]
        static auto create() -> Result<FrameworkToolCatalogOwner>;

        [[nodiscard]] auto toolCatalogHash() const -> ContentHash;

        [[nodiscard]]
        auto canonicalJcs() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        [[nodiscard]]
        auto validate(
            std::string toolName,
            CanonicalJson canonicalArgs
        ) const -> Result<ValidatedToolInvocation>;

        [[nodiscard]]
        auto describe(std::string_view toolName) const -> Result<ToolDescriptor>;

        [[nodiscard]]
        auto offeredTools(
            ControllerProfile profile,
            std::span<std::string const> heldCapabilities
        ) const -> std::vector<OfferedTool>;
    };
}
