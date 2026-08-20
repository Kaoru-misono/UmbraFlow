#pragma once

#include "project-plugin.hpp"

#include <json/value.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uf::operator_runtime
{
    class OperatorCoordinator;

    enum class ProjectToolPreconditionStatus : uint8
    {
        Known,
        Unknown,
        Stale,
        Conflict,
    };

    [[nodiscard]]
    auto projectToolPreconditionStatusWireName(
        ProjectToolPreconditionStatus status
    ) noexcept -> std::string_view;

    struct ProjectToolPrecondition final
    {
        std::string                   name{};
        ProjectToolPreconditionStatus status{ProjectToolPreconditionStatus::Unknown};
    };

    struct ObservedInstanceProposal final
    {
        std::string                localRef{};
        std::optional<std::string> parentLocalRef{};
        std::string                kind{};
        std::string                identitySchemaId{};
        json::Value                semanticIdentityBasis{};
        json::Value                opaqueProjectPayload{};
    };

    // The only observation shape a Project VM may propose. It has no final ID
    // or authority-binding member; those values exist only on the trusted
    // Operator side of the boundary.
    struct ProjectObservationProposal final
    {
        std::string                           schema{"umbraflow-project-observation-proposal/v1"};
        json::Value                           canonicalOpaquePayload{};
        std::vector<ProjectToolPrecondition>  projectToolPreconditions{};
        std::vector<ObservedInstanceProposal> observedInstanceProposals{};
    };

    // An opaque Operator mint. There is deliberately no parser or public
    // string constructor: business code may copy an ID it received or return
    // its wire spelling to the Coordinator for resolution, but cannot turn the
    // version-tagged bytes into authority.
    class ObservedInstanceId final
    {
        friend class OperatorCoordinator;

        std::string m_value;

        explicit ObservedInstanceId(std::string value);

    public:
        ObservedInstanceId(ObservedInstanceId const&) = default;
        ObservedInstanceId(ObservedInstanceId&&) noexcept = default;
        auto operator=(ObservedInstanceId const&) -> ObservedInstanceId& = default;
        auto operator=(ObservedInstanceId&&) noexcept -> ObservedInstanceId& = default;
        ~ObservedInstanceId() = default;

        [[nodiscard]]
        auto value() const noexcept UF_LIFETIME_BOUND -> std::string const&;

        auto operator==(ObservedInstanceId const&) const -> bool = default;
    };

    struct ObservedInstance final
    {
        ObservedInstanceId                observedInstanceId;
        std::optional<ObservedInstanceId> parentObservedInstanceId{};
        std::string                       kind{};
        json::Value                       opaqueProjectPayload{};
    };

    // The final closed envelope. Its constructor is Operator-private so a
    // caller cannot submit IDs or replace the binding the mint selected.
    class ProjectObservation final
    {
        friend class OperatorCoordinator;

        json::Value                          m_canonicalOpaquePayload;
        std::vector<ProjectToolPrecondition> m_projectToolPreconditions;
        std::vector<ObservedInstance>        m_observedInstances;
        std::string                          m_canonicalBytes;
        ContentHash                          m_hash;

        ProjectObservation(
            json::Value canonicalOpaquePayload,
            std::vector<ProjectToolPrecondition> projectToolPreconditions,
            std::vector<ObservedInstance> observedInstances,
            std::string canonicalBytes,
            ContentHash hash
        );

    public:
        ProjectObservation(ProjectObservation const&) = default;
        ProjectObservation(ProjectObservation&&) noexcept = default;
        auto operator=(ProjectObservation const&) -> ProjectObservation& = default;
        auto operator=(ProjectObservation&&) noexcept -> ProjectObservation& = default;
        ~ProjectObservation() = default;

        [[nodiscard]] static auto schema() noexcept -> std::string_view;

        [[nodiscard]]
        auto canonicalOpaquePayload() const noexcept UF_LIFETIME_BOUND
            -> json::Value const&;

        [[nodiscard]]
        auto projectToolPreconditions() const noexcept UF_LIFETIME_BOUND
            -> std::span<ProjectToolPrecondition const>;

        [[nodiscard]]
        auto observedInstances() const noexcept UF_LIFETIME_BOUND
            -> std::span<ObservedInstance const>;

        [[nodiscard]]
        auto canonicalBytes() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        [[nodiscard]] auto hash() const -> ContentHash;
    };

    enum class ObservedInstanceWorldScopeKind : uint8
    {
        Account,
        Run,
    };

    class ObservedInstanceWorldScope final
    {
        ObservedInstanceWorldScopeKind m_kind;
        std::string                    m_scopeId;
        uint64                         m_generation;

        ObservedInstanceWorldScope(
            ObservedInstanceWorldScopeKind kind,
            std::string scopeId,
            uint64 generation
        );

    public:
        [[nodiscard]]
        static auto account(
            std::string scopeId,
            uint64 generation
        ) -> Result<ObservedInstanceWorldScope>;

        [[nodiscard]]
        static auto run(
            std::string scopeId,
            uint64 generation
        ) -> Result<ObservedInstanceWorldScope>;

        [[nodiscard]] auto kind() const noexcept -> ObservedInstanceWorldScopeKind;

        [[nodiscard]]
        auto scopeId() const noexcept UF_LIFETIME_BOUND -> std::string const&;

        [[nodiscard]] auto generation() const noexcept -> uint64;
    };

    [[nodiscard]]
    auto observedInstanceWorldScopeKindWireName(
        ObservedInstanceWorldScopeKind kind
    ) noexcept -> std::string_view;

    // Each callable is trusted deployment authority that applies one exact
    // identity schema from the registration's closed reference set. The
    // registry binds those callables to the verified registration before the
    // Coordinator can use any of them.
    using SemanticIdentityBasisValidator = std::function<Status(json::Value const& basis)>;

    struct ObservedInstanceIdentitySchema final
    {
        std::string                    schemaId{};
        ContentHash                    schemaHash;
        SemanticIdentityBasisValidator validate{};
    };

    class ObservedInstanceIdentitySchemas final
    {
        class State;

        friend class OperatorCoordinator;

        std::shared_ptr<State const> m_state;

        explicit ObservedInstanceIdentitySchemas(
            std::shared_ptr<State const> p_state
        ) noexcept;

        [[nodiscard]] auto contains(std::string_view schemaId) const -> bool;

        [[nodiscard]]
        auto validate(
            std::string_view schemaId,
            json::Value const& basis
        ) const -> Status;

    public:
        ObservedInstanceIdentitySchemas(ObservedInstanceIdentitySchemas const&) noexcept = default;
        ObservedInstanceIdentitySchemas(ObservedInstanceIdentitySchemas&&) noexcept = default;
        auto operator=(ObservedInstanceIdentitySchemas const&) noexcept
            -> ObservedInstanceIdentitySchemas& = default;
        auto operator=(ObservedInstanceIdentitySchemas&&) noexcept
            -> ObservedInstanceIdentitySchemas& = default;
        ~ObservedInstanceIdentitySchemas() = default;

        [[nodiscard]]
        static auto create(
            VerifiedProjectRegistration const& registration,
            std::vector<ObservedInstanceIdentitySchema> schemas
        ) -> Result<ObservedInstanceIdentitySchemas>;

        [[nodiscard]] auto projectRegistrationHash() const -> ContentHash;
    };

    enum class ProjectObservationErrorCode : uint8
    {
        MalformedAuthorityInput,
        MalformedProposal,
        PreconditionNameNotNamespaced,
        PreconditionStatusOutsideFactDomain,
        InvalidWorldScopeGeneration,
        DuplicatePreconditionName,
        DuplicateObservedInstanceLocalRef,
        ObservedInstanceParentMissing,
        ObservedInstanceParentCycle,
        ObservedInstanceIdentitySchemaNotRegistered,
        SemanticIdentityBasisSchemaViolation,
        ObservedInstanceCollision,
        ObservedInstanceScopeMismatch,
        ObservedInstanceStale,
    };

    [[nodiscard]]
    auto projectObservationErrorCode(
        Error const& error
    ) noexcept -> std::optional<ProjectObservationErrorCode>;

    [[nodiscard]]
    auto projectObservationErrorWireName(
        ProjectObservationErrorCode code
    ) noexcept -> std::string_view;

    [[nodiscard]]
    auto fail(
        ProjectObservationErrorCode code,
        std::string message,
        std::source_location location = std::source_location::current()
    ) -> std::unexpected<Error>;

    // The ledger metadata for one stored observation. It carries the final
    // closed envelope the canonical mint produced, not the derive proposal
    // bytes -- what the row stores is what this type holds, so its hash is the
    // row's observation_hash and its payload is the row's
    // canonical_observation.
    class StoredProjectObservation final
    {
        friend class OperatorCoordinator;

        ContentHash        m_projectRegistrationHash;
        ContentHash        m_pluginModuleManifestHash;
        std::string        m_projectInstanceKey;
        ContentHash        m_stateResolutionHash;
        uint64             m_projectStateRevision;
        ContentHash        m_projectStateHash;
        uint64             m_revision;
        ProjectObservation m_payload;

        StoredProjectObservation(
            ContentHash projectRegistrationHash,
            ContentHash pluginModuleManifestHash,
            std::string projectInstanceKey,
            ContentHash stateResolutionHash,
            uint64 projectStateRevision,
            ContentHash projectStateHash,
            uint64 revision,
            ProjectObservation payload
        );

    public:
        StoredProjectObservation(StoredProjectObservation const&) = default;
        StoredProjectObservation(StoredProjectObservation&&) noexcept = default;
        auto operator=(StoredProjectObservation const&)
            -> StoredProjectObservation& = default;
        auto operator=(StoredProjectObservation&&) noexcept
            -> StoredProjectObservation& = default;
        ~StoredProjectObservation() = default;

        [[nodiscard]] auto projectRegistrationHash() const -> ContentHash;
        [[nodiscard]] auto pluginModuleManifestHash() const -> ContentHash;

        [[nodiscard]]
        auto projectInstanceKey() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        [[nodiscard]] auto stateResolutionHash() const -> ContentHash;
        [[nodiscard]] auto projectStateRevision() const noexcept -> uint64;
        [[nodiscard]] auto projectStateHash() const -> ContentHash;
        [[nodiscard]] auto revision() const noexcept -> uint64;
        [[nodiscard]] auto hash() const -> ContentHash;

        [[nodiscard]]
        auto payload() const noexcept UF_LIFETIME_BOUND
            -> ProjectObservation const&;
    };
}
