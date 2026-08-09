#include "journal-entry.hpp"

#include <domain/error.hpp>

#include <string>
#include <utility>

namespace uf::operator_runtime
{
    ValidatedJournalEntryData::ValidatedJournalEntryData(
        ContentHash projectRegistrationHash,
        std::string namespacedEventType,
        ContentHash payloadSchemaHash,
        CanonicalJson payload,
        CanonicalJson provenance
    )
        : m_projectRegistrationHash{projectRegistrationHash}
        , m_namespacedEventType{std::move(namespacedEventType)}
        , m_payloadSchemaHash{payloadSchemaHash}
        , m_payload{std::move(payload)}
        , m_provenance{std::move(provenance)}
    {
    }

    auto ValidatedJournalEntryData::projectRegistrationHash() const
        -> ContentHash
    {
        return m_projectRegistrationHash;
    }

    auto ValidatedJournalEntryData::namespacedEventType() const noexcept
        -> std::string const&
    {
        return m_namespacedEventType;
    }

    auto ValidatedJournalEntryData::payloadSchemaHash() const -> ContentHash
    {
        return m_payloadSchemaHash;
    }

    auto ValidatedJournalEntryData::payload() const noexcept
        -> CanonicalJson const&
    {
        return m_payload;
    }

    auto ValidatedJournalEntryData::provenance() const noexcept
        -> CanonicalJson const&
    {
        return m_provenance;
    }

    ProjectJournalSchemaOwner::ProjectJournalSchemaOwner(
        ContentHash projectRegistrationHash,
        JournalPayloadSchemaValidator validatePayload,
        JournalProvenanceValidator validateProvenance
    )
        : m_projectRegistrationHash{projectRegistrationHash}
        , m_validatePayload{std::move(validatePayload)}
        , m_validateProvenance{std::move(validateProvenance)}
    {
    }

    auto ProjectJournalSchemaOwner::create(
        VerifiedProjectRegistration const& registration,
        JournalPayloadSchemaValidator validatePayload,
        JournalProvenanceValidator validateProvenance
    ) -> Result<ProjectJournalSchemaOwner>
    {
        if (!validatePayload || !validateProvenance)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "ProjectJournalSchemaOwner requires payload and provenance validators"
            );
        }
        return ProjectJournalSchemaOwner{
            registration.hash(),
            std::move(validatePayload),
            std::move(validateProvenance),
        };
    }

    auto ProjectJournalSchemaOwner::validate(
        std::string namespacedEventType,
        CanonicalJson payload,
        CanonicalJson provenance
    ) const -> Result<ValidatedJournalEntryData>
    {
        if (namespacedEventType.empty())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Journal event type must be non-empty"
            );
        }
        UF_TRY_VALUE_CONTEXT(
            payloadSchemaHash,
            m_validatePayload(namespacedEventType, payload.bytes()),
            "validating namespaced Journal payload schema"
        );
        UF_TRY_CONTEXT(
            m_validateProvenance(provenance.bytes()),
            "validating fixed JournalProvenance schema"
        );
        return ValidatedJournalEntryData{
            m_projectRegistrationHash,
            std::move(namespacedEventType),
            payloadSchemaHash,
            std::move(payload),
            std::move(provenance),
        };
    }

}
