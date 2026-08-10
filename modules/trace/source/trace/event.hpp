#pragma once

#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uf::trace
{
    class TraceRecorder;

    inline constexpr auto k_traceSchema = std::string_view{"umbraflow-trace/v2"};
    inline constexpr auto k_traceSchemaHash = std::string_view{
        "b7fb253bae3bd4c4307751a3d43271e5adda91af0870a6dcf359fca91029ccc1"
    };

    // Audit payloads deliberately have no byte, array, or nested-object value.
    // This keeps production Trace a small semantic stream rather than a covert
    // Replay Bundle. Larger structures belong behind a content-addressed
    // reference owned by their actual subsystem.
    using TraceScalar = std::variant<
        std::monostate,
        bool,
        int64,
        uint64,
        std::string
    >;

    struct TraceField final
    {
        std::string name{};
        TraceScalar value{};
    };

    struct TraceReference final
    {
        std::string type{};
        std::string id{};
    };

    struct AuditMetadata final
    {
        std::string                 actor{};
        std::vector<TraceReference> references{};
    };

    struct TypedTracePayload final
    {
        ContentHash schemaHash;

        std::vector<TraceField> fields{};
    };

    struct TraceEventSpec final
    {
        std::string eventType{};

        AuditMetadata     audit{};
        TypedTracePayload payload;
    };

    struct TraceStreamSpec final
    {
        std::string sessionId{};
        ContentHash sessionManifestHash;
        std::string producer{};
    };

    // An immutable event after recorder-owned stream identity, sequence, and
    // audit time have been attached. Only TraceRecorder can construct one.
    class TraceEvent final
    {
        friend class TraceRecorder;

        TraceEventSpec  m_spec;
        TraceStreamSpec m_stream;
        uint64          m_sequence;
        int64           m_recordedAtUnixMillis;

        TraceEvent(
            TraceEventSpec spec,
            TraceStreamSpec stream,
            uint64 sequence,
            int64 recordedAtUnixMillis
        );

    public:
        TraceEvent(TraceEvent const&) = default;
        TraceEvent(TraceEvent&&) noexcept = default;
        auto operator=(TraceEvent const&) -> TraceEvent& = default;
        auto operator=(TraceEvent&&) noexcept -> TraceEvent& = default;

        ~TraceEvent() = default;

        [[nodiscard]]
        auto eventType() const noexcept UF_LIFETIME_BOUND -> std::string const&;

        [[nodiscard]]
        auto audit() const noexcept UF_LIFETIME_BOUND -> AuditMetadata const&;

        [[nodiscard]]
        auto payload() const noexcept UF_LIFETIME_BOUND -> TypedTracePayload const&;

        [[nodiscard]]
        auto sessionId() const noexcept UF_LIFETIME_BOUND -> std::string const&;

        [[nodiscard]] auto sessionManifestHash() const noexcept -> ContentHash;

        [[nodiscard]]
        auto producer() const noexcept UF_LIFETIME_BOUND -> std::string const&;

        [[nodiscard]] auto sequence() const noexcept -> uint64;
        [[nodiscard]] auto recordedAtUnixMillis() const noexcept -> int64;
    };

    // Puts payload fields into the member order serializeTraceEvent then emits
    // verbatim. RFC 8785 section 3.2.3 orders JSON member names by UTF-16 code
    // unit, which is neither UTF-8 byte order nor code-point order, so a
    // default string comparison computes a different order for any name a
    // supplementary code point can reach. Sorting the caller's vector in place
    // is the whole operation. Equal names come out adjacent, which is what the
    // recorder's duplicate-name check reads.
    auto sortTraceFieldsCanonically(std::vector<TraceField>& fields) -> void;

    // Stable single-line JSON. The payload remains typed and schema-addressed;
    // this function is an encoder only and no Trace JSON reader exists.
    [[nodiscard]]
    auto serializeTraceEvent(TraceEvent const& event) -> std::string;
}
