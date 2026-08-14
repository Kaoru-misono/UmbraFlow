#include "event.hpp"

#include <core/text/json-text.hpp>
#include <core/utility/variant-match.hpp>

#include <domain/content-hash.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::trace
{
    namespace
    {
        auto appendField(
            std::string& output,
            TraceField const& field
        ) -> void
        {
            output += "{\"name\":";
            appendJsonString(output, field.name);
            output += ",\"type\":";
            matchVariant(
                field.value,
                [&output](std::monostate)
                {
                    output += "\"null\"";
                },
                [&output](bool value)
                {
                    output += "\"bool\",\"value\":";
                    output += value ? "true" : "false";
                },
                [&output](int64 value)
                {
                    output += "\"int64\",\"value\":";
                    output += std::format("{}", value);
                },
                [&output](uint64 value)
                {
                    output += "\"uint64\",\"value\":";
                    output += std::format("{}", value);
                },
                [&output](std::string const& value)
                {
                    output += "\"text\",\"value\":";
                    appendJsonString(output, value);
                }
            );
            output += '}';
        }

        auto appendReferences(
            std::string& output,
            std::vector<TraceReference> const& references
        ) -> void
        {
            output += '[';
            auto first = true;
            for (auto const& reference : references)
            {
                if (!first)
                {
                    output += ',';
                }
                first = false;
                output += "{\"type\":";
                appendJsonString(output, reference.type);
                output += ",\"id\":";
                appendJsonString(output, reference.id);
                output += '}';
            }
            output += ']';
        }

        auto appendFields(
            std::string& output,
            std::vector<TraceField> const& fields
        ) -> void
        {
            output += '[';
            auto first = true;
            for (auto const& field : fields)
            {
                if (!first)
                {
                    output += ',';
                }
                first = false;
                appendField(output, field);
            }
            output += ']';
        }
    }

    TraceEvent::TraceEvent(
        TraceEventSpec spec,
        TraceStreamSpec stream,
        uint64 sequence,
        int64 recordedAtUnixMillis
    )
        : m_spec{std::move(spec)}
        , m_stream{std::move(stream)}
        , m_sequence{sequence}
        , m_recordedAtUnixMillis{recordedAtUnixMillis}
    {
    }

    auto TraceEvent::eventType() const noexcept -> std::string const&
    {
        return m_spec.eventType;
    }

    auto TraceEvent::audit() const noexcept -> AuditMetadata const&
    {
        return m_spec.audit;
    }

    auto TraceEvent::payload() const noexcept -> TypedTracePayload const&
    {
        return m_spec.payload;
    }

    auto TraceEvent::sessionId() const noexcept -> std::string const&
    {
        return m_stream.sessionId;
    }

    auto TraceEvent::sessionManifestHash() const noexcept -> ContentHash
    {
        return m_stream.sessionManifestHash;
    }

    auto TraceEvent::producer() const noexcept -> std::string const&
    {
        return m_stream.producer;
    }

    auto TraceEvent::sequence() const noexcept -> uint64
    {
        return m_sequence;
    }

    auto TraceEvent::recordedAtUnixMillis() const noexcept -> int64
    {
        return m_recordedAtUnixMillis;
    }

    auto sortTraceFieldsCanonically(std::vector<TraceField>& fields) -> void
    {
        std::ranges::sort(fields, jsonMemberNameLess, &TraceField::name);
    }

    auto serializeTraceEvent(TraceEvent const& event) -> std::string
    {
        auto output = std::string{"{\"schema\":"};
        appendJsonString(output, k_traceSchema);
        output += ",\"event_type\":";
        appendJsonString(output, event.eventType());
        output += ",\"session_id\":";
        appendJsonString(output, event.sessionId());
        output += ",\"session_manifest_hash\":";
        appendJsonString(output, event.sessionManifestHash().hex());
        output += ",\"monotonic_sequence\":";
        output += std::format("{}", event.sequence());
        output += ",\"recorded_at_unix_millis\":";
        output += std::format("{}", event.recordedAtUnixMillis());
        output += ",\"audit\":{\"actor\":";
        appendJsonString(output, event.audit().actor);
        output += ",\"producer\":";
        appendJsonString(output, event.producer());
        output += ",\"references\":";
        appendReferences(output, event.audit().references);
        output += "},\"payload\":{\"fields\":";
        appendFields(output, event.payload().fields);
        output += "}}";
        return output;
    }
}
