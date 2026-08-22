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
    class SnapshotObservationReference;
    class ToolCallIssuingContext;

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
    class ToolCallIssuingContext;

    // One durable coordinate in a run's call tree, and the whole of what an
    // issuing context numbers its children under.
    //
    // A tree has exactly two kinds of coordinate and they are one kind here:
    // the root request at the top, and every call position beneath it. The
    // run's own context is anchored on the root request, so a call an actor
    // issues directly is a real positioned row whose parent names a real
    // durable row -- there is no absent parent and no null to read a meaning
    // into. A handler's context is anchored on the position of the call it
    // implements.
    class ToolCallParent final
    {
        friend class ToolCallPositionIdentity;
        friend class ToolCallIssuingContext;

        ContentHash m_rootIdentity;
        ContentHash m_identity;

        ToolCallParent(ContentHash rootIdentity, ContentHash identity);

    public:
        [[nodiscard]] auto rootIdentity() const -> ContentHash;

        // The coordinate this parent names: the root request's own identity
        // for the run's context, and the parent call's position identity for a
        // handler's.
        [[nodiscard]] auto identity() const -> ContentHash;
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

    // One immutable call-position fingerprint, and a content address rather
    // than the replay lookup key: the key is the ordinal coordinate alone, and
    // everything else here is a stored attribute compared field by field at
    // that coordinate. Provider result, delivery classification and
    // Operator-selected admission material have no input in this factory, so
    // none can change or be smuggled into the call identity.
    //
    // Only ToolCallIssuingContext can mint one. That is what makes R4's
    // "scripts never see or influence the child index" structural: there is no
    // factory a caller can hand an ordinal to.
    class ToolCallPositionIdentity final
    {
        friend class ToolCallIssuingContext;

        ContentHash                m_identity;
        ContentHash                m_rootIdentity;
        ContentHash                m_parentIdentity;
        uint32                     m_sequence;
        ToolExecutionIdentity      m_executionIdentity;
        ToolProviderIdentity       m_provider;
        std::string                m_toolName;
        std::string                m_toolVersion;
        std::string                m_canonicalArgs;
        ContentHash                m_canonicalArgsHash;
        std::optional<ContentHash> m_observationReference;
        ToolDescriptor             m_descriptor;

        ToolCallPositionIdentity(
            ContentHash identity,
            ContentHash rootIdentity,
            ContentHash parentIdentity,
            uint32 sequence,
            ToolExecutionIdentity executionIdentity,
            ToolProviderIdentity provider,
            std::string toolName,
            std::string toolVersion,
            std::string canonicalArgs,
            ContentHash canonicalArgsHash,
            std::optional<ContentHash> observationReference,
            ToolDescriptor descriptor
        );

        [[nodiscard]]
        static auto create(
            ContentHash const& rootIdentity,
            ToolCallParent const& parent,
            uint32 sequence,
            ToolExecutionIdentity const& executionIdentity,
            ValidatedToolInvocation const& invocation,
            std::optional<ContentHash> observationReference
        ) -> Result<ToolCallPositionIdentity>;

    public:
        [[nodiscard]] auto identity() const -> ContentHash;
        [[nodiscard]] auto rootIdentity() const -> ContentHash;

        // The durable coordinate this position hangs from. It is never
        // absent: a call the run's own context issued names the root request,
        // and a child of a handler names that handler's position.
        [[nodiscard]] auto parentIdentity() const -> ContentHash;

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

        [[nodiscard]]
        auto canonicalArgs() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        [[nodiscard]] auto canonicalArgsHash() const -> ContentHash;

        // The observation this call consumes, or std::nullopt when the Tool
        // consumes none. Per R4 it is a stored attribute compared field by
        // field at the coordinate, never part of the address.
        [[nodiscard]]
        auto observationReference() const noexcept UF_LIFETIME_BOUND
            -> std::optional<ContentHash> const&;

        [[nodiscard]]
        auto descriptor() const noexcept UF_LIFETIME_BOUND
            -> ToolDescriptor const&;

        [[nodiscard]] auto asParent() const -> ToolCallParent;
    };

    // The one place a call coordinate comes from. Per R4 the call sequence is a
    // monotone child index per issuing context -- the root script's context,
    // and one per live handler invocation -- assigned and incremented
    // exclusively by this seam. A caller cannot see, pass, or influence it:
    // issue() takes the invocation and nothing that could name a position, and
    // ToolCallPositionIdentity has no other factory.
    //
    // Per-parent rather than run-global numbering is what keeps each context's
    // numbering a function of its own behaviour: a replayed parent's handler
    // never runs, so a replayed child costs its parent exactly one increment
    // regardless of how large its subtree was.
    //
    // This is a value and stores no borrow. It is the run context's member in
    // production, which is what gives the scoped invoke primitive a counter
    // strictly outlived by the run it belongs to.
    class ToolCallIssuingContext final
    {
        ContentHash           m_rootIdentity;
        ToolCallParent        m_parent;
        ToolExecutionIdentity m_executionIdentity;
        uint32                m_issuedChildren{};

        ToolCallIssuingContext(
            ContentHash rootIdentity,
            ToolCallParent parent,
            ToolExecutionIdentity executionIdentity
        );

        [[nodiscard]]
        auto issueNext(
            ValidatedToolInvocation const& invocation,
            std::optional<ContentHash> observationReference
        ) -> Result<ToolCallPositionIdentity>;

    public:
        // The run's own context, anchored on the root request itself. Its
        // children are the root-positioned calls of this run, which is what
        // "root call" means in the scoped tools facade: scripts never mint
        // root request identities.
        [[nodiscard]]
        static auto forRoot(
            ToolRootRequestIdentity const& root,
            ToolExecutionIdentity executionIdentity
        ) -> ToolCallIssuingContext;

        // The context one live handler invocation issues its children from.
        // The handler call must be a position this context issued, so a
        // handler cannot name another context's position as its own parent.
        [[nodiscard]]
        auto forHandler(ToolCallPositionIdentity const& handlerCall) const
            -> Result<ToolCallIssuingContext>;

        [[nodiscard]]
        auto issue(ValidatedToolInvocation const& invocation)
            -> Result<ToolCallPositionIdentity>;

        // A call that consumes one observation. The reference is a call-scoped
        // borrow whose identity is copied out here; nothing is retained.
        [[nodiscard]]
        auto issueAgainstObservation(
            ValidatedToolInvocation const& invocation,
            SnapshotObservationReference const& observation
        ) -> Result<ToolCallPositionIdentity>;

        [[nodiscard]] auto rootIdentity() const -> ContentHash;

        [[nodiscard]]
        auto parent() const noexcept UF_LIFETIME_BOUND
            -> ToolCallParent const&;

        // How many children this context has issued. It is what seals the
        // context at teardown: a restarted script that terminates leaving
        // recorded calls beyond this count unconsumed is divergence.
        [[nodiscard]] auto issuedChildren() const noexcept -> uint32;
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

        // Every tool this catalog declares, in the catalog's own order. The
        // binding table needs the whole set rather than one lookup at a time,
        // because "a declared Tool with no binding" is a statement about the
        // set and cannot be answered one name at a time.
        [[nodiscard]] auto toolNames() const -> std::vector<std::string>;

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

    // The registration's Tool binding table, joined once against the catalog it
    // names and the closure that implements it. Only bind() mints one, so a
    // value of this type is proof that all three bind-time refusals passed:
    // no binding names an unexported entry, no declared Tool is unbound, and
    // no binding names a Tool the catalog never declared.
    //
    // The join is what a Project's one compiled program per registration
    // generation is compiled with: entryPoints() is the union of the bound
    // entries, and the per-entry variance a handler runs under lives in the
    // entry's descriptor, which admission reads per call and the compiler
    // never sees.
    class ProjectToolBindingTable final
    {
        ContentHash                     m_projectRegistrationHash;
        std::vector<ProjectToolBinding> m_bindings;

        ProjectToolBindingTable(
            ContentHash projectRegistrationHash,
            std::vector<ProjectToolBinding> bindings
        );

    public:
        // The catalog must be the one this registration pinned, and
        // exportedEntryPoints is a call-scoped borrow of the entries the
        // Project closure exports. Nothing here retains either.
        [[nodiscard]]
        static auto bind(
            VerifiedProjectRegistration const& registration,
            ProjectToolCatalogSchemaOwner const& catalog,
            std::span<std::string const> exportedEntryPoints
        ) -> Result<ProjectToolBindingTable>;

        [[nodiscard]] auto projectRegistrationHash() const -> ContentHash;

        [[nodiscard]]
        auto bindings() const noexcept UF_LIFETIME_BOUND
            -> std::vector<ProjectToolBinding> const&;

        // The entry a dispatcher runs for one Tool. A name this table did not
        // bind is a refusal rather than an absence: every declared Tool is
        // bound, so an unknown name is a Tool this registration never declared.
        [[nodiscard]]
        auto entryPointFor(std::string_view toolName) const -> Result<std::string>;

        // The union of bound entries, sorted and unique.
        [[nodiscard]] auto entryPoints() const -> std::vector<std::string>;
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

        // Every Tool this catalog declares, in its own order. It is the same
        // whole-set question ProjectToolCatalogSchemaOwner::toolNames answers
        // and for the same reason: a caller that must render the discovery
        // table a scoped run reads needs the set, and a set cannot be
        // assembled one describe() at a time by a caller that does not already
        // know the names.
        [[nodiscard]] auto toolNames() const -> std::vector<std::string>;

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
