#pragma once

#include "manifest.hpp"
#include "project-plugin.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>

#include <domain/content-hash.hpp>

#include <functional>
#include <string>
#include <string_view>

namespace uf::operator_runtime
{
    class ProjectJournalSchemaOwner;

    // Authority-bearing Journal data. Only the schema owner bound to the exact
    // ProjectRegistration root can mint this value.
    class ValidatedJournalEntryData final
    {
        friend class ProjectJournalSchemaOwner;

        ContentHash   m_projectRegistrationHash;
        std::string   m_namespacedEventType;
        ContentHash   m_payloadSchemaHash;
        CanonicalJson m_payload;
        CanonicalJson m_provenance;

        ValidatedJournalEntryData(
            ContentHash projectRegistrationHash,
            std::string namespacedEventType,
            ContentHash payloadSchemaHash,
            CanonicalJson payload,
            CanonicalJson provenance
        );

    public:
        [[nodiscard]] auto projectRegistrationHash() const -> ContentHash;

        [[nodiscard]]
        auto namespacedEventType() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        [[nodiscard]] auto payloadSchemaHash() const -> ContentHash;

        [[nodiscard]]
        auto payload() const noexcept UF_LIFETIME_BOUND -> CanonicalJson const&;

        [[nodiscard]]
        auto provenance() const noexcept UF_LIFETIME_BOUND -> CanonicalJson const&;
    };

    // Trusted deployment callbacks. The payload validator selects the complete
    // schema by namespaced event type, validates the payload, and returns that
    // schema's real content hash. The provenance validator enforces the one
    // fixed JournalProvenance schema.
    using JournalPayloadSchemaValidator = std::function<
        Result<ContentHash>(
            std::string_view namespacedEventType,
            std::string_view exactPayloadJcs
        )
    >;
    using JournalProvenanceValidator = std::function<
        Status(std::string_view exactProvenanceJcs)
    >;

    class ProjectJournalSchemaOwner final
    {
        ContentHash                   m_projectRegistrationHash;
        JournalPayloadSchemaValidator m_validatePayload;
        JournalProvenanceValidator    m_validateProvenance;

        ProjectJournalSchemaOwner(
            ContentHash projectRegistrationHash,
            JournalPayloadSchemaValidator validatePayload,
            JournalProvenanceValidator validateProvenance
        );

    public:
        // The exact journal event schema manifest bytes are required so the
        // payload validator provably answers for the manifest this
        // registration named; without them the recorded payload_schema_hash is
        // whatever an arbitrary validator chose to return.
        [[nodiscard]]
        static auto create(
            VerifiedProjectRegistration const& registration,
            std::string_view exactJournalSchemaManifestBytes,
            JournalPayloadSchemaValidator validatePayload,
            JournalProvenanceValidator validateProvenance
        ) -> Result<ProjectJournalSchemaOwner>;

        [[nodiscard]]
        auto validate(
            std::string namespacedEventType,
            CanonicalJson payload,
            CanonicalJson provenance
        ) const -> Result<ValidatedJournalEntryData>;
    };
}
