#include "project-observation.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/text/utf8.hpp>

#include <domain/error.hpp>

#include <array>
#include <map>
#include <source_location>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace uf::operator_runtime
{
    namespace
    {
        constexpr auto k_projectObservationErrorCodes = std::array{
            ProjectObservationErrorCode::MalformedAuthorityInput,
            ProjectObservationErrorCode::MalformedProposal,
            ProjectObservationErrorCode::PreconditionNameNotNamespaced,
            ProjectObservationErrorCode::PreconditionStatusOutsideFactDomain,
            ProjectObservationErrorCode::InvalidWorldScopeGeneration,
            ProjectObservationErrorCode::DuplicatePreconditionName,
            ProjectObservationErrorCode::DuplicateObservedInstanceLocalRef,
            ProjectObservationErrorCode::ObservedInstanceParentMissing,
            ProjectObservationErrorCode::ObservedInstanceParentCycle,
            ProjectObservationErrorCode::ObservedInstanceIdentitySchemaNotRegistered,
            ProjectObservationErrorCode::SemanticIdentityBasisSchemaViolation,
            ProjectObservationErrorCode::ObservedInstanceCollision,
            ProjectObservationErrorCode::ObservedInstanceScopeMismatch,
            ProjectObservationErrorCode::ObservedInstanceStale,
        };

        [[nodiscard]]
        auto projectObservationErrorDetailValue(
            ProjectObservationErrorCode code
        ) noexcept -> int
        {
            auto const underlying = checkedCast<int>(std::to_underlying(code));
            UF_CHECK(underlying.has_value());
            auto const encoded = checkedAdd(*underlying, 1);
            UF_CHECK(encoded.has_value());
            return *encoded;
        }

        class ProjectObservationErrorCategory final : public std::error_category
        {
        public:
            [[nodiscard]] auto name() const noexcept -> char const* override
            {
                return "uf.operator.project-observation";
            }

            [[nodiscard]] auto message(int value) const -> std::string override
            {
                for (auto const code : k_projectObservationErrorCodes)
                {
                    if (projectObservationErrorDetailValue(code) == value)
                    {
                        return std::string{projectObservationErrorWireName(code)};
                    }
                }
                return "UnknownProjectObservationErrorCode";
            }
        };

        [[nodiscard]]
        auto projectObservationErrorCategory() noexcept
            -> std::error_category const&
        {
            static auto const s_category = ProjectObservationErrorCategory{};
            return s_category;
        }

        [[nodiscard]]
        auto isScopeIdentifier(std::string_view value) -> bool
        {
            if (value.empty() || value.size() > 128U || !isValidUtf8(value))
            {
                return false;
            }
            auto const valid = [](char character)
            {
                return (character >= 'A' && character <= 'Z')
                    || (character >= 'a' && character <= 'z')
                    || (character >= '0' && character <= '9')
                    || character == '.'
                    || character == '_'
                    || character == ':'
                    || character == '-';
            };
            if (!valid(value.front()))
            {
                return false;
            }
            for (auto const character : value)
            {
                if (!valid(character))
                {
                    return false;
                }
            }
            return true;
        }

    } // namespace

    class ObservedInstanceIdentitySchemas::State final
    {
    public:
        ContentHash projectRegistrationHash;
        std::map<
            std::string,
            SemanticIdentityBasisValidator,
            std::less<>> schemas{};
    };

    auto fail(
        ProjectObservationErrorCode code,
        std::string message,
        std::source_location location
    ) -> std::unexpected<Error>
    {
        return uf::fail(
            std::error_code{
                projectObservationErrorDetailValue(code),
                projectObservationErrorCategory(),
            },
            std::move(message),
            {},
            location
        );
    }

    auto projectToolPreconditionStatusWireName(
        ProjectToolPreconditionStatus status
    ) noexcept -> std::string_view
    {
        switch (status)
        {
        case ProjectToolPreconditionStatus::Known:    return "Known";
        case ProjectToolPreconditionStatus::Unknown:  return "Unknown";
        case ProjectToolPreconditionStatus::Stale:    return "Stale";
        case ProjectToolPreconditionStatus::Conflict: return "Conflict";
        }
        UF_UNREACHABLE_MSG("Unknown ProjectToolPreconditionStatus value");
    }

    ObservedInstanceId::ObservedInstanceId(std::string value)
        : m_value{std::move(value)}
    {
    }

    auto ObservedInstanceId::value() const noexcept -> std::string const&
    {
        return m_value;
    }

    ProjectObservation::ProjectObservation(
        json::Value canonicalOpaquePayload,
        std::vector<ProjectToolPrecondition> projectToolPreconditions,
        std::vector<ObservedInstance> observedInstances,
        std::string canonicalBytes,
        ContentHash hash
    )
        : m_canonicalOpaquePayload{std::move(canonicalOpaquePayload)}
        , m_projectToolPreconditions{std::move(projectToolPreconditions)}
        , m_observedInstances{std::move(observedInstances)}
        , m_canonicalBytes{std::move(canonicalBytes)}
        , m_hash{hash}
    {
    }

    auto ProjectObservation::schema() noexcept -> std::string_view
    {
        return "umbraflow-project-observation/v1";
    }

    auto ProjectObservation::canonicalOpaquePayload() const noexcept
        -> json::Value const&
    {
        return m_canonicalOpaquePayload;
    }

    auto ProjectObservation::projectToolPreconditions() const noexcept
        -> std::span<ProjectToolPrecondition const>
    {
        return m_projectToolPreconditions;
    }

    auto ProjectObservation::observedInstances() const noexcept
        -> std::span<ObservedInstance const>
    {
        return m_observedInstances;
    }

    auto ProjectObservation::canonicalBytes() const noexcept -> std::string const&
    {
        return m_canonicalBytes;
    }

    auto ProjectObservation::hash() const -> ContentHash
    {
        return m_hash;
    }

    ObservedInstanceWorldScope::ObservedInstanceWorldScope(
        ObservedInstanceWorldScopeKind kind,
        std::string scopeId,
        uint64 generation
    )
        : m_kind{kind}
        , m_scopeId{std::move(scopeId)}
        , m_generation{generation}
    {
    }

    auto ObservedInstanceWorldScope::account(
        std::string scopeId,
        uint64 generation
    ) -> Result<ObservedInstanceWorldScope>
    {
        if (!isScopeIdentifier(scopeId))
        {
            return fail(
                ProjectObservationErrorCode::MalformedAuthorityInput,
                "Observed instance world scope_id is outside its wire domain"
            );
        }
        return ObservedInstanceWorldScope{
            ObservedInstanceWorldScopeKind::Account,
            std::move(scopeId),
            generation,
        };
    }

    auto ObservedInstanceWorldScope::run(
        std::string scopeId,
        uint64 generation
    ) -> Result<ObservedInstanceWorldScope>
    {
        if (!isScopeIdentifier(scopeId))
        {
            return fail(
                ProjectObservationErrorCode::MalformedAuthorityInput,
                "Observed instance world scope_id is outside its wire domain"
            );
        }
        if (generation == 0U)
        {
            return fail(
                ProjectObservationErrorCode::InvalidWorldScopeGeneration,
                "Observed instance run scope generation must be at least one"
            );
        }
        return ObservedInstanceWorldScope{
            ObservedInstanceWorldScopeKind::Run,
            std::move(scopeId),
            generation,
        };
    }

    auto ObservedInstanceWorldScope::kind() const noexcept
        -> ObservedInstanceWorldScopeKind
    {
        return m_kind;
    }

    auto ObservedInstanceWorldScope::scopeId() const noexcept
        -> std::string const&
    {
        return m_scopeId;
    }

    auto ObservedInstanceWorldScope::generation() const noexcept -> uint64
    {
        return m_generation;
    }

    auto observedInstanceWorldScopeKindWireName(
        ObservedInstanceWorldScopeKind kind
    ) noexcept -> std::string_view
    {
        switch (kind)
        {
        case ObservedInstanceWorldScopeKind::Account: return "account";
        case ObservedInstanceWorldScopeKind::Run:     return "run";
        }
        UF_UNREACHABLE_MSG("Unknown ObservedInstanceWorldScopeKind value");
    }

    ObservedInstanceIdentitySchemas::ObservedInstanceIdentitySchemas(
        std::shared_ptr<State const> p_state
    ) noexcept
        : m_state{std::move(p_state)}
    {
    }

    auto ObservedInstanceIdentitySchemas::create(
        VerifiedProjectRegistration const& registration,
        std::vector<ObservedInstanceIdentitySchema> schemas
    ) -> Result<ObservedInstanceIdentitySchemas>
    {
        auto validators = std::map<
            std::string,
            SemanticIdentityBasisValidator,
            std::less<>>{};
        for (auto& schema : schemas)
        {
            if (schema.schemaId.empty() || !isValidUtf8(schema.schemaId) || !schema.validate)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Observed instance identity schemas require an ID and validator"
                );
            }
            auto const inserted = validators.try_emplace(
                std::move(schema.schemaId),
                std::move(schema.validate)
            ).second;
            if (!inserted)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Observed instance identity schema IDs must be unique"
                );
            }
        }
        // Built in place rather than as a designated-initialised temporary that
        // is then moved. State holds a std::map, whose move constructor this
        // standard library does not declare noexcept, so moving one here was the
        // only reason the type could throw while being constructed. Two members,
        // in declaration order.
        auto state = std::make_shared<State>(
            registration.hash(),
            std::move(validators)
        );
        return ObservedInstanceIdentitySchemas{
            std::shared_ptr<State const>{std::move(state)}
        };
    }

    auto ObservedInstanceIdentitySchemas::contains(
        std::string_view schemaId
    ) const -> bool
    {
        return m_state->schemas.contains(schemaId);
    }

    auto ObservedInstanceIdentitySchemas::validate(
        std::string_view schemaId,
        json::Value const& basis
    ) const -> Status
    {
        auto const found = m_state->schemas.find(schemaId);
        UF_CHECK(found != m_state->schemas.end());
        auto const result = found->second(basis);
        if (!result.has_value())
        {
            return fail(
                ProjectObservationErrorCode::SemanticIdentityBasisSchemaViolation,
                "Observed instance semantic identity basis violates its registered schema"
            );
        }
        return ok();
    }

    auto ObservedInstanceIdentitySchemas::projectRegistrationHash() const
        -> ContentHash
    {
        return m_state->projectRegistrationHash;
    }

    auto projectObservationErrorCode(
        Error const& error
    ) noexcept -> std::optional<ProjectObservationErrorCode>
    {
        auto const detail = error.detailCode();
        if (detail.category() != projectObservationErrorCategory())
        {
            return std::nullopt;
        }
        for (auto const code : k_projectObservationErrorCodes)
        {
            if (projectObservationErrorDetailValue(code) == detail.value())
            {
                return code;
            }
        }
        return std::nullopt;
    }

    auto projectObservationErrorWireName(
        ProjectObservationErrorCode code
    ) noexcept -> std::string_view
    {
        switch (code)
        {
        case ProjectObservationErrorCode::MalformedAuthorityInput:
            return "MalformedAuthorityInput";
        case ProjectObservationErrorCode::MalformedProposal:
            return "MalformedProposal";
        case ProjectObservationErrorCode::PreconditionNameNotNamespaced:
            return "PreconditionNameNotNamespaced";
        case ProjectObservationErrorCode::PreconditionStatusOutsideFactDomain:
            return "PreconditionStatusOutsideFactDomain";
        case ProjectObservationErrorCode::InvalidWorldScopeGeneration:
            return "InvalidWorldScopeGeneration";
        case ProjectObservationErrorCode::DuplicatePreconditionName:
            return "DuplicatePreconditionName";
        case ProjectObservationErrorCode::DuplicateObservedInstanceLocalRef:
            return "DuplicateObservedInstanceLocalRef";
        case ProjectObservationErrorCode::ObservedInstanceParentMissing:
            return "ObservedInstanceParentMissing";
        case ProjectObservationErrorCode::ObservedInstanceParentCycle:
            return "ObservedInstanceParentCycle";
        case ProjectObservationErrorCode::ObservedInstanceIdentitySchemaNotRegistered:
            return "ObservedInstanceIdentitySchemaNotRegistered";
        case ProjectObservationErrorCode::SemanticIdentityBasisSchemaViolation:
            return "SemanticIdentityBasisSchemaViolation";
        case ProjectObservationErrorCode::ObservedInstanceCollision:
            return "ObservedInstanceCollision";
        case ProjectObservationErrorCode::ObservedInstanceScopeMismatch:
            return "ObservedInstanceScopeMismatch";
        case ProjectObservationErrorCode::ObservedInstanceStale:
            return "ObservedInstanceStale";
        }
        UF_UNREACHABLE_MSG("Unknown ProjectObservationErrorCode value");
    }

    StoredProjectObservation::StoredProjectObservation(
        ContentHash projectRegistrationHash,
        ContentHash pluginHash,
        std::string projectInstanceKey,
        ContentHash stateResolutionHash,
        uint64 projectStateRevision,
        ContentHash projectStateHash,
        uint64 revision,
        ValidatedDocument payload
    )
        : m_projectRegistrationHash{projectRegistrationHash}
        , m_pluginHash{pluginHash}
        , m_projectInstanceKey{std::move(projectInstanceKey)}
        , m_stateResolutionHash{stateResolutionHash}
        , m_projectStateRevision{projectStateRevision}
        , m_projectStateHash{projectStateHash}
        , m_revision{revision}
        , m_payload{std::move(payload)}
    {
    }

    auto StoredProjectObservation::projectRegistrationHash() const -> ContentHash
    {
        return m_projectRegistrationHash;
    }

    auto StoredProjectObservation::pluginHash() const -> ContentHash
    {
        return m_pluginHash;
    }

    auto StoredProjectObservation::projectInstanceKey() const noexcept
        -> std::string const&
    {
        return m_projectInstanceKey;
    }

    auto StoredProjectObservation::stateResolutionHash() const -> ContentHash
    {
        return m_stateResolutionHash;
    }

    auto StoredProjectObservation::projectStateRevision() const noexcept -> uint64
    {
        return m_projectStateRevision;
    }

    auto StoredProjectObservation::projectStateHash() const -> ContentHash
    {
        return m_projectStateHash;
    }

    auto StoredProjectObservation::revision() const noexcept -> uint64
    {
        return m_revision;
    }

    auto StoredProjectObservation::hash() const -> ContentHash
    {
        return m_payload.contentHash();
    }

    auto StoredProjectObservation::payload() const noexcept
        -> ValidatedDocument const&
    {
        return m_payload;
    }
}
