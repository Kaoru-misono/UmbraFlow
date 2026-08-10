#include "journal-entry.hpp"

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::operator_runtime
{
    namespace
    {
        // JR:`JournalProvenance` in schema/umbraflow-journal-v1.schema.json,
        // spelled out here because that file is not readable at runtime and a
        // second spelling of a fixed schema is the whole risk this replaces.
        //
        // Reading is positional: the document is exact RFC 8785 JCS, so its
        // members arrive in UTF-16 code-unit order, which for these four is
        // kind, observation_ids, principal_id, source_hashes. That is also what
        // enforces `additionalProperties: false` and the four `required`
        // members -- any extra, missing or reordered member breaks the
        // sequence. A non-canonical ordering is refused rather than accepted.
        //
        // Neither pattern admits a character JCS would escape, so a string
        // carrying a backslash fails its pattern and no unescaping is needed.
        constexpr auto k_provenanceKinds = std::array<std::string_view, 5U>{
            "client_db",
            "human_correction",
            "inference",
            "observation",
            "policy",
        };

        [[nodiscard]]
        auto isIdentifierByte(char character) noexcept -> bool
        {
            return (character >= '0' && character <= '9')
                || (character >= 'A' && character <= 'Z')
                || (character >= 'a' && character <= 'z')
                || character == '.'
                || character == '_'
                || character == ':'
                || character == '-';
        }

        // JR:`Identifier`: ^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$
        [[nodiscard]]
        auto isIdentifier(std::string_view value) noexcept -> bool
        {
            if (value.empty() || value.size() > 128U)
            {
                return false;
            }
            auto const leading = value.front();
            if (
                !(leading >= '0' && leading <= '9')
                && !(leading >= 'A' && leading <= 'Z')
                && !(leading >= 'a' && leading <= 'z')
            )
            {
                return false;
            }
            return std::ranges::all_of(value, isIdentifierByte);
        }

        // JR:`Hash`: ^[0-9a-f]{64}$
        [[nodiscard]]
        auto isHash(std::string_view value) noexcept -> bool
        {
            return value.size() == 64U
                && std::ranges::all_of(
                    value,
                    [](char character)
                    {
                        return (character >= '0' && character <= '9')
                            || (character >= 'a' && character <= 'f');
                    }
                );
        }

        // A read position in one exact JCS document. It borrows the caller's
        // bytes for the duration of a single validateJournalProvenance call and
        // is never stored or returned, which is the whole of its lifetime
        // contract.
        class JcsCursor final
        {
            std::string_view m_document;
            std::size_t      m_at{};

        public:
            explicit JcsCursor(std::string_view document) noexcept
                : m_document{document}
            {
            }

            [[nodiscard]]
            auto expect(std::string_view literal) noexcept -> bool
            {
                if (!m_document.substr(m_at).starts_with(literal))
                {
                    return false;
                }
                m_at += literal.size();
                return true;
            }

            [[nodiscard]]
            auto atEnd() const noexcept -> bool
            {
                return m_at == m_document.size();
            }

            // The bytes between the quotes. Absent when the cursor is not on a
            // string token or the token carries an escape.
            [[nodiscard]]
            auto takeString() noexcept -> std::optional<std::string_view>
            {
                if (!expect("\""))
                {
                    return std::nullopt;
                }
                auto const rest = m_document.substr(m_at);
                auto const end  = rest.find('"');
                if (
                    end == std::string_view::npos
                    || rest.substr(0U, end).find('\\') != std::string_view::npos
                )
                {
                    return std::nullopt;
                }
                m_at += end + 1U;
                return rest.substr(0U, end);
            }

            // A JSON array of strings, already proven to satisfy uniqueItems.
            // Absent when the cursor is not on one or an element repeats.
            [[nodiscard]]
            auto takeUniqueStringArray()
                -> std::optional<std::vector<std::string_view>>
            {
                if (!expect("["))
                {
                    return std::nullopt;
                }
                auto elements = std::vector<std::string_view>{};
                if (expect("]"))
                {
                    return elements;
                }
                while (true)
                {
                    auto const element = takeString();
                    if (!element || std::ranges::contains(elements, *element))
                    {
                        return std::nullopt;
                    }
                    elements.emplace_back(*element);
                    if (expect("]"))
                    {
                        return elements;
                    }
                    if (!expect(","))
                    {
                        return std::nullopt;
                    }
                }
            }
        };

        [[nodiscard]]
        auto refuseProvenance(std::string_view member) -> Status
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Journal provenance does not satisfy the fixed "
                "JournalProvenance schema at "
                + std::string{member}
            );
        }

        [[nodiscard]]
        auto validateJournalProvenance(std::string_view exactProvenanceJcs)
            -> Status
        {
            auto cursor = JcsCursor{exactProvenanceJcs};
            if (!cursor.expect("{\"kind\":"))
            {
                return refuseProvenance("kind");
            }
            auto const kind = cursor.takeString();
            if (!kind || !std::ranges::contains(k_provenanceKinds, *kind))
            {
                return refuseProvenance("kind");
            }
            if (!cursor.expect(",\"observation_ids\":"))
            {
                return refuseProvenance("observation_ids");
            }
            auto const observationIds = cursor.takeUniqueStringArray();
            if (
                !observationIds
                || !std::ranges::all_of(*observationIds, isIdentifier)
            )
            {
                return refuseProvenance("observation_ids");
            }
            if (!cursor.expect(",\"principal_id\":"))
            {
                return refuseProvenance("principal_id");
            }
            if (!cursor.expect("null"))
            {
                auto const principalId = cursor.takeString();
                if (!principalId || !isIdentifier(*principalId))
                {
                    return refuseProvenance("principal_id");
                }
            }
            if (!cursor.expect(",\"source_hashes\":"))
            {
                return refuseProvenance("source_hashes");
            }
            auto const sourceHashes = cursor.takeUniqueStringArray();
            if (!sourceHashes || !std::ranges::all_of(*sourceHashes, isHash))
            {
                return refuseProvenance("source_hashes");
            }
            if (!cursor.expect("}") || !cursor.atEnd())
            {
                return refuseProvenance("the closing object");
            }
            return ok();
        }
    }

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
        JournalPayloadSchemaValidator validatePayload
    )
        : m_projectRegistrationHash{projectRegistrationHash}
        , m_validatePayload{std::move(validatePayload)}
    {
    }

    auto ProjectJournalSchemaOwner::create(
        VerifiedProjectRegistration const& registration,
        std::string_view exactJournalSchemaManifestBytes,
        JournalPayloadSchemaValidator validatePayload
    ) -> Result<ProjectJournalSchemaOwner>
    {
        if (!validatePayload)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "ProjectJournalSchemaOwner requires a payload validator"
            );
        }
        UF_TRY_VALUE(
            manifestHash,
            sha256(std::as_bytes(std::span{exactJournalSchemaManifestBytes}))
        );
        if (manifestHash != registration.journalEventSchemaManifestHash())
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "journal event schema manifest bytes do not match the "
                "registration's journal_event_schema_manifest_hash"
            );
        }
        return ProjectJournalSchemaOwner{
            registration.hash(),
            std::move(validatePayload),
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
            validateJournalProvenance(provenance.bytes()),
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
