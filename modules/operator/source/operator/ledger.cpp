#include "ledger.hpp"

#include "evidence-store.hpp"
#include "runtime-installation.hpp"
#include "snapshot-reference.hpp"
#include "tool-admission-request.hpp"

#include <core/error/contracts.hpp>
#include <core/safety/annotations.hpp>
#include <core/text/json-text.hpp>
#include <core/text/utf8.hpp>
#include <core/time/monotonic-time.hpp>

#include <domain/error.hpp>

#include <json/value.hpp>

#include <script/scoped-tool-program.hpp>

#include <task/platform/confined-file.hpp>
#include <task/runtime-model-file.hpp>

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace uf::operator_runtime
{
    namespace
    {
        struct DatabaseCloser final
        {
            auto operator()(sqlite3* database) const noexcept -> void
            {
                static_cast<void>(sqlite3_close_v2(database));
            }
        };

        using Database = std::unique_ptr<sqlite3, DatabaseCloser>;

        // A set of identifiers as the one text the ledger stores it under: a
        // JCS array, sorted and without repeats. Capability sets and
        // required-approval sets are both this shape, and both are hashed or
        // compared as whole documents, so two spellings of one set would be two
        // values.
        [[nodiscard]]
        auto canonicalNameArray(std::vector<std::string> names) -> std::string
        {
            std::ranges::sort(names);
            names.erase(std::ranges::unique(names).begin(), names.end());
            auto output = std::string{"["};
            auto first  = true;
            for (auto const& name : names)
            {
                if (!first)
                {
                    output.push_back(',');
                }
                first = false;
                appendJsonString(output, name);
            }
            output.push_back(']');
            return output;
        }

        // The same set, read back out of the column that holds it. A row this
        // process wrote is the only thing that reaches here, so unparseable
        // text is a broken database rather than bad input.
        [[nodiscard]]
        auto readNameArray(std::string_view stored) -> Result<std::vector<std::string>>
        {
            UF_TRY_VALUE(document, json::parse(stored));
            if (document.kind() != json::ValueKind::Array)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "a stored identifier set is not a JSON array"
                );
            }
            auto names = std::vector<std::string>{};
            for (auto const& item : document.items())
            {
                if (item.kind() != json::ValueKind::String)
                {
                    return fail(
                        AutomationErrorKind::InternalInvariant,
                        "a stored identifier set holds a value that is not a name"
                    );
                }
                names.emplace_back(item.string());
            }
            return names;
        }

        class Statement final
        {
            sqlite3_stmt* m_statement;

        public:
            explicit Statement(sqlite3_stmt* statement) noexcept
                : m_statement{statement}
            {
            }

            Statement(Statement&& other) noexcept
                : m_statement{std::exchange(other.m_statement, nullptr)}
            {
            }

            auto operator=(Statement&& other) noexcept -> Statement&
            {
                if (this != &other)
                {
                    static_cast<void>(sqlite3_finalize(m_statement));
                    m_statement = std::exchange(other.m_statement, nullptr);
                }
                return *this;
            }

            Statement(Statement const&) = delete;
            auto operator=(Statement const&) -> Statement& = delete;

            ~Statement()
            {
                static_cast<void>(sqlite3_finalize(m_statement));
            }

            [[nodiscard]] auto get() const noexcept -> sqlite3_stmt*
            {
                return m_statement;
            }
        };

        [[nodiscard]]
        auto databaseFailure(
            sqlite3* database,
            std::string_view action
        ) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::IoFailure,
                std::format("Operator database {}: {}", action, sqlite3_errmsg(database))
            );
        }

        [[nodiscard]]
        auto execute(
            sqlite3* database,
            std::string_view sql
        ) -> Status
        {
            auto* message = static_cast<char*>(nullptr);
            auto const code = sqlite3_exec(
                database,
                sql.data(),
                nullptr,
                nullptr,
                &message
            );
            if (code == SQLITE_OK)
            {
                return ok();
            }

            auto detail = message == nullptr
                ? std::string{sqlite3_errmsg(database)}
                : std::string{message};
            sqlite3_free(message);
            return fail(
                AutomationErrorKind::IoFailure,
                std::format("Operator database statement failed: {}", detail)
            );
        }

        [[nodiscard]]
        auto prepare(
            sqlite3* database,
            std::string_view sql
        ) -> Result<Statement>
        {
            auto* statement = static_cast<sqlite3_stmt*>(nullptr);
            auto const code = sqlite3_prepare_v3(
                database,
                sql.data(),
                static_cast<int>(sql.size()),
                SQLITE_PREPARE_PERSISTENT,
                &statement,
                nullptr
            );
            if (code != SQLITE_OK)
            {
                return databaseFailure(database, "could not prepare statement");
            }
            return Statement{statement};
        }

        [[nodiscard]]
        auto bindText(
            sqlite3* database,
            sqlite3_stmt* statement,
            int index,
            std::string_view value
        ) -> Status
        {
            auto const code = sqlite3_bind_text64(
                statement,
                index,
                value.data(),
                static_cast<sqlite3_uint64>(value.size()),
                SQLITE_TRANSIENT,
                SQLITE_UTF8
            );
            if (code != SQLITE_OK)
            {
                return databaseFailure(database, "could not bind text");
            }
            return ok();
        }

        [[nodiscard]]
        auto bindOptionalText(
            sqlite3* database,
            sqlite3_stmt* statement,
            int index,
            std::optional<std::string_view> value
        ) -> Status
        {
            if (value)
            {
                return bindText(database, statement, index, *value);
            }
            if (sqlite3_bind_null(statement, index) != SQLITE_OK)
            {
                return databaseFailure(database, "could not bind null text");
            }
            return ok();
        }

        [[nodiscard]]
        auto bindInteger(
            sqlite3* database,
            sqlite3_stmt* statement,
            int index,
            uint64 value
        ) -> Status
        {
            if (value > static_cast<uint64>(std::numeric_limits<sqlite3_int64>::max()))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Operator integer exceeds SQLite signed range"
                );
            }
            auto const code = sqlite3_bind_int64(
                statement,
                index,
                static_cast<sqlite3_int64>(value)
            );
            if (code != SQLITE_OK)
            {
                return databaseFailure(database, "could not bind integer");
            }
            return ok();
        }

        [[nodiscard]]
        auto bindOptionalInteger(
            sqlite3* database,
            sqlite3_stmt* statement,
            int index,
            std::optional<uint64> value
        ) -> Status
        {
            if (value)
            {
                return bindInteger(database, statement, index, *value);
            }
            if (sqlite3_bind_null(statement, index) != SQLITE_OK)
            {
                return databaseFailure(database, "could not bind null integer");
            }
            return ok();
        }

        [[nodiscard]]
        auto checkedSqlIncrement(
            uint64 value,
            std::string_view field
        ) -> Result<uint64>
        {
            if (value == static_cast<uint64>(std::numeric_limits<sqlite3_int64>::max()))
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    std::format("{} exhausted SQLite's integer range", field)
                );
            }
            return value + 1U;
        }

        [[nodiscard]]
        auto unixTimeMilliseconds() -> Result<uint64>
        {
            auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            if (elapsed < 0)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "System wall clock precedes the Unix epoch"
                );
            }
            return static_cast<uint64>(elapsed);
        }

        [[nodiscard]]
        auto expectDone(
            sqlite3* database,
            sqlite3_stmt* statement
        ) -> Status
        {
            if (sqlite3_step(statement) != SQLITE_DONE)
            {
                return databaseFailure(database, "write failed");
            }
            return ok();
        }

        [[nodiscard]]
        auto columnText(
            sqlite3_stmt* statement,
            int index
        ) -> std::string
        {
            auto const* const bytes = sqlite3_column_text(statement, index);
            auto const size = sqlite3_column_bytes(statement, index);
            if (bytes == nullptr || size <= 0)
            {
                return {};
            }
            // SAFETY: sqlite3_column_bytes reports the length of the very
            // buffer sqlite3_column_text returned for the same statement and
            // column, and both stay valid until the next step, reset or
            // finalize. The count arrives beside the pointer rather than within
            // it, so no expression can restate the bound; every read below is
            // bounded by the span this one statement builds.
            UF_UNSAFE_BUFFER_BEGIN
            auto const text = std::span<unsigned char const>{
                bytes,
                static_cast<std::size_t>(size)
            };
            UF_UNSAFE_BUFFER_END
            auto value = std::string{};
            value.reserve(text.size());
            for (auto const byte : text)
            {
                value.push_back(static_cast<char>(byte));
            }
            return value;
        }

        [[nodiscard]]
        auto optionalColumnText(
            sqlite3_stmt* statement,
            int index
        ) -> std::optional<std::string>
        {
            return sqlite3_column_type(statement, index) == SQLITE_NULL
                ? std::nullopt
                : std::optional{columnText(statement, index)};
        }

        // Every hash column in this database holds bare lowercase hex, because
        // that is the form each comparison against ContentHash::hex() needs.
        // Rebuilding a ContentHash from a column therefore has to restore the
        // canonical prefix that ContentHash::parse requires.
        [[nodiscard]]
        auto parseHashColumn(std::string_view columnHex) -> Result<ContentHash>
        {
            return ContentHash::parse(std::format("sha256:{}", columnHex));
        }

        [[nodiscard]]
        auto parseReceiptCounter(
            json::Value const& receipt,
            std::string_view memberName
        ) -> Result<uint64>
        {
            auto const* const p_value = receipt.find(memberName);
            if (
                p_value == nullptr
                || p_value->kind() != json::ValueKind::String
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Stored evidence receipt member '" + std::string{memberName}
                        + "' is not a decimal string"
                );
            }
            auto const text         = p_value->string();
            auto parsed             = uint64{};
            auto const* const begin = std::to_address(text.begin());
            auto const* const end   = std::to_address(text.end());
            auto const converted    = std::from_chars(begin, end, parsed);
            if (
                converted.ec != std::errc{}
                || converted.ptr != end
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Stored evidence receipt member '" + std::string{memberName}
                        + "' is not an exact uint64"
                );
            }
            return parsed;
        }

        [[nodiscard]]
        auto parseReceiptDimension(
            json::Value const& receipt,
            std::string_view memberName
        ) -> Result<uint32>
        {
            auto const* const p_value = receipt.find(memberName);
            if (
                p_value == nullptr
                || !p_value->isInteger()
                || p_value->number() <= 0.0
                || p_value->number()
                    > static_cast<double>(std::numeric_limits<uint32>::max())
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Stored evidence receipt member '" + std::string{memberName}
                        + "' is not a positive uint32"
                );
            }
            return static_cast<uint32>(p_value->number());
        }

        // Counters render as decimal strings for SnapshotObservationReference's
        // reason: RFC 8785 numbers are IEEE-754 doubles, so a byte count or an
        // instant above 2^53 would round inside a durable Tool outcome.
        // parseReceiptCounter above is the exact inverse and reads nothing else.
        [[nodiscard]]
        auto receiptCounter(uint64 value) -> json::Value
        {
            return json::Value::ofString(std::to_string(value));
        }

        // The inverse of evidenceReceiptJson below. The two are the only
        // spelling of a receipt's bytes; a third one in another module is how a
        // stored receipt and an answered receipt come to disagree.
        [[nodiscard]]
        auto parseEvidenceReceipt(json::Value const& value)
            -> Result<EvidenceArtifactReceipt>
        {
            if (value.kind() != json::ValueKind::Object)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Stored evidence receipt is not an object"
                );
            }
            auto const* const p_hash = value.find(k_screenshotSha256Member);
            auto const* const p_media = value.find("media_type");
            auto const* const p_frame = value.find("frame_identity");
            if (
                p_hash == nullptr
                || p_hash->kind() != json::ValueKind::String
                || p_media == nullptr
                || p_media->kind() != json::ValueKind::String
                || p_media->string().empty()
                || p_frame == nullptr
                || p_frame->kind() != json::ValueKind::Object
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Stored evidence receipt is missing its screenshot hash, media type or frame identity"
                );
            }
            UF_TRY_VALUE(
                contentHash,
                ContentHash::parse(std::format("sha256:{}", p_hash->string()))
            );
            UF_TRY_VALUE(byteCount, parseReceiptCounter(value, "byte_count"));
            UF_TRY_VALUE(createdAt, parseReceiptCounter(value, "created_at_unix_ms"));
            UF_TRY_VALUE(width, parseReceiptDimension(value, "width"));
            UF_TRY_VALUE(height, parseReceiptDimension(value, "height"));
            UF_TRY_VALUE(
                captureSession,
                parseReceiptCounter(*p_frame, "capture_session_id")
            );
            UF_TRY_VALUE(
                targetGeneration,
                parseReceiptCounter(*p_frame, "target_generation")
            );
            UF_TRY_VALUE(frameId, parseReceiptCounter(*p_frame, "frame_id"));

            auto rectangle = std::optional<PixelRect>{};
            auto const* const p_rectangle = value.find("rectangle");
            if (p_rectangle != nullptr)
            {
                if (p_rectangle->kind() != json::ValueKind::Object)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "Stored evidence receipt rectangle is not an object"
                    );
                }
                UF_TRY_VALUE(x, parseReceiptCounter(*p_rectangle, "x"));
                UF_TRY_VALUE(y, parseReceiptCounter(*p_rectangle, "y"));
                UF_TRY_VALUE(rectWidth, parseReceiptCounter(*p_rectangle, "width"));
                UF_TRY_VALUE(rectHeight, parseReceiptCounter(*p_rectangle, "height"));
                if (
                    x > std::numeric_limits<uint32>::max()
                    || y > std::numeric_limits<uint32>::max()
                    || rectWidth > std::numeric_limits<uint32>::max()
                    || rectHeight > std::numeric_limits<uint32>::max()
                )
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "Stored evidence receipt rectangle exceeds uint32 geometry"
                    );
                }
                UF_TRY_VALUE(
                    parsedRectangle,
                    PixelRect::create(
                        static_cast<uint32>(x),
                        static_cast<uint32>(y),
                        static_cast<uint32>(rectWidth),
                        static_cast<uint32>(rectHeight)
                    )
                );
                rectangle.emplace(parsedRectangle);
            }
            return EvidenceArtifactReceipt{
                .contentHash = contentHash,
                .byteCount   = byteCount,
                .mediaType           = std::string{p_media->string()},
                .width  = width,
                .height = height,
                .frameIdentity       = FrameIdentity{
                    CaptureSessionId{captureSession},
                    TargetGeneration::fromValue(targetGeneration),
                    FrameId{frameId},
                },
                .rectangle           = rectangle,
                .createdAtUnixMillis = createdAt,
            };
        }

        // Every screenshot receipt this root has committed, in the order the
        // calls that committed them were recorded.
        //
        // The filter is `a confirmed Framework call whose evidence states a
        // receipt` and deliberately names no Tool. provider_kind is the trust
        // boundary and the whole of it: a Project-answered Tool cannot claim to
        // have committed a blob, which is the property the retired IN-list gave
        // by accident because the two Tool names it listed were reserved.
        [[nodiscard]]
        auto evidenceReceipts(sqlite3* database)
            -> Result<std::vector<EvidenceArtifactReceipt>>
        {
            UF_TRY_VALUE(
                query,
                prepare(
                    database,
                    "SELECT history.evidence, history.evidence_hash "
                    "FROM tool_call_history history "
                    "JOIN tool_call_positions position "
                    "ON position.call_identity=history.call_identity "
                    "WHERE history.state='confirmed' "
                    "AND history.evidence IS NOT NULL "
                    "AND position.provider_kind='framework' "
                    "ORDER BY position.rowid"
                )
            );
            auto receipts = std::vector<EvidenceArtifactReceipt>{};
            auto step     = sqlite3_step(query.get());
            while (step == SQLITE_ROW)
            {
                auto const evidenceBytes = columnText(query.get(), 0);
                auto const evidenceHash  = columnText(query.get(), 1);
                UF_TRY_VALUE(evidence, CanonicalJson::parseExact(evidenceBytes));
                if (evidence.contentHash().hex() != evidenceHash)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "Stored Tool call evidence failed its ledger hash"
                    );
                }
                auto const* const p_receipt =
                    evidence.value().find(k_committedScreenshotMember);
                if (p_receipt == nullptr)
                {
                    step = sqlite3_step(query.get());
                    continue;
                }
                UF_TRY_VALUE(receipt, parseEvidenceReceipt(*p_receipt));
                receipts.emplace_back(std::move(receipt));
                step = sqlite3_step(query.get());
            }
            if (step != SQLITE_DONE)
            {
                return databaseFailure(
                    database,
                    "could not scan committed evidence receipts"
                );
            }
            return receipts;
        }

        // No in-class default for the catalog hash: ContentHash has no empty
        // state, and every provider variant supplies one.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
        struct PersistedToolProvider final
        {
            std::string_view           kind{};
            std::optional<ContentHash> projectRegistrationHash{};
            ContentHash                toolCatalogHash;
        };

        struct PersistedToolProviderVisitor final
        {
            auto operator()(FrameworkToolProvider const& provider) const
                -> PersistedToolProvider
            {
                return PersistedToolProvider{
                    .kind                    = "framework",
                    .projectRegistrationHash = std::nullopt,
                    .toolCatalogHash         = provider.toolCatalogHash,
                };
            }

            auto operator()(ProjectToolProvider const& provider) const
                -> PersistedToolProvider
            {
                return PersistedToolProvider{
                    .kind                    = "project",
                    .projectRegistrationHash = provider.projectRegistrationHash,
                    .toolCatalogHash         = provider.toolCatalogHash,
                };
            }
        };

        [[nodiscard]]
        auto persistedToolProvider(ToolProviderIdentity const& provider)
            -> PersistedToolProvider
        {
            return std::visit(PersistedToolProviderVisitor{}, provider);
        }

        // The same rule as a row filter over tool_call_history joined to
        // tool_call_positions, GENERATED from the predicate rather than
        // restated beside it. The (answerer, mutability) pairs are a closed
        // set, so every pair the predicate calls unrecordable becomes one
        // disjunct here and the two cannot disagree.
        [[nodiscard]]
        auto unrecordedEffectDispatchFilter() -> std::string
        {
            constexpr auto mutabilities = std::array{
                ToolMutability::ReadOnly,
                ToolMutability::Mutating,
            };
            auto filter = std::string{};
            for (auto const& answerer : k_toolAnswerers)
            {
                for (auto const mutability : mutabilities)
                {
                    if (
                        !toolCallEffectMayBeUnrecorded(
                            answerer.composition,
                            mutability
                        )
                    )
                    {
                        continue;
                    }
                    if (!filter.empty())
                    {
                        filter += " OR ";
                    }
                    filter += std::format(
                        "(history.mutating={} AND position.provider_kind='{}')",
                        mutability == ToolMutability::Mutating ? 1 : 0,
                        answerer.providerKind
                    );
                }
            }
            return "(" + filter + ")";
        }

        // The stored attributes at one call coordinate, in the order every
        // reader selects them. Per R4 the ordinal coordinate is the whole
        // replay lookup key and each of these is a stored attribute compared
        // field by field at that coordinate, so both readers share one list and
        // report the first field that diverged by name.
        //
        // call_identity is last because it is derived from all the others: any
        // real divergence moves it too, and naming it first would hide the
        // field that actually changed behind its address.
        constexpr auto k_toolCallPositionColumns = std::string_view{
            "run_identity, framework_release_identity, "
            "tool_runtime_protocol_identity, environment_identity, provider_kind, "
            "project_registration_hash, tool_catalog_hash, tool_name, tool_version, "
            "canonical_args, canonical_args_hash, observation_reference_hash, "
            "call_identity"
        };

        struct StoredToolCallAttribute final
        {
            std::string_view           field{};
            std::optional<std::string> expected{};
            std::optional<std::string> actual{};
        };

        [[nodiscard]]
        auto divergedToolCallField(
            sqlite3_stmt* row,
            ToolCallPositionIdentity const& call
        ) -> std::optional<std::string_view>
        {
            auto const provider   = persistedToolProvider(call.provider());
            auto const& execution = call.executionIdentity();
            auto const attributes = std::array{
                StoredToolCallAttribute{
                    "run_identity",
                    execution.runIdentity.hex(),
                    columnText(row, 0),
                },
                StoredToolCallAttribute{
                    "framework_release_identity",
                    execution.frameworkReleaseIdentity.hex(),
                    columnText(row, 1),
                },
                StoredToolCallAttribute{
                    "tool_runtime_protocol_identity",
                    execution.toolRuntimeProtocolIdentity.hex(),
                    columnText(row, 2),
                },
                StoredToolCallAttribute{
                    "environment_identity",
                    execution.environmentIdentity.hex(),
                    columnText(row, 3),
                },
                StoredToolCallAttribute{
                    "provider_kind",
                    std::string{provider.kind},
                    columnText(row, 4),
                },
                StoredToolCallAttribute{
                    "project_registration_hash",
                    provider.projectRegistrationHash.transform(
                        [](ContentHash const& hash) { return hash.hex(); }
                    ),
                    optionalColumnText(row, 5),
                },
                StoredToolCallAttribute{
                    "tool_catalog_hash",
                    provider.toolCatalogHash.hex(),
                    columnText(row, 6),
                },
                StoredToolCallAttribute{
                    "tool_name",
                    call.toolName(),
                    columnText(row, 7),
                },
                StoredToolCallAttribute{
                    "tool_version",
                    call.toolVersion(),
                    columnText(row, 8),
                },
                StoredToolCallAttribute{
                    "canonical_args",
                    call.canonicalArgs(),
                    columnText(row, 9),
                },
                StoredToolCallAttribute{
                    "canonical_args_hash",
                    call.canonicalArgsHash().hex(),
                    columnText(row, 10),
                },
                StoredToolCallAttribute{
                    "observation_reference",
                    call.observationReference().transform(
                        [](ContentHash const& hash) { return hash.hex(); }
                    ),
                    optionalColumnText(row, 11),
                },
                StoredToolCallAttribute{
                    "call_identity",
                    call.identity().hex(),
                    columnText(row, 12),
                },
            };
            auto const diverged = std::ranges::find_if(
                attributes,
                [](StoredToolCallAttribute const& attribute)
                {
                    return attribute.expected != attribute.actual;
                }
            );
            if (diverged == attributes.end())
            {
                return std::nullopt;
            }
            return diverged->field;
        }

        [[nodiscard]]
        auto toolCallCoordinateName(ToolCallPositionIdentity const& call)
            -> std::string
        {
            return std::format(
                "ordinal {} under parent coordinate {}",
                call.sequence(),
                call.parentIdentity().hex()
            );
        }

        [[nodiscard]]
        auto toolCallDivergenceMessage(
            ToolCallPositionIdentity const& call,
            std::string_view field
        ) -> std::string
        {
            return std::format(
                "Tool call at {} diverged from durable history: {} changed",
                toolCallCoordinateName(call),
                field
            );
        }

        [[nodiscard]]
        auto toolCallChangedParentMessage(ToolCallPositionIdentity const& call)
            -> std::string
        {
            return std::format(
                "Tool call at {} arrived under a different parent: this run "
                "recorded no position at that parent coordinate",
                toolCallCoordinateName(call)
            );
        }

        // Which of the two causes a coordinate miss has, decided from a second
        // source rather than from the miss alone.
        //
        // R4 gives a parent coordinate exactly two shapes -- the root request
        // the run's own context is anchored on, and the durable position a
        // handler's context is anchored on -- and admits no third and no absent
        // one. So a miss whose parent IS one of those is an ordinal past that
        // context's recorded frontier, which is a position genuinely absent; a
        // miss whose parent is NEITHER is a call that arrived under a parent
        // this run never had, which section 5.3 calls a divergence. Reporting
        // the second as an absent position sends the reader to the ordinal,
        // which is the one part of the coordinate that did not move.
        [[nodiscard]]
        auto toolCallParentIsRecorded(
            sqlite3* database,
            ToolRootRequestIdentity const& root,
            ToolCallPositionIdentity const& call
        ) -> Result<bool>
        {
            if (call.parentIdentity() == root.identity())
            {
                return true;
            }
            UF_TRY_VALUE(
                query,
                prepare(
                    database,
                    "SELECT 1 FROM tool_call_positions WHERE root_identity=?1 "
                    "AND call_identity=?2"
                )
            );
            UF_TRY(bindText(database, query.get(), 1, root.identity().hex()));
            UF_TRY(bindText(
                database,
                query.get(),
                2,
                call.parentIdentity().hex()
            ));
            auto const step = sqlite3_step(query.get());
            if (step == SQLITE_ROW)
            {
                return true;
            }
            if (step == SQLITE_DONE)
            {
                return false;
            }
            return databaseFailure(database, "could not read Tool call parent");
        }

        // Section 5.3 item 3: a deterministic-replay divergence terminates the
        // RUN, not only the call that noticed it. The mark is the run's own
        // state on its root request row, and the reason is the exact diagnostic
        // the refusal carries, so the row says which divergence stopped it.
        //
        // The first divergence wins. A later one is a consequence of the run
        // already being over rather than a second independent fact, and
        // overwriting the reason would replace the diagnosis a reader needs
        // with the one it caused.
        //
        // Nothing here reads the row back afterwards. Every caller has already
        // matched this root request row or read a durable position that holds a
        // foreign key into it, so the row exists; zero changed rows means the
        // run was already terminated, which is the outcome this asks for.
        [[nodiscard]]
        auto terminateToolRun(
            sqlite3* database,
            std::string_view rootIdentityHex,
            std::string_view reason
        ) -> Status
        {
            UF_TRY_VALUE(
                update,
                prepare(
                    database,
                    "UPDATE tool_root_requests SET state='terminated', "
                    "termination_reason=?2 WHERE root_identity=?1 AND "
                    "state='running'"
                )
            );
            UF_TRY(bindText(database, update.get(), 1, rootIdentityHex));
            UF_TRY(bindText(database, update.get(), 2, reason));
            return expectDone(database, update.get());
        }

        // The gate a terminated run closes. See k_toolRootRequestsDdl for what
        // "terminated" covers and what it deliberately leaves open.
        [[nodiscard]]
        auto requireLiveToolRun(
            sqlite3* database,
            std::string_view rootIdentityHex
        ) -> Status
        {
            UF_TRY_VALUE(
                query,
                prepare(
                    database,
                    "SELECT state, termination_reason FROM tool_root_requests "
                    "WHERE root_identity=?1"
                )
            );
            UF_TRY(bindText(database, query.get(), 1, rootIdentityHex));
            if (sqlite3_step(query.get()) != SQLITE_ROW)
            {
                // A missing row is not a case to classify. Every caller has
                // already matched this root request, and every durable position
                // holds a foreign key into it, so the row is there or the
                // database is damaged.
                return databaseFailure(
                    database,
                    "could not read the Tool run state"
                );
            }
            if (columnText(query.get(), 0) == "running")
            {
                return ok();
            }
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "Tool run {} was stopped by deterministic-replay divergence: {}",
                    rootIdentityHex,
                    columnText(query.get(), 1)
                )
            );
        }

        // The coordinate lookup itself: root, parent position and call sequence,
        // and nothing else. Folding tool name or arguments into this key is
        // what R4 forbids, because a changed argument would then miss the
        // lookup, be indistinguishable from a first new call, and execute.
        [[nodiscard]]
        auto prepareToolCallCoordinateQuery(
            sqlite3* database,
            ToolCallPositionIdentity const& call
        ) -> Result<Statement>
        {
            auto sql = std::string{"SELECT "};
            sql += k_toolCallPositionColumns;
            sql += " FROM tool_call_positions WHERE root_identity=?1 AND "
                   "parent_call_identity=?2 AND call_sequence=?3";
            UF_TRY_VALUE(query, prepare(database, sql));
            UF_TRY(bindText(database, query.get(), 1, call.rootIdentity().hex()));
            UF_TRY(bindText(
                database,
                query.get(),
                2,
                call.parentIdentity().hex()
            ));
            UF_TRY(bindInteger(database, query.get(), 3, call.sequence()));
            return query;
        }

        // The bounds arrive in unforgeable catalog-backed coordinates; their
        // relationship and authority are proved again from durable rows.
        [[nodiscard]]
        auto admittedToolAncestors(
            sqlite3* database,
            ToolAdmissionRequest const& request
        ) -> Result<std::vector<std::string>>
        {
            auto const& call = request.call;
            auto const projectCall = std::holds_alternative<ProjectToolProvider>(
                call.provider()
            );
            if (
                request.ancestors.size() + (projectCall ? 1U : 0U)
                > script::ScopedToolProgram::k_maximumProjectToolDepth
            )
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Project Tool call depth exceeds 16 active handlers"
                );
            }
            auto chain          = std::vector<std::string>{call.identity().hex()};
            auto expectedParent = request.root.identity();
            auto names          = std::set<std::string>{};
            for (auto const& ancestor : request.ancestors)
            {
                auto const& execution = ancestor.executionIdentity();
                auto const& childExecution = call.executionIdentity();
                if (
                    ancestor.rootIdentity() != request.root.identity()
                    || ancestor.parentIdentity() != expectedParent
                    || !std::holds_alternative<ProjectToolProvider>(ancestor.provider())
                    || execution.runIdentity != childExecution.runIdentity
                    || execution.frameworkReleaseIdentity
                        != childExecution.frameworkReleaseIdentity
                    || execution.toolRuntimeProtocolIdentity
                        != childExecution.toolRuntimeProtocolIdentity
                    || execution.environmentIdentity != childExecution.environmentIdentity
                    || !names.insert(ancestor.toolName()).second
                    || (projectCall && ancestor.toolName() == call.toolName())
                )
                {
                    return fail(
                        AutomationErrorKind::ActionRejected,
                        "Tool ancestry is cyclic or differs from its root and execution"
                    );
                }
                UF_TRY_VALUE(position, prepareToolCallCoordinateQuery(database, ancestor));
                if (
                    sqlite3_step(position.get()) != SQLITE_ROW
                    || divergedToolCallField(position.get(), ancestor).has_value()
                )
                {
                    return fail(
                        AutomationErrorKind::ActionRejected,
                        "Tool ancestor does not match its durable coordinate"
                    );
                }
                UF_TRY_VALUE(
                    authority,
                    prepare(
                        database,
                        "SELECT history.state, attempt.session_id, "
                        "attempt.execution_principal_id, attempt.execution_principal_kind, "
                        "attempt.controlled_target_id, attempt.project_registration_hash, "
                        "attempt.policy_hash, attempt.capability_profile_hash "
                        "FROM tool_call_history history JOIN tool_admission_attempts attempt "
                        "ON attempt.call_identity=history.call_identity AND "
                        "attempt.attempt_number=history.active_admission_attempt "
                        "WHERE history.call_identity=?1"
                    )
                );
                UF_TRY(bindText(database, authority.get(), 1, ancestor.identity().hex()));
                if (
                    sqlite3_step(authority.get()) != SQLITE_ROW
                    || columnText(authority.get(), 0) != "dispatching"
                    || columnText(authority.get(), 1) != request.controller.sessionId()
                    || columnText(authority.get(), 2) != request.controller.controllerId()
                    || columnText(authority.get(), 3)
                        != controllerKindWireName(request.controller.kind())
                    || columnText(authority.get(), 4)
                        != request.controller.controlledTargetId()
                    || columnText(authority.get(), 5)
                        != request.policyAuthority.projectRegistrationHash().hex()
                    || columnText(authority.get(), 6)
                        != request.policyAuthority.policyHash().hex()
                    || columnText(authority.get(), 7)
                        != request.controller.capabilityProfileHash().hex()
                )
                {
                    return fail(
                        AutomationErrorKind::ActionRejected,
                        "Tool ancestor must be a dispatching Project handler under the same authority"
                    );
                }
                // Admission/re-entry verifies the CURRENT live lease outside
                // this helper. The historical attempt keeps its original
                // epoch/fence, including after a legitimate restart takeover.
                UF_TRY(childToolWithinBounds(ancestor.descriptor(), call.descriptor()));
                expectedParent = ancestor.identity();
                chain.emplace_back(expectedParent.hex());
            }
            if (call.parentIdentity() != expectedParent)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Tool admission requires the complete durable Project ancestry"
                );
            }
            if (!request.ancestors.empty())
            {
                // Count the persisted tree as well as the VM's current-attempt
                // counter: a terminal Project child can replay without running
                // its own descendants, but cannot refund their durable cost.
                UF_TRY_VALUE(
                    count,
                    prepare(
                        database,
                        "WITH RECURSIVE descendants(call_identity) AS ("
                        "SELECT call_identity FROM tool_call_positions WHERE parent_call_identity=?1 "
                        "UNION SELECT child.call_identity FROM tool_call_positions child "
                        "JOIN descendants ON child.parent_call_identity=descendants.call_identity) "
                        "SELECT COUNT(*) FROM descendants"
                    )
                );
                UF_TRY(bindText(database, count.get(), 1, request.ancestors.front().identity().hex()));
                if (sqlite3_step(count.get()) != SQLITE_ROW)
                {
                    return databaseFailure(database, "could not count the Project Tool call tree");
                }
                if (
                    sqlite3_column_int64(count.get(), 0)
                    > static_cast<int64>(script::ScopedToolProgram::k_maximumProjectToolCalls)
                )
                {
                    return fail(
                        AutomationErrorKind::ActionRejected,
                        "Project Tool durable descendant call ceiling is exhausted"
                    );
                }
            }
            return chain;
        }

        [[nodiscard]]
        auto unresolvedToolDescendants(
            sqlite3* database,
            ContentHash const& callIdentity
        ) -> Result<bool>
        {
            UF_TRY_VALUE(
                query,
                prepare(
                    database,
                    "WITH RECURSIVE descendants(call_identity) AS ("
                    "SELECT call_identity FROM tool_call_positions WHERE parent_call_identity=?1 "
                    "UNION SELECT position.call_identity FROM tool_call_positions position "
                    "JOIN descendants ON position.parent_call_identity=descendants.call_identity) "
                    "SELECT 1 FROM descendants JOIN tool_call_history history "
                    "ON history.call_identity=descendants.call_identity "
                    "WHERE history.state IN ('admitted','dispatching','possible',"
                    "'terminally_unresolved') LIMIT 1"
                )
            );
            UF_TRY(bindText(database, query.get(), 1, callIdentity.hex()));
            auto const step = sqlite3_step(query.get());
            if (step == SQLITE_ROW)
            {
                return true;
            }
            if (step != SQLITE_DONE)
            {
                return databaseFailure(database, "could not inspect unresolved Tool descendants");
            }
            return false;
        }

        [[nodiscard]]
        auto ensureToolCallHistory(
            sqlite3* database,
            ToolCallPositionIdentity const& call
        ) -> Status
        {
            auto const mutating =
                call.descriptor().mutability == ToolMutability::Mutating;
            UF_TRY_VALUE(
                query,
                prepare(
                    database,
                    "SELECT mutating FROM tool_call_history WHERE call_identity=?1"
                )
            );
            UF_TRY(bindText(database, query.get(), 1, call.identity().hex()));
            auto const queryResult = sqlite3_step(query.get());
            if (queryResult == SQLITE_ROW)
            {
                if ((sqlite3_column_int(query.get(), 0) != 0) != mutating)
                {
                    return fail(
                        AutomationErrorKind::ActionRejected,
                        "Stored Tool call mutability disagrees with its catalog"
                    );
                }
                return ok();
            }
            if (queryResult != SQLITE_DONE)
            {
                return databaseFailure(database, "could not read Tool call history");
            }

            UF_TRY_VALUE(
                insert,
                prepare(
                    database,
                    "INSERT INTO tool_call_history(call_identity, mutating, state, "
                    "revision, active_admission_attempt) "
                    "VALUES(?1, ?2, 'proposed', 1, 0)"
                )
            );
            UF_TRY(bindText(database, insert.get(), 1, call.identity().hex()));
            UF_TRY(bindInteger(database, insert.get(), 2, mutating ? 1U : 0U));
            return expectDone(database, insert.get());
        }

        // The durable Tool call states that hold a mutation barrier: the two a
        // call passes through after admission, and the two uncertain terminals
        // that leave the target's world unknown. `proposed` is deliberately
        // absent -- a proposed call holds no admitted authority and section
        // 5.3's recovery is to re-admit it -- and so are the three settled
        // terminals, which have nothing left in flight.
        //
        // One spelling, because two readers ask this question: the per-target
        // barrier every mutating admission crosses, and the quiescence gate a
        // release upgrade's session pin crosses. A second list would let an
        // upgrade land in a state the admission barrier still calls active.
        constexpr auto k_activeToolMutationStates = std::string_view{
            "'admitted','dispatching','possible','terminally_unresolved'"
        };

        // The rows of one mutating call still holding the barrier, joined from
        // history through its position to the run that names the target.
        constexpr auto k_activeToolMutationJoin = std::string_view{
            "FROM tool_call_history history "
            "JOIN tool_call_positions position "
            "ON position.call_identity=history.call_identity "
            "JOIN tool_runs run ON run.root_identity=position.root_identity "
        };

        // excludedCallIdentities is the one live mutation chain the caller is
        // already inside: the call being admitted and every ancestor whose
        // handler is running it. Section 3.3 keeps a child in that one chain
        // rather than starting a second, so a mutating child under a mutating
        // parent is not its own barrier. The span is a call-scoped borrow and
        // nothing here retains it.
        [[nodiscard]]
        auto requireNoActiveToolMutation(
            sqlite3* database,
            std::string_view controlledTargetId,
            std::span<std::string const> excludedCallIdentities
        ) -> Status
        {
            auto sql = std::string{"SELECT history.call_identity, history.state "}
                + std::string{k_activeToolMutationJoin}
                + "WHERE run.controlled_target_id=?1 AND history.mutating=1 "
                  "AND history.state IN ("
                + std::string{k_activeToolMutationStates}
                + ")";
            for (auto index = std::size_t{}; index < excludedCallIdentities.size(); ++index)
            {
                sql += std::format(" AND history.call_identity<>?{}", index + 2U);
            }
            sql += " ORDER BY history.call_identity LIMIT 1";
            UF_TRY_VALUE(query, prepare(database, sql));
            UF_TRY(bindText(database, query.get(), 1, controlledTargetId));
            for (auto index = std::size_t{}; index < excludedCallIdentities.size(); ++index)
            {
                UF_TRY(bindText(
                    database,
                    query.get(),
                    static_cast<int>(index) + 2,
                    excludedCallIdentities[index]
                ));
            }
            auto const step = sqlite3_step(query.get());
            if (step == SQLITE_ROW)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    std::format(
                        "ControlledTarget mutation is frozen by Tool call {} in state {}",
                        columnText(query.get(), 0),
                        columnText(query.get(), 1)
                    )
                );
            }
            if (step != SQLITE_DONE)
            {
                return databaseFailure(
                    database,
                    "could not inspect active Tool mutation barriers"
                );
            }
            return ok();
        }

        struct EvaluatedToolMutation final
        {
            EffectiveEffectEnvelope  envelope;
            std::vector<std::string> requiredApprovals{};
        };

        [[nodiscard]]
        auto evaluateToolMutation(
            VerifiedPolicyArtifact const& policy,
            ToolDescriptor const& descriptor,
            std::string_view toolName,
            std::span<ProposedEffect const> effects,
            std::span<std::string const> controllerCapabilities
        ) -> Result<EvaluatedToolMutation>
        {
            if (effects.empty())
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Mutating Tool admission requires a concrete effect envelope"
                );
            }
            auto proposedEffects = std::vector<ProposedEffect>{
                effects.begin(),
                effects.end(),
            };
            UF_TRY_VALUE(
                envelope,
                deriveEffectiveEffectEnvelope(std::move(proposedEffects))
            );
            for (auto const& effect : envelope.effects)
            {
                UF_TRY(effectWithinBounds(descriptor, effect));
            }
            UF_TRY_VALUE(
                verdict,
                policy.evaluate(PolicyRequest{
                    .effects                = envelope.effects,
                    .controllerCapabilities = controllerCapabilities,
                    .toolName               = toolName,
                })
            );
            return EvaluatedToolMutation{
                .envelope          = std::move(envelope),
                .requiredApprovals = std::move(verdict.requiredApprovals),
            };
        }

        [[nodiscard]]
        auto restoreCanonicalJson(
            std::string bytes,
            std::string_view expectedHash,
            std::string_view field
        ) -> Result<CanonicalJson>
        {
            UF_TRY_VALUE(value, CanonicalJson::parseExact(std::move(bytes)));
            if (value.contentHash().hex() != expectedHash)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format("Stored {} hash does not match its canonical bytes", field)
                );
            }
            return value;
        }

        [[nodiscard]]
        auto requireName(
            std::string_view value,
            std::string_view field
        ) -> Status
        {
            if (value.empty() || !isValidUtf8(value))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format("{} must be non-empty valid UTF-8", field)
                );
            }
            return ok();
        }

        class Transaction final
        {
            sqlite3* m_database;
            bool m_active{true};

            explicit Transaction(sqlite3* database) noexcept
                : m_database{database}
            {
            }

        public:
            Transaction(Transaction&& other) noexcept
                : m_database{other.m_database}
                , m_active{std::exchange(other.m_active, false)}
            {
            }

            Transaction(Transaction const&) = delete;
            auto operator=(Transaction const&) -> Transaction& = delete;
            auto operator=(Transaction&&) -> Transaction& = delete;

            ~Transaction()
            {
                if (m_active)
                {
                    static_cast<void>(sqlite3_exec(
                        m_database,
                        "ROLLBACK",
                        nullptr,
                        nullptr,
                        nullptr
                    ));
                }
            }

            [[nodiscard]]
            static auto begin(sqlite3* database) -> Result<Transaction>
            {
                UF_TRY(execute(database, "BEGIN IMMEDIATE"));
                return Transaction{database};
            }

            [[nodiscard]] auto commit() -> Status
            {
                UF_TRY(execute(m_database, "COMMIT"));
                m_active = false;
                return ok();
            }
        };

        [[nodiscard]]
        auto readDatabaseInteger(
            sqlite3* database,
            std::string_view sql
        ) -> Result<uint64>
        {
            UF_TRY_VALUE(statement, prepare(database, sql));
            if (sqlite3_step(statement.get()) != SQLITE_ROW)
            {
                return databaseFailure(database, "could not read database identity");
            }
            return static_cast<uint64>(sqlite3_column_int64(statement.get(), 0));
        }

        [[nodiscard]]
        auto readDatabaseText(
            sqlite3* database,
            std::string_view sql
        ) -> Result<std::string>
        {
            UF_TRY_VALUE(statement, prepare(database, sql));
            if (sqlite3_step(statement.get()) != SQLITE_ROW)
            {
                return databaseFailure(database, "could not read database schema");
            }
            return columnText(statement.get(), 0);
        }

        // The layout refusal both doors share. open() runs it after creating the
        // directory it names; readInstalledRuntimeArtifact runs it instead of
        // creating one, so the two refuse the same shapes for the same reasons.
        [[nodiscard]]
        auto requirePlainDirectory(
            std::filesystem::path const& directory,
            std::string_view description
        ) -> Status
        {
            auto error        = std::error_code{};
            auto const status = std::filesystem::symlink_status(directory, error);
            if (
                error
                || !std::filesystem::is_directory(status)
                || std::filesystem::is_symlink(status)
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format("{} must be a plain directory", description),
                    error
                );
            }
            return ok();
        }

        // The sole Operator schema identity: sha256 over the canonicalization
        // exactDatabaseSchemaIdentity builds -- every sqlite_schema row ordered
        // by (type, name), each of its four columns written as
        // <byte length>:<value>. It therefore covers the STORED DDL TEXT --
        // reindenting the R"sql(...)" block below changes it even when the
        // schema is identical, and so does adding a comment inside it. Any
        // change to stored DDL below recomputes this value in the same change,
        // from a freshly created database rather than by hand. initialize()
        // verifies immediately after creating the schema, so a forgotten
        // recomputation cannot ship green.
        //
        // PRAGMA user_version has no identity and no upgrade role, so the DDL
        // does not write it. The archived upstream execution checklist's
        // "Delete-on-open has a deadline" section owns
        // the exact-pair migration policy.
        constexpr auto k_operatorDatabaseSchemaIdentity = std::string_view{
            "sha256:181f202e9d7516dff603a006dfabf4fef372a0413710e2bb23ad9cb75dc1bc12"
        };

        // A transition row records the applied exact pair; neither the row nor
        // its insertion order participates in identity or selects a migration.
        constexpr auto k_schemaIdentityTransitionsDdl = std::string_view{
            "CREATE TABLE schema_identity_transitions("
            "source_identity TEXT NOT NULL,"
            "target_identity TEXT NOT NULL,"
            "PRIMARY KEY(source_identity, target_identity)"
            ") STRICT"
        };

        // Which genesis RuntimeArtifact this root's layout named before the
        // framework constant moved, and which one it names now. Written and
        // never read by production code, on exactly the terms
        // schema_identity_transitions is: the audit of a materialisation that
        // was rewritten, not an input to one.
        constexpr auto k_genesisTransitionsDdl = std::string_view{
            "CREATE TABLE genesis_transitions("
            "source_artifact_root_hash TEXT NOT NULL,"
            "target_artifact_root_hash TEXT NOT NULL,"
            "PRIMARY KEY(source_artifact_root_hash, target_artifact_root_hash)"
            ") STRICT"
        };

        // No detail column. Both remaining kinds are complete in their kind and
        // subject identity, so a nullable one would be storage no producer can
        // fill and no reader could tell an absent value from an unwritten one.
        constexpr auto k_ledgerEventsDdl = std::string_view{
            "CREATE TABLE ledger_events("
            "sequence INTEGER PRIMARY KEY AUTOINCREMENT,"
            "session_epoch INTEGER NOT NULL CHECK(session_epoch > 0),"
            "controlled_target_id TEXT NOT NULL,"
            "kind TEXT NOT NULL CHECK(kind IN ("
            "'control_transitioned', 'external_input_detected')),"
            "subject_id TEXT NOT NULL"
            ") STRICT"
        };

        constexpr auto k_sessionPoliciesDdl = std::string_view{
            "CREATE TABLE session_policies("
            "session_id TEXT PRIMARY KEY REFERENCES sessions(session_id),"
            "policy_hash TEXT NOT NULL"
            ") STRICT"
        };

        constexpr auto k_availabilityHeadsDdl = std::string_view{
            "CREATE TABLE availability_heads("
            "controlled_target_id TEXT PRIMARY KEY,"
            "revision INTEGER NOT NULL CHECK(revision > 0),"
            "policy_hash TEXT NOT NULL,"
            "available_tools TEXT NOT NULL"
            ") STRICT"
        };

        constexpr auto k_releaseCapabilityApprovalsDdl = std::string_view{
            "CREATE TABLE release_capability_approvals("
            "artifact_root_hash TEXT NOT NULL,"
            "capability_profile_hash TEXT NOT NULL,"
            "controller_capabilities TEXT NOT NULL,"
            "evidence_hash TEXT NOT NULL,"
            "session_epoch INTEGER NOT NULL CHECK(session_epoch > 0),"
            "PRIMARY KEY(artifact_root_hash, capability_profile_hash)"
            ") STRICT"
        };

        constexpr auto k_runtimeUpgradeFailuresDdl = std::string_view{
            "CREATE TABLE runtime_upgrade_failures("
            "sequence INTEGER PRIMARY KEY AUTOINCREMENT,"
            "attempted_generation INTEGER NOT NULL CHECK(attempted_generation > 0),"
            "attempted_artifact_root_hash TEXT NOT NULL,"
            "restored_generation INTEGER NOT NULL CHECK(restored_generation > 0),"
            "restored_artifact_root_hash TEXT NOT NULL,"
            "reason TEXT NOT NULL"
            ") STRICT"
        };

        // The run's own state lives here rather than on tool_runs, and the
        // reason is which row exists when a divergence is detected. A root
        // request row is written by persistToolRootRequest before any position
        // can name it, and every durable position holds a foreign key into it;
        // a tool_runs row appears only at the first SUCCESSFUL admission. A
        // divergence at a coordinate whose call was never admitted therefore
        // has a root request to terminate and no run record to terminate, so
        // putting the state on tool_runs would need a branch for "no run row
        // yet" and would leave that divergence marking nothing.
        //
        // `terminated` is section 5.3's "terminates the run": the root is
        // closed to new work. No position may be created under it, no call may
        // be admitted at it, and no crashed dispatch may be re-entered at it.
        // Reading durable outcomes is untouched -- replayToolCall still answers
        // and an already-recorded coordinate still rejoins -- and a dispatch
        // that already crossed its boundary may still record what it did,
        // because refusing that would drop the record of an effect the world
        // may already carry.
        constexpr auto k_toolRootRequestsDdl = std::string_view{
            "CREATE TABLE tool_root_requests("
            "root_identity TEXT PRIMARY KEY CHECK(length(root_identity)=64 AND "
            "root_identity NOT GLOB '*[^0-9a-f]*'),"
            "caller_namespace TEXT NOT NULL CHECK(length(CAST(caller_namespace AS BLOB)) "
            "BETWEEN 1 AND 256),"
            "request_key TEXT NOT NULL CHECK(length(CAST(request_key AS BLOB)) "
            "BETWEEN 1 AND 256),"
            "request_preimage TEXT NOT NULL CHECK("
            "length(CAST(request_preimage AS BLOB)) > 0),"
            "request_preimage_hash TEXT NOT NULL CHECK(length(request_preimage_hash)=64 "
            "AND request_preimage_hash NOT GLOB '*[^0-9a-f]*'),"
            "state TEXT NOT NULL CHECK(state IN ('running','terminated')),"
            "termination_reason TEXT,"
            "CHECK((state='running' AND termination_reason IS NULL) OR "
            "(state='terminated' AND termination_reason IS NOT NULL AND "
            "length(CAST(termination_reason AS BLOB)) BETWEEN 1 AND 1024)),"
            "UNIQUE(caller_namespace, request_key)"
            ") STRICT"
        };

        // parent_call_identity has exactly one reading: the identity of the
        // durable coordinate this position hangs from. A run's own calls name
        // the root request, and a handler's children name the handler's
        // position, so there is no null standing in for "the top". SQLite
        // cannot state an alternation foreign key across two tables, and
        // inventing a shadow position row for every root request would be a
        // second spelling of the root, so persistToolCallPosition proves the
        // named coordinate exists -- the root request row it already read, or
        // the parent position row -- with a named refusal on either miss.
        constexpr auto k_toolCallPositionsDdl = std::string_view{
            "CREATE TABLE tool_call_positions("
            "call_identity TEXT PRIMARY KEY CHECK(length(call_identity)=64 AND "
            "call_identity NOT GLOB '*[^0-9a-f]*'),"
            "root_identity TEXT NOT NULL REFERENCES tool_root_requests(root_identity),"
            "parent_call_identity TEXT NOT NULL CHECK("
            "length(parent_call_identity)=64 AND "
            "parent_call_identity NOT GLOB '*[^0-9a-f]*'),"
            "call_sequence INTEGER NOT NULL CHECK(call_sequence BETWEEN 1 AND 4294967295),"
            "run_identity TEXT NOT NULL CHECK(length(run_identity)=64 AND "
            "run_identity NOT GLOB '*[^0-9a-f]*'),"
            "framework_release_identity TEXT NOT NULL CHECK(length(framework_release_identity)=64 "
            "AND framework_release_identity NOT GLOB '*[^0-9a-f]*'),"
            "tool_runtime_protocol_identity TEXT NOT NULL "
            "CHECK(length(tool_runtime_protocol_identity)=64 AND "
            "tool_runtime_protocol_identity NOT GLOB '*[^0-9a-f]*'),"
            "environment_identity TEXT NOT NULL CHECK(length(environment_identity)=64 AND "
            "environment_identity NOT GLOB '*[^0-9a-f]*'),"
            "provider_kind TEXT NOT NULL CHECK(provider_kind IN ('framework','project')),"
            "project_registration_hash TEXT,"
            "tool_catalog_hash TEXT NOT NULL CHECK(length(tool_catalog_hash)=64 AND "
            "tool_catalog_hash NOT GLOB '*[^0-9a-f]*'),"
            "tool_name TEXT NOT NULL CHECK(length(CAST(tool_name AS BLOB)) BETWEEN 1 AND 256),"
            "tool_version TEXT NOT NULL CHECK(length(CAST(tool_version AS BLOB)) BETWEEN 1 AND 256),"
            "canonical_args TEXT NOT NULL CHECK("
            "length(CAST(canonical_args AS BLOB)) > 0),"
            "canonical_args_hash TEXT NOT NULL CHECK(length(canonical_args_hash)=64 AND "
            "canonical_args_hash NOT GLOB '*[^0-9a-f]*'),"
            "observation_reference_hash TEXT CHECK(observation_reference_hash IS NULL "
            "OR (length(observation_reference_hash)=64 AND "
            "observation_reference_hash NOT GLOB '*[^0-9a-f]*')),"
            "CHECK((provider_kind='framework' AND project_registration_hash IS NULL) OR "
            "(provider_kind='project' AND project_registration_hash IS NOT NULL "
            "AND length(project_registration_hash)=64 AND "
            "project_registration_hash NOT GLOB '*[^0-9a-f]*')),"
            "UNIQUE(root_identity, call_identity),"
            "UNIQUE(root_identity, parent_call_identity, call_sequence)"
            ") STRICT"
        };

        constexpr auto k_toolCallHistoryDdl = std::string_view{
            "CREATE TABLE tool_call_history("
            "call_identity TEXT PRIMARY KEY REFERENCES "
            "tool_call_positions(call_identity),"
            "mutating INTEGER NOT NULL CHECK(mutating IN (0,1)),"
            "state TEXT NOT NULL CHECK(state IN ('proposed','admitted','dispatching',"
            "'confirmed','proven_absent','possible','terminal_failure',"
            "'terminally_unresolved')),"
            "revision INTEGER NOT NULL CHECK(revision > 0),"
            "active_admission_attempt INTEGER NOT NULL "
            "CHECK(active_admission_attempt >= 0),"
            "outcome_payload TEXT,"
            "outcome_payload_hash TEXT,"
            "evidence TEXT,"
            "evidence_hash TEXT,"
            "CHECK((evidence IS NULL AND evidence_hash IS NULL) OR "
            "(evidence IS NOT NULL AND evidence_hash IS NOT NULL "
            "AND length(evidence_hash)=64 AND "
            "evidence_hash NOT GLOB '*[^0-9a-f]*')),"
            "CHECK((state='proposed' AND active_admission_attempt=0 "
            "AND outcome_payload IS NULL AND outcome_payload_hash IS NULL "
            "AND evidence IS NULL AND evidence_hash IS NULL) OR "
            "(state IN ('admitted','dispatching') AND active_admission_attempt>0 "
            "AND outcome_payload IS NULL AND outcome_payload_hash IS NULL "
            "AND evidence IS NULL AND evidence_hash IS NULL) OR "
            "(state IN ('confirmed','proven_absent','possible',"
            "'terminal_failure','terminally_unresolved') "
            "AND active_admission_attempt>0 AND outcome_payload IS NOT NULL "
            "AND outcome_payload_hash IS NOT NULL "
            "AND length(outcome_payload_hash)=64 "
            "AND outcome_payload_hash NOT GLOB '*[^0-9a-f]*'))"
            ") STRICT"
        };

        constexpr auto k_toolRunsDdl = std::string_view{
            "CREATE TABLE tool_runs("
            "root_identity TEXT PRIMARY KEY REFERENCES "
            "tool_root_requests(root_identity),"
            "origin_principal_id TEXT NOT NULL,"
            "origin_principal_kind TEXT NOT NULL CHECK(origin_principal_kind IN "
            "('script','agent','human')),"
            "controlled_target_id TEXT NOT NULL,"
            "project_registration_hash TEXT NOT NULL CHECK("
            "length(project_registration_hash)=64 AND "
            "project_registration_hash NOT GLOB '*[^0-9a-f]*'),"
            "run_identity TEXT NOT NULL CHECK(length(run_identity)=64 AND "
            "run_identity NOT GLOB '*[^0-9a-f]*'),"
            "framework_release_identity TEXT NOT NULL CHECK("
            "length(framework_release_identity)=64 AND "
            "framework_release_identity NOT GLOB '*[^0-9a-f]*'),"
            "tool_runtime_protocol_identity TEXT NOT NULL CHECK("
            "length(tool_runtime_protocol_identity)=64 AND "
            "tool_runtime_protocol_identity NOT GLOB '*[^0-9a-f]*'),"
            "environment_identity TEXT NOT NULL CHECK("
            "length(environment_identity)=64 AND "
            "environment_identity NOT GLOB '*[^0-9a-f]*')"
            ") STRICT"
        };

        constexpr auto k_toolAdmissionAttemptsDdl = std::string_view{
            "CREATE TABLE tool_admission_attempts("
            "call_identity TEXT NOT NULL REFERENCES tool_call_history(call_identity),"
            "attempt_number INTEGER NOT NULL CHECK(attempt_number > 0),"
            "root_identity TEXT NOT NULL REFERENCES tool_runs(root_identity),"
            "origin_principal_id TEXT NOT NULL,"
            "origin_principal_kind TEXT NOT NULL CHECK(origin_principal_kind IN "
            "('script','agent','human')),"
            "execution_principal_id TEXT NOT NULL,"
            "execution_principal_kind TEXT NOT NULL CHECK(execution_principal_kind IN "
            "('script','agent','human')),"
            "session_id TEXT NOT NULL,"
            "session_epoch INTEGER NOT NULL CHECK(session_epoch > 0),"
            "controlled_target_id TEXT NOT NULL,"
            "project_registration_hash TEXT NOT NULL CHECK("
            "length(project_registration_hash)=64 AND "
            "project_registration_hash NOT GLOB '*[^0-9a-f]*'),"
            "policy_hash TEXT NOT NULL CHECK(length(policy_hash)=64 AND "
            "policy_hash NOT GLOB '*[^0-9a-f]*'),"
            "capability_profile_hash TEXT NOT NULL CHECK("
            "length(capability_profile_hash)=64 AND "
            "capability_profile_hash NOT GLOB '*[^0-9a-f]*'),"
            "lease_id TEXT NOT NULL,"
            "lease_revision INTEGER NOT NULL CHECK(lease_revision > 0),"
            "fencing_token INTEGER NOT NULL CHECK(fencing_token > 0),"
            "budget_snapshot TEXT NOT NULL,"
            "budget_snapshot_hash TEXT NOT NULL CHECK("
            "length(budget_snapshot_hash)=64 AND "
            "budget_snapshot_hash NOT GLOB '*[^0-9a-f]*'),"
            "effect_envelope TEXT,"
            "effect_envelope_hash TEXT,"
            "required_approvals TEXT,"
            "approval_tokens TEXT,"
            "approval_expires_at_unix_millis INTEGER "
            "CHECK(approval_expires_at_unix_millis IS NULL OR "
            "approval_expires_at_unix_millis > 0),"
            "CHECK((effect_envelope IS NULL AND effect_envelope_hash IS NULL "
            "AND required_approvals IS NULL AND approval_tokens IS NULL "
            "AND approval_expires_at_unix_millis IS NULL) OR "
            "(effect_envelope IS NOT NULL AND effect_envelope_hash IS NOT NULL "
            "AND length(effect_envelope_hash)=64 "
            "AND effect_envelope_hash NOT GLOB '*[^0-9a-f]*' "
            "AND required_approvals IS NOT NULL AND approval_tokens IS NOT NULL)),"
            "PRIMARY KEY(call_identity, attempt_number)"
            ") STRICT"
        };

        constexpr auto k_toolApprovalsDdl = std::string_view{
            "CREATE TABLE tool_approvals("
            "token TEXT PRIMARY KEY,"
            "call_identity TEXT NOT NULL REFERENCES tool_call_history(call_identity),"
            "root_identity TEXT NOT NULL REFERENCES tool_root_requests(root_identity),"
            "session_id TEXT NOT NULL,"
            "controller_id TEXT NOT NULL,"
            "controlled_target_id TEXT NOT NULL,"
            "lease_id TEXT NOT NULL,"
            "lease_revision INTEGER NOT NULL CHECK(lease_revision > 0),"
            "session_epoch INTEGER NOT NULL CHECK(session_epoch > 0),"
            "fencing_token INTEGER NOT NULL CHECK(fencing_token > 0),"
            "project_registration_hash TEXT NOT NULL CHECK("
            "length(project_registration_hash)=64 AND "
            "project_registration_hash NOT GLOB '*[^0-9a-f]*'),"
            "policy_hash TEXT NOT NULL CHECK(length(policy_hash)=64 AND "
            "policy_hash NOT GLOB '*[^0-9a-f]*'),"
            "effect_envelope_hash TEXT NOT NULL CHECK("
            "length(effect_envelope_hash)=64 AND "
            "effect_envelope_hash NOT GLOB '*[^0-9a-f]*'),"
            "approver_principal TEXT NOT NULL,"
            "approver_capability TEXT NOT NULL,"
            "authority_decision_id TEXT NOT NULL UNIQUE,"
            "expires_at_unix_millis INTEGER NOT NULL "
            "CHECK(expires_at_unix_millis > 0),"
            "consumed INTEGER NOT NULL DEFAULT 0 CHECK(consumed IN (0,1)),"
            "consumed_by_attempt INTEGER,"
            "CHECK((consumed=0 AND consumed_by_attempt IS NULL) OR "
            "(consumed=1 AND consumed_by_attempt IS NOT NULL "
            "AND consumed_by_attempt > 0))"
            ") STRICT"
        };

        // Generation-neutral storage retains exact historical manifests while
        // making the identity kind explicit. Format-2 single-source rows remain
        // audit/export data; only format-3 module-manifest rows may execute.
        constexpr auto k_projectRegistrationsDdl = std::string_view{
            "CREATE TABLE project_registrations("
            "registration_hash TEXT PRIMARY KEY,"
            "registration_format INTEGER NOT NULL,"
            "plugin_id TEXT NOT NULL,"
            "plugin_identity_kind TEXT NOT NULL CHECK(plugin_identity_kind IN "
            "('single_source', 'module_manifest')),"
            "plugin_identity_hash TEXT NOT NULL,"
            "canonical_manifest TEXT NOT NULL,"
            "CHECK((registration_format=2 AND plugin_identity_kind='single_source') "
            "OR (registration_format=3 AND plugin_identity_kind='module_manifest'))"
            ") STRICT"
        };

        constexpr auto k_format2ProjectRegistrationsDdl = std::string_view{
            "CREATE TABLE project_registrations("
            "registration_hash TEXT PRIMARY KEY,"
            "plugin_id TEXT NOT NULL,"
            "plugin_hash TEXT NOT NULL,"
            "canonical_manifest TEXT NOT NULL"
            ") STRICT"
        };

        // A ProjectInstance is the fact that this registration is deployed
        // under this key against this target, and nothing more. It names no
        // baseline and carries no state: what a Project's world holds is the
        // Project's own, kept through the durable channel it writes, and a
        // column here would be the framework holding an opinion about it.
        constexpr auto k_projectInstancesDdl = std::string_view{
            "CREATE TABLE project_instances("
            "plugin_id TEXT NOT NULL,"
            "project_instance_key TEXT NOT NULL,"
            "project_registration_hash TEXT NOT NULL "
            "REFERENCES project_registrations(registration_hash),"
            "PRIMARY KEY(plugin_id, project_instance_key),"
            "UNIQUE(project_registration_hash, project_instance_key)"
            ") STRICT"
        };

        // One row per distinct reading the Operator composed for a
        // ProjectInstance. Its revision is its own line: it advances when any
        // input the observation fingerprint covers moved and stays put when
        // the same world is observed again, so a snapshot can name a reading
        // rather than an occasion.
        //
        // It is a named constant rather than a member of the schema bundle
        // because the migration that drops the ProjectState columns rebuilds
        // the table from this exact text; SQLite strips only leading and
        // trailing whitespace, so the column indentation below is part of
        // schema identity and not formatting.
        constexpr auto k_projectObservationsDdl = std::string_view{
            R"sql(
                    CREATE TABLE IF NOT EXISTS project_observations(
                        plugin_id TEXT NOT NULL,
                        project_instance_key TEXT NOT NULL,
                        revision INTEGER NOT NULL CHECK(revision > 0),
                        project_registration_hash TEXT NOT NULL
                            REFERENCES project_registrations(registration_hash),
                        state_resolution_hash TEXT NOT NULL,
                        canonical_observation TEXT NOT NULL,
                        observation_hash TEXT NOT NULL,
                        FOREIGN KEY(plugin_id, project_instance_key)
                            REFERENCES project_instances(plugin_id, project_instance_key),
                        PRIMARY KEY(plugin_id, project_instance_key, revision)
                    ) STRICT
                )sql"
        };

        // canonical_parts is the exact SnapshotParts JCS and is the only thing
        // identity_hash and decision_basis_hash are recomputable from, which is
        // what lets a test falsify the derivation. The scalar columns below it
        // are not a second spelling of the same fact: they are the join keys,
        // and SQL cannot join through JSON text. One test asserts each scalar
        // equals its member in canonical_parts.
        //
        // A named constant for the reason project_observations is one: the
        // migration that drops the ProjectState revision rebuilds this table
        // from this exact text.
        constexpr auto k_snapshotsDdl = std::string_view{
            R"sql(
                    CREATE TABLE IF NOT EXISTS snapshots(
                        -- token and snapshot_revision are deliberately outside
                        -- canonical_parts: they name the stored row rather than
                        -- the capture. observation_id remains inside and names
                        -- the capture, so recapturing an identical world moves
                        -- identity_hash while decision_basis_hash stays stable.
                        token TEXT PRIMARY KEY,
                        session_id TEXT NOT NULL REFERENCES sessions(session_id),
                        snapshot_revision INTEGER NOT NULL
                            CHECK(snapshot_revision > 0),
                        session_epoch INTEGER NOT NULL CHECK(session_epoch > 0),
                        identity_hash TEXT NOT NULL,
                        decision_basis_hash TEXT NOT NULL,
                        canonical_parts TEXT NOT NULL,
                        lease_revision INTEGER NOT NULL CHECK(lease_revision > 0),
                        plugin_id TEXT NOT NULL,
                        project_instance_key TEXT NOT NULL,
                        observation_id TEXT NOT NULL,
                        target_generation INTEGER NOT NULL
                            CHECK(target_generation > 0),
                        state_resolution_hash TEXT NOT NULL,
                        project_observation_revision INTEGER NOT NULL
                            CHECK(project_observation_revision > 0),
                        availability_revision INTEGER NOT NULL
                            CHECK(availability_revision >= 0),
                        UNIQUE(session_id, snapshot_revision),
                        FOREIGN KEY(plugin_id, project_instance_key,
                                    project_observation_revision)
                            REFERENCES project_observations(
                                plugin_id, project_instance_key, revision
                            )
                    ) STRICT
                )sql"
        };

        // Which RuntimeArtifact each generation of this root holds. Generation
        // 0 is the genesis generation: every Operator root materialises it as
        // part of its layout, the same way it materialises the staging
        // directory and the empty database, so the row is there before any
        // installation happens and the CHECK admits it.
        //
        // It sits in its own constant rather than inside the layout block that
        // creates runtime_artifacts and runtime_state, because
        // admitTheGenesisGeneration rebuilds the table and the two paths must
        // store byte-identical DDL. That is also why the closing )sql" sits on
        // the same line as ) STRICT: a newline there would be stored by the
        // standalone path and not by the block, and schema identity is the
        // stored text.
        constexpr auto k_runtimeInstallationsDdl = std::string_view{
            R"sql(CREATE TABLE IF NOT EXISTS runtime_installations(
                        installed_generation INTEGER PRIMARY KEY
                            CHECK(installed_generation >= 0),
                        artifact_root_hash TEXT NOT NULL
                            REFERENCES runtime_artifacts(artifact_root_hash),
                        UNIQUE(installed_generation, artifact_root_hash)
                    ) STRICT)sql"
        };

        // The session table and its one partial index, as the schema bundle
        // above also stores them. The migration that adds the world-scope
        // columns rebuilds the table from this exact text; SQLite strips only
        // leading and trailing whitespace, so the column indentation below is
        // part of schema identity and not formatting.
        constexpr auto k_sessionsDdl = std::string_view{
            R"sql(
                    CREATE TABLE IF NOT EXISTS sessions(
                        session_id TEXT PRIMARY KEY,
                        authenticated_controller_id TEXT NOT NULL,
                        idempotency_namespace TEXT NOT NULL,
                        manifest_hash TEXT NOT NULL,
                        runtime_artifact_root_hash TEXT NOT NULL,
                        installed_generation INTEGER NOT NULL
                            CHECK(installed_generation >= 0),
                        project_registration_hash TEXT NOT NULL
                            REFERENCES project_registrations(registration_hash),
                        -- The capability set this session holds, as the exact
                        -- JCS array capability_profile_hash is the sha256 of.
                        -- The hash alone was a caller field with no content
                        -- behind it, so a policy rule naming a required
                        -- capability had nothing to be judged against.
                        controller_capabilities TEXT NOT NULL,
                        capability_profile_hash TEXT NOT NULL,
                        session_epoch INTEGER NOT NULL CHECK(session_epoch > 0),
                        controlled_target_id TEXT NOT NULL,
                        project_instance_key TEXT NOT NULL,
                        mode TEXT NOT NULL CHECK(mode IN ('read', 'write')),
                        -- Which of the three operators holds this session. It
                        -- is part of the immutable pinned tuple, so a
                        -- controller cannot become another kind between two
                        -- commands, and bindController reads it here rather
                        -- than accepting it.
                        controller_kind TEXT NOT NULL
                            CHECK(controller_kind IN ('script', 'agent', 'human')),
                        -- The observed-instance world this session observes in,
                        -- stored under the same three columns
                        -- observed_instance_bindings carry so a snapshot can
                        -- rebuild the scope that minted its observation
                        -- without a second spelling. It is part of the
                        -- immutable pinned tuple.
                        world_scope_kind TEXT NOT NULL
                            CHECK(world_scope_kind IN ('account', 'run')),
                        world_scope_id TEXT NOT NULL,
                        world_scope_generation TEXT NOT NULL
                            CHECK(
                                length(world_scope_generation) > 0
                                AND world_scope_generation NOT GLOB '*[^0-9]*'
                                AND (
                                    world_scope_kind = 'account'
                                    OR world_scope_generation != '0'
                                )
                            ),
                        active INTEGER NOT NULL CHECK(active IN (0, 1)),
                        FOREIGN KEY(project_registration_hash, project_instance_key)
                            REFERENCES project_instances(
                                project_registration_hash,
                                project_instance_key
                            ),
                        FOREIGN KEY(installed_generation, runtime_artifact_root_hash)
                            REFERENCES runtime_installations(
                                installed_generation,
                                artifact_root_hash
                            )
                    ) STRICT;
            )sql"
        };

        // The mint binding and its three immutability triggers, as the schema
        // bundle also stores them. local_ref is the model target the instance
        // was observed at -- the name the proposal's local_ref carried at mint
        // -- and the migration that adds the column backfills rows minted
        // before it with the empty sentinel, which a Tool-call input delivery
        // refuses, so a migrated binding can never be resolved to a target it
        // was never observed at.
        constexpr auto k_observedInstanceBindingsDdl = std::string_view{
            R"sql(
                    -- The bidirectional Operator-private mint binding. It is
                    -- independent of scope lifetime: no scope row owns it and
                    -- no cascade can remove it. This schema implements no
                    -- reference-expiry proof, so cleanup is forbidden rather
                    -- than guessing whether an Operation, backup or
                    -- audit record still resolves through the binding.
                    CREATE TABLE IF NOT EXISTS observed_instance_bindings(
                        canonical_authority TEXT PRIMARY KEY,
                        observed_instance_id TEXT NOT NULL UNIQUE
                            CHECK(
                                length(observed_instance_id) = 68
                                AND substr(observed_instance_id, 1, 4) = 'oi1_'
                                AND substr(observed_instance_id, 5)
                                    NOT GLOB '*[^0-9a-f]*'
                            ),
                        plugin_id TEXT NOT NULL,
                        project_registration_hash TEXT NOT NULL,
                        project_instance_key TEXT NOT NULL,
                        world_scope_kind TEXT NOT NULL
                            CHECK(world_scope_kind IN ('account', 'run')),
                        world_scope_id TEXT NOT NULL,
                        world_scope_generation TEXT NOT NULL
                            CHECK(
                                length(world_scope_generation) > 0
                                AND world_scope_generation NOT GLOB '*[^0-9]*'
                                AND (
                                    world_scope_kind = 'account'
                                    OR world_scope_generation != '0'
                                )
                            ),
                        -- The model target the instance was observed at, as
                        -- the proposal named it. It is part of the binding,
                        -- not the world scope, because it answers "which
                        -- target did this instance name" and the deliver path
                        -- compares it with the receipt's own target.
                        local_ref TEXT NOT NULL,
                        FOREIGN KEY(plugin_id, project_instance_key)
                            REFERENCES project_instances(plugin_id, project_instance_key),
                        FOREIGN KEY(project_registration_hash, project_instance_key)
                            REFERENCES project_instances(
                                project_registration_hash,
                                project_instance_key
                            )
                    ) STRICT;

                    CREATE TRIGGER forbid_observed_instance_binding_replacement
                    BEFORE INSERT ON observed_instance_bindings
                    WHEN EXISTS(
                        SELECT 1 FROM observed_instance_bindings
                        WHERE canonical_authority = new.canonical_authority
                            OR observed_instance_id = new.observed_instance_id
                    )
                    BEGIN
                        SELECT RAISE(
                            ABORT,
                            'observed instance bindings are immutable'
                        );
                    END;

                    CREATE TRIGGER forbid_observed_instance_binding_mutation
                    BEFORE UPDATE ON observed_instance_bindings
                    BEGIN
                        SELECT RAISE(
                            ABORT,
                            'observed instance bindings are immutable'
                        );
                    END;

                    CREATE TRIGGER forbid_observed_instance_binding_cleanup
                    BEFORE DELETE ON observed_instance_bindings
                    BEGIN
                        SELECT RAISE(
                            ABORT,
                            'observed instance binding cleanup requires a reference-expiry proof'
                        );
                    END;
            )sql"
        };

        constexpr auto k_oneActiveWriteSessionIndexDdl = std::string_view{
            R"sql(
                    CREATE UNIQUE INDEX IF NOT EXISTS one_active_write_session_per_instance
                    ON sessions(project_registration_hash, project_instance_key)
                    WHERE mode='write' AND active=1;
            )sql"
        };

        // Retention is automatic at the write that creates each row. Snapshot
        // and observation rows that an Operation still names are audit input
        // and remain until that Operation has its own retention ruling; the
        // unclaimed history around them is bounded here.
        constexpr auto k_retainedLedgerEvents       = uint64{128};
        constexpr auto k_retainedSnapshotHeads      = uint64{32};
        constexpr auto k_retainedObservationHeads   = uint64{32};

        [[nodiscard]]
        auto exactDatabaseSchemaIdentity(sqlite3* database) -> Result<ContentHash>
        {
            UF_TRY_VALUE(
                query,
                prepare(
                    database,
                    "SELECT type, name, tbl_name, coalesce(sql, '') FROM sqlite_schema "
                    "WHERE name NOT LIKE 'sqlite_%' ORDER BY type, name"
                )
            );
            auto canonical = std::string{};
            auto step      = sqlite3_step(query.get());
            while (step == SQLITE_ROW)
            {
                for (auto column = 0; column < 4; ++column)
                {
                    auto const value = columnText(query.get(), column);
                    canonical += std::to_string(value.size());
                    canonical.push_back(':');
                    canonical += value;
                }
                step = sqlite3_step(query.get());
            }
            if (step != SQLITE_DONE)
            {
                return databaseFailure(database, "could not fingerprint exact schema");
            }
            return sha256(std::as_bytes(std::span{canonical}));
        }

        [[nodiscard]]
        auto verifyExactDatabaseSchema(
            sqlite3* database,
            std::string_view expectedIdentity
        ) -> Status
        {
            UF_TRY_VALUE(actual, exactDatabaseSchemaIdentity(database));
            UF_TRY_VALUE(
                expected,
                ContentHash::parse(expectedIdentity)
            );
            if (actual != expected)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "Operator database schema identity sha256:{} does not match {}",
                        actual.hex(),
                        expectedIdentity
                    )
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto verifyExactDatabaseSchema(sqlite3* database) -> Status
        {
            return verifyExactDatabaseSchema(
                database,
                k_operatorDatabaseSchemaIdentity
            );
        }

        struct SchemaMigration final
        {
            std::string_view sourceIdentity{};
            std::string_view targetIdentity{};
            auto (*apply)(sqlite3*, SchemaMigration const&) -> Status{};
        };

        [[nodiscard]]
        auto recordSchemaIdentityTransition(
            sqlite3* database,
            SchemaMigration const& migration
        ) -> Status
        {
            UF_TRY_VALUE(
                insert,
                prepare(
                    database,
                    "INSERT INTO schema_identity_transitions("
                    "source_identity, target_identity) VALUES(?1, ?2)"
                )
            );
            UF_TRY(bindText(database, insert.get(), 1, migration.sourceIdentity));
            UF_TRY(bindText(database, insert.get(), 2, migration.targetIdentity));
            return expectDone(database, insert.get());
        }

        // The audit table for a genesis materialisation that moved. It is the
        // newest step, so it runs last in every registered chain below and a
        // fresh database creates it beside the rest of the layout.
        [[nodiscard]]
        auto addGenesisTransitions(sqlite3* database) -> Status
        {
            return execute(database, k_genesisTransitionsDdl);
        }

        [[nodiscard]]
        auto addReleaseUpgradeEvidenceTables(sqlite3* database) -> Status
        {
            UF_TRY(execute(database, k_releaseCapabilityApprovalsDdl));
            return execute(database, k_runtimeUpgradeFailuresDdl);
        }

        [[nodiscard]]
        auto addToolRuntimePersistence(sqlite3* database) -> Status
        {
            UF_TRY(execute(database, k_toolCallHistoryDdl));
            UF_TRY(execute(database, k_toolRunsDdl));
            UF_TRY(execute(database, k_toolAdmissionAttemptsDdl));
            return execute(database, k_toolApprovalsDdl);
        }

        [[nodiscard]]
        auto addToolIdentityPersistence(sqlite3* database) -> Status
        {
            UF_TRY(execute(database, k_toolRootRequestsDdl));
            UF_TRY(execute(database, k_toolCallPositionsDdl));
            return addToolRuntimePersistence(database);
        }

        // The generation in which a run carries its own state, so that section
        // 5.3's "a divergence terminates the run" is a durable fact rather than
        // a refusal one caller saw.
        //
        // The table is rebuilt rather than altered because tool_call_positions
        // and tool_runs reference it: SQLite can neither ADD COLUMN with the
        // NOT NULL CHECK this schema's identity is taken from, nor rename the
        // table without rewriting those references.
        //
        // Every carried row becomes `running`. A database written before this
        // column existed recorded no termination because nothing could write
        // one, so `running` is what those rows always said rather than a
        // sentinel standing in for an unknown value; a run that did diverge in
        // such a database diverges again at the same coordinate, and that is
        // what marks it.
        [[nodiscard]]
        auto addToolRunTerminationState(sqlite3* database) -> Status
        {
            UF_TRY(execute(database, "PRAGMA defer_foreign_keys=ON"));
            UF_TRY(execute(
                database,
                "CREATE TABLE prior_tool_root_requests("
                "root_identity TEXT PRIMARY KEY,"
                "caller_namespace TEXT NOT NULL,"
                "request_key TEXT NOT NULL,"
                "request_preimage TEXT NOT NULL,"
                "request_preimage_hash TEXT NOT NULL"
                ") STRICT"
            ));
            UF_TRY(execute(
                database,
                "INSERT INTO prior_tool_root_requests SELECT root_identity, "
                "caller_namespace, request_key, request_preimage, "
                "request_preimage_hash FROM tool_root_requests"
            ));
            UF_TRY(execute(database, "DROP TABLE tool_root_requests"));
            UF_TRY(execute(database, k_toolRootRequestsDdl));
            UF_TRY(execute(
                database,
                "INSERT INTO tool_root_requests(root_identity, caller_namespace, "
                "request_key, request_preimage, request_preimage_hash, state, "
                "termination_reason) SELECT root_identity, caller_namespace, "
                "request_key, request_preimage, request_preimage_hash, "
                "'running', NULL FROM prior_tool_root_requests"
            ));
            return execute(database, "DROP TABLE prior_tool_root_requests");
        }

        // The generation in which `rejected` left the durable Tool call
        // vocabulary. Every admission refusal returns before the state UPDATE
        // and no completion or reconciliation kind maps to it, so no row can
        // carry it -- and the rebuilt table's own CHECK is what would refuse
        // one that somehow did, rather than a guard beside this copy that
        // nothing could trigger.
        //
        // It is rebuilt rather than altered for the reason above: several
        // tables hold a foreign key into it, and a CHECK constraint is part of
        // the stored DDL this schema's identity is taken from.
        [[nodiscard]]
        auto dropRejectedToolCallState(sqlite3* database) -> Status
        {
            UF_TRY(execute(database, "PRAGMA defer_foreign_keys=ON"));
            UF_TRY(execute(
                database,
                "CREATE TABLE prior_tool_call_history("
                "call_identity TEXT PRIMARY KEY,"
                "mutating INTEGER NOT NULL,"
                "state TEXT NOT NULL,"
                "revision INTEGER NOT NULL,"
                "active_admission_attempt INTEGER NOT NULL,"
                "outcome_payload TEXT,"
                "outcome_payload_hash TEXT,"
                "evidence TEXT,"
                "evidence_hash TEXT"
                ") STRICT"
            ));
            UF_TRY(execute(
                database,
                "INSERT INTO prior_tool_call_history SELECT call_identity, "
                "mutating, state, revision, active_admission_attempt, "
                "outcome_payload, outcome_payload_hash, evidence, evidence_hash "
                "FROM tool_call_history"
            ));
            UF_TRY(execute(database, "DROP TABLE tool_call_history"));
            UF_TRY(execute(database, k_toolCallHistoryDdl));
            UF_TRY(execute(
                database,
                "INSERT INTO tool_call_history(call_identity, mutating, state, "
                "revision, active_admission_attempt, outcome_payload, "
                "outcome_payload_hash, evidence, evidence_hash) SELECT "
                "call_identity, mutating, state, revision, "
                "active_admission_attempt, outcome_payload, outcome_payload_hash, "
                "evidence, evidence_hash FROM prior_tool_call_history"
            ));
            return execute(database, "DROP TABLE prior_tool_call_history");
        }

        // Rebuilds tool_call_positions into the exact current DDL, carrying
        // every row through a prior table.
        //
        // It is rebuilt rather than altered because tool_call_history
        // references it, so SQLite cannot ADD COLUMN or change a column's
        // nullability while preserving the exact final DDL this schema's
        // identity is taken from.
        //
        // observationSource names the expression the observation column is
        // taken from: the literal NULL for a generation that predates the
        // column, and the column itself once it existed. That is exact rather
        // than lossy -- no call recorded before the column existed consumed an
        // observation, because nothing could present one.
        //
        // parent_call_identity is backfilled with root_identity wherever an
        // earlier generation left it null. Every such row was a call the run's
        // own context issued, and that context is now anchored on the root
        // request itself, so the root identity is the coordinate those rows
        // always denoted rather than a sentinel standing in for an unknown one.
        [[nodiscard]]
        auto rebuildToolCallPositions(
            sqlite3* database,
            std::string_view observationSource
        ) -> Status
        {
            UF_TRY(execute(database, "PRAGMA defer_foreign_keys=ON"));
            UF_TRY(execute(
                database,
                "CREATE TABLE prior_tool_call_positions("
                "call_identity TEXT PRIMARY KEY,"
                "root_identity TEXT NOT NULL,"
                "parent_call_identity TEXT,"
                "call_sequence INTEGER NOT NULL,"
                "run_identity TEXT NOT NULL,"
                "framework_release_identity TEXT NOT NULL,"
                "tool_runtime_protocol_identity TEXT NOT NULL,"
                "environment_identity TEXT NOT NULL,"
                "provider_kind TEXT NOT NULL,"
                "project_registration_hash TEXT,"
                "tool_catalog_hash TEXT NOT NULL,"
                "tool_name TEXT NOT NULL,"
                "tool_version TEXT NOT NULL,"
                "canonical_args TEXT NOT NULL,"
                "canonical_args_hash TEXT NOT NULL,"
                "observation_reference_hash TEXT"
                ") STRICT"
            ));
            UF_TRY(execute(
                database,
                "INSERT INTO prior_tool_call_positions SELECT call_identity, "
                "root_identity, parent_call_identity, call_sequence, run_identity, "
                "framework_release_identity, tool_runtime_protocol_identity, "
                "environment_identity, provider_kind, project_registration_hash, "
                "tool_catalog_hash, tool_name, tool_version, canonical_args, "
                "canonical_args_hash, "
                + std::string{observationSource}
                + " FROM tool_call_positions"
            ));
            UF_TRY(execute(database, "DROP TABLE tool_call_positions"));
            UF_TRY(execute(database, k_toolCallPositionsDdl));
            UF_TRY(execute(
                database,
                "INSERT INTO tool_call_positions(call_identity, root_identity, "
                "parent_call_identity, call_sequence, run_identity, "
                "framework_release_identity, tool_runtime_protocol_identity, "
                "environment_identity, provider_kind, project_registration_hash, "
                "tool_catalog_hash, tool_name, tool_version, canonical_args, "
                "canonical_args_hash, observation_reference_hash) SELECT "
                "call_identity, root_identity, "
                "coalesce(parent_call_identity, root_identity), call_sequence, "
                "run_identity, framework_release_identity, "
                "tool_runtime_protocol_identity, environment_identity, provider_kind, "
                "project_registration_hash, tool_catalog_hash, tool_name, "
                "tool_version, canonical_args, canonical_args_hash, "
                "observation_reference_hash FROM prior_tool_call_positions"
            ));
            return execute(database, "DROP TABLE prior_tool_call_positions");
        }

        // The Z6 leaf cut. Old exact-pair migrations may arrive before or
        // after nested-call storage existed, so the column probe decides
        // whether an admission-table rebuild is required. Either way the
        // obsolete grant table is absent when this returns.
        [[nodiscard]]
        auto removeNestedToolCallSchema(sqlite3* database) -> Status
        {
            UF_TRY_VALUE(
                columns,
                prepare(database, "PRAGMA table_info(tool_admission_attempts)")
            );
            auto carriesDelegation = false;
            for (;;)
            {
                auto const step = sqlite3_step(columns.get());
                if (step == SQLITE_DONE)
                {
                    break;
                }
                if (step != SQLITE_ROW)
                {
                    return databaseFailure(
                        database,
                        "could not inspect Tool admission columns"
                    );
                }
                carriesDelegation = carriesDelegation
                    || columnText(columns.get(), 1) == "delegation_grant_id";
            }
            if (carriesDelegation)
            {
                UF_TRY(execute(database, "PRAGMA defer_foreign_keys=ON"));
                UF_TRY(execute(
                    database,
                    "ALTER TABLE tool_admission_attempts RENAME TO "
                    "prior_tool_admission_attempts"
                ));
                UF_TRY(execute(database, k_toolAdmissionAttemptsDdl));
                UF_TRY(execute(
                    database,
                    "INSERT INTO tool_admission_attempts(call_identity, "
                    "attempt_number, root_identity, origin_principal_id, "
                    "origin_principal_kind, execution_principal_id, "
                    "execution_principal_kind, session_id, session_epoch, "
                    "controlled_target_id, project_registration_hash, "
                    "policy_hash, capability_profile_hash, lease_id, "
                    "lease_revision, fencing_token, budget_snapshot, "
                    "budget_snapshot_hash, effect_envelope, "
                    "effect_envelope_hash, required_approvals, approval_tokens, "
                    "approval_expires_at_unix_millis) SELECT call_identity, "
                    "attempt_number, root_identity, origin_principal_id, "
                    "origin_principal_kind, execution_principal_id, "
                    "execution_principal_kind, session_id, session_epoch, "
                    "controlled_target_id, project_registration_hash, "
                    "policy_hash, capability_profile_hash, lease_id, "
                    "lease_revision, fencing_token, budget_snapshot, "
                    "budget_snapshot_hash, effect_envelope, "
                    "effect_envelope_hash, required_approvals, approval_tokens, "
                    "approval_expires_at_unix_millis FROM "
                    "prior_tool_admission_attempts"
                ));
                UF_TRY(execute(
                    database,
                    "DROP TABLE prior_tool_admission_attempts"
                ));
            }
            return execute(database, "DROP TABLE IF EXISTS tool_delegation_grants");
        }

        // project_state_schema_hash was a second copy of a member the same
        // row's canonical_manifest already carries, and both readers compared
        // it inside the same disjunction as a full canonical-bytes comparison.
        // The rebuild drops the column and keeps every registration: the bytes
        // the column copied are still there, still compared.
        [[nodiscard]]
        auto dropRegistrationStateSchemaHash(sqlite3* database) -> Status
        {
            UF_TRY(execute(database, "PRAGMA defer_foreign_keys=ON"));
            UF_TRY(execute(
                database,
                "CREATE TABLE prior_project_registrations("
                "registration_hash TEXT PRIMARY KEY,"
                "plugin_id TEXT NOT NULL,"
                "plugin_hash TEXT NOT NULL,"
                "canonical_manifest TEXT NOT NULL) STRICT"
            ));
            UF_TRY(execute(
                database,
                "INSERT INTO prior_project_registrations SELECT registration_hash, "
                "plugin_id, plugin_hash, canonical_manifest FROM project_registrations"
            ));
            UF_TRY(execute(database, "DROP TABLE project_registrations"));
            UF_TRY(execute(database, k_format2ProjectRegistrationsDdl));
            UF_TRY(execute(
                database,
                "INSERT INTO project_registrations SELECT registration_hash, "
                "plugin_id, plugin_hash, canonical_manifest "
                "FROM prior_project_registrations"
            ));
            return execute(database, "DROP TABLE prior_project_registrations");
        }

        // The exact format-2 -> generation-neutral transition. Registration
        // hashes and canonical bytes are copied byte-for-byte; the former
        // plugin_hash becomes the explicitly historical single_source identity.
        [[nodiscard]]
        auto makeRegistrationIdentityGenerationNeutral(sqlite3* database) -> Status
        {
            UF_TRY(execute(database, "PRAGMA defer_foreign_keys=ON"));
            UF_TRY(execute(
                database,
                "CREATE TABLE prior_project_registrations("
                "registration_hash TEXT PRIMARY KEY,"
                "plugin_id TEXT NOT NULL,"
                "plugin_hash TEXT NOT NULL,"
                "canonical_manifest TEXT NOT NULL) STRICT"
            ));
            UF_TRY(execute(
                database,
                "INSERT INTO prior_project_registrations SELECT registration_hash, "
                "plugin_id, plugin_hash, canonical_manifest FROM project_registrations"
            ));
            UF_TRY(execute(database, "DROP TABLE project_registrations"));
            UF_TRY(execute(database, k_projectRegistrationsDdl));
            UF_TRY(execute(
                database,
                "INSERT INTO project_registrations("
                "registration_hash, registration_format, plugin_id, "
                "plugin_identity_kind, plugin_identity_hash, canonical_manifest) "
                "SELECT registration_hash, 2, plugin_id, 'single_source', "
                "plugin_hash, canonical_manifest FROM prior_project_registrations"
            ));
            return execute(database, "DROP TABLE prior_project_registrations");
        }

        // The three world-scope columns are NOT NULL with no default, so
        // SQLite cannot ADD COLUMN them in place: the table is rebuilt from
        // its exact final DDL. Pre-scope sessions (pinned before the columns
        // existed) cannot claim a world scope -- the U2b/U2c ruling forbids
        // inferring one -- so their rows are backfilled with the
        // empty-account sentinel, which passes the column CHECKs and is
        // refused by restoreSessionWorldScope. Such a session can no longer
        // observe; every other row keeps its bytes.
        [[nodiscard]]
        auto addSessionWorldScopeColumns(sqlite3* database) -> Status
        {
            UF_TRY(execute(database, "PRAGMA defer_foreign_keys=ON"));
            UF_TRY(execute(
                database,
                "CREATE TABLE prior_sessions("
                "session_id TEXT PRIMARY KEY,"
                "authenticated_controller_id TEXT NOT NULL,"
                "idempotency_namespace TEXT NOT NULL,"
                "manifest_hash TEXT NOT NULL,"
                "runtime_artifact_root_hash TEXT NOT NULL,"
                "installed_generation INTEGER NOT NULL,"
                "project_registration_hash TEXT NOT NULL,"
                "controller_capabilities TEXT NOT NULL,"
                "capability_profile_hash TEXT NOT NULL,"
                "session_epoch INTEGER NOT NULL,"
                "controlled_target_id TEXT NOT NULL,"
                "project_instance_key TEXT NOT NULL,"
                "mode TEXT NOT NULL,"
                "controller_kind TEXT NOT NULL,"
                "active INTEGER NOT NULL"
                ") STRICT"
            ));
            UF_TRY(execute(
                database,
                "INSERT INTO prior_sessions(session_id, authenticated_controller_id, "
                "idempotency_namespace, manifest_hash, runtime_artifact_root_hash, "
                "installed_generation, project_registration_hash, "
                "controller_capabilities, capability_profile_hash, session_epoch, "
                "controlled_target_id, project_instance_key, mode, controller_kind, "
                "active) SELECT session_id, authenticated_controller_id, "
                "idempotency_namespace, manifest_hash, runtime_artifact_root_hash, "
                "installed_generation, project_registration_hash, "
                "controller_capabilities, capability_profile_hash, session_epoch, "
                "controlled_target_id, project_instance_key, mode, controller_kind, "
                "active FROM sessions"
            ));
            UF_TRY(execute(database, "DROP TABLE sessions"));
            UF_TRY(execute(database, k_sessionsDdl));
            UF_TRY(execute(
                database,
                "INSERT INTO sessions(session_id, authenticated_controller_id, "
                "idempotency_namespace, manifest_hash, runtime_artifact_root_hash, "
                "installed_generation, project_registration_hash, "
                "controller_capabilities, capability_profile_hash, session_epoch, "
                "controlled_target_id, project_instance_key, mode, controller_kind, "
                "active, world_scope_kind, world_scope_id, world_scope_generation) "
                "SELECT session_id, authenticated_controller_id, "
                "idempotency_namespace, manifest_hash, runtime_artifact_root_hash, "
                "installed_generation, project_registration_hash, "
                "controller_capabilities, capability_profile_hash, session_epoch, "
                "controlled_target_id, project_instance_key, mode, controller_kind, "
                "active, 'account', '', '0' FROM prior_sessions"
            ));
            UF_TRY(execute(database, k_oneActiveWriteSessionIndexDdl));
            return execute(database, "DROP TABLE prior_sessions");
        }

        // local_ref is NOT NULL with no default, so SQLite cannot ADD COLUMN it
        // in place: the table is rebuilt from its exact final DDL, triggers
        // included. Bindings minted before the column existed were never
        // observed under a recorded target, so their rows are backfilled with
        // the empty sentinel, which a Tool-call input delivery refuses -- a
        // migrated binding can never be resolved to a target it never claimed,
        // the same
        // fail-closed ruling the world-scope sentinel follows. Every other
        // byte of every row survives.
        [[nodiscard]]
        auto addObservedInstanceBindingLocalRef(sqlite3* database) -> Status
        {
            UF_TRY(execute(database, "PRAGMA defer_foreign_keys=ON"));
            UF_TRY(execute(
                database,
                "CREATE TABLE prior_observed_instance_bindings("
                "canonical_authority TEXT PRIMARY KEY,"
                "observed_instance_id TEXT NOT NULL UNIQUE,"
                "plugin_id TEXT NOT NULL,"
                "project_registration_hash TEXT NOT NULL,"
                "project_instance_key TEXT NOT NULL,"
                "world_scope_kind TEXT NOT NULL,"
                "world_scope_id TEXT NOT NULL,"
                "world_scope_generation TEXT NOT NULL"
                ") STRICT"
            ));
            UF_TRY(execute(
                database,
                "INSERT INTO prior_observed_instance_bindings("
                "canonical_authority, observed_instance_id, plugin_id, "
                "project_registration_hash, project_instance_key, "
                "world_scope_kind, world_scope_id, world_scope_generation) "
                "SELECT canonical_authority, observed_instance_id, plugin_id, "
                "project_registration_hash, project_instance_key, "
                "world_scope_kind, world_scope_id, world_scope_generation "
                "FROM observed_instance_bindings"
            ));
            UF_TRY(execute(database, "DROP TABLE observed_instance_bindings"));
            UF_TRY(execute(database, k_observedInstanceBindingsDdl));
            UF_TRY(execute(
                database,
                "INSERT INTO observed_instance_bindings("
                "canonical_authority, observed_instance_id, plugin_id, "
                "project_registration_hash, project_instance_key, "
                "world_scope_kind, world_scope_id, world_scope_generation, "
                "local_ref) SELECT canonical_authority, observed_instance_id, "
                "plugin_id, project_registration_hash, project_instance_key, "
                "world_scope_kind, world_scope_id, world_scope_generation, '' "
                "FROM prior_observed_instance_bindings"
            ));
            return execute(database, "DROP TABLE prior_observed_instance_bindings");
        }

        // The generation in which the Operation dispatch spine was deleted.
        // reserveDispatch, recordDeliveryOutcome and issueApproval were the
        // only writers of these five tables and step minting -- their only
        // source of rows -- was already gone, so every one of them is dropped
        // rather than carried: a table nothing can write and nothing reads is
        // storage with nothing keeping it true.
        //
        // ledger_events is NOT rebuilt here. Its CHECK enumerated a
        // delivery_outcome_recorded kind that lost its producer with these
        // tables, and two operation kinds that lost theirs one generation
        // later; rebuildLedgerEvents below retires all three in one step, and
        // every chain that reaches this one reaches that one too.
        [[nodiscard]]
        auto dropOperationDispatchTables(sqlite3* database) -> Status
        {
            UF_TRY(execute(database, "PRAGMA defer_foreign_keys=ON"));
            UF_TRY(execute(database, "DROP TABLE IF EXISTS approvals"));
            UF_TRY(execute(database, "DROP TABLE IF EXISTS operation_steps"));
            UF_TRY(execute(database, "DROP TABLE IF EXISTS dispatches"));
            UF_TRY(execute(database, "DROP TABLE IF EXISTS authority_decisions"));
            return execute(database, "DROP TABLE IF EXISTS operation_plans");
        }

        // Rebuilds ledger_events into the exact current DDL, carrying every row
        // whose kind the current CHECK still admits.
        //
        // Three kinds are dropped rather than carried, and this is the one
        // place a registered migration here discards audit.
        // delivery_outcome_recorded reports the outcome of a dispatch whose
        // whole subsystem is gone; operation_created and
        // operation_state_changed name a row in the table the next step drops,
        // so their subject id resolves to nothing. The rebuilt CHECK is what
        // refuses a retired kind rather than a comment.
        //
        // The table is rebuilt rather than altered because the detail column
        // goes with those kinds, and because a CHECK constraint is part of the
        // stored DDL this schema identity is taken from.
        [[nodiscard]]
        auto rebuildLedgerEvents(sqlite3* database) -> Status
        {
            UF_TRY(execute(database, "PRAGMA defer_foreign_keys=ON"));
            UF_TRY(execute(
                database,
                "ALTER TABLE ledger_events RENAME TO prior_ledger_events"
            ));
            UF_TRY(execute(database, k_ledgerEventsDdl));
            UF_TRY(execute(
                database,
                "INSERT INTO ledger_events(sequence, session_epoch, "
                "controlled_target_id, kind, subject_id) SELECT sequence, "
                "session_epoch, controlled_target_id, kind, subject_id "
                "FROM prior_ledger_events WHERE kind NOT IN ("
                "'delivery_outcome_recorded', 'operation_created', "
                "'operation_state_changed')"
            ));
            return execute(database, "DROP TABLE prior_ledger_events");
        }

        // The generation in which the Operation surface left the ledger
        // entirely.
        //
        // operations had one writer, submitCommand, and four readers besides
        // it; all of them are deleted in the same change, so the table is
        // dropped rather than carried. reconciliations goes with it because its
        // only non-audit column was a NOT NULL foreign key into operations: no
        // row of it could outlive the drop, and nothing ever wrote one.
        //
        // external_input_findings.operation_id was a nullable reference into
        // a table this generation deletes, and every writer bound NULL to it.
        // The table is rebuilt without a column no value could ever occupy;
        // every other byte of every row is carried.
        //
        // It is rebuilt rather than altered because SQLite cannot drop a column
        // that participates in a foreign key, and because the exact final DDL
        // TEXT is what this schema identity is taken from -- so the text below
        // is the creating block's own text, byte for byte.
        //
        // journal_events carried the same nullable reference and is not rebuilt
        // here: dropProjectStateInterpretation deletes that table outright, and
        // rebuilding a table one step before dropping it is work whose result
        // nothing can observe.
        [[nodiscard]]
        auto dropOperationSurface(sqlite3* database) -> Status
        {
            UF_TRY(execute(database, "PRAGMA defer_foreign_keys=ON"));
            UF_TRY(execute(
                database,
                "ALTER TABLE external_input_findings RENAME TO "
                "prior_external_input_findings"
            ));
            UF_TRY(execute(
                database,
                R"sql(CREATE TABLE IF NOT EXISTS external_input_findings(
                        finding_id TEXT PRIMARY KEY,
                        controlled_target_id TEXT NOT NULL,
                        session_epoch INTEGER NOT NULL CHECK(session_epoch > 0),
                        reporter_session_id TEXT NOT NULL
                            REFERENCES sessions(session_id),
                        detected_after_cursor INTEGER NOT NULL
                            CHECK(detected_after_cursor >= 0),
                        invalidated_snapshot_revision INTEGER NOT NULL
                            CHECK(invalidated_snapshot_revision >= 0),
                        required_action TEXT NOT NULL
                            CHECK(required_action IN (
                                'freeze_and_reobserve', 'freeze_and_reconcile'
                            )),
                        reason TEXT NOT NULL
                    ) STRICT)sql"
            ));
            UF_TRY(execute(
                database,
                "INSERT INTO external_input_findings(finding_id, "
                "controlled_target_id, session_epoch, reporter_session_id, "
                "detected_after_cursor, invalidated_snapshot_revision, "
                "required_action, reason) SELECT finding_id, "
                "controlled_target_id, session_epoch, reporter_session_id, "
                "detected_after_cursor, invalidated_snapshot_revision, "
                "required_action, reason FROM prior_external_input_findings"
            ));
            UF_TRY(execute(database, "DROP TABLE prior_external_input_findings"));
            UF_TRY(execute(database, "DROP TABLE reconciliations"));
            UF_TRY(execute(database, "DROP TABLE operations"));
            return rebuildLedgerEvents(database);
        }

        // The generation in which the framework stopped interpreting a
        // Project's state.
        //
        // The Journal, the fold, the revision and the final compare-and-swap
        // defined how a Project's own state evolves, which is the Project's
        // decision. Everything that recorded that reading goes: the Journal
        // events, the materialized ProjectState, the call-bound proposals that
        // would have published one, the baseline event a ProjectInstance named,
        // and the ProjectState revision and digest the observation and snapshot
        // rows carried into a decision basis.
        //
        // The only production writer of journal_events was the sequence-0
        // baseline provisioning wrote, and project_state held its fold. Both
        // are deleted with their tables rather than carried into a shape
        // nothing reads: what the rows meant was a Project's own state, and the
        // framework has no reading of it to preserve.
        //
        // The three surviving tables are rebuilt rather than altered because
        // each is a foreign-key participant and because the exact stored DDL
        // TEXT is what this schema identity is taken from. project_observations
        // and project_instances are carried through explicit prior_ tables
        // rather than renamed: ALTER TABLE RENAME rewrites references to the
        // renamed table in every other table's stored DDL, which would edit the
        // very text the identity is taken over.
        [[nodiscard]]
        auto dropProjectStateInterpretation(sqlite3* database) -> Status
        {
            UF_TRY(execute(database, "PRAGMA defer_foreign_keys=ON"));

            // IF EXISTS because the registered source generations straddle the
            // point at which the proposal tables were added; a source that
            // never had them has nothing to drop, and a branch here would be a
            // second reading of which generation this is.
            UF_TRY(execute(
                database,
                "DROP TABLE IF EXISTS journal_proposal_effects;"
                "DROP TABLE IF EXISTS journal_proposal_events;"
                "DROP TABLE IF EXISTS journal_batch_proposals;"
                "DROP TABLE IF EXISTS project_state;"
                "DROP TABLE IF EXISTS journal_events"
            ));

            UF_TRY(execute(
                database,
                "ALTER TABLE snapshots RENAME TO prior_snapshots"
            ));
            UF_TRY(execute(database, k_snapshotsDdl));
            UF_TRY(execute(
                database,
                "INSERT INTO snapshots(token, session_id, snapshot_revision, "
                "session_epoch, identity_hash, decision_basis_hash, "
                "canonical_parts, lease_revision, plugin_id, "
                "project_instance_key, observation_id, target_generation, "
                "state_resolution_hash, project_observation_revision, "
                "availability_revision) SELECT token, session_id, "
                "snapshot_revision, session_epoch, identity_hash, "
                "decision_basis_hash, canonical_parts, lease_revision, "
                "plugin_id, project_instance_key, observation_id, "
                "target_generation, state_resolution_hash, "
                "project_observation_revision, availability_revision "
                "FROM prior_snapshots"
            ));
            UF_TRY(execute(database, "DROP TABLE prior_snapshots"));

            UF_TRY(execute(
                database,
                "CREATE TABLE prior_project_observations("
                "plugin_id TEXT NOT NULL,"
                "project_instance_key TEXT NOT NULL,"
                "revision INTEGER NOT NULL,"
                "project_registration_hash TEXT NOT NULL,"
                "state_resolution_hash TEXT NOT NULL,"
                "canonical_observation TEXT NOT NULL,"
                "observation_hash TEXT NOT NULL) STRICT"
            ));
            UF_TRY(execute(
                database,
                "INSERT INTO prior_project_observations SELECT plugin_id, "
                "project_instance_key, revision, project_registration_hash, "
                "state_resolution_hash, canonical_observation, observation_hash "
                "FROM project_observations"
            ));
            UF_TRY(execute(database, "DROP TABLE project_observations"));
            UF_TRY(execute(database, k_projectObservationsDdl));
            UF_TRY(execute(
                database,
                "INSERT INTO project_observations SELECT * FROM "
                "prior_project_observations"
            ));
            UF_TRY(execute(database, "DROP TABLE prior_project_observations"));

            UF_TRY(execute(
                database,
                "CREATE TABLE prior_project_instances("
                "plugin_id TEXT NOT NULL,"
                "project_instance_key TEXT NOT NULL,"
                "project_registration_hash TEXT NOT NULL) STRICT"
            ));
            UF_TRY(execute(
                database,
                "INSERT INTO prior_project_instances SELECT plugin_id, "
                "project_instance_key, project_registration_hash "
                "FROM project_instances"
            ));
            UF_TRY(execute(database, "DROP TABLE project_instances"));
            UF_TRY(execute(database, k_projectInstancesDdl));
            UF_TRY(execute(
                database,
                "INSERT INTO project_instances SELECT * FROM "
                "prior_project_instances"
            ));
            return execute(database, "DROP TABLE prior_project_instances");
        }

        // The generation in which generation 0 stopped meaning "nothing is
        // installed" and started meaning the genesis generation.
        //
        // Every Operator root now materialises the genesis RuntimeArtifact as
        // part of its layout, so generation 0 names a real artifact root and a
        // session may pin it. Both CHECKs that spelled "a generation is
        // positive" become "a generation is non-negative"; nothing else about
        // either table moves, and the first REAL installation still lands at
        // generation 1 because the CHECK is the only thing that changed.
        //
        // The rows are NOT written here. ensureGenesisGeneration runs on every
        // open, after the schema is at target, and writes them for a root
        // created before this change and a root created after it alike --
        // one mechanism rather than a migration branch and a creation branch.
        //
        // Both tables are rebuilt rather than altered: SQLite cannot change a
        // CHECK in place. Neither is renamed out of the way either, because
        // sessions carries a foreign key into runtime_installations and four
        // tables carry one into sessions -- ALTER TABLE RENAME rewrites those
        // references in the other tables' stored DDL, which is the very text
        // this schema identity is taken over.
        [[nodiscard]]
        auto admitTheGenesisGeneration(sqlite3* database) -> Status
        {
            UF_TRY(execute(database, "PRAGMA defer_foreign_keys=ON"));
            UF_TRY(execute(
                database,
                "CREATE TABLE prior_runtime_installations("
                "installed_generation INTEGER PRIMARY KEY,"
                "artifact_root_hash TEXT NOT NULL) STRICT"
            ));
            UF_TRY(execute(
                database,
                "INSERT INTO prior_runtime_installations(installed_generation, "
                "artifact_root_hash) SELECT installed_generation, "
                "artifact_root_hash FROM runtime_installations"
            ));
            UF_TRY(execute(database, "DROP TABLE runtime_installations"));
            UF_TRY(execute(database, k_runtimeInstallationsDdl));
            UF_TRY(execute(
                database,
                "INSERT INTO runtime_installations(installed_generation, "
                "artifact_root_hash) SELECT installed_generation, "
                "artifact_root_hash FROM prior_runtime_installations"
            ));
            UF_TRY(execute(database, "DROP TABLE prior_runtime_installations"));

            UF_TRY(execute(
                database,
                "CREATE TABLE prior_sessions("
                "session_id TEXT PRIMARY KEY,"
                "authenticated_controller_id TEXT NOT NULL,"
                "idempotency_namespace TEXT NOT NULL,"
                "manifest_hash TEXT NOT NULL,"
                "runtime_artifact_root_hash TEXT NOT NULL,"
                "installed_generation INTEGER NOT NULL,"
                "project_registration_hash TEXT NOT NULL,"
                "controller_capabilities TEXT NOT NULL,"
                "capability_profile_hash TEXT NOT NULL,"
                "session_epoch INTEGER NOT NULL,"
                "controlled_target_id TEXT NOT NULL,"
                "project_instance_key TEXT NOT NULL,"
                "mode TEXT NOT NULL,"
                "controller_kind TEXT NOT NULL,"
                "world_scope_kind TEXT NOT NULL,"
                "world_scope_id TEXT NOT NULL,"
                "world_scope_generation TEXT NOT NULL,"
                "active INTEGER NOT NULL"
                ") STRICT"
            ));
            UF_TRY(execute(
                database,
                "INSERT INTO prior_sessions(session_id, "
                "authenticated_controller_id, idempotency_namespace, "
                "manifest_hash, runtime_artifact_root_hash, "
                "installed_generation, project_registration_hash, "
                "controller_capabilities, capability_profile_hash, "
                "session_epoch, controlled_target_id, project_instance_key, "
                "mode, controller_kind, world_scope_kind, world_scope_id, "
                "world_scope_generation, active) SELECT session_id, "
                "authenticated_controller_id, idempotency_namespace, "
                "manifest_hash, runtime_artifact_root_hash, "
                "installed_generation, project_registration_hash, "
                "controller_capabilities, capability_profile_hash, "
                "session_epoch, controlled_target_id, project_instance_key, "
                "mode, controller_kind, world_scope_kind, world_scope_id, "
                "world_scope_generation, active FROM sessions"
            ));
            UF_TRY(execute(database, "DROP TABLE sessions"));
            UF_TRY(execute(database, k_sessionsDdl));
            UF_TRY(execute(
                database,
                "INSERT INTO sessions(session_id, "
                "authenticated_controller_id, idempotency_namespace, "
                "manifest_hash, runtime_artifact_root_hash, "
                "installed_generation, project_registration_hash, "
                "controller_capabilities, capability_profile_hash, "
                "session_epoch, controlled_target_id, project_instance_key, "
                "mode, controller_kind, world_scope_kind, world_scope_id, "
                "world_scope_generation, active) SELECT session_id, "
                "authenticated_controller_id, idempotency_namespace, "
                "manifest_hash, runtime_artifact_root_hash, "
                "installed_generation, project_registration_hash, "
                "controller_capabilities, capability_profile_hash, "
                "session_epoch, controlled_target_id, project_instance_key, "
                "mode, controller_kind, world_scope_kind, world_scope_id, "
                "world_scope_generation, active FROM prior_sessions"
            ));
            UF_TRY(execute(database, k_oneActiveWriteSessionIndexDdl));
            return execute(database, "DROP TABLE prior_sessions");
        }

        // The pair whose source is the schema the immediately prior
        // generation created. It carries no step beyond the genesis
        // generation, because nothing else about the schema moved with it.
        [[nodiscard]]
        auto migrateGenesisGeneration(
            sqlite3* database,
            SchemaMigration const& migration
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY(admitTheGenesisGeneration(database));
            UF_TRY(removeNestedToolCallSchema(database));
            UF_TRY(addGenesisTransitions(database));
            UF_TRY(recordSchemaIdentityTransition(database, migration));
            UF_TRY(verifyExactDatabaseSchema(database, migration.targetIdentity));
            return transaction.commit();
        }

        [[nodiscard]]
        auto migrateProjectStateInterpretation(
            sqlite3* database,
            SchemaMigration const& migration
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY(dropProjectStateInterpretation(database));
            UF_TRY(admitTheGenesisGeneration(database));
            UF_TRY(removeNestedToolCallSchema(database));
            UF_TRY(addGenesisTransitions(database));
            UF_TRY(recordSchemaIdentityTransition(database, migration));
            UF_TRY(verifyExactDatabaseSchema(database, migration.targetIdentity));
            return transaction.commit();
        }

        [[nodiscard]]
        auto migrateSessionWorldScope(
            sqlite3* database,
            SchemaMigration const& migration
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY(addSessionWorldScopeColumns(database));
            UF_TRY(addObservedInstanceBindingLocalRef(database));
            UF_TRY(makeRegistrationIdentityGenerationNeutral(database));
            UF_TRY(addToolIdentityPersistence(database));
            UF_TRY(dropOperationDispatchTables(database));
            UF_TRY(addToolRunTerminationState(database));
            UF_TRY(dropRejectedToolCallState(database));
            UF_TRY(dropOperationSurface(database));
            UF_TRY(dropProjectStateInterpretation(database));
            UF_TRY(admitTheGenesisGeneration(database));
            UF_TRY(removeNestedToolCallSchema(database));
            UF_TRY(addGenesisTransitions(database));
            UF_TRY(recordSchemaIdentityTransition(database, migration));
            UF_TRY(verifyExactDatabaseSchema(database, migration.targetIdentity));
            return transaction.commit();
        }

        [[nodiscard]]
        auto migrateOperatorU9Schema(
            sqlite3* database,
            SchemaMigration const& migration
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY(execute(
                database,
                "CREATE TABLE IF NOT EXISTS schema_identity_transitions("
                "source_identity TEXT NOT NULL,"
                "target_identity TEXT NOT NULL,"
                "PRIMARY KEY(source_identity, target_identity)"
                ") STRICT"
            ));
            UF_TRY(execute(database, k_sessionPoliciesDdl));
            UF_TRY(execute(database, k_availabilityHeadsDdl));
            UF_TRY(addReleaseUpgradeEvidenceTables(database));
            UF_TRY(dropRegistrationStateSchemaHash(database));
            UF_TRY(addSessionWorldScopeColumns(database));
            UF_TRY(addObservedInstanceBindingLocalRef(database));
            UF_TRY(makeRegistrationIdentityGenerationNeutral(database));
            UF_TRY(addToolIdentityPersistence(database));
            UF_TRY(dropOperationDispatchTables(database));
            UF_TRY(addToolRunTerminationState(database));
            UF_TRY(dropRejectedToolCallState(database));
            UF_TRY(dropOperationSurface(database));
            UF_TRY(dropProjectStateInterpretation(database));
            UF_TRY(admitTheGenesisGeneration(database));
            UF_TRY(removeNestedToolCallSchema(database));
            UF_TRY(addGenesisTransitions(database));
            UF_TRY(recordSchemaIdentityTransition(database, migration));

            // No migration commits under an identity other than the exact
            // target named by its registration.
            UF_TRY(verifyExactDatabaseSchema(database, migration.targetIdentity));
            return transaction.commit();
        }

        [[nodiscard]]
        auto migrateReleaseUpgradeEvidence(
            sqlite3* database,
            SchemaMigration const& migration
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY(addReleaseUpgradeEvidenceTables(database));
            UF_TRY(dropRegistrationStateSchemaHash(database));
            UF_TRY(addSessionWorldScopeColumns(database));
            UF_TRY(addObservedInstanceBindingLocalRef(database));
            UF_TRY(makeRegistrationIdentityGenerationNeutral(database));
            UF_TRY(addToolIdentityPersistence(database));
            UF_TRY(dropOperationDispatchTables(database));
            UF_TRY(addToolRunTerminationState(database));
            UF_TRY(dropRejectedToolCallState(database));
            UF_TRY(dropOperationSurface(database));
            UF_TRY(dropProjectStateInterpretation(database));
            UF_TRY(admitTheGenesisGeneration(database));
            UF_TRY(removeNestedToolCallSchema(database));
            UF_TRY(addGenesisTransitions(database));
            UF_TRY(recordSchemaIdentityTransition(database, migration));
            UF_TRY(verifyExactDatabaseSchema(database, migration.targetIdentity));
            return transaction.commit();
        }

        [[nodiscard]]
        auto migrateRegistrationStateSchemaHash(
            sqlite3* database,
            SchemaMigration const& migration
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY(dropRegistrationStateSchemaHash(database));
            UF_TRY(addSessionWorldScopeColumns(database));
            UF_TRY(addObservedInstanceBindingLocalRef(database));
            UF_TRY(makeRegistrationIdentityGenerationNeutral(database));
            UF_TRY(addToolIdentityPersistence(database));
            UF_TRY(dropOperationDispatchTables(database));
            UF_TRY(addToolRunTerminationState(database));
            UF_TRY(dropRejectedToolCallState(database));
            UF_TRY(dropOperationSurface(database));
            UF_TRY(dropProjectStateInterpretation(database));
            UF_TRY(admitTheGenesisGeneration(database));
            UF_TRY(removeNestedToolCallSchema(database));
            UF_TRY(addGenesisTransitions(database));
            UF_TRY(recordSchemaIdentityTransition(database, migration));
            UF_TRY(verifyExactDatabaseSchema(database, migration.targetIdentity));
            return transaction.commit();
        }

        [[nodiscard]]
        auto migrateObservedInstanceBindingLocalRef(
            sqlite3* database,
            SchemaMigration const& migration
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY(addObservedInstanceBindingLocalRef(database));
            UF_TRY(makeRegistrationIdentityGenerationNeutral(database));
            UF_TRY(addToolIdentityPersistence(database));
            UF_TRY(dropOperationDispatchTables(database));
            UF_TRY(addToolRunTerminationState(database));
            UF_TRY(dropRejectedToolCallState(database));
            UF_TRY(dropOperationSurface(database));
            UF_TRY(dropProjectStateInterpretation(database));
            UF_TRY(admitTheGenesisGeneration(database));
            UF_TRY(removeNestedToolCallSchema(database));
            UF_TRY(addGenesisTransitions(database));
            UF_TRY(recordSchemaIdentityTransition(database, migration));
            UF_TRY(verifyExactDatabaseSchema(database, migration.targetIdentity));
            return transaction.commit();
        }

        [[nodiscard]]
        auto migrateRegistrationIdentityGeneration(
            sqlite3* database,
            SchemaMigration const& migration
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY(makeRegistrationIdentityGenerationNeutral(database));
            UF_TRY(addToolIdentityPersistence(database));
            UF_TRY(dropOperationDispatchTables(database));
            UF_TRY(addToolRunTerminationState(database));
            UF_TRY(dropRejectedToolCallState(database));
            UF_TRY(dropOperationSurface(database));
            UF_TRY(dropProjectStateInterpretation(database));
            UF_TRY(admitTheGenesisGeneration(database));
            UF_TRY(removeNestedToolCallSchema(database));
            UF_TRY(addGenesisTransitions(database));
            UF_TRY(recordSchemaIdentityTransition(database, migration));
            UF_TRY(verifyExactDatabaseSchema(database, migration.targetIdentity));
            return transaction.commit();
        }

        [[nodiscard]]
        auto migrateToolIdentityPersistence(
            sqlite3* database,
            SchemaMigration const& migration
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY(addToolIdentityPersistence(database));
            UF_TRY(dropOperationDispatchTables(database));
            UF_TRY(addToolRunTerminationState(database));
            UF_TRY(dropRejectedToolCallState(database));
            UF_TRY(dropOperationSurface(database));
            UF_TRY(dropProjectStateInterpretation(database));
            UF_TRY(admitTheGenesisGeneration(database));
            UF_TRY(removeNestedToolCallSchema(database));
            UF_TRY(addGenesisTransitions(database));
            UF_TRY(recordSchemaIdentityTransition(database, migration));
            UF_TRY(verifyExactDatabaseSchema(database, migration.targetIdentity));
            return transaction.commit();
        }

        [[nodiscard]]
        auto migrateToolRuntimePersistence(
            sqlite3* database,
            SchemaMigration const& migration
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY(rebuildToolCallPositions(database, "NULL"));
            UF_TRY(addToolRuntimePersistence(database));
            UF_TRY(dropOperationDispatchTables(database));
            UF_TRY(addToolRunTerminationState(database));
            UF_TRY(dropRejectedToolCallState(database));
            UF_TRY(dropOperationSurface(database));
            UF_TRY(dropProjectStateInterpretation(database));
            UF_TRY(admitTheGenesisGeneration(database));
            UF_TRY(removeNestedToolCallSchema(database));
            UF_TRY(addGenesisTransitions(database));
            UF_TRY(recordSchemaIdentityTransition(database, migration));
            UF_TRY(verifyExactDatabaseSchema(database, migration.targetIdentity));
            return transaction.commit();
        }

        [[nodiscard]]
        auto addToolAdmissionAuthority(
            sqlite3* database,
            SchemaMigration const& migration
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY(execute(database, "PRAGMA defer_foreign_keys=ON"));
            UF_TRY(rebuildToolCallPositions(database, "NULL"));
            UF_TRY(execute(
                database,
                "ALTER TABLE tool_admission_attempts RENAME TO "
                "prior_tool_admission_attempts"
            ));
            UF_TRY(execute(database, k_toolAdmissionAttemptsDdl));
            UF_TRY(execute(
                database,
                "INSERT INTO tool_admission_attempts(call_identity, attempt_number, "
                "root_identity, origin_principal_id, origin_principal_kind, "
                "execution_principal_id, execution_principal_kind, session_id, "
                "session_epoch, controlled_target_id, project_registration_hash, "
                "policy_hash, capability_profile_hash, lease_id, lease_revision, "
                "fencing_token, budget_snapshot, budget_snapshot_hash) SELECT "
                "call_identity, attempt_number, root_identity, origin_principal_id, "
                "origin_principal_kind, execution_principal_id, "
                "execution_principal_kind, session_id, session_epoch, "
                "controlled_target_id, project_registration_hash, policy_hash, "
                "capability_profile_hash, lease_id, lease_revision, fencing_token, "
                "budget_snapshot, budget_snapshot_hash FROM "
                "prior_tool_admission_attempts"
            ));
            UF_TRY(execute(database, "DROP TABLE prior_tool_admission_attempts"));
            UF_TRY(execute(database, k_toolApprovalsDdl));
            UF_TRY(dropOperationDispatchTables(database));
            UF_TRY(addToolRunTerminationState(database));
            UF_TRY(dropRejectedToolCallState(database));
            UF_TRY(dropOperationSurface(database));
            UF_TRY(dropProjectStateInterpretation(database));
            UF_TRY(admitTheGenesisGeneration(database));
            UF_TRY(removeNestedToolCallSchema(database));
            UF_TRY(addGenesisTransitions(database));
            UF_TRY(recordSchemaIdentityTransition(database, migration));
            UF_TRY(verifyExactDatabaseSchema(database, migration.targetIdentity));
            return transaction.commit();
        }

        [[nodiscard]]
        auto addToolApprovals(
            sqlite3* database,
            SchemaMigration const& migration
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY(execute(database, "PRAGMA defer_foreign_keys=ON"));
            {
                UF_TRY_VALUE(
                    legacyTokenQuery,
                    prepare(
                        database,
                        "SELECT count(*) FROM tool_admission_attempts "
                        "WHERE approval_token IS NOT NULL"
                    )
                );
                auto const step = sqlite3_step(legacyTokenQuery.get());
                if (step != SQLITE_ROW)
                {
                    return databaseFailure(
                        database,
                        "could not inspect immediate-prior Tool approvals"
                    );
                }
                if (sqlite3_column_int64(legacyTokenQuery.get(), 0) != 0)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "Immediate-prior Tool admission rows contain an approval "
                        "token that generation never issued"
                    );
                }
            }
            UF_TRY(rebuildToolCallPositions(database, "NULL"));
            UF_TRY(execute(
                database,
                "ALTER TABLE tool_admission_attempts RENAME TO "
                "prior_tool_admission_attempts"
            ));
            UF_TRY(execute(database, k_toolAdmissionAttemptsDdl));
            UF_TRY(execute(
                database,
                "INSERT INTO tool_admission_attempts(call_identity, attempt_number, "
                "root_identity, origin_principal_id, origin_principal_kind, "
                "execution_principal_id, execution_principal_kind, session_id, "
                "session_epoch, controlled_target_id, project_registration_hash, "
                "policy_hash, capability_profile_hash, lease_id, lease_revision, "
                "fencing_token, budget_snapshot, budget_snapshot_hash, "
                "effect_envelope, effect_envelope_hash, required_approvals, "
                "approval_tokens) SELECT call_identity, attempt_number, "
                "root_identity, origin_principal_id, origin_principal_kind, "
                "execution_principal_id, execution_principal_kind, session_id, "
                "session_epoch, controlled_target_id, project_registration_hash, "
                "policy_hash, capability_profile_hash, lease_id, lease_revision, "
                "fencing_token, budget_snapshot, budget_snapshot_hash, "
                "effect_envelope, effect_envelope_hash, required_approvals, "
                "CASE WHEN effect_envelope IS NULL THEN NULL ELSE '[]' END "
                "FROM prior_tool_admission_attempts"
            ));
            UF_TRY(execute(database, "DROP TABLE prior_tool_admission_attempts"));
            UF_TRY(execute(database, k_toolApprovalsDdl));
            UF_TRY(dropOperationDispatchTables(database));
            UF_TRY(addToolRunTerminationState(database));
            UF_TRY(dropRejectedToolCallState(database));
            UF_TRY(dropOperationSurface(database));
            UF_TRY(dropProjectStateInterpretation(database));
            UF_TRY(admitTheGenesisGeneration(database));
            UF_TRY(removeNestedToolCallSchema(database));
            UF_TRY(addGenesisTransitions(database));
            UF_TRY(recordSchemaIdentityTransition(database, migration));
            UF_TRY(verifyExactDatabaseSchema(database, migration.targetIdentity));
            return transaction.commit();
        }

        [[nodiscard]]
        auto migrateProjectToolLeaves(
            sqlite3* database,
            SchemaMigration const& migration
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY(removeNestedToolCallSchema(database));
            UF_TRY(addGenesisTransitions(database));
            UF_TRY(recordSchemaIdentityTransition(database, migration));
            UF_TRY(verifyExactDatabaseSchema(database, migration.targetIdentity));
            return transaction.commit();
        }

        [[nodiscard]]
        auto migrateNestedToolCalls(
            sqlite3* database,
            SchemaMigration const& migration
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY(rebuildToolCallPositions(database, "NULL"));
            UF_TRY(dropOperationDispatchTables(database));
            UF_TRY(addToolRunTerminationState(database));
            UF_TRY(dropRejectedToolCallState(database));
            UF_TRY(dropOperationSurface(database));
            UF_TRY(dropProjectStateInterpretation(database));
            UF_TRY(admitTheGenesisGeneration(database));
            UF_TRY(removeNestedToolCallSchema(database));
            UF_TRY(addGenesisTransitions(database));
            UF_TRY(recordSchemaIdentityTransition(database, migration));
            UF_TRY(verifyExactDatabaseSchema(database, migration.targetIdentity));
            return transaction.commit();
        }

        // The generation in which the root run became a real positioned call.
        // parent_call_identity stops being nullable and every row an earlier
        // generation left null is backfilled with its own root identity, which
        // is the coordinate a run's own context is now anchored on. The
        // one_top_level_tool_call_position partial index goes with the null:
        // UNIQUE(root_identity, parent_call_identity, call_sequence) never
        // constrained rows whose parent was null, and it constrains them now.
        [[nodiscard]]
        auto migrateRootPositionedToolCalls(
            sqlite3* database,
            SchemaMigration const& migration
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY(rebuildToolCallPositions(
                database,
                "observation_reference_hash"
            ));
            UF_TRY(dropOperationDispatchTables(database));
            UF_TRY(addToolRunTerminationState(database));
            UF_TRY(dropRejectedToolCallState(database));
            UF_TRY(dropOperationSurface(database));
            UF_TRY(dropProjectStateInterpretation(database));
            UF_TRY(admitTheGenesisGeneration(database));
            UF_TRY(removeNestedToolCallSchema(database));
            UF_TRY(addGenesisTransitions(database));
            UF_TRY(recordSchemaIdentityTransition(database, migration));
            UF_TRY(verifyExactDatabaseSchema(database, migration.targetIdentity));
            return transaction.commit();
        }

        // The generation that deleted the Operation dispatch spine, and the one
        // whose source identity is the schema the immediately prior generation
        // created. It carries no step of its own beyond the drop, because
        // nothing else about the schema moved with it.
        [[nodiscard]]
        auto migrateOperationDispatchRemoval(
            sqlite3* database,
            SchemaMigration const& migration
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY(dropOperationDispatchTables(database));
            UF_TRY(addToolRunTerminationState(database));
            UF_TRY(dropRejectedToolCallState(database));
            UF_TRY(dropOperationSurface(database));
            UF_TRY(dropProjectStateInterpretation(database));
            UF_TRY(admitTheGenesisGeneration(database));
            UF_TRY(removeNestedToolCallSchema(database));
            UF_TRY(addGenesisTransitions(database));
            UF_TRY(recordSchemaIdentityTransition(database, migration));
            UF_TRY(verifyExactDatabaseSchema(database, migration.targetIdentity));
            return transaction.commit();
        }

        // The generation that gave a run its own durable state and retired the
        // `rejected` Tool call state, and the one whose source identity is the
        // schema the immediately prior generation created. It carries no step
        // of its own beyond those two, because nothing else about the schema
        // moved with them.
        [[nodiscard]]
        auto migrateToolRunTermination(
            sqlite3* database,
            SchemaMigration const& migration
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY(addToolRunTerminationState(database));
            UF_TRY(dropRejectedToolCallState(database));
            UF_TRY(dropOperationSurface(database));
            UF_TRY(dropProjectStateInterpretation(database));
            UF_TRY(admitTheGenesisGeneration(database));
            UF_TRY(removeNestedToolCallSchema(database));
            UF_TRY(addGenesisTransitions(database));
            UF_TRY(recordSchemaIdentityTransition(database, migration));
            UF_TRY(verifyExactDatabaseSchema(database, migration.targetIdentity));
            return transaction.commit();
        }

        // The generation that cut the Operation surface out of the ledger, and
        // the one whose source identity is the schema the immediately prior
        // generation created. It carries no step of its own beyond the drop,
        // because nothing else about the schema moved with it.
        [[nodiscard]]
        auto migrateOperationSurfaceRemoval(
            sqlite3* database,
            SchemaMigration const& migration
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY(dropOperationSurface(database));
            UF_TRY(dropProjectStateInterpretation(database));
            UF_TRY(admitTheGenesisGeneration(database));
            UF_TRY(removeNestedToolCallSchema(database));
            UF_TRY(addGenesisTransitions(database));
            UF_TRY(recordSchemaIdentityTransition(database, migration));
            UF_TRY(verifyExactDatabaseSchema(database, migration.targetIdentity));
            return transaction.commit();
        }

        // The generation that gave a root somewhere to record a genesis
        // materialisation that moved, and the one whose source identity is the
        // schema the immediately prior generation created. It carries no step
        // of its own beyond the new table, because nothing else about the
        // schema moved with it.
        [[nodiscard]]
        auto migrateGenesisTransitions(
            sqlite3* database,
            SchemaMigration const& migration
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY(addGenesisTransitions(database));
            UF_TRY(recordSchemaIdentityTransition(database, migration));
            UF_TRY(verifyExactDatabaseSchema(database, migration.targetIdentity));
            return transaction.commit();
        }

        // A registered pair must have a reproducible fixture that constructs
        // its source identity and proves the migration runs and lands on the
        // target. A pair that cannot be reproduced must be deleted, not kept:
        // a guard nothing can reach is the mirror of a guard production does
        // not reach.
        constexpr auto k_schemaMigrations = std::array{
            SchemaMigration{
                .sourceIdentity =
                    "sha256:f6a8064ca9b4d6fb0cfdce68e3d99f8e3cfd9e507183366f0d313458146afe77",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateGenesisTransitions,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:045925eefabef97b964f6a21db0da81cdc6a2c293c21e7f495011fe3d1b9277f",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateProjectToolLeaves,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:9cd2477518ee53c63c0412fe95202cde66011d3226f3243b8266119f7dbc4d76",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateProjectStateInterpretation,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:bd087692cab06397a98d74e60c7f8e792e7f8f7195e73960daefa1be60c3c62d",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateOperationSurfaceRemoval,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:f34626677a80bbf2436bd7bb476385e5f5d142c8b07882e0481946f73f41d2df",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateToolRunTermination,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:d6b81490eb210f8f271bd72523a4475b1eca878235fa0a077598f8016fb11c02",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateToolRunTermination,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:5c0e9a22691b36600cf861157a8b08385e4ec546a86ad0c29849adc5584014ee",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateOperationDispatchRemoval,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:6caa3b9a5f712571a59242bb9a7c34277e6f9e7624fcf1102f74846f46f7631c",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateRootPositionedToolCalls,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:53c56cce2064c47a07bd29529320aac7e7f8f4e8c01a74dc54da936159dd44f8",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateNestedToolCalls,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:14fbb87b8e84ce4c9f977d423a1b6e981e0425e06ef17f9a7822e6d32a8e87a4",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = addToolApprovals,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:64d3396e51680ec12cb91d944357f965e2136065fbb7b7fccc009d168fc4ac80",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = addToolAdmissionAuthority,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:50375791a22d12ab8b03f83eb48afc2183091e0d95b07fc5e4be47bb9aa07062",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateToolRuntimePersistence,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:d26b0e12be915009587a72312d4b46f4afc88509df5432f967eb15b016c24257",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateToolIdentityPersistence,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:b26344e031574f95020ed445e16e9de396f76442d98c5a3b758a91d84660237e",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateRegistrationIdentityGeneration,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:869fb0a128df4a0026bb429449fae03d6b43244c9cef4e794dfdd648421bcc19",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateRegistrationStateSchemaHash,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:d96860862dc25fb6efb21d09f59dcc99e3eed9508a5b6a6766937a15b3186eb9",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateReleaseUpgradeEvidence,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:1b70212548858e70daf7f120a0245d0af93fd3ff1e9cbab48d7dfa271b57f302",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateReleaseUpgradeEvidence,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:2a8fdd44c39346f1ee7d380b0c1cf0f51fa07b68db396a593446e3029421a23b",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateOperatorU9Schema,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:035e04f2e066eb90c457a0af7440356274551be4abd6496b620879e9d4e3b133",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateSessionWorldScope,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:26a38c2fd4357f538a99cb1b54573f6c2998e19e9a09252e7e9792c45745cec9",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateObservedInstanceBindingLocalRef,
            },
            SchemaMigration{
                .sourceIdentity =
                    "sha256:5ed5e558e04f24347a97a0de03550eaa02f6e48ddb64c29bf09e05c50e55d194",
                .targetIdentity = k_operatorDatabaseSchemaIdentity,
                .apply          = migrateGenesisGeneration,
            },
        };

        [[nodiscard]]
        auto upgradeOrVerifyExactDatabaseSchema(sqlite3* database) -> Status
        {
            UF_TRY_VALUE(actual, exactDatabaseSchemaIdentity(database));
            auto const actualIdentity = std::format("sha256:{}", actual.hex());
            if (actualIdentity == k_operatorDatabaseSchemaIdentity)
            {
                return ok();
            }

            auto const migration = std::ranges::find_if(
                k_schemaMigrations,
                [&actualIdentity](SchemaMigration const& candidate)
                {
                    return candidate.sourceIdentity == actualIdentity
                        && candidate.targetIdentity
                            == k_operatorDatabaseSchemaIdentity;
                }
            );
            if (migration == k_schemaMigrations.end())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "Operator database schema identity {} has no registered "
                        "audit-preserving disposition to {}; database left intact",
                        actualIdentity,
                        k_operatorDatabaseSchemaIdentity
                    )
                );
            }
            return migration->apply(database, *migration);
        }

        // The one query that answers "does this ledger pin that generation to
        // that artifact". Shared, so the coordinator's door and the read-only
        // door cannot come to answer it differently.
        [[nodiscard]]
        auto requireInstalledArtifactPin(
            sqlite3* database,
            uint64 installedGeneration,
            ContentHash const& artifactRootHash
        ) -> Status
        {
            UF_TRY_VALUE(
                query,
                prepare(
                    database,
                    "SELECT 1 FROM runtime_installations WHERE installed_generation=?1 "
                    "AND artifact_root_hash=?2"
                )
            );
            UF_TRY(bindInteger(database, query.get(), 1, installedGeneration));
            UF_TRY(bindText(database, query.get(), 2, artifactRootHash.hex()));
            if (sqlite3_step(query.get()) != SQLITE_ROW)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "RuntimeArtifact root is not pinned to the requested installed generation"
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto activeInstalledGeneration(
            sqlite3* database,
            ContentHash const& compatibleArtifactRootHash
        ) -> Result<uint64>
        {
            UF_TRY_VALUE(
                query,
                prepare(
                    database,
                    "SELECT state.installed_generation FROM runtime_state state "
                    "JOIN runtime_installations installation "
                    "ON installation.installed_generation=state.installed_generation "
                    "AND installation.artifact_root_hash="
                    "state.active_runtime_artifact_root_hash "
                    "WHERE state.singleton=1 "
                    "AND state.active_runtime_artifact_root_hash=?1"
                )
            );
            UF_TRY(bindText(
                database,
                query.get(),
                1,
                compatibleArtifactRootHash.hex()
            ));
            if (sqlite3_step(query.get()) != SQLITE_ROW)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "No active RuntimeArtifact is compatible with the required root"
                );
            }
            return static_cast<uint64>(sqlite3_column_int64(query.get(), 0));
        }

        // The gate a release upgrade's session pin crosses: no Tool mutation
        // anywhere in this ledger may still hold the barrier while the release
        // under it changes.
        //
        // It is deliberately not scoped to one controlled target, which is what
        // separates it from requireNoActiveToolMutation. An upgrade replaces the
        // RuntimeArtifact the whole production root runs, so a mutation in
        // flight against ANY target would resume under bytes other than the ones
        // it was admitted against.
        //
        // It excludes no issuing root either. A session pin is not inside a
        // call, so every active mutation is a barrier to it.
        [[nodiscard]]
        auto requireQuiescentSessionPin(sqlite3* database) -> Status
        {
            UF_TRY_VALUE(
                mutationQuery,
                prepare(
                    database,
                    std::string{
                        "SELECT history.call_identity, history.state, "
                        "run.controlled_target_id "
                    }
                        + std::string{k_activeToolMutationJoin}
                        + "WHERE history.mutating=1 AND history.state IN ("
                        + std::string{k_activeToolMutationStates}
                        + ") ORDER BY history.call_identity LIMIT 1"
                )
            );
            auto const step = sqlite3_step(mutationQuery.get());
            if (step == SQLITE_ROW)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    std::format(
                        "Session pin refused for unterminated mutating Tool call "
                        "{} in state {} on ControlledTarget {}",
                        columnText(mutationQuery.get(), 0),
                        columnText(mutationQuery.get(), 1),
                        columnText(mutationQuery.get(), 2)
                    )
                );
            }
            if (step != SQLITE_DONE)
            {
                return databaseFailure(
                    database,
                    "could not inspect Tool mutation quiescence for a session pin"
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto isReleaseUpgradeSessionPin(
            sqlite3* database,
            ContentHash const& artifactRootHash,
            ContentHash const& projectRegistrationHash,
            std::string_view projectInstanceKey
        ) -> Result<bool>
        {
            UF_TRY_VALUE(
                query,
                prepare(
                    database,
                    "SELECT 1 FROM sessions "
                    "WHERE project_registration_hash=?1 AND project_instance_key=?2 "
                    "AND runtime_artifact_root_hash<>?3 LIMIT 1"
                )
            );
            UF_TRY(bindText(database, query.get(), 1, projectRegistrationHash.hex()));
            UF_TRY(bindText(database, query.get(), 2, projectInstanceKey));
            UF_TRY(bindText(database, query.get(), 3, artifactRootHash.hex()));
            return sqlite3_step(query.get()) == SQLITE_ROW;
        }

        [[nodiscard]]
        auto requireApprovedCapabilityExpansion(
            sqlite3* database,
            ContentHash const& artifactRootHash,
            ContentHash const& projectRegistrationHash,
            std::string_view projectInstanceKey,
            std::vector<std::string> const& controllerCapabilities,
            ContentHash const& capabilityProfileHash
        ) -> Status
        {
            UF_TRY_VALUE(
                priorQuery,
                prepare(
                    database,
                    "SELECT controller_capabilities FROM sessions "
                    "WHERE project_registration_hash=?1 AND project_instance_key=?2 "
                    "AND runtime_artifact_root_hash<>?3 "
                    "ORDER BY session_epoch DESC, session_id LIMIT 1"
                )
            );
            UF_TRY(bindText(
                database,
                priorQuery.get(),
                1,
                projectRegistrationHash.hex()
            ));
            UF_TRY(bindText(database, priorQuery.get(), 2, projectInstanceKey));
            UF_TRY(bindText(database, priorQuery.get(), 3, artifactRootHash.hex()));
            if (sqlite3_step(priorQuery.get()) != SQLITE_ROW)
            {
                return ok();
            }

            UF_TRY_VALUE(
                priorCapabilities,
                readNameArray(columnText(priorQuery.get(), 0))
            );
            auto currentCapabilities = controllerCapabilities;
            std::ranges::sort(currentCapabilities);
            currentCapabilities.erase(
                std::ranges::unique(currentCapabilities).begin(),
                currentCapabilities.end()
            );
            auto const expanded = (
                currentCapabilities.size() > priorCapabilities.size()
                && std::ranges::includes(
                    currentCapabilities,
                    priorCapabilities
                )
            );
            if (!expanded)
            {
                return ok();
            }

            UF_TRY_VALUE(
                approvalQuery,
                prepare(
                    database,
                    "SELECT 1 FROM release_capability_approvals "
                    "WHERE artifact_root_hash=?1 AND capability_profile_hash=?2"
                )
            );
            UF_TRY(bindText(
                database,
                approvalQuery.get(),
                1,
                artifactRootHash.hex()
            ));
            UF_TRY(bindText(
                database,
                approvalQuery.get(),
                2,
                capabilityProfileHash.hex()
            ));
            if (sqlite3_step(approvalQuery.get()) == SQLITE_ROW)
            {
                return ok();
            }

            auto const added = std::ranges::find_if(
                currentCapabilities,
                [&priorCapabilities](std::string const& capability)
                {
                    return !std::ranges::binary_search(
                        priorCapabilities,
                        capability
                    );
                }
            );
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "Session pin refused capability expansion '{}' without "
                    "recorded approval",
                    *added
                )
            );
        }

        [[nodiscard]]
        auto rollbackRuntimeArtifactUpgrade(
            sqlite3* database,
            RuntimeArtifactPin const& attempted,
            RuntimeArtifactPin const& predecessor,
            std::string_view reason
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY_VALUE(
                restoredGeneration,
                checkedSqlIncrement(
                    attempted.installedGeneration,
                    "rollback RuntimeArtifact generation"
                )
            );
            UF_TRY_VALUE(
                installationInsert,
                prepare(
                    database,
                    "INSERT INTO runtime_installations("
                    "installed_generation, artifact_root_hash) VALUES(?1, ?2)"
                )
            );
            UF_TRY(bindInteger(
                database,
                installationInsert.get(),
                1,
                restoredGeneration
            ));
            UF_TRY(bindText(
                database,
                installationInsert.get(),
                2,
                predecessor.artifactRootHash.hex()
            ));
            UF_TRY(expectDone(database, installationInsert.get()));

            UF_TRY_VALUE(
                stateUpdate,
                prepare(
                    database,
                    "UPDATE runtime_state SET installed_generation=?1, "
                    "active_runtime_artifact_root_hash=?2 WHERE singleton=1 "
                    "AND installed_generation=?3 "
                    "AND active_runtime_artifact_root_hash=?4"
                )
            );
            UF_TRY(bindInteger(database, stateUpdate.get(), 1, restoredGeneration));
            UF_TRY(bindText(
                database,
                stateUpdate.get(),
                2,
                predecessor.artifactRootHash.hex()
            ));
            UF_TRY(bindInteger(
                database,
                stateUpdate.get(),
                3,
                attempted.installedGeneration
            ));
            UF_TRY(bindText(
                database,
                stateUpdate.get(),
                4,
                attempted.artifactRootHash.hex()
            ));
            UF_TRY(expectDone(database, stateUpdate.get()));
            if (sqlite3_changes(database) != 1)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "RuntimeArtifact rollback lost its active-generation compare-and-swap"
                );
            }

            UF_TRY_VALUE(
                auditInsert,
                prepare(
                    database,
                    "INSERT INTO runtime_upgrade_failures("
                    "attempted_generation, attempted_artifact_root_hash, "
                    "restored_generation, restored_artifact_root_hash, reason) "
                    "VALUES(?1, ?2, ?3, ?4, ?5)"
                )
            );
            UF_TRY(bindInteger(
                database,
                auditInsert.get(),
                1,
                attempted.installedGeneration
            ));
            UF_TRY(bindText(
                database,
                auditInsert.get(),
                2,
                attempted.artifactRootHash.hex()
            ));
            UF_TRY(bindInteger(database, auditInsert.get(), 3, restoredGeneration));
            UF_TRY(bindText(
                database,
                auditInsert.get(),
                4,
                predecessor.artifactRootHash.hex()
            ));
            UF_TRY(bindText(database, auditInsert.get(), 5, reason));
            UF_TRY(expectDone(database, auditInsert.get()));
            return transaction.commit();
        }

        // An opaque name nobody can predict, `bytes` CSPRNG bytes rendered as
        // lowercase hex, so the caller states the width its own constraint
        // needs rather than inheriting one chosen for another caller.
        [[nodiscard]]
        auto randomToken(sqlite3* database, uint32 bytes) -> Result<std::string>
        {
            UF_TRY_VALUE(
                statement,
                prepare(database, std::format("SELECT lower(hex(randomblob({})))", bytes))
            );
            if (sqlite3_step(statement.get()) != SQLITE_ROW)
            {
                return databaseFailure(database, "could not mint opaque token");
            }
            return columnText(statement.get(), 0);
        }

        // Opaque identity with no length constraint on it.
        constexpr auto k_opaqueTokenBytes = uint32{32};

        // The RuntimeArtifact staging leaf, and its width is arithmetic rather
        // than taste.
        //
        // A staged file's path is the production root plus
        // `runtime-artifacts\.staging\<token>\`, while the same file's final
        // path is the production root plus `runtime-artifacts\<64 hex>\`. Every
        // layer under those paths is bounded by the platform -- Win32 measures
        // an ordinary path against MAX_PATH, 260 wide characters -- so if the
        // staging leaf is WIDER than the destination leaf, a root that the
        // installed artifact fits inside can still fail while being installed.
        // At 32 bytes it was: `.staging\` plus 64 hex is 73 against the
        // destination's 64, and a 144-character root, an ordinary Windows path,
        // failed partway through an install having already written some of it.
        //
        // 27 bytes is 54 hex characters, so `.staging\` plus the token is 63
        // against the destination's 64. The destination is therefore always the
        // binding constraint: whatever root the artifact can live in, it can be
        // installed into. The unguessability the staging leaf exists for is
        // untouched at 216 bits -- it is what stops anyone pre-creating the
        // directory or planting a link inside it before the writes land.
        constexpr auto k_stagingTokenBytes = uint32{27};

        [[nodiscard]]
        auto isLocalReference(std::string_view value) -> bool
        {
            if (value.empty() || value.size() > 128U)
            {
                return false;
            }
            auto const alphanumeric = [](char character)
            {
                return (character >= 'A' && character <= 'Z')
                    || (character >= 'a' && character <= 'z')
                    || (character >= '0' && character <= '9');
            };
            if (!alphanumeric(value.front()))
            {
                return false;
            }
            return std::ranges::all_of(
                value,
                [alphanumeric](char character)
                {
                    return alphanumeric(character)
                        || character == '.'
                        || character == '_'
                        || character == ':'
                        || character == '-';
                }
            );
        }

        [[nodiscard]]
        auto isNamespacedIdentifier(std::string_view value) -> bool
        {
            auto const alphabetic = [](char character)
            {
                return (character >= 'A' && character <= 'Z')
                    || (character >= 'a' && character <= 'z');
            };
            auto const alphanumeric = [alphabetic](char character)
            {
                return alphabetic(character)
                    || (character >= '0' && character <= '9');
            };
            if (value.empty() || !alphabetic(value.front()))
            {
                return false;
            }
            auto hasSeparator  = false;
            auto startsSegment = false;
            for (auto const character : value)
            {
                if (character == '.')
                {
                    if (startsSegment)
                    {
                        return false;
                    }
                    hasSeparator  = true;
                    startsSegment = true;
                    continue;
                }
                if (startsSegment && !alphanumeric(character))
                {
                    return false;
                }
                if (
                    !alphanumeric(character)
                    && character != '_'
                    && character != '-'
                )
                {
                    return false;
                }
                startsSegment = false;
            }
            return hasSeparator && !startsSegment;
        }

        // A member the schema has already declared required, so its absence
        // would be a defect in this reader rather than in the document. This
        // is the reader for the Operator's OWN stored rows, whose bytes were
        // validated before they were written; a stored envelope missing a
        // member is an internal invariant, not an input to be refused.
        [[nodiscard]]
        auto member(
            json::Value const& object UF_LIFETIME_BOUND,
            std::string_view name
        ) -> json::Value const&
        {
            auto const* const p_member = object.find(name);
            UF_CHECK(p_member != nullptr);
            return *p_member;
        }

        // A member the proposal contract requires, read out of the parsed
        // derive output. The parsed value is authoritative for what was
        // validated: a project may pin a permissive observation schema, so a
        // member the contract requires can be absent from a document that
        // schema stamped. Refusing with the member's name is what keeps that
        // failure closed instead of terminating.
        [[nodiscard]]
        auto checkedMember(
            json::Value const& object UF_LIFETIME_BOUND,
            std::string_view name
        ) -> Result<json::Value const*>
        {
            auto const* const p_member = object.find(name);
            if (p_member == nullptr)
            {
                return fail(
                    ProjectObservationErrorCode::MalformedProposal,
                    "Derived observation output is missing member '"
                        + std::string{name} + "'"
                );
            }
            return p_member;
        }

        [[nodiscard]]
        auto parseProjectToolPreconditionStatus(
            json::Value const& statusValue
        ) -> Result<ProjectToolPreconditionStatus>
        {
            static constexpr auto k_statuses = std::array{
                std::pair{
                    std::string_view{"Known"},
                    ProjectToolPreconditionStatus::Known,
                },
                std::pair{
                    std::string_view{"Unknown"},
                    ProjectToolPreconditionStatus::Unknown,
                },
                std::pair{
                    std::string_view{"Stale"},
                    ProjectToolPreconditionStatus::Stale,
                },
                std::pair{
                    std::string_view{"Conflict"},
                    ProjectToolPreconditionStatus::Conflict,
                },
            };
            if (statusValue.kind() != json::ValueKind::String)
            {
                return fail(
                    ProjectObservationErrorCode::MalformedProposal,
                    "Project tool precondition status is not a string"
                );
            }
            auto const found = std::ranges::find(
                k_statuses,
                statusValue.string(),
                &std::pair<std::string_view, ProjectToolPreconditionStatus>::first
            );
            if (found == k_statuses.end())
            {
                return fail(
                    ProjectObservationErrorCode::MalformedProposal,
                    "Project tool precondition status is outside its wire domain"
                );
            }
            return found->second;
        }

        // The schema owner already parsed and validated the derive output once;
        // this maps the value it retained to the proposal the Operator mints
        // from, with no second parse of the bytes. A member missing from the
        // output or of the wrong kind is a MalformedProposal, the same code the
        // shape checks below report for a proposal that never fits the wire.
        [[nodiscard]]
        auto proposalFromDerived(json::Value const& document)
            -> Result<ProjectObservationProposal>
        {
            if (document.kind() != json::ValueKind::Object)
            {
                return fail(
                    ProjectObservationErrorCode::MalformedProposal,
                    "Derived observation output is not an object"
                );
            }
            auto const* const p_schema = document.find("schema");
            if (
                p_schema == nullptr
                || p_schema->kind() != json::ValueKind::String
                || p_schema->string() != "umbraflow-project-observation-proposal/v1"
            )
            {
                return fail(
                    ProjectObservationErrorCode::MalformedProposal,
                    "Derived observation output is not a project observation proposal"
                );
            }

            UF_TRY_VALUE(
                p_preconditions,
                checkedMember(document, "project_tool_preconditions")
            );
            auto const& preconditions = *p_preconditions;
            if (preconditions.kind() != json::ValueKind::Array)
            {
                return fail(
                    ProjectObservationErrorCode::MalformedProposal,
                    "Project observation proposal tool preconditions are not an array"
                );
            }
            auto projectToolPreconditions = std::vector<ProjectToolPrecondition>{};
            projectToolPreconditions.reserve(preconditions.items().size());
            for (auto const& precondition : preconditions.items())
            {
                if (precondition.kind() != json::ValueKind::Object)
                {
                    return fail(
                        ProjectObservationErrorCode::MalformedProposal,
                        "Project observation proposal tool precondition is not an object"
                    );
                }
                UF_TRY_VALUE(p_name, checkedMember(precondition, "name"));
                auto const& name = *p_name;
                if (name.kind() != json::ValueKind::String)
                {
                    return fail(
                        ProjectObservationErrorCode::MalformedProposal,
                        "Project tool precondition name is not a string"
                    );
                }
                UF_TRY_VALUE(
                    p_status,
                    checkedMember(precondition, "status")
                );
                UF_TRY_VALUE(
                    status,
                    parseProjectToolPreconditionStatus(*p_status)
                );
                projectToolPreconditions.emplace_back(
                    ProjectToolPrecondition{
                        .name   = std::string{name.string()},
                        .status = status,
                    }
                );
            }

            UF_TRY_VALUE(
                p_instances,
                checkedMember(document, "observed_instance_proposals")
            );
            auto const& instances = *p_instances;
            if (instances.kind() != json::ValueKind::Array)
            {
                return fail(
                    ProjectObservationErrorCode::MalformedProposal,
                    "Project observation proposal instances are not an array"
                );
            }
            auto observedInstanceProposals = std::vector<ObservedInstanceProposal>{};
            observedInstanceProposals.reserve(instances.items().size());
            for (auto const& instance : instances.items())
            {
                if (instance.kind() != json::ValueKind::Object)
                {
                    return fail(
                        ProjectObservationErrorCode::MalformedProposal,
                        "Project observation proposal instance is not an object"
                    );
                }
                auto proposal = ObservedInstanceProposal{};
                UF_TRY_VALUE(p_localRef, checkedMember(instance, "local_ref"));
                auto const& localRef = *p_localRef;
                if (localRef.kind() != json::ValueKind::String)
                {
                    return fail(
                        ProjectObservationErrorCode::MalformedProposal,
                        "Observed instance local_ref is not a string"
                    );
                }
                proposal.localRef = std::string{localRef.string()};
                auto const* const p_parent = instance.find("parent_local_ref");
                if (p_parent != nullptr)
                {
                    if (p_parent->kind() != json::ValueKind::String)
                    {
                        return fail(
                            ProjectObservationErrorCode::MalformedProposal,
                            "Observed instance parent_local_ref is not a string"
                        );
                    }
                    proposal.parentLocalRef = std::string{p_parent->string()};
                }
                UF_TRY_VALUE(p_kind, checkedMember(instance, "kind"));
                auto const& kind = *p_kind;
                if (kind.kind() != json::ValueKind::String)
                {
                    return fail(
                        ProjectObservationErrorCode::MalformedProposal,
                        "Observed instance kind is not a string"
                    );
                }
                proposal.kind = std::string{kind.string()};
                UF_TRY_VALUE(
                    p_schemaId,
                    checkedMember(instance, "identity_schema_id")
                );
                auto const& schemaId = *p_schemaId;
                if (schemaId.kind() != json::ValueKind::String)
                {
                    return fail(
                        ProjectObservationErrorCode::MalformedProposal,
                        "Observed instance identity_schema_id is not a string"
                    );
                }
                proposal.identitySchemaId = std::string{schemaId.string()};
                UF_TRY_VALUE(
                    p_basis,
                    checkedMember(instance, "semantic_identity_basis")
                );
                proposal.semanticIdentityBasis = *p_basis;
                UF_TRY_VALUE(
                    p_opaque,
                    checkedMember(instance, "opaque_project_payload")
                );
                proposal.opaqueProjectPayload = *p_opaque;
                observedInstanceProposals.emplace_back(std::move(proposal));
            }

            UF_TRY_VALUE(
                p_canonicalPayload,
                checkedMember(document, "canonical_opaque_payload")
            );
            return ProjectObservationProposal{
                .schema                    = "umbraflow-project-observation-proposal/v1",
                .canonicalOpaquePayload    = *p_canonicalPayload,
                .projectToolPreconditions  = std::move(projectToolPreconditions),
                .observedInstanceProposals = std::move(observedInstanceProposals),
            };
        }

        // Rebuilds the scope a session was pinned under from its three stored
        // columns. The DDL CHECK already guarantees the generation digits and
        // the kind/zero pairing, so a failure here is a defect in the stored
        // tuple rather than in a caller.
        [[nodiscard]]
        auto restoreWorldScopeKind(
            std::string_view wire
        ) -> std::optional<ObservedInstanceWorldScopeKind>
        {
            static constexpr auto k_kinds = std::array{
                std::pair{
                    std::string_view{"account"},
                    ObservedInstanceWorldScopeKind::Account,
                },
                std::pair{
                    std::string_view{"run"},
                    ObservedInstanceWorldScopeKind::Run,
                },
            };
            auto const found = std::ranges::find(
                k_kinds,
                wire,
                &std::pair<std::string_view, ObservedInstanceWorldScopeKind>::first
            );
            if (found == k_kinds.end())
            {
                return std::nullopt;
            }
            return found->second;
        }

        [[nodiscard]]
        auto restoreSessionWorldScope(
            std::string_view kindWire,
            std::string_view scopeId,
            std::string_view generationText
        ) -> Result<ObservedInstanceWorldScope>
        {
            auto const kind = restoreWorldScopeKind(kindWire);
            if (!kind)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "Session world scope kind is outside its stored domain"
                );
            }
            auto generation = uint64{};
            auto const* const begin = std::to_address(generationText.begin());
            auto const* const end   = std::to_address(generationText.end());
            auto const parsed       = std::from_chars(begin, end, generation);
            if (parsed.ec != std::errc{} || parsed.ptr != end)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "Session world scope generation is not a stored non-negative integer"
                );
            }
            auto scopeIdText = std::string{scopeId};
            switch (*kind)
            {
            case ObservedInstanceWorldScopeKind::Account:
                return ObservedInstanceWorldScope::account(
                    std::move(scopeIdText),
                    generation
                );
            case ObservedInstanceWorldScopeKind::Run:
                return ObservedInstanceWorldScope::run(
                    std::move(scopeIdText),
                    generation
                );
            }
            return fail(
                AutomationErrorKind::InternalInvariant,
                "Session world scope kind is outside its stored domain"
            );
        }

        [[nodiscard]]
        auto requireExecutableRegistrationFormat(
            sqlite3_stmt* statement,
            int column
        ) -> Status
        {
            if (
                sqlite3_column_int64(statement, column) != 3
                || columnText(statement, column + 1) != "module_manifest"
            )
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "legacy registration is audit-only"
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto requireExecutableRegistration(
            sqlite3* database,
            ContentHash registrationHash
        ) -> Status
        {
            UF_TRY_VALUE(
                query,
                prepare(
                    database,
                    "SELECT registration_format, plugin_identity_kind "
                    "FROM project_registrations "
                    "WHERE registration_hash=?1"
                )
            );
            UF_TRY(bindText(database, query.get(), 1, registrationHash.hex()));
            if (sqlite3_step(query.get()) == SQLITE_ROW)
            {
                return requireExecutableRegistrationFormat(query.get(), 0);
            }
            return ok();
        }

        [[nodiscard]]
        auto validateProjectObservationProposalShape(
            ProjectObservationProposal const& proposal
        ) -> Status
        {
            if (proposal.schema != "umbraflow-project-observation-proposal/v1")
            {
                return fail(
                    ProjectObservationErrorCode::MalformedProposal,
                    "Project observation proposal carries the wrong schema tag"
                );
            }
            // The empty name is forbidden, not merely odd: a Tool call
            // resolves its observation to the binding's local_ref and the
            // deliver check compares it with the receipt's own target, while
            // the migration sentinel for pre-local_ref bindings is exactly the
            // empty name -- a binding that names nothing could be mistaken for
            // one that names a target.
            for (auto const& instance : proposal.observedInstanceProposals)
            {
                if (
                    !isLocalReference(instance.localRef)
                    || (
                        instance.parentLocalRef.has_value()
                        && !isLocalReference(*instance.parentLocalRef)
                    )
                    || !isNamespacedIdentifier(instance.kind)
                    || instance.identitySchemaId.empty()
                    || instance.identitySchemaId.size() > 512U
                    || !isValidUtf8(instance.identitySchemaId)
                    || instance.semanticIdentityBasis.kind() != json::ValueKind::Object
                )
                {
                    return fail(
                        ProjectObservationErrorCode::MalformedProposal,
                        "Project observation proposal instance is outside its wire shape"
                    );
                }
            }
            for (auto const& precondition : proposal.projectToolPreconditions)
            {
                if (!isNamespacedIdentifier(precondition.name))
                {
                    return fail(
                        ProjectObservationErrorCode::PreconditionNameNotNamespaced,
                        "Project tool precondition name is not namespaced"
                    );
                }
            }
            for (auto const& precondition : proposal.projectToolPreconditions)
            {
                switch (precondition.status)
                {
                case ProjectToolPreconditionStatus::Known:
                case ProjectToolPreconditionStatus::Unknown:
                case ProjectToolPreconditionStatus::Stale:
                case ProjectToolPreconditionStatus::Conflict:
                    break;
                default:
                    return fail(
                        ProjectObservationErrorCode::PreconditionStatusOutsideFactDomain,
                        "Project tool precondition status is outside its four-value domain"
                    );
                }
            }
            return ok();
        }

        [[nodiscard]]
        auto validateProjectObservationProposalRelations(
            ProjectObservationProposal const& proposal
        ) -> Status
        {
            auto preconditionNames = std::set<std::string>{};
            for (auto const& precondition : proposal.projectToolPreconditions)
            {
                if (!preconditionNames.emplace(precondition.name).second)
                {
                    return fail(
                        ProjectObservationErrorCode::DuplicatePreconditionName,
                        "Project observation proposal repeats a precondition name"
                    );
                }
            }

            auto indexes = std::map<std::string, std::size_t>{};
            for (
                auto index = std::size_t{};
                index < proposal.observedInstanceProposals.size();
                ++index
            )
            {
                if (!indexes.emplace(
                    proposal.observedInstanceProposals[index].localRef,
                    index
                ).second)
                {
                    return fail(
                        ProjectObservationErrorCode::DuplicateObservedInstanceLocalRef,
                        "Project observation proposal repeats an instance local_ref"
                    );
                }
            }

            auto parentIndexes = std::vector<std::optional<std::size_t>>{};
            parentIndexes.reserve(proposal.observedInstanceProposals.size());
            for (auto const& instance : proposal.observedInstanceProposals)
            {
                if (!instance.parentLocalRef)
                {
                    parentIndexes.emplace_back(std::nullopt);
                    continue;
                }
                auto const found = indexes.find(*instance.parentLocalRef);
                if (found == indexes.end())
                {
                    return fail(
                        ProjectObservationErrorCode::ObservedInstanceParentMissing,
                        "Observed instance parent_local_ref is absent from its proposal"
                    );
                }
                parentIndexes.emplace_back(found->second);
            }

            enum class VisitState : uint8
            {
                Unvisited,
                Visiting,
                Visited,
            };
            auto states = std::vector<VisitState>(
                parentIndexes.size(),
                VisitState::Unvisited
            );
            auto const containsCycle = [
                &parentIndexes,
                &states
            ](auto const& visit, std::size_t index) -> bool
            {
                switch (states[index])
                {
                case VisitState::Visiting:  return true;
                case VisitState::Visited:   return false;
                case VisitState::Unvisited: break;
                }
                states[index] = VisitState::Visiting;
                if (
                    parentIndexes[index].has_value()
                    && visit(visit, *parentIndexes[index])
                )
                {
                    return true;
                }
                states[index] = VisitState::Visited;
                return false;
            };
            for (
                auto index = std::size_t{};
                index < parentIndexes.size();
                ++index
            )
            {
                if (containsCycle(containsCycle, index))
                {
                    return fail(
                        ProjectObservationErrorCode::ObservedInstanceParentCycle,
                        "Observed instance parent relation contains a cycle"
                    );
                }
            }
            return ok();
        }

        // Every observed instance id a command's canonical arguments spell,
        // across every nesting level. The id format is the ledger's own
        // ("oi1_" plus the random hex the mint drew), so a string carrying the
        // prefix IS an instance id wherever the project put it; the scan is
        // what lets the production entry gate resolve ids the framework does
        // not parse out of project-shaped arguments.
        auto collectObservedInstanceIds(
            json::Value const& value,
            std::vector<std::string>& ids
        ) -> void
        {
            switch (value.kind())
            {
            case json::ValueKind::Object:
                for (auto const& [name, member] : value.members())
                {
                    static_cast<void>(name);
                    collectObservedInstanceIds(member, ids);
                }
                break;
            case json::ValueKind::Array:
                for (auto const& item : value.items())
                {
                    collectObservedInstanceIds(item, ids);
                }
                break;
            case json::ValueKind::String:
                if (value.string().starts_with("oi1_"))
                {
                    ids.emplace_back(value.string());
                }
                break;
            case json::ValueKind::Null:
            case json::ValueKind::Boolean:
            case json::ValueKind::Number:
                break;
            }
        }

        [[nodiscard]]
        auto observedInstanceAuthorityBytes(
            ObservedInstanceContext const& context,
            ObservedInstanceWorldScope const& worldScope,
            ObservedInstanceProposal const& proposal
        ) -> std::string
        {
            auto output = std::string{"{\"identity_schema_id\":"};
            appendJsonString(output, proposal.identitySchemaId);
            output += ",\"kind\":";
            appendJsonString(output, proposal.kind);
            output += ",\"plugin_id\":";
            appendJsonString(output, context.pluginId);
            output += ",\"project_instance_key\":";
            appendJsonString(output, context.projectInstanceKey);
            output += ",\"project_registration_hash\":";
            appendJsonString(output, context.projectRegistrationHash.hex());
            output += ",\"schema\":\"umbraflow-observed-instance-authority-input/v1\"";
            output += ",\"semantic_identity_basis\":";
            output += json::canonicalBytes(proposal.semanticIdentityBasis);
            output += ",\"world_scope\":{\"generation\":";
            output += std::to_string(worldScope.generation());
            output += ",\"kind\":";
            appendJsonString(
                output,
                observedInstanceWorldScopeKindWireName(worldScope.kind())
            );
            output += ",\"scope_id\":";
            appendJsonString(output, worldScope.scopeId());
            output += "}}";
            return output;
        }

        [[nodiscard]]
        auto readObservedInstanceContext(
            sqlite3* database,
            uint64 currentSessionEpoch,
            ControlLease const& lease
        ) -> Result<ObservedInstanceContext>
        {
            UF_TRY_VALUE(
                query,
                prepare(
                    database,
                    "SELECT active_lease.lease_id, active_lease.session_id, "
                    "active_lease.controller_id, active_lease.session_epoch, "
                    "active_lease.fencing_token, active_lease.revision, "
                    "active_lease.capability_profile_hash, session.project_instance_key, "
                    "session.project_registration_hash, registration.plugin_id, "
                    "registration.plugin_identity_hash, registration.registration_format, "
                    "registration.plugin_identity_kind "
                    "FROM control_leases active_lease "
                    "JOIN sessions session ON session.session_id=active_lease.session_id "
                    "JOIN project_registrations registration ON "
                    "registration.registration_hash=session.project_registration_hash "
                    "WHERE active_lease.controlled_target_id=?1 AND session.active=1 "
                    "AND session.session_epoch=?2"
                )
            );
            UF_TRY(bindText(database, query.get(), 1, lease.controlledTargetId));
            UF_TRY(bindInteger(database, query.get(), 2, currentSessionEpoch));
            if (sqlite3_step(query.get()) != SQLITE_ROW)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Observed instance operation requires an active control lease"
                );
            }
            UF_TRY(requireExecutableRegistrationFormat(query.get(), 11));
            auto const matches = columnText(query.get(), 0) == lease.leaseId
                && columnText(query.get(), 1) == lease.sessionId
                && columnText(query.get(), 2) == lease.controllerId
                && static_cast<uint64>(sqlite3_column_int64(query.get(), 3))
                    == lease.sessionEpoch
                && static_cast<uint64>(sqlite3_column_int64(query.get(), 4))
                    == lease.fencingToken
                && static_cast<uint64>(sqlite3_column_int64(query.get(), 5))
                    == lease.revision
                && columnText(query.get(), 6) == lease.capabilityProfileHash.hex()
                && lease.sessionEpoch == currentSessionEpoch;
            if (!matches)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Observed instance control lease was superseded"
                );
            }
            UF_TRY_VALUE(registrationHash, parseHashColumn(columnText(query.get(), 8)));
            return ObservedInstanceContext{
                .pluginId                 = columnText(query.get(), 9),
                .pluginModuleManifestHash = columnText(query.get(), 10),
                .projectRegistrationHash  = registrationHash,
                .projectInstanceKey       = columnText(query.get(), 7),
            };
        }

        [[nodiscard]]
        auto mintObservedInstanceBinding(
            sqlite3* database,
            ObservedInstanceContext const& context,
            ObservedInstanceWorldScope const& worldScope,
            std::string const& canonicalAuthority,
            std::string_view localRef
        ) -> Result<std::string>
        {
            UF_TRY_VALUE(
                existing,
                prepare(
                    database,
                    "SELECT observed_instance_id FROM observed_instance_bindings "
                    "WHERE canonical_authority=?1"
                )
            );
            UF_TRY(bindText(database, existing.get(), 1, canonicalAuthority));
            if (sqlite3_step(existing.get()) == SQLITE_ROW)
            {
                return columnText(existing.get(), 0);
            }

            for (;;)
            {
                UF_TRY_VALUE(randomBytes, randomToken(database, k_opaqueTokenBytes));
                auto observedInstanceId = std::string{"oi1_"} + randomBytes;
                UF_TRY_VALUE(
                    collision,
                    prepare(
                        database,
                        "SELECT 1 FROM observed_instance_bindings "
                        "WHERE observed_instance_id=?1"
                    )
                );
                UF_TRY(bindText(database, collision.get(), 1, observedInstanceId));
                if (sqlite3_step(collision.get()) == SQLITE_ROW)
                {
                    continue;
                }

                UF_TRY_VALUE(
                    insert,
                    prepare(
                        database,
                        "INSERT INTO observed_instance_bindings("
                        "canonical_authority, observed_instance_id, plugin_id, "
                        "project_registration_hash, project_instance_key, "
                        "world_scope_kind, world_scope_id, world_scope_generation, "
                        "local_ref) "
                        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)"
                    )
                );
                UF_TRY(bindText(database, insert.get(), 1, canonicalAuthority));
                UF_TRY(bindText(database, insert.get(), 2, observedInstanceId));
                UF_TRY(bindText(database, insert.get(), 3, context.pluginId));
                UF_TRY(bindText(
                    database,
                    insert.get(),
                    4,
                    context.projectRegistrationHash.hex()
                ));
                UF_TRY(bindText(database, insert.get(), 5, context.projectInstanceKey));
                UF_TRY(bindText(
                    database,
                    insert.get(),
                    6,
                    observedInstanceWorldScopeKindWireName(worldScope.kind())
                ));
                UF_TRY(bindText(database, insert.get(), 7, worldScope.scopeId()));
                UF_TRY(bindText(
                    database,
                    insert.get(),
                    8,
                    std::to_string(worldScope.generation())
                ));
                UF_TRY(bindText(database, insert.get(), 9, localRef));
                UF_TRY(expectDone(database, insert.get()));
                return observedInstanceId;
            }
        }

        [[nodiscard]]
        auto finalProjectObservationValue(
            json::Value const& canonicalOpaquePayload,
            std::span<ProjectToolPrecondition const> preconditions,
            std::span<ObservedInstance const> instances
        ) -> json::Value
        {
            auto preconditionValues = std::vector<json::Value>{};
            preconditionValues.reserve(preconditions.size());
            for (auto const& precondition : preconditions)
            {
                preconditionValues.emplace_back(json::Value::ofObject({
                    json::Member{"name", json::Value::ofString(precondition.name)},
                    json::Member{
                        "status",
                        json::Value::ofString(std::string{
                            projectToolPreconditionStatusWireName(precondition.status)
                        }),
                    },
                }));
            }

            auto instanceValues = std::vector<json::Value>{};
            instanceValues.reserve(instances.size());
            for (auto const& instance : instances)
            {
                auto members = std::vector<json::Member>{};
                members.emplace_back(
                    "observed_instance_id",
                    json::Value::ofString(instance.observedInstanceId.value())
                );
                if (instance.parentObservedInstanceId)
                {
                    members.emplace_back(
                        "parent_observed_instance_id",
                        json::Value::ofString(
                            instance.parentObservedInstanceId->value()
                        )
                    );
                }
                members.emplace_back("kind", json::Value::ofString(instance.kind));
                members.emplace_back(
                    "opaque_project_payload",
                    instance.opaqueProjectPayload
                );
                instanceValues.emplace_back(json::Value::ofObject(std::move(members)));
            }

            return json::Value::ofObject({
                json::Member{
                    "canonical_opaque_payload",
                    canonicalOpaquePayload,
                },
                json::Member{
                    "observed_instances",
                    json::Value::ofArray(std::move(instanceValues)),
                },
                json::Member{
                    "project_tool_preconditions",
                    json::Value::ofArray(std::move(preconditionValues)),
                },
                json::Member{
                    "schema",
                    json::Value::ofString(std::string{ProjectObservation::schema()}),
                },
            });
        }

        [[nodiscard]]
        auto initialize(sqlite3* database) -> Status
        {
            if (sqlite3_busy_timeout(database, 5'000) != SQLITE_OK)
            {
                return databaseFailure(database, "could not set busy timeout");
            }

            UF_TRY(execute(
                database,
                "PRAGMA journal_mode=WAL;"
                "PRAGMA foreign_keys=ON;"
                "PRAGMA synchronous=FULL;"
                "PRAGMA trusted_schema=OFF;"
            ));

            UF_TRY_VALUE(journalMode, readDatabaseText(database, "PRAGMA journal_mode"));
            UF_TRY_VALUE(
                foreignKeys,
                readDatabaseInteger(database, "PRAGMA foreign_keys")
            );
            UF_TRY_VALUE(
                synchronous,
                readDatabaseInteger(database, "PRAGMA synchronous")
            );
            UF_TRY_VALUE(
                trustedSchema,
                readDatabaseInteger(database, "PRAGMA trusted_schema")
            );
            if (
                journalMode != "wal"
                || foreignKeys != 1U
                || synchronous != 2U
                || trustedSchema != 0U
            )
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    "Operator database safety PRAGMA read-back failed"
                );
            }

            UF_TRY_VALUE(
                schemaObjectCount,
                readDatabaseInteger(
                    database,
                    "SELECT COUNT(*) FROM sqlite_schema WHERE name NOT LIKE 'sqlite_%'"
                )
            );
            if (schemaObjectCount != 0U)
            {
                return upgradeOrVerifyExactDatabaseSchema(database);
            }

            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY(execute(
                database,
                R"sql(
                    CREATE TABLE IF NOT EXISTS runtime_artifacts(
                        artifact_root_hash TEXT PRIMARY KEY
                    ) STRICT;

                    CREATE TABLE IF NOT EXISTS runtime_state(
                        singleton INTEGER PRIMARY KEY CHECK(singleton = 1),
                        current_session_epoch INTEGER NOT NULL
                            CHECK(current_session_epoch >= 0),
                        installed_generation INTEGER NOT NULL
                            CHECK(installed_generation >= 0),
                        active_runtime_artifact_root_hash TEXT
                            REFERENCES runtime_artifacts(artifact_root_hash)
                    ) STRICT;
                    INSERT INTO runtime_state(
                        singleton,
                        current_session_epoch,
                        installed_generation,
                        active_runtime_artifact_root_hash
                    ) VALUES(1, 0, 0, NULL);

)sql"
                R"sql(
                    CREATE TABLE IF NOT EXISTS fencing_high_water(
                        controlled_target_id TEXT PRIMARY KEY,
                        fencing_token INTEGER NOT NULL CHECK(fencing_token > 0)
                    ) STRICT;

                    CREATE TABLE IF NOT EXISTS control_leases(
                        controlled_target_id TEXT PRIMARY KEY,
                        lease_id TEXT NOT NULL UNIQUE,
                        session_id TEXT NOT NULL REFERENCES sessions(session_id),
                        controller_id TEXT NOT NULL,
                        session_epoch INTEGER NOT NULL CHECK(session_epoch > 0),
                        fencing_token INTEGER NOT NULL CHECK(fencing_token > 0),
                        revision INTEGER NOT NULL CHECK(revision > 0),
                        capability_profile_hash TEXT NOT NULL
                    ) STRICT;

                    CREATE TABLE IF NOT EXISTS control_transitions(
                        sequence INTEGER PRIMARY KEY AUTOINCREMENT,
                        controlled_target_id TEXT NOT NULL,
                        session_id TEXT NOT NULL,
                        controller_id TEXT NOT NULL,
                        lease_id TEXT NOT NULL,
                        session_epoch INTEGER NOT NULL,
                        fencing_token INTEGER NOT NULL,
                        transition TEXT NOT NULL,
                        reason TEXT NOT NULL
                    ) STRICT;

)sql"
                // One statement sequence, three literals: MSVC caps a single
                // string literal and the schema outgrew it here. Adjacent
                // literals concatenate before anything reads them, so the SQL
                // text -- and therefore k_operatorDatabaseSchemaIdentity, which
                // covers the STORED DDL -- is byte-identical to the unsplit
                // block. The seam must add no character of its own: it sits
                // between a newline and a newline, and it moves to wherever the
                // cap requires without moving the fingerprint, because the two
                // halves concatenate to the same bytes wherever it sits.
                R"sql(

                    -- What out-of-band human input leaves behind. It
                    -- deliberately has no tool_name, tool_version,
                    -- canonical_args or snapshot column, and no state: a
                    -- finding is a report that the world moved and never a
                    -- request, so nothing here can be read as one.
                    --
                    -- invalidated_snapshot_revision is the snapshot revision
                    -- this target had reached when the input was seen; every
                    -- token at or below it is refused afterwards, which is how
                    -- a keystroke stops the automation without terminating it.
                    CREATE TABLE IF NOT EXISTS external_input_findings(
                        finding_id TEXT PRIMARY KEY,
                        controlled_target_id TEXT NOT NULL,
                        session_epoch INTEGER NOT NULL CHECK(session_epoch > 0),
                        reporter_session_id TEXT NOT NULL
                            REFERENCES sessions(session_id),
                        detected_after_cursor INTEGER NOT NULL
                            CHECK(detected_after_cursor >= 0),
                        invalidated_snapshot_revision INTEGER NOT NULL
                            CHECK(invalidated_snapshot_revision >= 0),
                        required_action TEXT NOT NULL
                            CHECK(required_action IN (
                                'freeze_and_reobserve', 'freeze_and_reconcile'
                            )),
                        reason TEXT NOT NULL
                    ) STRICT;

                    -- What one online Agent binding has left to spend, and the
                    -- marker the no-progress rule compares each step against.
                    -- pinSession writes it from the exact AgentProfile bytes
                    -- the session manifest's agent_profile_hash names, so there
                    -- is no path from a ControllerBinding to any of these
                    -- numbers except downwards.
                    --
                    -- Each CHECK is the enforcement of its own ceiling and not
                    -- a second guard beside one in C++: a charge is an
                    -- unconditional decrement and the constraint is what
                    -- refuses it at zero. Relaxing a CHECK therefore lets one
                    -- more command through, which is what makes the ceiling
                    -- falsifiable.
                    --
                    -- The row is inert after a restart rather than reset: a new
                    -- session epoch deactivates every session the previous one
                    -- left behind, so no binding can be minted against this row
                    -- again and a re-pinned session starts from a new one.
                    -- There is deliberately no agent_profile_hash column. The
                    -- session row already names the manifest this budget was
                    -- pinned with, and that manifest names the profile, so a
                    -- column here would be a second spelling of a fact the
                    -- session already determines -- and one nothing reads.
                    CREATE TABLE IF NOT EXISTS agent_budgets(
                        session_id TEXT PRIMARY KEY REFERENCES sessions(session_id),
                        deadline_steady_millis INTEGER NOT NULL
                            CHECK(deadline_steady_millis > 0),
                        remaining_tool_calls INTEGER NOT NULL
                            CHECK(remaining_tool_calls >= 0),
                        remaining_mutations INTEGER NOT NULL
                            CHECK(remaining_mutations >= 0),
                        remaining_observations INTEGER NOT NULL
                            CHECK(remaining_observations >= 0),
                        remaining_risk_units INTEGER NOT NULL
                            CHECK(remaining_risk_units >= 0),
                        last_state_fingerprint TEXT NOT NULL,
                        last_command_fingerprint TEXT NOT NULL,
                        consecutive_no_progress_steps INTEGER NOT NULL
                            CHECK(consecutive_no_progress_steps >= 0)
                    ) STRICT;

                )sql"
            ));
            UF_TRY(execute(database, k_runtimeInstallationsDdl));
            UF_TRY(execute(database, k_observedInstanceBindingsDdl));
            UF_TRY(execute(database, k_sessionsDdl));
            UF_TRY(execute(database, k_oneActiveWriteSessionIndexDdl));
            UF_TRY(execute(database, k_projectRegistrationsDdl));
            UF_TRY(execute(database, k_projectInstancesDdl));
            UF_TRY(execute(database, k_projectObservationsDdl));
            UF_TRY(execute(database, k_snapshotsDdl));
            UF_TRY(execute(database, k_ledgerEventsDdl));
            UF_TRY(execute(database, k_sessionPoliciesDdl));
            UF_TRY(execute(database, k_availabilityHeadsDdl));
            UF_TRY(execute(database, k_schemaIdentityTransitionsDdl));
            UF_TRY(execute(database, k_genesisTransitionsDdl));
            UF_TRY(execute(database, k_releaseCapabilityApprovalsDdl));
            UF_TRY(execute(database, k_runtimeUpgradeFailuresDdl));
            UF_TRY(addToolIdentityPersistence(database));
            UF_TRY(verifyExactDatabaseSchema(database));
            return transaction.commit();
        }

        [[nodiscard]]
        auto pathToUtf8(std::filesystem::path const& path) -> std::string
        {
            auto const encoded = path.generic_u8string();
            return std::string{encoded.begin(), encoded.end()};
        }

        struct ReadOnlyOperatorLayout final
        {
            Database              database{};
            std::filesystem::path runtimeArtifactRoot{};
        };

        [[nodiscard]]
        auto openReadOnlyOperatorLayout(
            std::filesystem::path const& runtimeDirectory
        ) -> Result<ReadOnlyOperatorLayout>
        {
            if (runtimeDirectory.empty())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Operator runtime directory must not be empty"
                );
            }

            // A reader checks the existing layout and cannot bootstrap one.
            UF_TRY(requirePlainDirectory(runtimeDirectory, "Operator runtime root"));
            auto const runtimeArtifactRoot = runtimeDirectory / "runtime-artifacts";
            UF_TRY(requirePlainDirectory(
                runtimeArtifactRoot,
                "Production RuntimeArtifact root"
            ));

            auto const databasePath   = runtimeDirectory / "operator-runtime.sqlite";
            auto error                = std::error_code{};
            auto const databaseStatus = std::filesystem::symlink_status(
                databasePath,
                error
            );
            if (
                error
                || !std::filesystem::is_regular_file(databaseStatus)
                || std::filesystem::is_symlink(databaseStatus)
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Operator database path must be an existing plain file",
                    error
                );
            }

            // The SQLite capability makes the separation executable: adding a
            // write to this path yields SQLITE_READONLY rather than changing
            // the ledger.
            auto* rawDatabase = static_cast<sqlite3*>(nullptr);
            auto const openCode = sqlite3_open_v2(
                pathToUtf8(databasePath).c_str(),
                &rawDatabase,
                SQLITE_OPEN_READONLY
                    | SQLITE_OPEN_FULLMUTEX
                    | SQLITE_OPEN_EXRESCODE
                    | SQLITE_OPEN_NOFOLLOW,
                nullptr
            );
            auto database = Database{rawDatabase};
            if (openCode != SQLITE_OK || database == nullptr)
            {
                auto const detail = database == nullptr
                    ? std::string{"SQLite returned no database handle"}
                    : std::string{sqlite3_errmsg(database.get())};
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format("Could not open Operator database read-only: {}", detail)
                );
            }

            // No busy timeout: a live Coordinator holds an exclusive lock, so
            // a read-only command refuses immediately instead of stalling.
            UF_TRY(execute(database.get(), "PRAGMA trusted_schema=OFF"));
            UF_TRY(verifyExactDatabaseSchema(database.get()));
            return ReadOnlyOperatorLayout{
                .database            = std::move(database),
                .runtimeArtifactRoot = runtimeArtifactRoot,
            };
        }

        [[nodiscard]]
        auto deliveryOutcomeWireName(
            task::DeliveryOutcome outcome
        ) noexcept -> std::string_view
        {
            switch (outcome)
            {
            case task::DeliveryOutcome::NotDelivered: return "not_delivered";
            case task::DeliveryOutcome::Delivered: return "delivered";
            case task::DeliveryOutcome::TransportUnknown: return "transport_unknown";
            }

            UF_UNREACHABLE_MSG("Unknown DeliveryOutcome value");
        }


        [[nodiscard]]
        auto appendLedgerEvent(
            sqlite3* database,
            uint64 sessionEpoch,
            std::string_view controlledTargetId,
            LedgerEventKind kind,
            std::string_view subjectId
        ) -> Status;

        [[nodiscard]]
        auto sessionModeWireName(SessionMode mode) noexcept -> std::string_view
        {
            switch (mode)
            {
            case SessionMode::Read: return "read";
            case SessionMode::Write: return "write";
            }

            UF_UNREACHABLE_MSG("Unknown SessionMode value");
        }

        [[nodiscard]]
        auto ledgerEventWireName(LedgerEventKind kind) noexcept -> std::string_view
        {
            switch (kind)
            {
            case LedgerEventKind::ControlTransitioned: return "control_transitioned";
            case LedgerEventKind::ExternalInputDetected: return "external_input_detected";
            }

            UF_UNREACHABLE_MSG("Unknown LedgerEventKind value");
        }

        [[nodiscard]]
        auto parseLedgerEventKind(std::string_view value) -> Result<LedgerEventKind>
        {
            constexpr auto kinds = std::array{
                LedgerEventKind::ControlTransitioned,
                LedgerEventKind::ExternalInputDetected,
            };
            auto const match = std::ranges::find_if(
                kinds,
                [value](LedgerEventKind candidate)
                {
                    return ledgerEventWireName(candidate) == value;
                }
            );
            if (match == kinds.end())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format("Unknown ledger event kind: {}", value)
                );
            }
            return *match;
        }

        [[nodiscard]]
        auto externalInputActionWireName(
            ExternalInputAction action
        ) noexcept -> std::string_view
        {
            switch (action)
            {
            case ExternalInputAction::FreezeAndReobserve: return "freeze_and_reobserve";
            case ExternalInputAction::FreezeAndReconcile: return "freeze_and_reconcile";
            }

            UF_UNREACHABLE_MSG("Unknown ExternalInputAction value");
        }

        // The cursor as it stands right now: the sequence of the last appended
        // event, or 0 before the first one. Read inside the caller's
        // transaction, so nothing can commit between reading it and using it.
        [[nodiscard]]
        auto currentEventCursor(sqlite3* database) -> Result<uint64>
        {
            UF_TRY_VALUE(
                query,
                prepare(
                    database,
                    "SELECT COALESCE(MAX(sequence), 0) FROM ledger_events"
                )
            );
            if (sqlite3_step(query.get()) != SQLITE_ROW)
            {
                return databaseFailure(database, "could not read the event cursor");
            }
            return static_cast<uint64>(sqlite3_column_int64(query.get(), 0));
        }

        // Keep a bounded working set without breaking the snapshot join. A
        // retained snapshot makes its ProjectObservation a composition
        // dependency; that exception is expressed by the NOT EXISTS clause
        // rather than by a second lifetime flag that could disagree with the
        // foreign keys.
        [[nodiscard]]
        auto pruneSnapshotHistory(
            sqlite3* database,
            std::string_view sessionId,
            std::string_view pluginId,
            std::string_view projectInstanceKey
        ) -> Status
        {
            UF_TRY_VALUE(
                snapshotPrune,
                prepare(
                    database,
                    "DELETE FROM snapshots WHERE session_id=?1 "
                    "AND token NOT IN (SELECT token FROM snapshots "
                    "WHERE session_id=?1 ORDER BY snapshot_revision DESC LIMIT ?2)"
                )
            );
            UF_TRY(bindText(database, snapshotPrune.get(), 1, sessionId));
            UF_TRY(bindInteger(
                database,
                snapshotPrune.get(),
                2,
                k_retainedSnapshotHeads
            ));
            UF_TRY(expectDone(database, snapshotPrune.get()));

            UF_TRY_VALUE(
                observationPrune,
                prepare(
                    database,
                    "DELETE FROM project_observations WHERE plugin_id=?1 "
                    "AND project_instance_key=?2 AND revision NOT IN ("
                    "SELECT revision FROM project_observations WHERE plugin_id=?1 "
                    "AND project_instance_key=?2 ORDER BY revision DESC LIMIT ?3) "
                    "AND NOT EXISTS(SELECT 1 FROM snapshots snapshot "
                    "WHERE snapshot.plugin_id=project_observations.plugin_id "
                    "AND snapshot.project_instance_key="
                    "project_observations.project_instance_key "
                    "AND snapshot.project_observation_revision="
                    "project_observations.revision)"
                )
            );
            UF_TRY(bindText(database, observationPrune.get(), 1, pluginId));
            UF_TRY(bindText(
                database,
                observationPrune.get(),
                2,
                projectInstanceKey
            ));
            UF_TRY(bindInteger(
                database,
                observationPrune.get(),
                3,
                k_retainedObservationHeads
            ));
            return expectDone(database, observationPrune.get());
        }

        // Appended in the same transaction as the fact it records, never after
        // it. SQLite allows one writer at a time and every mutating path here
        // opens BEGIN IMMEDIATE, so sequences are assigned in commit order and
        // a rolled-back append leaves no gap.
        [[nodiscard]]
        auto appendLedgerEvent(
            sqlite3* database,
            uint64 sessionEpoch,
            std::string_view controlledTargetId,
            LedgerEventKind kind,
            std::string_view subjectId
        ) -> Status
        {
            UF_TRY_VALUE(
                insert,
                prepare(
                    database,
                    "INSERT INTO ledger_events(session_epoch, controlled_target_id, "
                    "kind, subject_id) VALUES(?1, ?2, ?3, ?4)"
                )
            );
            UF_TRY(bindInteger(database, insert.get(), 1, sessionEpoch));
            UF_TRY(bindText(database, insert.get(), 2, controlledTargetId));
            UF_TRY(bindText(database, insert.get(), 3, ledgerEventWireName(kind)));
            UF_TRY(bindText(database, insert.get(), 4, subjectId));
            UF_TRY(expectDone(database, insert.get()));
            UF_TRY_VALUE(
                prune,
                prepare(
                    database,
                    "DELETE FROM ledger_events WHERE sequence NOT IN ("
                    "SELECT sequence FROM ledger_events "
                    "ORDER BY sequence DESC LIMIT ?1)"
                )
            );
            UF_TRY(bindInteger(database, prune.get(), 1, k_retainedLedgerEvents));
            return expectDone(database, prune.get());
        }

        // The clock the Agent time budget is measured on, read by the Operator
        // and never supplied by a caller: a controller that could state the
        // current instant could state one before its own deadline, and the
        // budget would be a suggestion.
        //
        // Steady rather than wall, because a budget a clock adjustment can
        // widen is not a budget. The stored deadline is therefore meaningful
        // only inside the process that wrote it -- which is exactly the
        // lifetime of the session epoch it belongs to, and the same reason the
        // budget does not survive a restart.
        [[nodiscard]]
        auto steadyMillisecondsNow() -> Result<uint64>
        {
            auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                MonotonicInstant::now().timePoint().time_since_epoch()
            ).count();
            if (elapsed < 0)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "Steady clock reads before its own epoch"
                );
            }
            return static_cast<uint64>(elapsed);
        }

        // One binding's remaining ceilings and its progress marker.
        struct AgentBudgetState final
        {
            std::string lastStateFingerprint{};
            std::string lastCommandFingerprint{};
            uint64      deadlineSteadyMillis{};
            uint64      remainingToolCalls{};
            uint64      remainingMutations{};
            uint64      remainingObservations{};
            uint64      remainingRiskUnits{};
            uint64      consecutiveNoProgressSteps{};
        };

        // Every pinned session has a budget row, whoever controls it, so an
        // absent one is a broken invariant rather than a controller that
        // happens to run without ceilings. A ceiling the operator declared
        // unbounded is k_unboundedBudget in that row, which is a number the row
        // carries and not a hole in it.
        [[nodiscard]]
        auto readAgentBudget(
            sqlite3* database,
            std::string_view sessionId
        ) -> Result<AgentBudgetState>
        {
            UF_TRY_VALUE(
                query,
                prepare(
                    database,
                    "SELECT deadline_steady_millis, remaining_tool_calls, "
                    "remaining_mutations, remaining_observations, "
                    "remaining_risk_units, last_state_fingerprint, "
                    "last_command_fingerprint, consecutive_no_progress_steps "
                    "FROM agent_budgets WHERE session_id=?1"
                )
            );
            UF_TRY(bindText(database, query.get(), 1, sessionId));
            if (sqlite3_step(query.get()) != SQLITE_ROW)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    std::format(
                        "pinned session {} carries no budget row",
                        sessionId
                    )
                );
            }
            return AgentBudgetState{
                .lastStateFingerprint   = columnText(query.get(), 5),
                .lastCommandFingerprint = columnText(query.get(), 6),
                .deadlineSteadyMillis   = static_cast<uint64>(
                    sqlite3_column_int64(query.get(), 0)
                ),
                .remainingToolCalls     = static_cast<uint64>(
                    sqlite3_column_int64(query.get(), 1)
                ),
                .remainingMutations     = static_cast<uint64>(
                    sqlite3_column_int64(query.get(), 2)
                ),
                .remainingObservations  = static_cast<uint64>(
                    sqlite3_column_int64(query.get(), 3)
                ),
                .remainingRiskUnits     = static_cast<uint64>(
                    sqlite3_column_int64(query.get(), 4)
                ),
                .consecutiveNoProgressSteps = static_cast<uint64>(
                    sqlite3_column_int64(query.get(), 7)
                ),
            };
        }

        // No in-class hash default: every snapshot is content-addressed before
        // it can enter an admission attempt.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
        struct ToolBudgetSnapshot final
        {
            std::string bytes{};
            ContentHash hash;
        };

        [[nodiscard]]
        auto toolBudgetSnapshot(
            AgentBudgetState const& budget
        ) -> Result<ToolBudgetSnapshot>
        {
            auto bytes = std::format(
                "{{\"consecutive_no_progress_steps\":{},"
                "\"deadline_steady_millis\":{},"
                "\"remaining_mutations\":{},"
                "\"remaining_observations\":{},"
                "\"remaining_risk_units\":{},"
                "\"remaining_tool_calls\":{}}}",
                budget.consecutiveNoProgressSteps,
                budget.deadlineSteadyMillis,
                budget.remainingMutations,
                budget.remainingObservations,
                budget.remainingRiskUnits,
                budget.remainingToolCalls
            );
            UF_TRY_VALUE(hash, sha256(std::as_bytes(std::span{bytes})));
            return ToolBudgetSnapshot{
                .bytes = std::move(bytes),
                .hash  = hash,
            };
        }

        // Compared, never decremented: elapsed time is not a quantity the
        // ledger hands out. Inclusive at the limit, so a step submitted at the
        // deadline itself is still inside the budget and only one past it is
        // not.
        [[nodiscard]]
        auto requireWithinAgentDeadline(AgentBudgetState const& budget) -> Status
        {
            UF_TRY_VALUE(now, steadyMillisecondsNow());
            if (now > budget.deadlineSteadyMillis)
            {
                return fail(
                    AutomationErrorKind::Timeout,
                    "Agent time budget expired before this call"
                );
            }
            return ok();
        }

        // Spends one column of one binding's budget. There is deliberately no
        // comparison here: the column's own CHECK is what refuses the spend at
        // zero, so the ceiling has exactly one spelling and relaxing that
        // constraint lets one more call through.
        [[nodiscard]]
        auto chargeAgentBudget(
            sqlite3* database,
            std::string_view sessionId,
            std::string_view sql,
            uint64 amount,
            std::string_view exhausted
        ) -> Status
        {
            UF_TRY_VALUE(update, prepare(database, sql));
            UF_TRY(bindText(database, update.get(), 1, sessionId));
            UF_TRY(bindInteger(database, update.get(), 2, amount));
            auto const step = sqlite3_step(update.get());
            if (step == SQLITE_DONE)
            {
                return ok();
            }
            if ((step & 0xFF) == SQLITE_CONSTRAINT)
            {
                return fail(AutomationErrorKind::ActionRejected, std::string{exhausted});
            }
            return databaseFailure(database, "could not charge an agent budget");
        }

        [[nodiscard]]
        auto parseSessionMode(std::string_view value) -> Result<SessionMode>
        {
            constexpr auto modes = std::array{SessionMode::Read, SessionMode::Write};
            auto const match = std::ranges::find_if(
                modes,
                [value](SessionMode candidate)
                {
                    return sessionModeWireName(candidate) == value;
                }
            );
            if (match == modes.end())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format("Unknown session mode: {}", value)
                );
            }
            return *match;
        }

        // Re-reads the pinned row a binding names and refuses one whose session
        // is no longer active in this epoch. The binding is evidence of who the
        // caller was when bindController minted it; this row is the authority
        // now, so every entry point that takes a binding starts here.
        //
        // Activity is the whole of what the row can tell us that the binding
        // cannot. Every other member -- the controller id, the capability hash,
        // the controlled target, the kind -- was copied out of this same row,
        // and resumeSession changes none of those pinned columns, so comparing
        // them back would compare a value against the column it came from. Only
        // bindController can mint a binding, so there is no forged one for such
        // a comparison to catch.
        //
        // The epoch is not re-tested here either, and that is the same
        // reduction rather than a second one: opening the database begins a new
        // session epoch and deactivates every session the previous one left
        // behind, so "from a dead epoch" and "inactive" are one fact recorded
        // twice. bindController still names the epoch because that is where a
        // binding's epoch value is established; testing it again on every call
        // afterwards is a conjunct that cannot fail on its own.
        [[nodiscard]]
        auto requireLiveBinding(
            sqlite3* database,
            ControllerBinding const& controller
        ) -> Result<SessionMode>
        {
            UF_TRY_VALUE(
                query,
                prepare(
                    database,
                    "SELECT mode FROM sessions WHERE session_id=?1 AND active=1"
                )
            );
            UF_TRY(bindText(database, query.get(), 1, controller.sessionId()));
            if (sqlite3_step(query.get()) != SQLITE_ROW)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "ControllerBinding names no active session"
                );
            }
            return parseSessionMode(columnText(query.get(), 0));
        }

        auto appendHashMember(std::string& output, ContentHash const& hash) -> void
        {
            appendJsonString(output, hash.hex());
        }

        // OP:`DecisionBasis`, whose fourth member is the digest over the other
        // three and therefore cannot be inside it. Which three is the whole
        // requirement: everything the snapshot identity carries and this does
        // not -- lease, fencing token, epoch, token, every revision counter,
        // every observation and target identifier, every wall clock -- is
        // authority, naming or progress rather than decision input, and folding
        // any of it in would make an identical world observed after a takeover
        // read as a different decision.
        struct DecisionBasisParts final
        {
            ContentHash projectObservationHash;
            ContentHash sessionManifestHash;
            ContentHash stateResolutionHash;
        };

        [[nodiscard]]
        auto deriveDecisionBasis(
            DecisionBasisParts const& parts
        ) -> Result<ContentHash>
        {
            auto material = std::string{"{\"project_observation_hash\":"};
            appendHashMember(material, parts.projectObservationHash);
            material += ",\"session_manifest_hash\":";
            appendHashMember(material, parts.sessionManifestHash);
            material += ",\"state_resolution_hash\":";
            appendHashMember(material, parts.stateResolutionHash);
            material.push_back('}');
            return sha256(std::as_bytes(std::span{material}));
        }

        // OP:`SnapshotParts`, all fifteen members in JCS order. It is the exact
        // text stored as snapshots.canonical_parts, so identity_hash is
        // recomputable from the row and a test can falsify the derivation
        // rather than only compare it against itself.
        struct SnapshotPartsInputs final
        {
            ContentHash      decisionBasisHash;
            ContentHash      projectObservationHash;
            ContentHash      policyHash;
            ContentHash      sessionManifestHash;
            ContentHash      stateResolutionHash;
            std::string_view availableToolsJcs{};
            std::string_view controlledTargetId{};
            std::string_view leaseId{};
            std::string_view observationId{};
            std::string_view projectInstanceKey{};
            uint64           availabilityRevision{};
            uint64           fencingToken{};
            uint64           projectObservationRevision{};
            uint64           sessionEpoch{};
            uint64           targetGeneration{};
        };

        [[nodiscard]]
        auto snapshotPartsJcs(SnapshotPartsInputs const& parts) -> std::string
        {
            auto output = std::string{"{\"availability_revision\":"};
            output += std::to_string(parts.availabilityRevision);
            output += ",\"available_tools\":";
            output += parts.availableToolsJcs;
            output += ",\"controlled_target_id\":";
            appendJsonString(output, parts.controlledTargetId);
            output += ",\"decision_basis_hash\":";
            appendHashMember(output, parts.decisionBasisHash);
            output += ",\"fencing_token\":";
            output += std::to_string(parts.fencingToken);
            output += ",\"lease_id\":";
            appendJsonString(output, parts.leaseId);
            output += ",\"observation_id\":";
            appendJsonString(output, parts.observationId);
            output += ",\"policy_hash\":";
            appendHashMember(output, parts.policyHash);
            output += ",\"project_instance_key\":";
            appendJsonString(output, parts.projectInstanceKey);
            output += ",\"project_observation_hash\":";
            appendHashMember(output, parts.projectObservationHash);
            output += ",\"project_observation_revision\":";
            output += std::to_string(parts.projectObservationRevision);
            output += ",\"session_epoch\":";
            output += std::to_string(parts.sessionEpoch);
            output += ",\"session_manifest_hash\":";
            appendHashMember(output, parts.sessionManifestHash);
            output += ",\"state_resolution_hash\":";
            appendHashMember(output, parts.stateResolutionHash);
            output += ",\"target_generation\":";
            output += std::to_string(parts.targetGeneration);
            output.push_back('}');
            return output;
        }

        // Every column of the live lease row compared against the value the
        // caller presented. It is one helper rather than four copies of the
        // same eight bindings because a copy that drops a column is a lease
        // check that passes for a superseded controller.
        [[nodiscard]]
        auto requireLiveLease(
            sqlite3* database,
            ControlLease const& lease,
            std::string_view staleMessage
        ) -> Status
        {
            UF_TRY_VALUE(
                query,
                prepare(
                    database,
                    "SELECT 1 FROM control_leases WHERE controlled_target_id=?1 "
                    "AND lease_id=?2 AND session_id=?3 AND controller_id=?4 "
                    "AND session_epoch=?5 AND fencing_token=?6 AND revision=?7 "
                    "AND capability_profile_hash=?8"
                )
            );
            UF_TRY(bindText(database, query.get(), 1, lease.controlledTargetId));
            UF_TRY(bindText(database, query.get(), 2, lease.leaseId));
            UF_TRY(bindText(database, query.get(), 3, lease.sessionId));
            UF_TRY(bindText(database, query.get(), 4, lease.controllerId));
            UF_TRY(bindInteger(database, query.get(), 5, lease.sessionEpoch));
            UF_TRY(bindInteger(database, query.get(), 6, lease.fencingToken));
            UF_TRY(bindInteger(database, query.get(), 7, lease.revision));
            UF_TRY(bindText(
                database,
                query.get(),
                8,
                lease.capabilityProfileHash.hex()
            ));
            if (sqlite3_step(query.get()) != SQLITE_ROW)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    std::string{staleMessage}
                );
            }
            return ok();
        }

        // What a reconciliation of one uncertain delivery has to hold before
        // anything is asked of a provider: a live binding, a live lease, and
        // the durable run's own origin principal, controlled target and
        // registration, under a session that is still active on the same
        // registration. It reads only, so it needs no transaction of its own
        // and can run before the query rather than around it.
        //
        // `operation` names the door in every refusal it raises, because the
        // two doors that ask this question are read apart and a caller has to
        // be told which of them refused.
        [[nodiscard]]
        auto requireDurableRunAuthority(
            sqlite3* database,
            std::string_view operation,
            ControllerBinding const& controller,
            ControlLease const& lease,
            std::string_view rootIdentityHex
        ) -> Status
        {
            UF_TRY(requireLiveBinding(database, controller));
            UF_TRY(requireLiveLease(
                database,
                lease,
                std::format("{} control lease was superseded", operation)
            ));
            UF_TRY_VALUE(
                authorityQuery,
                prepare(
                    database,
                    "SELECT run.origin_principal_id, run.origin_principal_kind, "
                    "run.controlled_target_id, run.project_registration_hash, "
                    "session.project_registration_hash FROM tool_runs run "
                    "JOIN sessions session ON session.session_id=?2 "
                    "WHERE run.root_identity=?1 AND session.active=1"
                )
            );
            UF_TRY(bindText(database, authorityQuery.get(), 1, rootIdentityHex));
            UF_TRY(bindText(
                database,
                authorityQuery.get(),
                2,
                controller.sessionId()
            ));
            if (sqlite3_step(authorityQuery.get()) != SQLITE_ROW)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    std::format(
                        "{} requires its durable run and active session",
                        operation
                    )
                );
            }
            auto const exactAuthority =
                columnText(authorityQuery.get(), 0) == controller.controllerId()
                && columnText(authorityQuery.get(), 1)
                    == controllerKindWireName(controller.kind())
                && columnText(authorityQuery.get(), 2)
                    == controller.controlledTargetId()
                && columnText(authorityQuery.get(), 3)
                    == columnText(authorityQuery.get(), 4);
            if (!exactAuthority)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    std::format(
                        "{} authority differs from the durable run",
                        operation
                    )
                );
            }
            return ok();
        }

        // The content address of an artifact directory, recorded in its own
        // transaction BEFORE the directory is written. A publication that then
        // fails its compare-and-swap leaves a runtime_artifacts row no
        // installation names, which is exactly what
        // reclaimUnreferencedRuntimeArtifacts removes; recording it inside the
        // installing transaction instead would roll the row back and strand the
        // directory with nothing in the database naming it.
        [[nodiscard]]
        auto registerArtifactRoot(
            sqlite3* database,
            std::string_view artifactRootHash
        ) -> Status
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY_VALUE(
                insert,
                prepare(
                    database,
                    "INSERT OR IGNORE INTO runtime_artifacts(artifact_root_hash) "
                    "VALUES(?1)"
                )
            );
            UF_TRY(bindText(database, insert.get(), 1, artifactRootHash));
            UF_TRY(expectDone(database, insert.get()));
            return transaction.commit();
        }

        // A root whose generation 0 names a genesis RuntimeArtifact an earlier
        // era of this framework wrote. H_genesis is a framework constant, so
        // moving the constant moves every root's materialisation of it -- the
        // same way admitTheGenesisGeneration rebuilds stored DDL when the
        // schema identity moves. The old digest is never loaded, only
        // recognised, so that it can be migrated away rather than accepted.
        //
        // Every row a future pin travels through follows the constant, and
        // nothing else does. runtime_upgrade_failures and
        // release_capability_approvals keep their bytes: neither carries a
        // foreign key, and both record what happened rather than what will be
        // loaded.
        [[nodiscard]]
        auto migrateGenesisArtifactRoot(
            sqlite3* database,
            std::string_view supersededRootHash,
            std::string_view genesisRootHash
        ) -> Status
        {
            // sessions holds a foreign key into runtime_installations on the
            // exact pair being rewritten here, so the parent cannot move ahead
            // of the child under a live check. This is the instrument
            // admitTheGenesisGeneration rebuilds those same two tables under,
            // and SQLite clears it at the end of the enclosing transaction.
            UF_TRY(execute(database, "PRAGMA defer_foreign_keys=ON"));

            // Every generation holding it, not only generation 0.
            // rollbackRuntimeArtifactUpgrade appends the restored hash as a NEW
            // generation, so a root that rolled back off genesis holds it above
            // generation 0 as well, possibly as the active pin.
            UF_TRY_VALUE(
                installations,
                prepare(
                    database,
                    "UPDATE runtime_installations SET artifact_root_hash=?2 "
                    "WHERE artifact_root_hash=?1"
                )
            );
            UF_TRY(bindText(database, installations.get(), 1, supersededRootHash));
            UF_TRY(bindText(database, installations.get(), 2, genesisRootHash));
            UF_TRY(expectDone(database, installations.get()));

            UF_TRY_VALUE(
                sessions,
                prepare(
                    database,
                    "UPDATE sessions SET runtime_artifact_root_hash=?2 "
                    "WHERE runtime_artifact_root_hash=?1"
                )
            );
            UF_TRY(bindText(database, sessions.get(), 1, supersededRootHash));
            UF_TRY(bindText(database, sessions.get(), 2, genesisRootHash));
            UF_TRY(expectDone(database, sessions.get()));

            // Without this a root that was never upgraded would open its first
            // session against an artifact the current trusted parser refuses by
            // declared format, which is "init then explore" permanently broken
            // on every root written before the cut.
            UF_TRY_VALUE(
                active,
                prepare(
                    database,
                    "UPDATE runtime_state SET active_runtime_artifact_root_hash=?2 "
                    "WHERE singleton=1 AND active_runtime_artifact_root_hash=?1"
                )
            );
            UF_TRY(bindText(database, active.get(), 1, supersededRootHash));
            UF_TRY(bindText(database, active.get(), 2, genesisRootHash));
            UF_TRY(expectDone(database, active.get()));

            // The superseded runtime_artifacts row and its directory are NOT
            // removed here. Nothing names them now, which is exactly the state
            // reclaimUnreferencedRuntimeArtifacts already owns; a hand-deletion
            // beside it would be a second collector for one condition.
            UF_TRY_VALUE(
                audit,
                prepare(
                    database,
                    "INSERT INTO genesis_transitions("
                    "source_artifact_root_hash, target_artifact_root_hash) "
                    "VALUES(?1, ?2)"
                )
            );
            UF_TRY(bindText(database, audit.get(), 1, supersededRootHash));
            UF_TRY(bindText(database, audit.get(), 2, genesisRootHash));
            return expectDone(database, audit.get());
        }

        // The genesis generation, materialized as part of the root's layout.
        //
        // This is the whole of what makes "init then explore --runtime <root>"
        // work: a root that has never been upgraded holds H_genesis at
        // generation 0, so a first session has something to pin. Genesis grants
        // nothing -- an empty model under an absent policy artifact resolves to
        // deny-all -- so pinning it decides nothing on the operator's behalf.
        //
        // It runs on EVERY open, for a root created before the genesis
        // generation existed and a root created after it alike. That is one
        // mechanism rather than a creation path and a migration path: the DDL
        // moved the CHECK, and this moves the rows, and neither asks which kind
        // of root it is looking at.
        //
        // The active pin is only claimed when there is none. A root already
        // running an installed generation keeps it: genesis is layout, not a
        // release, and it has never been the thing a root was upgraded to.
        //
        // Four dispositions for what generation 0 holds, and no fifth: absent
        // gets the current digest written; the current digest passes; a digest
        // named in k_formerGenesisArtifactRootHashes is migrated onto the
        // current one in this transaction; anything else is refused by name.
        [[nodiscard]]
        auto ensureGenesisGeneration(
            sqlite3* database,
            std::filesystem::path const& runtimeArtifactRoot
        ) -> Status
        {
            UF_TRY_VALUE(
                genesisRootHash,
                detail::ensureGenesisRuntimeArtifact(runtimeArtifactRoot)
            );
            auto const hex = genesisRootHash.hex();
            UF_TRY(registerArtifactRoot(database, hex));
            UF_TRY_VALUE(transaction, Transaction::begin(database));

            // What generation 0 already names, if it names anything. It is read
            // before a byte is written because the answer decides between the
            // four dispositions, and the statement is scoped so its read is
            // finished before the writes below.
            auto recorded = std::optional<std::string>{};
            {
                UF_TRY_VALUE(
                    query,
                    prepare(
                        database,
                        "SELECT artifact_root_hash FROM runtime_installations "
                        "WHERE installed_generation=0"
                    )
                );
                auto const step = sqlite3_step(query.get());
                if (step == SQLITE_ROW)
                {
                    recorded = columnText(query.get(), 0);
                }
                else if (step != SQLITE_DONE)
                {
                    return databaseFailure(
                        database,
                        "could not read the genesis RuntimeArtifact generation"
                    );
                }
            }

            // Generation 0 names the framework's empty model. Which BYTES that
            // model has is this binary's constant and has moved before, so a
            // digest the framework itself wrote in an earlier era is migrated
            // to the current one rather than read as tampering. A digest the
            // framework never wrote is refused by name, and that is the whole
            // of what this refusal now claims.
            if (recorded.has_value() && *recorded != hex)
            {
                auto const superseded = std::ranges::contains(
                    task::k_formerGenesisArtifactRootHashes,
                    std::string_view{*recorded}
                );
                if (!superseded)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "Operator root pins generation 0 to RuntimeArtifact "
                            "root sha256:{}, which is neither the genesis "
                            "RuntimeArtifact sha256:{} nor a genesis this "
                            "framework superseded",
                            *recorded,
                            hex
                        )
                    );
                }
                UF_TRY(migrateGenesisArtifactRoot(database, *recorded, hex));
            }

            UF_TRY_VALUE(
                installation,
                prepare(
                    database,
                    "INSERT OR IGNORE INTO runtime_installations("
                    "installed_generation, artifact_root_hash) VALUES(0, ?1)"
                )
            );
            UF_TRY(bindText(database, installation.get(), 1, hex));
            UF_TRY(expectDone(database, installation.get()));
            UF_TRY_VALUE(
                claim,
                prepare(
                    database,
                    "UPDATE runtime_state SET active_runtime_artifact_root_hash=?1 "
                    "WHERE singleton=1 AND active_runtime_artifact_root_hash IS NULL"
                )
            );
            UF_TRY(bindText(database, claim.get(), 1, hex));
            UF_TRY(expectDone(database, claim.get()));
            return transaction.commit();
        }

        // One coordinator owns a runtime directory at a time, and this is what
        // makes that true rather than assumed. beginSessionEpoch below clears
        // every control lease and deactivates every session on the reading that
        // whatever those rows describe died with the process that wrote them.
        // Without an exclusive lock a second open performs those clears against
        // a coordinator that is still running and strips live leases. sqlite
        // holds the lock for the connection's lifetime under this locking mode,
        // so the refusal below is the whole of the enforcement.
        [[nodiscard]]
        auto claimExclusiveOwnership(sqlite3* database) -> Status
        {
            UF_TRY(execute(database, "PRAGMA locking_mode=EXCLUSIVE"));
            auto const code = sqlite3_exec(
                database,
                "BEGIN EXCLUSIVE",
                nullptr,
                nullptr,
                nullptr
            );
            if ((code & 0xFF) == SQLITE_BUSY)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Another Operator coordinator holds this runtime directory"
                );
            }
            if (code != SQLITE_OK)
            {
                return databaseFailure(database, "could not claim the runtime directory");
            }
            return execute(database, "COMMIT");
        }

        [[nodiscard]]
        auto beginSessionEpoch(sqlite3* database) -> Result<uint64>
        {
            UF_TRY_VALUE(transaction, Transaction::begin(database));
            UF_TRY_VALUE(
                query,
                prepare(
                    database,
                    "SELECT current_session_epoch FROM runtime_state WHERE singleton=1"
                )
            );
            if (sqlite3_step(query.get()) != SQLITE_ROW)
            {
                return databaseFailure(database, "could not read session epoch");
            }
            auto const prior = static_cast<uint64>(sqlite3_column_int64(query.get(), 0));
            if (prior == static_cast<uint64>(std::numeric_limits<sqlite3_int64>::max()))
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "Operator session epoch exhausted"
                );
            }
            auto const next = prior + 1U;
            UF_TRY_VALUE(
                update,
                prepare(
                    database,
                    "UPDATE runtime_state SET current_session_epoch=?1 "
                    "WHERE singleton=1 AND current_session_epoch=?2"
                )
            );
            UF_TRY(bindInteger(database, update.get(), 1, next));
            UF_TRY(bindInteger(database, update.get(), 2, prior));
            UF_TRY(expectDone(database, update.get()));
            if (sqlite3_changes(database) != 1)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Operator session epoch lost its startup CAS"
                );
            }
            UF_TRY(execute(database, "DELETE FROM control_leases"));
            UF_TRY(execute(database, "UPDATE sessions SET active=0 WHERE active=1"));
            UF_TRY(transaction.commit());
            return next;
        }
    }

    // Defined here rather than beside its durable-spelling overload because the
    // provider-identity-to-provider_kind mapping is this file's, and a second
    // copy of it next to the answerer table would be a second answer to "which
    // kind is this provider" -- the exact duplication the answerer table exists
    // to close.
    auto toolEffectComposition(ToolProviderIdentity const& provider)
        -> ToolEffectComposition
    {
        return toolEffectComposition(persistedToolProvider(provider).kind);
    }

    struct OperatorCoordinator::Impl final
    {
        Database              database;
        std::filesystem::path path;
        std::filesystem::path runtimeArtifactRoot;
        EvidenceArtifactStore evidenceStore;
        uint64                sessionEpoch{};
    };

    OperatorCoordinator::OperatorCoordinator(std::unique_ptr<Impl> implementation)
        : m_impl{std::move(implementation)}
    {
    }

    OperatorCoordinator::OperatorCoordinator(OperatorCoordinator&&) noexcept = default;
    auto OperatorCoordinator::operator=(OperatorCoordinator&&) noexcept
        -> OperatorCoordinator& = default;
    OperatorCoordinator::~OperatorCoordinator() = default;

    auto OperatorCoordinator::open(
        std::filesystem::path const& runtimeDirectory
    ) -> Result<OperatorCoordinator>
    {
        if (runtimeDirectory.empty())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Operator runtime directory must not be empty"
            );
        }

        auto error = std::error_code{};
        std::filesystem::create_directories(runtimeDirectory, error);
        if (error)
        {
            return fail(
                AutomationErrorKind::IoFailure,
                "Could not create Operator runtime directory",
                error
            );
        }

        UF_TRY(requirePlainDirectory(runtimeDirectory, "Operator runtime root"));

        auto const databasePath        = runtimeDirectory / "operator-runtime.sqlite";
        auto const runtimeArtifactRoot = runtimeDirectory / "runtime-artifacts";
        std::filesystem::create_directories(runtimeArtifactRoot, error);
        if (error)
        {
            return fail(
                AutomationErrorKind::IoFailure,
                "Could not create production RuntimeArtifact root",
                error
            );
        }
        UF_TRY(requirePlainDirectory(
            runtimeArtifactRoot,
            "Production RuntimeArtifact root"
        ));

        // The staging directory is part of the production layout rather than
        // something an installation creates on its way past, because
        // reclamation sweeps it and both need one owner for the name.
        auto const stagingRoot = runtimeArtifactRoot
            / std::string{detail::k_stagingDirectoryName};
        std::filesystem::create_directories(stagingRoot, error);
        if (error)
        {
            return fail(
                AutomationErrorKind::IoFailure,
                "Could not create production RuntimeArtifact staging root",
                error
            );
        }
        UF_TRY(requirePlainDirectory(
            stagingRoot,
            "Production RuntimeArtifact staging root"
        ));
        UF_TRY_VALUE(evidenceStore, EvidenceArtifactStore::open(runtimeDirectory));
        auto const databaseStatus = std::filesystem::symlink_status(databasePath, error);
        if (!error && std::filesystem::exists(databaseStatus))
        {
            if (
                !std::filesystem::is_regular_file(databaseStatus)
                || std::filesystem::is_symlink(databaseStatus)
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Operator database path must be a plain file"
                );
            }
        }
        else if (error && error != std::errc::no_such_file_or_directory)
        {
            return fail(
                AutomationErrorKind::IoFailure,
                "Could not inspect Operator database path",
                error
            );
        }
        auto* rawDatabase = static_cast<sqlite3*>(nullptr);
        auto const openCode = sqlite3_open_v2(
            pathToUtf8(databasePath).c_str(),
            &rawDatabase,
            SQLITE_OPEN_CREATE
                | SQLITE_OPEN_READWRITE
                | SQLITE_OPEN_FULLMUTEX
                | SQLITE_OPEN_EXRESCODE
                | SQLITE_OPEN_NOFOLLOW,
            nullptr
        );
        auto database = Database{rawDatabase};
        if (openCode != SQLITE_OK || database == nullptr)
        {
            auto const detail = database == nullptr
                ? std::string{"SQLite returned no database handle"}
                : std::string{sqlite3_errmsg(database.get())};
            return fail(
                AutomationErrorKind::IoFailure,
                std::format("Could not open Operator database: {}", detail)
            );
        }

        // Before the schema is touched, so a second coordinator is refused by
        // name rather than by whichever statement happens to hit the lock.
        UF_TRY(claimExclusiveOwnership(database.get()));
        UF_TRY(initialize(database.get()));

        // Part of the layout, beside the directories and the database above:
        // every Operator root holds the genesis generation from its first open.
        UF_TRY(ensureGenesisGeneration(database.get(), runtimeArtifactRoot));
        UF_TRY_VALUE(sessionEpoch, beginSessionEpoch(database.get()));
        auto coordinator = OperatorCoordinator{std::make_unique<Impl>(
            Impl{
                .database            = std::move(database),
                .path                = databasePath,
                .runtimeArtifactRoot = runtimeArtifactRoot,
                .evidenceStore       = std::move(evidenceStore),
                .sessionEpoch        = sessionEpoch,
            }
        )};
        UF_TRY(coordinator.recoverUncertainToolCalls());
        return coordinator;
    }

    auto OperatorCoordinator::databasePath() const -> std::filesystem::path
    {
        return m_impl->path;
    }

    auto OperatorCoordinator::publishEvidenceArtifact(
        EvidenceArtifactSpec const& spec
    ) -> Result<EvidenceArtifactReceipt>
    {
        return m_impl->evidenceStore.publish(spec);
    }

    auto OperatorCoordinator::evidenceArtifactReceipt(ContentHash const& hash)
        -> Result<std::optional<EvidenceArtifactReceipt>>
    {
        UF_TRY_VALUE(receipts, evidenceReceipts(m_impl->database.get()));
        auto const found = std::ranges::find(
            receipts,
            hash,
            &EvidenceArtifactReceipt::contentHash
        );
        if (found == receipts.end())
        {
            return std::nullopt;
        }
        return *found;
    }

    auto OperatorCoordinator::readEvidenceArtifact(
        ContentHash const& hash,
        std::size_t maximumBytes
    ) -> Result<std::vector<std::byte>>
    {
        return m_impl->evidenceStore.read(hash, maximumBytes);
    }

    auto OperatorCoordinator::installRuntimeArtifact(
        RuntimeArtifactInstallRequest const& request
    ) -> Result<task::InstalledRuntimeArtifact>
    {
        // The artifact directory is published before this transaction and is
        // deliberately NOT removed when the compare-and-swap below fails. It is
        // content-addressed and re-verified on every open, and a concurrent
        // publisher may have put the identical bytes there first -- which is
        // one of the ways the CAS fails. Deleting it on our own failure would
        // break their installation to tidy ours.
        //
        // What the failure leaves behind instead is a runtime_artifacts row no
        // installation names, which reclaimUnreferencedRuntimeArtifacts is free
        // to remove once nothing else does either.
        UF_TRY_VALUE(
            stagingToken,
            randomToken(m_impl->database.get(), k_stagingTokenBytes)
        );
        UF_TRY_VALUE(
            source,
            detail::readRuntimeArtifactSource(
                m_impl->runtimeArtifactRoot,
                request.artifactDirectory,
                request.artifactRootHash
            )
        );

        UF_TRY(registerArtifactRoot(
            m_impl->database.get(),
            request.artifactRootHash.hex()
        ));
        UF_TRY_VALUE(
            artifact,
            detail::publishRuntimeArtifact(
                m_impl->runtimeArtifactRoot,
                source,
                stagingToken
            )
        );
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY_VALUE(
            stateQuery,
            prepare(
                m_impl->database.get(),
                "SELECT installed_generation, active_runtime_artifact_root_hash "
                "FROM runtime_state WHERE singleton=1"
            )
        );
        if (sqlite3_step(stateQuery.get()) != SQLITE_ROW)
        {
            return databaseFailure(
                m_impl->database.get(),
                "could not read installed RuntimeArtifact generation"
            );
        }
        auto const currentGeneration = static_cast<uint64>(
            sqlite3_column_int64(stateQuery.get(), 0)
        );
        auto const currentRoot = columnText(stateQuery.get(), 1);
        if (currentGeneration != request.expectedInstalledGeneration)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "RuntimeArtifact installed-generation compare-and-swap failed"
            );
        }
        if (currentRoot == request.artifactRootHash.hex())
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "RuntimeArtifact is already the active installed generation"
            );
        }
        UF_TRY_VALUE(
            nextGeneration,
            checkedSqlIncrement(currentGeneration, "installed RuntimeArtifact generation")
        );

        UF_TRY_VALUE(
            installationInsert,
            prepare(
                m_impl->database.get(),
                "INSERT INTO runtime_installations(installed_generation, artifact_root_hash) "
                "VALUES(?1, ?2)"
            )
        );
        UF_TRY(bindInteger(
            m_impl->database.get(),
            installationInsert.get(),
            1,
            nextGeneration
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            installationInsert.get(),
            2,
            request.artifactRootHash.hex()
        ));
        UF_TRY(expectDone(m_impl->database.get(), installationInsert.get()));

        UF_TRY_VALUE(
            update,
            prepare(
                m_impl->database.get(),
                "UPDATE runtime_state SET installed_generation=?1, "
                "active_runtime_artifact_root_hash=?2 WHERE singleton=1 "
                "AND installed_generation=?3"
            )
        );
        UF_TRY(bindInteger(m_impl->database.get(), update.get(), 1, nextGeneration));
        UF_TRY(bindText(
            m_impl->database.get(),
            update.get(),
            2,
            request.artifactRootHash.hex()
        ));
        UF_TRY(bindInteger(
            m_impl->database.get(),
            update.get(),
            3,
            request.expectedInstalledGeneration
        ));
        UF_TRY(expectDone(m_impl->database.get(), update.get()));
        if (sqlite3_changes(m_impl->database.get()) != 1)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "RuntimeArtifact installed-generation compare-and-swap lost its transaction"
            );
        }

        UF_TRY(transaction.commit());
        return task::InstalledRuntimeArtifact{
            std::move(artifact),
            nextGeneration,
        };
    }

    auto OperatorCoordinator::activeRuntimeArtifactPin()
        -> Result<RuntimeArtifactPin>
    {
        UF_TRY_VALUE(
            query,
            prepare(
                m_impl->database.get(),
                "SELECT installed_generation, active_runtime_artifact_root_hash "
                "FROM runtime_state WHERE singleton=1 "
                "AND active_runtime_artifact_root_hash IS NOT NULL"
            )
        );
        if (sqlite3_step(query.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "No RuntimeArtifact release is active"
            );
        }
        auto const generation = static_cast<uint64>(
            sqlite3_column_int64(query.get(), 0)
        );
        auto encodedRoot = std::string{"sha256:"};
        encodedRoot += columnText(query.get(), 1);
        UF_TRY_VALUE(rootHash, ContentHash::parse(encodedRoot));
        return RuntimeArtifactPin{
            .installedGeneration = generation,
            .artifactRootHash    = rootHash,
        };
    }

    auto OperatorCoordinator::approveReleaseCapabilities(
        ReleaseCapabilityApproval const& approval
    ) -> Status
    {
        for (auto const& capability : approval.controllerCapabilities)
        {
            UF_TRY(requireName(capability, "approved release capability"));
        }
        auto const capabilities = canonicalNameArray(
            approval.controllerCapabilities
        );
        UF_TRY_VALUE(
            capabilityProfileHash,
            sha256(std::as_bytes(std::span{capabilities}))
        );
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY_VALUE(
            insert,
            prepare(
                m_impl->database.get(),
                "INSERT OR IGNORE INTO release_capability_approvals("
                "artifact_root_hash, capability_profile_hash, "
                "controller_capabilities, evidence_hash, session_epoch) "
                "VALUES(?1, ?2, ?3, ?4, ?5)"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            1,
            approval.artifactRootHash.hex()
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            2,
            capabilityProfileHash.hex()
        ));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 3, capabilities));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            4,
            approval.evidenceHash.hex()
        ));
        UF_TRY(bindInteger(
            m_impl->database.get(),
            insert.get(),
            5,
            m_impl->sessionEpoch
        ));
        UF_TRY(expectDone(m_impl->database.get(), insert.get()));

        UF_TRY_VALUE(
            query,
            prepare(
                m_impl->database.get(),
                "SELECT controller_capabilities, evidence_hash "
                "FROM release_capability_approvals "
                "WHERE artifact_root_hash=?1 AND capability_profile_hash=?2"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            query.get(),
            1,
            approval.artifactRootHash.hex()
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            query.get(),
            2,
            capabilityProfileHash.hex()
        ));
        if (
            sqlite3_step(query.get()) != SQLITE_ROW
            || columnText(query.get(), 0) != capabilities
            || columnText(query.get(), 1) != approval.evidenceHash.hex()
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Release capability profile already names different approval evidence"
            );
        }
        return transaction.commit();
    }

    auto OperatorCoordinator::upgradeRuntimeArtifactAndPinSession(
        RuntimeArtifactInstallRequest const& installation,
        SessionPin const& pin,
        SessionManifest const& manifest,
        AgentProfile const& agentProfile
    ) -> Status
    {
        UF_TRY_VALUE(predecessor, activeRuntimeArtifactPin());
        auto installed = installRuntimeArtifact(installation);
        if (!installed)
        {
            return std::unexpected{std::move(installed).error()};
        }
        auto const attempted = RuntimeArtifactPin{
            .installedGeneration = installed->installedGeneration(),
            .artifactRootHash    = installed->rootHash(),
        };
        auto pinned = pinSession(pin, manifest, agentProfile);
        if (pinned)
        {
            return ok();
        }

        auto failure      = std::move(pinned).error();
        auto const reason = std::string{failure.message()};
        UF_TRY(rollbackRuntimeArtifactUpgrade(
            m_impl->database.get(),
            attempted,
            predecessor,
            reason
        ));
        return std::unexpected{std::move(failure)};
    }

    auto OperatorCoordinator::reclaimUnreferencedRuntimeArtifacts()
        -> Result<ReclaimedRuntimeArtifacts>
    {
        // The removals happen INSIDE the write transaction, so the whole
        // reference set is read against one consistent state rather than
        // row by row while it moves.
        //
        // Rolling back after a directory has been removed is safe in the one
        // direction it can happen: a runtime_artifacts row whose directory is
        // missing is still unreferenced, so the next pass finishes the job.
        //
        // There is no in-flight-publication exemption and none may be added.
        // claimExclusiveOwnership refuses a second coordinator for the lifetime
        // of the connection, and OperatorCoordinator carries no synchronization
        // of its own, so a publisher concurrent with this sweep is not a state
        // the design admits. A table recording claims against it was carried
        // until 2026-08-11 and was unreachable in every path.
        UF_TRY_VALUE(
            artifactDirectory,
            task_platform::ConfinedRoot::open(m_impl->runtimeArtifactRoot)
        );
        UF_TRY_VALUE(
            stagingDirectory,
            task_platform::ConfinedRoot::open(
                m_impl->runtimeArtifactRoot / std::string{detail::k_stagingDirectoryName}
            )
        );
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));

        // Every row is read before the first write, because a statement that is
        // still stepping does not see its own transaction's later changes.
        UF_TRY_VALUE(
            orphanQuery,
            prepare(
                m_impl->database.get(),
                "SELECT artifact_root_hash FROM runtime_artifacts WHERE "
                "artifact_root_hash NOT IN "
                "(SELECT artifact_root_hash FROM runtime_installations) AND "
                "artifact_root_hash NOT IN "
                "(SELECT active_runtime_artifact_root_hash FROM runtime_state "
                "WHERE singleton=1 AND active_runtime_artifact_root_hash IS NOT NULL) "
                "ORDER BY artifact_root_hash"
            )
        );
        auto orphans    = std::vector<std::string>{};
        auto orphanStep = sqlite3_step(orphanQuery.get());
        while (orphanStep == SQLITE_ROW)
        {
            orphans.emplace_back(columnText(orphanQuery.get(), 0));
            orphanStep = sqlite3_step(orphanQuery.get());
        }
        if (orphanStep != SQLITE_DONE)
        {
            return databaseFailure(
                m_impl->database.get(),
                "could not scan unreferenced RuntimeArtifacts"
            );
        }

        for (auto const& hash : orphans)
        {
            UF_TRY(artifactDirectory.removeTree(hash));
            UF_TRY_VALUE(
                deletion,
                prepare(
                    m_impl->database.get(),
                    "DELETE FROM runtime_artifacts WHERE artifact_root_hash=?1"
                )
            );
            UF_TRY(bindText(m_impl->database.get(), deletion.get(), 1, hash));
            UF_TRY(expectDone(m_impl->database.get(), deletion.get()));
        }

        UF_TRY_VALUE(stagingNames, stagingDirectory.childNames());
        auto reclaimedStagings = uint64{};
        for (auto const& name : stagingNames)
        {
            UF_TRY(stagingDirectory.removeTree(name));
            ++reclaimedStagings;
        }

        UF_TRY(transaction.commit());
        return ReclaimedRuntimeArtifacts{
            .artifactDirectories = static_cast<uint64>(orphans.size()),
            .stagingDirectories  = reclaimedStagings,
        };
    }

    auto OperatorCoordinator::reclaimUnreferencedEvidenceArtifacts(
        uint64 maximumAgeMillis
    ) -> Result<ReclaimedEvidenceArtifacts>
    {
        UF_TRY_VALUE(receipts, evidenceReceipts(m_impl->database.get()));
        UF_TRY_VALUE(currentUnixMillis, unixTimeMilliseconds());

        auto retained = std::vector<ContentHash>{};
        retained.reserve(receipts.size());
        for (auto const& receipt : receipts)
        {
            auto const age = currentUnixMillis >= receipt.createdAtUnixMillis
                ? currentUnixMillis - receipt.createdAtUnixMillis
                : uint64{0U};
            if (age <= maximumAgeMillis)
            {
                retained.emplace_back(receipt.contentHash);
            }
        }
        std::ranges::sort(retained);
        auto const uniqueEnd = std::ranges::unique(retained).begin();
        retained.erase(uniqueEnd, retained.end());
        return m_impl->evidenceStore.reclaim(retained);
    }

    auto OperatorCoordinator::openInstalledRuntimeArtifact(
        uint64 installedGeneration,
        ContentHash const& artifactRootHash
    ) -> Result<task::InstalledRuntimeArtifact>
    {
        UF_TRY(requireInstalledArtifactPin(
            m_impl->database.get(),
            installedGeneration,
            artifactRootHash
        ));
        UF_TRY_VALUE(
            artifact,
            detail::openProductionRuntimeArtifact(
                m_impl->runtimeArtifactRoot,
                artifactRootHash
            )
        );
        return task::InstalledRuntimeArtifact{
            std::move(artifact),
            installedGeneration,
        };
    }

    auto OperatorCoordinator::readInstalledRuntimeArtifact(
        std::filesystem::path const& runtimeDirectory,
        uint64 installedGeneration,
        ContentHash const& artifactRootHash
    ) -> Result<task::InstalledRuntimeArtifact>
    {
        UF_TRY_VALUE(layout, openReadOnlyOperatorLayout(runtimeDirectory));
        UF_TRY(requireInstalledArtifactPin(
            layout.database.get(),
            installedGeneration,
            artifactRootHash
        ));
        UF_TRY_VALUE(
            artifact,
            detail::openProductionRuntimeArtifact(
                layout.runtimeArtifactRoot,
                artifactRootHash
            )
        );
        return task::InstalledRuntimeArtifact{
            std::move(artifact),
            installedGeneration,
        };
    }

    auto OperatorCoordinator::readActiveInstalledRuntimeArtifact(
        std::filesystem::path const& runtimeDirectory,
        ContentHash const& compatibleArtifactRootHash
    ) -> Result<task::InstalledRuntimeArtifact>
    {
        UF_TRY_VALUE(layout, openReadOnlyOperatorLayout(runtimeDirectory));
        UF_TRY_VALUE(
            installedGeneration,
            activeInstalledGeneration(
                layout.database.get(),
                compatibleArtifactRootHash
            )
        );
        UF_TRY_VALUE(
            artifact,
            detail::openProductionRuntimeArtifact(
                layout.runtimeArtifactRoot,
                compatibleArtifactRootHash
            )
        );
        return task::InstalledRuntimeArtifact{
            std::move(artifact),
            installedGeneration,
        };
    }

    auto OperatorCoordinator::openActiveInstalledRuntimeArtifact(
        ContentHash const& compatibleArtifactRootHash
    ) -> Result<task::InstalledRuntimeArtifact>
    {
        UF_TRY_VALUE(
            installedGeneration,
            activeInstalledGeneration(
                m_impl->database.get(),
                compatibleArtifactRootHash
            )
        );
        return openInstalledRuntimeArtifact(
            installedGeneration,
            compatibleArtifactRootHash
        );
    }

    // Exactly one shape a restart finds mid-dispatch cannot be accounted for:
    // the MUTATING DIRECT LEAF -- a call answered by a Framework provider whose
    // descriptor declares it mutating. That is the one row where the world may
    // or may not have moved and no durable record can say, so it is classified
    // uncertain and never dispatched again; only an invoked reconciliation
    // query can resolve it afterwards.
    //
    // Everything else survives as dispatching and is re-entered. A Project
    // handler can affect the world only through recorded children. Re-running
    // it rejoins those exact coordinates and refuses divergence, including a
    // shortened child sequence. An unresolved child keeps the frame dispatching.
    // A read-only Framework leaf declares no effect for delivery to be uncertain about,
    // so re-running its provider delivers nothing twice. Classifying either
    // uncertain would invent a barrier nothing can resolve.
    //
    // The filter is generated from toolCallEffectMayBeUnrecorded, so the rule a
    // restart applies and the rule a re-entry refuses on are one rule.
    //
    // Every interrupted dispatch loses its capability here whether or not its
    // state moves: the history revision advances for all of them, so a
    // ToolCallDispatch minted before this open matches no active dispatch and
    // its terminal write is refused. A row that survives as dispatching is one
    // a NEW incarnation may re-enter, not one an old holder may still answer.
    auto OperatorCoordinator::recoverUncertainToolCalls() -> Result<uint64>
    {
        UF_TRY_VALUE(
            explanation,
            CanonicalJson::parseExact(
                R"({"reason":"operator_restart_after_dispatch_started"})"
            )
        );
        auto* const database = m_impl->database.get();
        auto const unrecordable =
            std::string{
                "SELECT history.call_identity FROM tool_call_history history "
                "JOIN tool_call_positions position "
                "ON position.call_identity=history.call_identity "
                "WHERE history.state='dispatching' AND "
            }
            + unrecordedEffectDispatchFilter();
        auto const updateSql =
            "UPDATE tool_call_history SET state='possible', revision=revision+1, "
            "outcome_payload=?1, outcome_payload_hash=?2 "
            "WHERE call_identity IN("
            + unrecordable + ")";
        UF_TRY_VALUE(transaction, Transaction::begin(database));
        UF_TRY_VALUE(
            revisionQuery,
            prepare(
                database,
                "SELECT MAX(revision) FROM tool_call_history "
                "WHERE state='dispatching'"
            )
        );
        if (sqlite3_step(revisionQuery.get()) != SQLITE_ROW)
        {
            return databaseFailure(
                database,
                "could not inspect dispatching Tool revisions"
            );
        }
        if (sqlite3_column_type(revisionQuery.get(), 0) != SQLITE_NULL)
        {
            auto const maximumRevision = static_cast<uint64>(
                sqlite3_column_int64(revisionQuery.get(), 0)
            );
            UF_TRY(checkedSqlIncrement(
                maximumRevision,
                "Tool call history revision"
            ));
        }
        UF_TRY_VALUE(update, prepare(database, updateSql));
        UF_TRY(bindText(database, update.get(), 1, explanation.bytes()));
        UF_TRY(bindText(
            database,
            update.get(),
            2,
            explanation.contentHash().hex()
        ));
        UF_TRY(expectDone(database, update.get()));
        auto const recovered = static_cast<uint64>(sqlite3_changes(database));

        // The rows left dispatching, after the uncertain ones have moved out of
        // that state. One statement, run second, so nothing is bumped twice.
        UF_TRY_VALUE(
            supersede,
            prepare(
                database,
                "UPDATE tool_call_history SET revision=revision+1 "
                "WHERE state='dispatching'"
            )
        );
        UF_TRY(expectDone(database, supersede.get()));
        UF_TRY(transaction.commit());
        return recovered;
    }

    auto OperatorCoordinator::registerProject(
        ProjectIdentity const& project
    ) -> Status
    {
        auto const registrationHash = project.hash();
        auto const pluginId = project.pluginId();
        auto const& canonicalManifest = project.canonicalJcs();
        UF_TRY(requireName(pluginId, "plugin_id"));
        UF_TRY(requireName(canonicalManifest, "canonical project registration"));
        UF_TRY_VALUE(
            computedHash,
            sha256(std::as_bytes(std::span{canonicalManifest}))
        );
        if (computedHash != registrationHash)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Project registration hash does not match canonical bytes"
            );
        }
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));

        UF_TRY_VALUE(
            insert,
            prepare(
                m_impl->database.get(),
                "INSERT OR IGNORE INTO project_registrations"
                "(registration_hash, registration_format, plugin_id, "
                "plugin_identity_kind, plugin_identity_hash, canonical_manifest) "
                "VALUES(?1, 3, ?2, 'module_manifest', ?3, ?4)"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 1, registrationHash.hex()));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 2, pluginId));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            3,
            project.moduleIdentityHash().hex()
        ));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 4, canonicalManifest));
        UF_TRY(expectDone(m_impl->database.get(), insert.get()));

        UF_TRY_VALUE(
            query,
            prepare(
                m_impl->database.get(),
                "SELECT registration_format, plugin_id, plugin_identity_kind, "
                "plugin_identity_hash, canonical_manifest "
                "FROM project_registrations "
                "WHERE registration_hash=?1"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), query.get(), 1, registrationHash.hex()));
        if (sqlite3_step(query.get()) != SQLITE_ROW)
        {
            return databaseFailure(m_impl->database.get(), "could not verify registration");
        }
        if (
            sqlite3_column_int64(query.get(), 0) != 3
            || columnText(query.get(), 1) != pluginId
            || columnText(query.get(), 2) != "module_manifest"
            || columnText(query.get(), 3) != project.moduleIdentityHash().hex()
            || columnText(query.get(), 4) != canonicalManifest
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Project registration hash already names different immutable bytes"
            );
        }
        return transaction.commit();
    }

    auto OperatorCoordinator::provisionProjectInstance(
        ProjectIdentity const& project,
        std::string const& projectInstanceKey
    ) -> Status
    {
        UF_TRY(requireName(projectInstanceKey, "project_instance_key"));

        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY_VALUE(
            registrationQuery,
            prepare(
                m_impl->database.get(),
                "SELECT registration_format, plugin_identity_kind "
                "FROM project_registrations WHERE registration_hash=?1"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            registrationQuery.get(),
            1,
            project.hash().hex()
        ));
        if (sqlite3_step(registrationQuery.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "ProjectInstance requires a registered ProjectRegistration"
            );
        }

        // Existence under the executable identity kind is the whole of what
        // this row can still tell provisioning. The plugin id, the code digest
        // and the canonical bytes were compared here as well, and none of the
        // three could be made to fail: the row is keyed by the digest of its
        // own canonical bytes, registerProject proves that digest before it
        // writes, and every other column is derived from those same bytes -- so
        // a row found under this root already holds this identity's fields, and
        // the comparison was three restatements of the lookup that found it.
        UF_TRY(requireExecutableRegistrationFormat(registrationQuery.get(), 0));

        UF_TRY_VALUE(
            existing,
            prepare(
                m_impl->database.get(),
                "SELECT project_registration_hash "
                "FROM project_instances WHERE plugin_id=?1 "
                "AND project_instance_key=?2"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            existing.get(),
            1,
            project.pluginId()
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            existing.get(),
            2,
            projectInstanceKey
        ));
        if (sqlite3_step(existing.get()) == SQLITE_ROW)
        {
            // Provisioning states one fact, so provisioning it again with the
            // same fact is the fact already being true rather than a second
            // write. A key already held by another registration is a different
            // fact and is refused.
            if (columnText(existing.get(), 0) == project.hash().hex())
            {
                return transaction.commit();
            }
            return fail(
                AutomationErrorKind::ActionRejected,
                "project_instance_key is immutable and already provisioned"
            );
        }

        UF_TRY_VALUE(
            instanceInsert,
            prepare(
                m_impl->database.get(),
                "INSERT INTO project_instances(project_instance_key, plugin_id, "
                "project_registration_hash) VALUES(?1, ?2, ?3)"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            instanceInsert.get(),
            1,
            projectInstanceKey
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            instanceInsert.get(),
            2,
            project.pluginId()
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            instanceInsert.get(),
            3,
            project.hash().hex()
        ));
        UF_TRY(expectDone(m_impl->database.get(), instanceInsert.get()));

        return transaction.commit();
    }

    auto OperatorCoordinator::pinSession(
        SessionPin const& pin,
        SessionManifest const& manifest,
        AgentProfile const& agentProfile
    ) -> Status
    {
        UF_TRY(requireName(pin.sessionId, "session_id"));
        UF_TRY(requireName(pin.authenticatedControllerId, "authenticated_controller_id"));
        UF_TRY(requireName(pin.idempotencyNamespace, "idempotency_namespace"));
        UF_TRY(requireName(pin.controlledTargetId, "controlled_target_id"));
        UF_TRY(requireName(pin.projectInstanceKey, "project_instance_key"));
        if (pin.projectRegistrationHash != manifest.projectRegistrationHash())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "SessionManifest does not bind the selected "
                    "ProjectRegistration: manifest names {}, pin selected {}",
                    manifest.projectRegistrationHash().hex(),
                    pin.projectRegistrationHash.hex()
                )
            );
        }
        if (agentProfile.sessionManifestHash() != manifest.hash())
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "AgentProfile was verified against a different SessionManifest"
            );
        }
        for (auto const& capability : pin.controllerCapabilities)
        {
            UF_TRY(requireName(capability, "controller capability"));
        }

        // The profile hash is the sha256 of the set, derived here and never
        // stated: a caller that could name it could pin a session whose
        // capability hash and capability set were about different things.
        auto const capabilities = canonicalNameArray(pin.controllerCapabilities);
        UF_TRY_VALUE(
            capabilityProfileHash,
            sha256(std::as_bytes(std::span{capabilities}))
        );

        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY(requireExecutableRegistration(
            m_impl->database.get(),
            pin.projectRegistrationHash
        ));

        auto const runtimeArtifactRootHash = manifest.runtimeModelArtifactRootHash();
        UF_TRY_VALUE(
            runtimeArtifactQuery,
            prepare(
                m_impl->database.get(),
                "SELECT installed_generation FROM runtime_state WHERE singleton=1 "
                "AND active_runtime_artifact_root_hash=?1"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            runtimeArtifactQuery.get(),
            1,
            runtimeArtifactRootHash.hex()
        ));
        if (sqlite3_step(runtimeArtifactQuery.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "SessionManifest RuntimeArtifact is not production-installed"
            );
        }
        auto const installedGeneration = static_cast<uint64>(
            sqlite3_column_int64(runtimeArtifactQuery.get(), 0)
        );
        UF_TRY_VALUE(
            releaseUpgrade,
            isReleaseUpgradeSessionPin(
                m_impl->database.get(),
                runtimeArtifactRootHash,
                pin.projectRegistrationHash,
                pin.projectInstanceKey
            )
        );
        if (releaseUpgrade)
        {
            UF_TRY(requireQuiescentSessionPin(m_impl->database.get()));
            UF_TRY(requireApprovedCapabilityExpansion(
                m_impl->database.get(),
                runtimeArtifactRootHash,
                pin.projectRegistrationHash,
                pin.projectInstanceKey,
                pin.controllerCapabilities,
                capabilityProfileHash
            ));
        }

        UF_TRY_VALUE(
            instanceQuery,
            prepare(
                m_impl->database.get(),
                "SELECT 1 FROM project_instances WHERE project_registration_hash=?1 "
                "AND project_instance_key=?2"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            instanceQuery.get(),
            1,
            pin.projectRegistrationHash.hex()
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            instanceQuery.get(),
            2,
            pin.projectInstanceKey
        ));
        if (
            sqlite3_step(instanceQuery.get()) != SQLITE_ROW
        )
        {
            // No row means no ProjectInstance exists for this exact pair --
            // there is no "actual" registration hash to print beside it. The
            // table's natural key also needs plugin_id, which this pin does
            // not carry, so which registration (if any) the instance key
            // really is pinned to cannot be named here without a further
            // lookup this refusal does not owe. What it does already hold is
            // the pair it searched for.
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "Session requires an existing ProjectInstance pinned to "
                    "project_registration_hash {} at project_instance_key {}",
                    pin.projectRegistrationHash.hex(),
                    pin.projectInstanceKey
                )
            );
        }

        UF_TRY_VALUE(
            insert,
            prepare(
                m_impl->database.get(),
                "INSERT OR IGNORE INTO sessions"
                "(session_id, authenticated_controller_id, idempotency_namespace, "
                "manifest_hash, runtime_artifact_root_hash, installed_generation, "
                "project_registration_hash, controller_capabilities, "
                "capability_profile_hash, session_epoch, "
                "controlled_target_id, project_instance_key, mode, controller_kind, "
                "world_scope_kind, world_scope_id, world_scope_generation, "
                "active) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, "
                "?15, ?16, ?17, 1)"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 1, pin.sessionId));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            2,
            pin.authenticatedControllerId
        ));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 3, pin.idempotencyNamespace));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 4, manifest.hash().hex()));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            5,
            runtimeArtifactRootHash.hex()
        ));
        UF_TRY(bindInteger(
            m_impl->database.get(),
            insert.get(),
            6,
            installedGeneration
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            7,
            pin.projectRegistrationHash.hex()
        ));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 8, capabilities));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            9,
            capabilityProfileHash.hex()
        ));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 10, m_impl->sessionEpoch));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 11, pin.controlledTargetId));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 12, pin.projectInstanceKey));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            13,
            sessionModeWireName(pin.mode)
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            14,
            controllerKindWireName(pin.kind)
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            15,
            observedInstanceWorldScopeKindWireName(pin.worldScope.kind())
        ));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 16, pin.worldScope.scopeId()));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            17,
            std::to_string(pin.worldScope.generation())
        ));
        UF_TRY(expectDone(m_impl->database.get(), insert.get()));

        UF_TRY_VALUE(
            query,
            prepare(
                m_impl->database.get(),
                "SELECT authenticated_controller_id, idempotency_namespace, manifest_hash, "
                "runtime_artifact_root_hash, installed_generation, "
                "project_registration_hash, controller_capabilities, "
                "capability_profile_hash, session_epoch, "
                "controlled_target_id, project_instance_key, mode, controller_kind, "
                "world_scope_kind, world_scope_id, world_scope_generation, "
                "active "
                "FROM sessions WHERE session_id=?1"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), query.get(), 1, pin.sessionId));
        if (sqlite3_step(query.get()) != SQLITE_ROW)
        {
            return databaseFailure(m_impl->database.get(), "could not pin session");
        }
        auto const matches = columnText(query.get(), 0) == pin.authenticatedControllerId
            && columnText(query.get(), 1) == pin.idempotencyNamespace
            && columnText(query.get(), 2) == manifest.hash().hex()
            && columnText(query.get(), 3) == runtimeArtifactRootHash.hex()
            && static_cast<uint64>(sqlite3_column_int64(query.get(), 4))
                == installedGeneration
            && columnText(query.get(), 5) == pin.projectRegistrationHash.hex()
            && columnText(query.get(), 6) == capabilities
            && columnText(query.get(), 7) == capabilityProfileHash.hex()
            && static_cast<uint64>(sqlite3_column_int64(query.get(), 8))
                == m_impl->sessionEpoch
            && columnText(query.get(), 9) == pin.controlledTargetId
            && columnText(query.get(), 10) == pin.projectInstanceKey
            && columnText(query.get(), 11) == sessionModeWireName(pin.mode)
            && columnText(query.get(), 12) == controllerKindWireName(pin.kind)
            && columnText(query.get(), 13)
                == observedInstanceWorldScopeKindWireName(pin.worldScope.kind())
            && columnText(query.get(), 14) == pin.worldScope.scopeId()
            && columnText(query.get(), 15) == std::to_string(pin.worldScope.generation())
            && sqlite3_column_int(query.get(), 16) == 1;
        if (!matches)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "session_id already names a different immutable session tuple"
            );
        }

        UF_TRY_VALUE(
            policyInsert,
            prepare(
                m_impl->database.get(),
                "INSERT OR IGNORE INTO session_policies(session_id, policy_hash) "
                "VALUES(?1, ?2)"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), policyInsert.get(), 1, pin.sessionId));
        UF_TRY(bindText(
            m_impl->database.get(),
            policyInsert.get(),
            2,
            manifest.policyArtifactHash().hex()
        ));
        UF_TRY(expectDone(m_impl->database.get(), policyInsert.get()));
        UF_TRY_VALUE(
            policyQuery,
            prepare(
                m_impl->database.get(),
                "SELECT policy_hash FROM session_policies WHERE session_id=?1"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), policyQuery.get(), 1, pin.sessionId));
        if (
            sqlite3_step(policyQuery.get()) != SQLITE_ROW
            || columnText(policyQuery.get(), 0) != manifest.policyArtifactHash().hex()
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "session_id already names a different policy artifact"
            );
        }

        {
            auto const budget = agentProfile.budget();
            UF_TRY_VALUE(now, steadyMillisecondsNow());
            constexpr auto ceiling = static_cast<uint64>(
                std::numeric_limits<sqlite3_int64>::max()
            );

            // An unbounded elapsed ceiling is a deadline nothing reaches
            // rather than an arithmetic overflow: it is stored as the largest
            // instant the column holds, so requireWithinAgentDeadline compares
            // against it on exactly the same path every bounded session uses.
            auto const deadline = budget.maximumElapsedMillis == k_unboundedBudget
                ? k_unboundedBudget
                : now + budget.maximumElapsedMillis;
            if (
                budget.maximumElapsedMillis != k_unboundedBudget
                && budget.maximumElapsedMillis > ceiling - now
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "session time budget exhausted SQLite's integer range"
                );
            }

            // OR IGNORE, so that re-pinning an existing session leaves its
            // budget exactly as spent. A conflict rule that replaced the row
            // would make pinning again the one way to refresh a spent budget,
            // and an exhausted Agent would only have to ask for its own session
            // twice.
            UF_TRY_VALUE(
                budgetInsert,
                prepare(
                    m_impl->database.get(),
                    "INSERT OR IGNORE INTO agent_budgets(session_id, "
                    "deadline_steady_millis, remaining_tool_calls, "
                    "remaining_mutations, remaining_observations, "
                    "remaining_risk_units, last_state_fingerprint, "
                    "last_command_fingerprint, consecutive_no_progress_steps) "
                    "VALUES(?1, ?2, ?3, ?4, ?5, ?6, '', '', 0)"
                )
            );
            UF_TRY(bindText(m_impl->database.get(), budgetInsert.get(), 1, pin.sessionId));
            UF_TRY(bindInteger(
                m_impl->database.get(),
                budgetInsert.get(),
                2,
                deadline
            ));
            UF_TRY(bindInteger(
                m_impl->database.get(),
                budgetInsert.get(),
                3,
                budget.maximumToolCalls
            ));
            UF_TRY(bindInteger(
                m_impl->database.get(),
                budgetInsert.get(),
                4,
                budget.maximumMutations
            ));
            UF_TRY(bindInteger(
                m_impl->database.get(),
                budgetInsert.get(),
                5,
                budget.maximumObservations
            ));
            UF_TRY(bindInteger(
                m_impl->database.get(),
                budgetInsert.get(),
                6,
                budget.maximumRiskUnits
            ));
            UF_TRY(expectDone(m_impl->database.get(), budgetInsert.get()));
        }
        return transaction.commit();
    }

    auto OperatorCoordinator::resumeSession(
        SessionResume const& resume,
        SessionManifest const& manifest
    ) -> Result<ControllerBinding>
    {
        UF_TRY(requireName(
            resume.authenticatedControllerId,
            "authenticated_controller_id"
        ));
        UF_TRY(requireName(resume.controlledTargetId, "controlled_target_id"));

        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY(requireExecutableRegistration(
            m_impl->database.get(),
            manifest.projectRegistrationHash()
        ));
        UF_TRY_VALUE(
            installedGeneration,
            activeInstalledGeneration(
                m_impl->database.get(),
                manifest.runtimeModelArtifactRootHash()
            )
        );
        UF_TRY_VALUE(
            query,
            prepare(
                m_impl->database.get(),
                "SELECT session_id, session_epoch FROM sessions "
                "WHERE authenticated_controller_id=?1 AND manifest_hash=?2 "
                "AND runtime_artifact_root_hash=?3 "
                "AND project_registration_hash=?4 "
                "AND controlled_target_id=?5 AND mode=?6 "
                "AND controller_kind=?7 AND active=0 "
                "ORDER BY session_epoch DESC, session_id"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            query.get(),
            1,
            resume.authenticatedControllerId
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            query.get(),
            2,
            manifest.hash().hex()
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            query.get(),
            3,
            manifest.runtimeModelArtifactRootHash().hex()
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            query.get(),
            4,
            manifest.projectRegistrationHash().hex()
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            query.get(),
            5,
            resume.controlledTargetId
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            query.get(),
            6,
            sessionModeWireName(resume.mode)
        ));
        UF_TRY(bindText(
            m_impl->database.get(),
            query.get(),
            7,
            controllerKindWireName(resume.kind)
        ));
        if (sqlite3_step(query.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "No compatible prior session is available to resume"
            );
        }
        auto const sessionId = columnText(query.get(), 0);
        auto const priorEpoch = static_cast<uint64>(
            sqlite3_column_int64(query.get(), 1)
        );
        auto const second = sqlite3_step(query.get());
        if (
            second == SQLITE_ROW
            && static_cast<uint64>(sqlite3_column_int64(query.get(), 1))
                == priorEpoch
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "More than one most-recent compatible prior session exists"
            );
        }
        if (second != SQLITE_ROW && second != SQLITE_DONE)
        {
            return databaseFailure(
                m_impl->database.get(),
                "could not select a prior session to resume"
            );
        }

        // The one thing a process epoch destroys. Every ceiling but the clock
        // is a durable counter that survives a restart exactly as it stood; the
        // elapsed ceiling was turned into a steady-clock instant by the process
        // that pinned it, and no later process can say what that instant means.
        // So a session whose operator BOUND its time cannot resume, and one
        // whose operator declared it unbounded has no instant to lose.
        UF_TRY_VALUE(
            priorBudget,
            readAgentBudget(m_impl->database.get(), sessionId)
        );
        if (priorBudget.deadlineSteadyMillis != k_unboundedBudget)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "a session whose declared time budget binds cannot resume "
                "across a process epoch"
            );
        }

        UF_TRY_VALUE(
            policyInsert,
            prepare(
                m_impl->database.get(),
                "INSERT OR IGNORE INTO session_policies(session_id, policy_hash) "
                "VALUES(?1, ?2)"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), policyInsert.get(), 1, sessionId));
        UF_TRY(bindText(
            m_impl->database.get(),
            policyInsert.get(),
            2,
            manifest.policyArtifactHash().hex()
        ));
        UF_TRY(expectDone(m_impl->database.get(), policyInsert.get()));
        UF_TRY_VALUE(
            policyQuery,
            prepare(
                m_impl->database.get(),
                "SELECT policy_hash FROM session_policies WHERE session_id=?1"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), policyQuery.get(), 1, sessionId));
        if (
            sqlite3_step(policyQuery.get()) != SQLITE_ROW
            || columnText(policyQuery.get(), 0) != manifest.policyArtifactHash().hex()
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Prior session policy does not match the supplied SessionManifest"
            );
        }

        UF_TRY_VALUE(
            update,
            prepare(
                m_impl->database.get(),
                "UPDATE sessions SET installed_generation=?1, "
                "session_epoch=?2, active=1 WHERE session_id=?3 "
                "AND session_epoch=?4 AND active=0"
            )
        );
        UF_TRY(bindInteger(
            m_impl->database.get(),
            update.get(),
            1,
            installedGeneration
        ));
        UF_TRY(bindInteger(
            m_impl->database.get(),
            update.get(),
            2,
            m_impl->sessionEpoch
        ));
        UF_TRY(bindText(m_impl->database.get(), update.get(), 3, sessionId));
        UF_TRY(bindInteger(m_impl->database.get(), update.get(), 4, priorEpoch));
        UF_TRY(expectDone(m_impl->database.get(), update.get()));
        if (sqlite3_changes(m_impl->database.get()) != 1)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Prior session resume lost its compare-and-swap"
            );
        }

        UF_TRY_VALUE(controller, bindController(sessionId));
        UF_TRY(transaction.commit());
        return controller;
    }

    auto OperatorCoordinator::bindController(
        std::string const& sessionId
    ) -> Result<ControllerBinding>
    {
        UF_TRY(requireName(sessionId, "session_id"));
        UF_TRY_VALUE(
            query,
            prepare(
                m_impl->database.get(),
                "SELECT authenticated_controller_id, controlled_target_id, "
                "capability_profile_hash, session_epoch, controller_kind, mode "
                "FROM sessions WHERE session_id=?1 AND active=1 AND session_epoch=?2"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), query.get(), 1, sessionId));
        UF_TRY(bindInteger(
            m_impl->database.get(),
            query.get(),
            2,
            m_impl->sessionEpoch
        ));
        if (sqlite3_step(query.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Cannot bind a controller to an unknown current-epoch session"
            );
        }
        UF_TRY_VALUE(capabilityHash, parseHashColumn(columnText(query.get(), 2)));
        UF_TRY_VALUE(kind, parseControllerKind(columnText(query.get(), 4)));
        UF_TRY_VALUE(mode, parseSessionMode(columnText(query.get(), 5)));
        if (kind == ControllerKind::Agent && mode == SessionMode::Read)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "An Agent cannot bind a read-mode session"
            );
        }
        return ControllerBinding{
            sessionId,
            columnText(query.get(), 0),
            columnText(query.get(), 1),
            capabilityHash,
            static_cast<uint64>(sqlite3_column_int64(query.get(), 3)),
            kind,
        };
    }

    auto OperatorCoordinator::acquireLease(
        ControllerBinding const& controller
    ) -> Result<ControlLease>
    {
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY_VALUE(
            mode,
            requireLiveBinding(m_impl->database.get(), controller)
        );
        if (mode != SessionMode::Write)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Cannot acquire a lease for a read-mode session"
            );
        }
        auto const& sessionId = controller.sessionId();
        auto const& target = controller.controlledTargetId();
        auto const& controllerId = controller.controllerId();
        auto const capabilityProfileHash = controller.capabilityProfileHash().hex();
        auto const capabilityHash = controller.capabilityProfileHash();
        auto const sessionEpoch = controller.sessionEpoch();

        UF_TRY_VALUE(
            activeQuery,
            prepare(
                m_impl->database.get(),
                "SELECT 1 FROM control_leases WHERE controlled_target_id=?1"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), activeQuery.get(), 1, target));
        if (sqlite3_step(activeQuery.get()) == SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "ControlledTarget already has an active write lease"
            );
        }

        auto previousFence = uint64{0};
        UF_TRY_VALUE(
            highWaterQuery,
            prepare(
                m_impl->database.get(),
                "SELECT fencing_token FROM fencing_high_water WHERE controlled_target_id=?1"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), highWaterQuery.get(), 1, target));
        if (sqlite3_step(highWaterQuery.get()) == SQLITE_ROW)
        {
            previousFence = static_cast<uint64>(sqlite3_column_int64(highWaterQuery.get(), 0));
        }
        if (previousFence == static_cast<uint64>(std::numeric_limits<sqlite3_int64>::max()))
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "Control fencing token exhausted"
            );
        }
        auto const nextFence = previousFence + 1U;
        UF_TRY_VALUE(leaseId, randomToken(m_impl->database.get(), k_opaqueTokenBytes));

        UF_TRY_VALUE(
            highWaterWrite,
            prepare(
                m_impl->database.get(),
                "INSERT INTO fencing_high_water(controlled_target_id, fencing_token) "
                "VALUES(?1, ?2) ON CONFLICT(controlled_target_id) DO UPDATE SET "
                "fencing_token=excluded.fencing_token"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), highWaterWrite.get(), 1, target));
        UF_TRY(bindInteger(m_impl->database.get(), highWaterWrite.get(), 2, nextFence));
        UF_TRY(expectDone(m_impl->database.get(), highWaterWrite.get()));

        UF_TRY_VALUE(
            leaseWrite,
            prepare(
                m_impl->database.get(),
                "INSERT INTO control_leases(controlled_target_id, lease_id, session_id, "
                "controller_id, session_epoch, fencing_token, revision, "
                "capability_profile_hash) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?6, ?7)"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), leaseWrite.get(), 1, target));
        UF_TRY(bindText(m_impl->database.get(), leaseWrite.get(), 2, leaseId));
        UF_TRY(bindText(m_impl->database.get(), leaseWrite.get(), 3, sessionId));
        UF_TRY(bindText(m_impl->database.get(), leaseWrite.get(), 4, controllerId));
        UF_TRY(bindInteger(m_impl->database.get(), leaseWrite.get(), 5, sessionEpoch));
        UF_TRY(bindInteger(m_impl->database.get(), leaseWrite.get(), 6, nextFence));
        UF_TRY(bindText(
            m_impl->database.get(),
            leaseWrite.get(),
            7,
            capabilityProfileHash
        ));
        UF_TRY(expectDone(m_impl->database.get(), leaseWrite.get()));

        UF_TRY_VALUE(
            transitionWrite,
            prepare(
                m_impl->database.get(),
                "INSERT INTO control_transitions(controlled_target_id, session_id, controller_id, "
                "lease_id, session_epoch, fencing_token, transition, reason) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, 'acquire', 'ordinary acquire')"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), transitionWrite.get(), 1, target));
        UF_TRY(bindText(m_impl->database.get(), transitionWrite.get(), 2, sessionId));
        UF_TRY(bindText(m_impl->database.get(), transitionWrite.get(), 3, controllerId));
        UF_TRY(bindText(m_impl->database.get(), transitionWrite.get(), 4, leaseId));
        UF_TRY(bindInteger(m_impl->database.get(), transitionWrite.get(), 5, sessionEpoch));
        UF_TRY(bindInteger(m_impl->database.get(), transitionWrite.get(), 6, nextFence));
        UF_TRY(expectDone(m_impl->database.get(), transitionWrite.get()));

        UF_TRY(appendLedgerEvent(
            m_impl->database.get(),
            sessionEpoch,
            target,
            LedgerEventKind::ControlTransitioned,
            leaseId
        ));

        UF_TRY(transaction.commit());
        return ControlLease{
            .leaseId               = std::move(leaseId),
            .sessionId             = sessionId,
            .controlledTargetId    = target,
            .controllerId          = controllerId,
            .sessionEpoch          = sessionEpoch,
            .fencingToken          = nextFence,
            .revision              = nextFence,
            .capabilityProfileHash = capabilityHash,
        };
    }

    auto controlFence(ControlLease const& lease) -> task::ControlFence
    {
        return task::ControlFence{
            .controlledTargetId = lease.controlledTargetId,
            .sessionEpoch       = lease.sessionEpoch,
            .fencingToken       = lease.fencingToken,
        };
    }

    auto OperatorCoordinator::takeoverLease(
        ControllerBinding const& controller,
        std::string const& reason
    ) -> Result<ControlTakeover>
    {
        UF_TRY(requireName(reason, "takeover reason"));
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY_VALUE(
            mode,
            requireLiveBinding(m_impl->database.get(), controller)
        );
        if (mode != SessionMode::Write)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Cannot take over control with a read-mode session"
            );
        }
        auto const& sessionId = controller.sessionId();
        auto const& target = controller.controlledTargetId();
        auto const& controllerId = controller.controllerId();
        auto const capabilityProfileHash = controller.capabilityProfileHash().hex();
        auto const capabilityHash = controller.capabilityProfileHash();
        auto const sessionEpoch = controller.sessionEpoch();

        auto previousFence = uint64{0};
        UF_TRY_VALUE(
            highWaterQuery,
            prepare(
                m_impl->database.get(),
                "SELECT fencing_token FROM fencing_high_water WHERE controlled_target_id=?1"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), highWaterQuery.get(), 1, target));
        if (sqlite3_step(highWaterQuery.get()) == SQLITE_ROW)
        {
            previousFence = static_cast<uint64>(
                sqlite3_column_int64(highWaterQuery.get(), 0)
            );
        }
        if (previousFence == static_cast<uint64>(std::numeric_limits<sqlite3_int64>::max()))
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "Control fencing token exhausted"
            );
        }
        auto const nextFence = previousFence + 1U;
        UF_TRY_VALUE(leaseId, randomToken(m_impl->database.get(), k_opaqueTokenBytes));

        UF_TRY_VALUE(
            highWaterWrite,
            prepare(
                m_impl->database.get(),
                "INSERT INTO fencing_high_water(controlled_target_id, fencing_token) "
                "VALUES(?1, ?2) ON CONFLICT(controlled_target_id) DO UPDATE SET "
                "fencing_token=excluded.fencing_token"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), highWaterWrite.get(), 1, target));
        UF_TRY(bindInteger(m_impl->database.get(), highWaterWrite.get(), 2, nextFence));
        UF_TRY(expectDone(m_impl->database.get(), highWaterWrite.get()));

        UF_TRY_VALUE(
            leaseWrite,
            prepare(
                m_impl->database.get(),
                "INSERT INTO control_leases(controlled_target_id, lease_id, session_id, "
                "controller_id, session_epoch, fencing_token, revision, "
                "capability_profile_hash) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?6, ?7) "
                "ON CONFLICT(controlled_target_id) DO UPDATE SET lease_id=excluded.lease_id, "
                "session_id=excluded.session_id, controller_id=excluded.controller_id, "
                "session_epoch=excluded.session_epoch, fencing_token=excluded.fencing_token, "
                "revision=excluded.revision, "
                "capability_profile_hash=excluded.capability_profile_hash"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), leaseWrite.get(), 1, target));
        UF_TRY(bindText(m_impl->database.get(), leaseWrite.get(), 2, leaseId));
        UF_TRY(bindText(m_impl->database.get(), leaseWrite.get(), 3, sessionId));
        UF_TRY(bindText(m_impl->database.get(), leaseWrite.get(), 4, controllerId));
        UF_TRY(bindInteger(m_impl->database.get(), leaseWrite.get(), 5, sessionEpoch));
        UF_TRY(bindInteger(m_impl->database.get(), leaseWrite.get(), 6, nextFence));
        UF_TRY(bindText(
            m_impl->database.get(),
            leaseWrite.get(),
            7,
            capabilityProfileHash
        ));
        UF_TRY(expectDone(m_impl->database.get(), leaseWrite.get()));

        UF_TRY_VALUE(
            transitionWrite,
            prepare(
                m_impl->database.get(),
                "INSERT INTO control_transitions(controlled_target_id, session_id, controller_id, "
                "lease_id, session_epoch, fencing_token, transition, reason) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, 'takeover', ?7)"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), transitionWrite.get(), 1, target));
        UF_TRY(bindText(m_impl->database.get(), transitionWrite.get(), 2, sessionId));
        UF_TRY(bindText(m_impl->database.get(), transitionWrite.get(), 3, controllerId));
        UF_TRY(bindText(m_impl->database.get(), transitionWrite.get(), 4, leaseId));
        UF_TRY(bindInteger(m_impl->database.get(), transitionWrite.get(), 5, sessionEpoch));
        UF_TRY(bindInteger(m_impl->database.get(), transitionWrite.get(), 6, nextFence));
        UF_TRY(bindText(m_impl->database.get(), transitionWrite.get(), 7, reason));
        UF_TRY(expectDone(m_impl->database.get(), transitionWrite.get()));

        UF_TRY(appendLedgerEvent(
            m_impl->database.get(),
            sessionEpoch,
            target,
            LedgerEventKind::ControlTransitioned,
            leaseId
        ));

        UF_TRY(transaction.commit());
        return ControlTakeover{
            .lease = ControlLease{
                .leaseId               = std::move(leaseId),
                .sessionId             = sessionId,
                .controlledTargetId    = target,
                .controllerId          = controllerId,
                .sessionEpoch          = sessionEpoch,
                .fencingToken          = nextFence,
                .revision              = nextFence,
                .capabilityProfileHash = capabilityHash,
            },
        };
    }

    auto OperatorCoordinator::releaseLease(
        ControlLease const& lease
    ) -> Result<uint64>
    {
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY_VALUE(
            leaseQuery,
            prepare(
                m_impl->database.get(),
                "SELECT 1 FROM control_leases WHERE controlled_target_id=?1 "
                "AND lease_id=?2 AND session_id=?3 AND controller_id=?4 "
                "AND session_epoch=?5 AND fencing_token=?6 AND revision=?7 "
                "AND capability_profile_hash=?8"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), leaseQuery.get(), 1, lease.controlledTargetId));
        UF_TRY(bindText(m_impl->database.get(), leaseQuery.get(), 2, lease.leaseId));
        UF_TRY(bindText(m_impl->database.get(), leaseQuery.get(), 3, lease.sessionId));
        UF_TRY(bindText(m_impl->database.get(), leaseQuery.get(), 4, lease.controllerId));
        UF_TRY(bindInteger(m_impl->database.get(), leaseQuery.get(), 5, lease.sessionEpoch));
        UF_TRY(bindInteger(m_impl->database.get(), leaseQuery.get(), 6, lease.fencingToken));
        UF_TRY(bindInteger(m_impl->database.get(), leaseQuery.get(), 7, lease.revision));
        UF_TRY(bindText(
            m_impl->database.get(),
            leaseQuery.get(),
            8,
            lease.capabilityProfileHash.hex()
        ));
        if (sqlite3_step(leaseQuery.get()) != SQLITE_ROW)
        {
            return fail(AutomationErrorKind::ActionRejected, "Control lease is stale");
        }
        if (
            lease.sessionEpoch != m_impl->sessionEpoch
            || lease.fencingToken
                == static_cast<uint64>(std::numeric_limits<sqlite3_int64>::max())
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Control lease cannot be released in this session epoch"
            );
        }
        auto const nextFence = lease.fencingToken + 1U;

        UF_TRY_VALUE(
            highWaterUpdate,
            prepare(
                m_impl->database.get(),
                "UPDATE fencing_high_water SET fencing_token=?1 "
                "WHERE controlled_target_id=?2 AND fencing_token=?3"
            )
        );
        UF_TRY(bindInteger(m_impl->database.get(), highWaterUpdate.get(), 1, nextFence));
        UF_TRY(bindText(
            m_impl->database.get(),
            highWaterUpdate.get(),
            2,
            lease.controlledTargetId
        ));
        UF_TRY(bindInteger(
            m_impl->database.get(),
            highWaterUpdate.get(),
            3,
            lease.fencingToken
        ));
        UF_TRY(expectDone(m_impl->database.get(), highWaterUpdate.get()));
        if (sqlite3_changes(m_impl->database.get()) != 1)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Control fencing high-water lost its release CAS"
            );
        }

        UF_TRY_VALUE(
            leaseDelete,
            prepare(
                m_impl->database.get(),
                "DELETE FROM control_leases WHERE controlled_target_id=?1 AND lease_id=?2"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            leaseDelete.get(),
            1,
            lease.controlledTargetId
        ));
        UF_TRY(bindText(m_impl->database.get(), leaseDelete.get(), 2, lease.leaseId));
        UF_TRY(expectDone(m_impl->database.get(), leaseDelete.get()));
        if (sqlite3_changes(m_impl->database.get()) != 1)
        {
            return fail(AutomationErrorKind::ActionRejected, "Control lease lost its release CAS");
        }

        UF_TRY_VALUE(
            transitionWrite,
            prepare(
                m_impl->database.get(),
                "INSERT INTO control_transitions(controlled_target_id, session_id, controller_id, "
                "lease_id, session_epoch, fencing_token, transition, reason) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, 'release', 'explicit release')"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            transitionWrite.get(),
            1,
            lease.controlledTargetId
        ));
        UF_TRY(bindText(m_impl->database.get(), transitionWrite.get(), 2, lease.sessionId));
        UF_TRY(bindText(m_impl->database.get(), transitionWrite.get(), 3, lease.controllerId));
        UF_TRY(bindText(m_impl->database.get(), transitionWrite.get(), 4, lease.leaseId));
        UF_TRY(bindInteger(m_impl->database.get(), transitionWrite.get(), 5, lease.sessionEpoch));
        UF_TRY(bindInteger(m_impl->database.get(), transitionWrite.get(), 6, nextFence));
        UF_TRY(expectDone(m_impl->database.get(), transitionWrite.get()));

        UF_TRY(appendLedgerEvent(
            m_impl->database.get(),
            lease.sessionEpoch,
            lease.controlledTargetId,
            LedgerEventKind::ControlTransitioned,
            lease.leaseId
        ));

        UF_TRY(transaction.commit());
        return nextFence;
    }

    auto OperatorCoordinator::createSnapshot(
        ControlLease const& lease,
        ProjectIdentity const& project,
        ProjectToolCatalogSchemaOwner const& catalog,
        ObservedInstanceIdentitySchemas const& identitySchemas,
        task::UiObservationSnapshot const& observation
    ) -> Result<SnapshotRecord>
    {
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY_VALUE(
            leaseQuery,
            prepare(
                m_impl->database.get(),
                "SELECT lease_id, session_id, controller_id, session_epoch, fencing_token, "
                "revision, capability_profile_hash FROM control_leases "
                "WHERE controlled_target_id=?1"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            leaseQuery.get(),
            1,
            lease.controlledTargetId
        ));
        if (sqlite3_step(leaseQuery.get()) != SQLITE_ROW)
        {
            return fail(AutomationErrorKind::ActionRejected, "Control lease is not active");
        }
        auto const matches = columnText(leaseQuery.get(), 0) == lease.leaseId
            && columnText(leaseQuery.get(), 1) == lease.sessionId
            && columnText(leaseQuery.get(), 2) == lease.controllerId
            && static_cast<uint64>(sqlite3_column_int64(leaseQuery.get(), 3))
                == lease.sessionEpoch
            && static_cast<uint64>(sqlite3_column_int64(leaseQuery.get(), 4))
                == lease.fencingToken
            && static_cast<uint64>(sqlite3_column_int64(leaseQuery.get(), 5))
                == lease.revision
            && columnText(leaseQuery.get(), 6) == lease.capabilityProfileHash.hex()
            && lease.sessionEpoch == m_impl->sessionEpoch;
        if (!matches)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Control lease was superseded before snapshot creation"
            );
        }

        UF_TRY_VALUE(
            sessionQuery,
            prepare(
                m_impl->database.get(),
                "SELECT session.project_instance_key, session.manifest_hash, "
                "session.controlled_target_id, session.runtime_artifact_root_hash, "
                "registration.plugin_id, registration.plugin_identity_hash, "
                "session.project_registration_hash, session.controller_kind, "
                "session.controller_capabilities, policy.policy_hash, "
                "session.world_scope_kind, session.world_scope_id, "
                "session.world_scope_generation, registration.registration_format, "
                "registration.plugin_identity_kind "
                "FROM sessions session JOIN project_registrations registration "
                "ON registration.registration_hash=session.project_registration_hash "
                "JOIN session_policies policy ON policy.session_id=session.session_id "
                "WHERE session.session_id=?1 AND session.active=1 "
                "AND session.session_epoch=?2"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), sessionQuery.get(), 1, lease.sessionId));
        UF_TRY(bindInteger(
            m_impl->database.get(),
            sessionQuery.get(),
            2,
            m_impl->sessionEpoch
        ));
        if (sqlite3_step(sessionQuery.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Snapshot session is not active in this epoch"
            );
        }
        UF_TRY(requireExecutableRegistrationFormat(sessionQuery.get(), 13));
        auto const projectInstanceKey  = columnText(sessionQuery.get(), 0);
        auto const sessionManifestHex  = columnText(sessionQuery.get(), 1);
        auto const controlledTargetId = columnText(sessionQuery.get(), 2);
        auto const sessionArtifactRoot = columnText(sessionQuery.get(), 3);
        auto const pluginId            = columnText(sessionQuery.get(), 4);

        // A handle for another registration is a different project reading
        // this world, so the identity is compared before anything is read.
        if (
            project.pluginId() != pluginId
            || project.moduleIdentityHash().hex()
                != columnText(sessionQuery.get(), 5)
            || project.hash().hex() != columnText(sessionQuery.get(), 6)
            || catalog.projectRegistrationHash().hex() != columnText(sessionQuery.get(), 6)
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Snapshot registration does not match the pinned session registration"
            );
        }

        // The scope this session was pinned under, rebuilt from its stored
        // tuple. It is the scope the observation's instances mint in: the
        // plugin output carries no scope and cannot name one.
        UF_TRY_VALUE(
            worldScope,
            restoreSessionWorldScope(
                columnText(sessionQuery.get(), 10),
                columnText(sessionQuery.get(), 11),
                columnText(sessionQuery.get(), 12)
            )
        );

        // Without this a snapshot can be composed from a UI observation taken
        // through a RuntimeArtifact the session never pinned, and
        // session_manifest_hash would attest to a model that produced none of
        // the evidence.
        if (observation.artifactRootHash().hex() != sessionArtifactRoot)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "UI observation was taken through a RuntimeArtifact this session "
                "did not pin"
            );
        }

        // Charged here rather than after the composition, and inside the same
        // BEGIN IMMEDIATE: a refused observation budget must not first pay for
        // a plugin derive under the write lock. There is no ControllerBinding
        // parameter and none is wanted -- the lease already names the session
        // this observation is charged to, and a binding beside it would be a
        // second spelling of one identity that would then have to be kept
        // equal. There is no caller-supplied instant either, for the reason
        // steadyMillisecondsNow states.
        UF_TRY_VALUE(snapshotKind, parseControllerKind(columnText(sessionQuery.get(), 7)));
        UF_TRY_VALUE(
            snapshotCapabilities,
            readNameArray(columnText(sessionQuery.get(), 8))
        );
        auto availableTools = catalog.offeredTools(
            controllerProfile(snapshotKind),
            snapshotCapabilities
        );
        auto availableToolNames = std::vector<std::string>{};
        availableToolNames.reserve(availableTools.size());
        for (auto const& tool : availableTools)
        {
            availableToolNames.emplace_back(tool.name);
        }
        auto const availableToolsJcs = canonicalNameArray(availableToolNames);
        UF_TRY_VALUE(policyHash, parseHashColumn(columnText(sessionQuery.get(), 9)));
        UF_TRY_VALUE(
            snapshotBudget,
            readAgentBudget(m_impl->database.get(), lease.sessionId)
        );
        UF_TRY(requireWithinAgentDeadline(snapshotBudget));
        UF_TRY(chargeAgentBudget(
            m_impl->database.get(),
            lease.sessionId,
            "UPDATE agent_budgets SET remaining_observations = "
            "remaining_observations - ?2 WHERE session_id=?1",
            1U,
            "session observation budget is exhausted"
        ));

        UF_TRY_VALUE(
            priorQuery,
            prepare(
                m_impl->database.get(),
                "SELECT revision, canonical_observation, observation_hash, "
                "state_resolution_hash, project_registration_hash "
                "FROM project_observations "
                "WHERE plugin_id=?1 AND project_instance_key=?2 "
                "ORDER BY revision DESC LIMIT 1"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), priorQuery.get(), 1, pluginId));
        UF_TRY(bindText(m_impl->database.get(), priorQuery.get(), 2, projectInstanceKey));
        auto priorRevision      = uint64{};
        auto priorObservationId = std::string{};
        auto priorFingerprint   = std::string{};
        if (sqlite3_step(priorQuery.get()) == SQLITE_ROW)
        {
            priorRevision      = static_cast<uint64>(sqlite3_column_int64(priorQuery.get(), 0));
            priorObservationId = columnText(priorQuery.get(), 2);
            priorFingerprint   = columnText(priorQuery.get(), 3);
            priorFingerprint += '\0';
            priorFingerprint += columnText(priorQuery.get(), 4);
            priorFingerprint += '\0';
            priorFingerprint += priorObservationId;
        }

        // No project code runs here. A Project's reading of its own world is
        // a bound Project Tool the actors call, so a snapshot composes the
        // empty observation, and a Project that wants to state one publishes it
        // through publishProjectObservation from inside such a call. What the
        // row carries is every Operator-owned reading above -- the prior
        // observation's identity and the UI resolution -- so a snapshot remains
        // a complete decision basis rather than the project's own opinion of
        // one.
        auto const proposal = ProjectObservationProposal{};

        // The context the mint re-checks its authorities against. The lease was
        // validated at the top of this function and the session row read above,
        // so the facts come from this same transaction's reads.
        UF_TRY_VALUE(registrationHash, parseHashColumn(columnText(sessionQuery.get(), 6)));
        UF_TRY_VALUE(
            finalObservation,
            mintProjectObservation(
                ObservedInstanceContext{
                    .pluginId                 = pluginId,
                    .pluginModuleManifestHash = columnText(sessionQuery.get(), 5),
                    .projectRegistrationHash  = registrationHash,
                    .projectInstanceKey       = projectInstanceKey,
                },
                worldScope,
                identitySchemas,
                project,
                proposal
            )
        );

        // The fingerprint names the final closed observation the canonical
        // mint produced -- the bytes the row stores.
        auto const observationHex     = finalObservation.hash().hex();
        auto const stateResolutionHex = observation.stateResolutionHash().hex();
        auto currentFingerprint       = stateResolutionHex;
        currentFingerprint += '\0';
        currentFingerprint += project.hash().hex();
        currentFingerprint += '\0';
        currentFingerprint += observationHex;

        // "任一 UI/artifact/plugin 输入变化" made executable: an
        // identical reading of an identical world keeps its revision, so a
        // re-observation does not invent a new state kind revision and does not
        // move the snapshot identity.
        auto observationRevision = priorRevision;
        if (priorRevision == 0U || priorFingerprint != currentFingerprint)
        {
            UF_TRY_VALUE(
                nextRevision,
                checkedSqlIncrement(priorRevision, "ProjectObservation revision")
            );
            observationRevision = nextRevision;
            UF_TRY_VALUE(
                observationInsert,
                prepare(
                    m_impl->database.get(),
                    "INSERT INTO project_observations(plugin_id, project_instance_key, "
                    "revision, project_registration_hash, state_resolution_hash, "
                    "canonical_observation, observation_hash) "
                    "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)"
                )
            );
            UF_TRY(bindText(m_impl->database.get(), observationInsert.get(), 1, pluginId));
            UF_TRY(bindText(
                m_impl->database.get(),
                observationInsert.get(),
                2,
                projectInstanceKey
            ));
            UF_TRY(bindInteger(
                m_impl->database.get(),
                observationInsert.get(),
                3,
                observationRevision
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                observationInsert.get(),
                4,
                project.hash().hex()
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                observationInsert.get(),
                5,
                stateResolutionHex
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                observationInsert.get(),
                6,
                finalObservation.canonicalBytes()
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                observationInsert.get(),
                7,
                observationHex
            ));
            UF_TRY(expectDone(m_impl->database.get(), observationInsert.get()));
        }

        UF_TRY_VALUE(sessionManifestHash, parseHashColumn(sessionManifestHex));

        // StoredProjectObservation is minted here, in the member function body: its
        // single friend is OperatorCoordinator, which reaches its member
        // functions and neither Impl nor the file-local helpers above. Its
        // payload is the final closed envelope, the bytes the row stores.
        auto projectObservation = StoredProjectObservation{
            project.hash(),
            project.moduleIdentityHash(),
            projectInstanceKey,
            observation.stateResolutionHash(),
            observationRevision,
            std::move(finalObservation),
        };

        UF_TRY_VALUE(
            decisionBasisHash,
            deriveDecisionBasis(DecisionBasisParts{
                .projectObservationHash = projectObservation.hash(),
                .sessionManifestHash    = sessionManifestHash,
                .stateResolutionHash    = observation.stateResolutionHash(),
            })
        );

        UF_TRY_VALUE(
            availabilityQuery,
            prepare(
                m_impl->database.get(),
                "SELECT revision, policy_hash, available_tools "
                "FROM availability_heads WHERE controlled_target_id=?1"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            availabilityQuery.get(),
            1,
            controlledTargetId
        ));
        auto availabilityRevision = uint64{1};
        auto const availabilityStep = sqlite3_step(availabilityQuery.get());
        if (availabilityStep == SQLITE_ROW)
        {
            auto const priorAvailabilityRevision = static_cast<uint64>(
                sqlite3_column_int64(availabilityQuery.get(), 0)
            );
            availabilityRevision = priorAvailabilityRevision;
            if (
                columnText(availabilityQuery.get(), 1) != policyHash.hex()
                || columnText(availabilityQuery.get(), 2) != availableToolsJcs
            )
            {
                UF_TRY_VALUE(
                    nextRevision,
                    checkedSqlIncrement(
                        priorAvailabilityRevision,
                        "availability revision"
                    )
                );
                availabilityRevision = nextRevision;
                UF_TRY_VALUE(
                    availabilityUpdate,
                    prepare(
                        m_impl->database.get(),
                        "UPDATE availability_heads SET revision=?2, policy_hash=?3, "
                        "available_tools=?4 WHERE controlled_target_id=?1 AND revision=?5"
                    )
                );
                UF_TRY(bindText(
                    m_impl->database.get(),
                    availabilityUpdate.get(),
                    1,
                    controlledTargetId
                ));
                UF_TRY(bindInteger(
                    m_impl->database.get(),
                    availabilityUpdate.get(),
                    2,
                    availabilityRevision
                ));
                UF_TRY(bindText(
                    m_impl->database.get(),
                    availabilityUpdate.get(),
                    3,
                    policyHash.hex()
                ));
                UF_TRY(bindText(
                    m_impl->database.get(),
                    availabilityUpdate.get(),
                    4,
                    availableToolsJcs
                ));
                UF_TRY(bindInteger(
                    m_impl->database.get(),
                    availabilityUpdate.get(),
                    5,
                    priorAvailabilityRevision
                ));
                UF_TRY(expectDone(m_impl->database.get(), availabilityUpdate.get()));
                if (sqlite3_changes(m_impl->database.get()) != 1)
                {
                    return fail(
                        AutomationErrorKind::ActionRejected,
                        "Availability revision lost its compare-and-swap"
                    );
                }
            }
        }
        else if (availabilityStep == SQLITE_DONE)
        {
            UF_TRY_VALUE(
                availabilityInsert,
                prepare(
                    m_impl->database.get(),
                    "INSERT INTO availability_heads(controlled_target_id, revision, "
                    "policy_hash, available_tools) VALUES(?1, 1, ?2, ?3)"
                )
            );
            UF_TRY(bindText(
                m_impl->database.get(),
                availabilityInsert.get(),
                1,
                controlledTargetId
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                availabilityInsert.get(),
                2,
                policyHash.hex()
            ));
            UF_TRY(bindText(
                m_impl->database.get(),
                availabilityInsert.get(),
                3,
                availableToolsJcs
            ));
            UF_TRY(expectDone(m_impl->database.get(), availabilityInsert.get()));
        }
        else
        {
            return databaseFailure(
                m_impl->database.get(),
                "could not read the control availability revision"
            );
        }

        UF_TRY_VALUE(
            revisionQuery,
            prepare(
                m_impl->database.get(),
                "SELECT COALESCE(MAX(snapshot_revision), 0) FROM snapshots "
                "WHERE session_id=?1"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), revisionQuery.get(), 1, lease.sessionId));
        if (sqlite3_step(revisionQuery.get()) != SQLITE_ROW)
        {
            return databaseFailure(
                m_impl->database.get(),
                "could not read the session snapshot revision"
            );
        }
        UF_TRY_VALUE(
            snapshotRevision,
            checkedSqlIncrement(
                static_cast<uint64>(sqlite3_column_int64(revisionQuery.get(), 0)),
                "snapshot revision"
            )
        );

        auto const canonicalParts = snapshotPartsJcs(SnapshotPartsInputs{
            .decisionBasisHash          = decisionBasisHash,
            .projectObservationHash     = projectObservation.hash(),
            .policyHash                 = policyHash,
            .sessionManifestHash        = sessionManifestHash,
            .stateResolutionHash        = observation.stateResolutionHash(),
            .availableToolsJcs          = availableToolsJcs,
            .controlledTargetId         = controlledTargetId,
            .leaseId                    = lease.leaseId,
            .observationId              = observation.observationId(),
            .projectInstanceKey         = projectInstanceKey,
            .availabilityRevision       = availabilityRevision,
            .fencingToken               = lease.fencingToken,
            .projectObservationRevision = observationRevision,
            .sessionEpoch               = lease.sessionEpoch,
            .targetGeneration           = observation.targetGeneration().value(),
        });
        UF_TRY_VALUE(
            identityHash,
            sha256(std::as_bytes(std::span{canonicalParts}))
        );

        UF_TRY_VALUE(token, randomToken(m_impl->database.get(), k_opaqueTokenBytes));
        UF_TRY_VALUE(
            insert,
            prepare(
                m_impl->database.get(),
                "INSERT INTO snapshots(token, session_id, snapshot_revision, session_epoch, "
                "identity_hash, decision_basis_hash, canonical_parts, lease_revision, "
                "plugin_id, project_instance_key, observation_id, target_generation, "
                "state_resolution_hash, project_observation_revision, "
                "availability_revision) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15)"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 1, token));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 2, lease.sessionId));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 3, snapshotRevision));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 4, lease.sessionEpoch));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 5, identityHash.hex()));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 6, decisionBasisHash.hex()));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 7, canonicalParts));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 8, lease.revision));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 9, pluginId));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 10, projectInstanceKey));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            11,
            observation.observationId()
        ));
        UF_TRY(bindInteger(
            m_impl->database.get(),
            insert.get(),
            12,
            observation.targetGeneration().value()
        ));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 13, stateResolutionHex));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 14, observationRevision));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 15, availabilityRevision));
        UF_TRY(expectDone(m_impl->database.get(), insert.get()));

        UF_TRY(pruneSnapshotHistory(
            m_impl->database.get(),
            lease.sessionId,
            pluginId,
            projectInstanceKey
        ));

        // The join point, read inside the transaction that published this
        // record. Nothing can commit between the composition and this read, so
        // a controller subscribing from here is handed exactly what happened
        // after the world it is looking at.
        UF_TRY_VALUE(eventCursor, currentEventCursor(m_impl->database.get()));
        UF_TRY(transaction.commit());
        return SnapshotRecord{
            .token                = std::move(token),
            .sessionId            = lease.sessionId,
            .identityHash         = identityHash,
            .decisionBasisHash    = decisionBasisHash,
            .stateResolutionHash  = observation.stateResolutionHash(),
            .canonicalParts       = canonicalParts,
            .sessionEpoch         = lease.sessionEpoch,
            .leaseRevision        = lease.revision,
            .snapshotRevision     = snapshotRevision,
            .availabilityRevision = availabilityRevision,
            .policyHash           = policyHash,
            .availableTools       = std::move(availableTools),
            .observation          = std::move(projectObservation),
            .eventCursor          = SubscriptionCursor{eventCursor},
        };
    }

    auto OperatorCoordinator::mintProjectObservation(
        ObservedInstanceContext const& context,
        ObservedInstanceWorldScope const& worldScope,
        ObservedInstanceIdentitySchemas const& identitySchemas,
        ProjectIdentity const& project,
        ProjectObservationProposal const& proposal
    ) -> Result<ProjectObservation>
    {
        // Shape-level codes precede every semantic relationship code in the
        // interface-lock registry.
        UF_TRY(validateProjectObservationProposalShape(proposal));
        UF_TRY(validateProjectObservationProposalRelations(proposal));

        // Registration closure membership is a whole-proposal pass and must
        // outrank every basis violation, collision and scope refusal,
        // irrespective of proposal order.
        for (auto const& instance : proposal.observedInstanceProposals)
        {
            if (!identitySchemas.contains(instance.identitySchemaId))
            {
                return fail(
                    ProjectObservationErrorCode::ObservedInstanceIdentitySchemaNotRegistered,
                    "Observed instance identity_schema_id is outside the registration closure"
                );
            }
        }
        for (auto const& instance : proposal.observedInstanceProposals)
        {
            UF_TRY(identitySchemas.validate(
                instance.identitySchemaId,
                instance.semanticIdentityBasis
            ));
        }

        auto minted = std::vector<ObservedInstanceId>{};
        minted.reserve(proposal.observedInstanceProposals.size());
        auto localRefById = std::map<std::string, std::string>{};
        for (auto const& instance : proposal.observedInstanceProposals)
        {
            auto const authority = observedInstanceAuthorityBytes(
                context,
                worldScope,
                instance
            );
            UF_TRY_VALUE(
                observedInstanceId,
                mintObservedInstanceBinding(
                    m_impl->database.get(),
                    context,
                    worldScope,
                    authority,
                    instance.localRef
                )
            );
            auto const [found, inserted] = localRefById.try_emplace(
                observedInstanceId,
                instance.localRef
            );
            if (!inserted && found->second != instance.localRef)
            {
                return fail(
                    ProjectObservationErrorCode::ObservedInstanceCollision,
                    "Different observed instance local_ref values minted one ID"
                );
            }
            minted.emplace_back(ObservedInstanceId{std::move(observedInstanceId)});
        }

        if (
            project.pluginId() != context.pluginId
            || project.moduleIdentityHash().hex()
                != context.pluginModuleManifestHash
            || project.hash() != context.projectRegistrationHash
            || identitySchemas.projectRegistrationHash()
                != context.projectRegistrationHash
        )
        {
            return fail(
                ProjectObservationErrorCode::ObservedInstanceScopeMismatch,
                "Observed instance authorities do not match the active registration"
            );
        }

        auto indexes = std::map<std::string, std::size_t>{};
        for (
            auto index = std::size_t{};
            index < proposal.observedInstanceProposals.size();
            ++index
        )
        {
            indexes.emplace(
                proposal.observedInstanceProposals[index].localRef,
                index
            );
        }
        auto instances = std::vector<ObservedInstance>{};
        instances.reserve(proposal.observedInstanceProposals.size());
        for (
            auto index = std::size_t{};
            index < proposal.observedInstanceProposals.size();
            ++index
        )
        {
            auto parent = std::optional<ObservedInstanceId>{};
            auto const& proposed = proposal.observedInstanceProposals[index];
            if (proposed.parentLocalRef)
            {
                parent.emplace(minted[indexes.at(*proposed.parentLocalRef)]);
            }
            instances.emplace_back(ObservedInstance{
                .observedInstanceId       = minted[index],
                .parentObservedInstanceId = std::move(parent),
                .kind                     = proposed.kind,
                .opaqueProjectPayload     = proposed.opaqueProjectPayload,
            });
        }

        auto value = finalProjectObservationValue(
            proposal.canonicalOpaquePayload,
            proposal.projectToolPreconditions,
            instances
        );
        auto canonicalBytes = json::canonicalBytes(value);
        UF_TRY_VALUE(
            hash,
            sha256(std::as_bytes(std::span{canonicalBytes}))
        );
        return ProjectObservation{
            proposal.canonicalOpaquePayload,
            proposal.projectToolPreconditions,
            std::move(instances),
            std::move(canonicalBytes),
            hash,
        };
    }

    auto OperatorCoordinator::publishProjectObservation(
        ControlLease const& lease,
        ProjectIdentity const& project,
        ObservedInstanceWorldScope const& worldScope,
        ObservedInstanceIdentitySchemas const& identitySchemas,
        ProjectObservationProposal const& proposal
    ) -> Result<ProjectObservation>
    {
        UF_TRY_VALUE(
            derivedContext,
            readObservedInstanceContext(
                m_impl->database.get(),
                m_impl->sessionEpoch,
                lease
            )
        );
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY_VALUE(
            observation,
            mintProjectObservation(
                derivedContext,
                worldScope,
                identitySchemas,
                project,
                proposal
            )
        );
        UF_TRY(transaction.commit());
        return observation;
    }

    auto OperatorCoordinator::resolveObservedInstance(
        ControlLease const& lease,
        ObservedInstanceWorldScope const& worldScope,
        ProjectObservation const& freshObservation,
        std::string_view observedInstanceId
    ) -> Result<ObservedInstanceId>
    {
        UF_TRY_VALUE(
            context,
            readObservedInstanceContext(
                m_impl->database.get(),
                m_impl->sessionEpoch,
                lease
            )
        );
        UF_TRY_VALUE(
            query,
            prepare(
                m_impl->database.get(),
                "SELECT plugin_id, project_registration_hash, project_instance_key, "
                "world_scope_kind, world_scope_id, world_scope_generation "
                "FROM observed_instance_bindings WHERE observed_instance_id=?1"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), query.get(), 1, observedInstanceId));
        if (sqlite3_step(query.get()) != SQLITE_ROW)
        {
            return fail(
                ProjectObservationErrorCode::ObservedInstanceStale,
                "Observed instance ID is not a known persistent binding"
            );
        }
        auto const scopeMatches = columnText(query.get(), 0) == context.pluginId
            && columnText(query.get(), 1) == context.projectRegistrationHash.hex()
            && columnText(query.get(), 2) == context.projectInstanceKey
            && columnText(query.get(), 3)
                == observedInstanceWorldScopeKindWireName(worldScope.kind())
            && columnText(query.get(), 4) == worldScope.scopeId()
            && columnText(query.get(), 5) == std::to_string(worldScope.generation());
        if (!scopeMatches)
        {
            return fail(
                ProjectObservationErrorCode::ObservedInstanceScopeMismatch,
                "Observed instance ID belongs to another registration, project or scope"
            );
        }
        auto const fresh = std::ranges::any_of(
            freshObservation.observedInstances(),
            [observedInstanceId](ObservedInstance const& instance)
            {
                return instance.observedInstanceId.value() == observedInstanceId;
            }
        );
        if (!fresh)
        {
            return fail(
                ProjectObservationErrorCode::ObservedInstanceStale,
                "Observed instance ID is absent from the fresh observation"
            );
        }
        return ObservedInstanceId{std::string{observedInstanceId}};
    }

    auto OperatorCoordinator::persistToolRootRequest(
        ToolRootRequestIdentity const& root
    ) -> Result<StoredToolRootRequest>
    {
        auto* const database = m_impl->database.get();
        UF_TRY_VALUE(transaction, Transaction::begin(database));
        UF_TRY_VALUE(
            query,
            prepare(
                database,
                "SELECT root_identity, request_preimage, request_preimage_hash "
                "FROM tool_root_requests WHERE caller_namespace=?1 AND request_key=?2"
            )
        );
        UF_TRY(bindText(database, query.get(), 1, root.callerNamespace().value()));
        UF_TRY(bindText(database, query.get(), 2, root.requestKey().value()));
        auto const queryResult = sqlite3_step(query.get());
        if (queryResult == SQLITE_ROW)
        {
            auto const exactMatch = columnText(query.get(), 0) == root.identity().hex()
                && columnText(query.get(), 1) == root.requestPreimage().bytes()
                && columnText(query.get(), 2)
                    == root.requestPreimage().contentHash().hex();
            if (!exactMatch)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Tool root request key was replayed with different canonical material"
                );
            }
            UF_TRY(transaction.commit());
            return StoredToolRootRequest{
                .rootIdentity = root.identity(),
                .lookup       = ToolIdentityLookup::Existing,
            };
        }
        if (queryResult != SQLITE_DONE)
        {
            return databaseFailure(database, "could not read Tool root request");
        }

        UF_TRY_VALUE(
            insert,
            prepare(
                database,
                "INSERT INTO tool_root_requests(root_identity, caller_namespace, "
                "request_key, request_preimage, request_preimage_hash, state) "
                "VALUES(?1, ?2, ?3, ?4, ?5, 'running')"
            )
        );
        UF_TRY(bindText(database, insert.get(), 1, root.identity().hex()));
        UF_TRY(bindText(database, insert.get(), 2, root.callerNamespace().value()));
        UF_TRY(bindText(database, insert.get(), 3, root.requestKey().value()));
        UF_TRY(bindText(database, insert.get(), 4, root.requestPreimage().bytes()));
        UF_TRY(bindText(
            database,
            insert.get(),
            5,
            root.requestPreimage().contentHash().hex()
        ));
        UF_TRY(expectDone(database, insert.get()));
        UF_TRY(transaction.commit());
        return StoredToolRootRequest{
            .rootIdentity = root.identity(),
            .lookup       = ToolIdentityLookup::Created,
        };
    }

    auto OperatorCoordinator::persistToolCallPosition(
        ToolRootRequestIdentity const& root,
        ToolCallPositionIdentity const& call
    ) -> Result<StoredToolCallPosition>
    {
        if (call.rootIdentity() != root.identity())
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool call position belongs to a different root request"
            );
        }

        auto* const database = m_impl->database.get();
        UF_TRY_VALUE(transaction, Transaction::begin(database));
        UF_TRY_VALUE(
            rootQuery,
            prepare(
                database,
                "SELECT caller_namespace, request_key, request_preimage, "
                "request_preimage_hash FROM tool_root_requests WHERE root_identity=?1"
            )
        );
        UF_TRY(bindText(database, rootQuery.get(), 1, root.identity().hex()));
        auto const rootQueryResult = sqlite3_step(rootQuery.get());
        if (rootQueryResult != SQLITE_ROW)
        {
            if (rootQueryResult != SQLITE_DONE)
            {
                return databaseFailure(database, "could not read Tool call root request");
            }
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool call position requires its root request to be persisted first"
            );
        }
        auto const rootMatches =
            columnText(rootQuery.get(), 0) == root.callerNamespace().value()
            && columnText(rootQuery.get(), 1) == root.requestKey().value()
            && columnText(rootQuery.get(), 2) == root.requestPreimage().bytes()
            && columnText(rootQuery.get(), 3)
                == root.requestPreimage().contentHash().hex();
        if (!rootMatches)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Persisted Tool root request does not match its canonical material"
            );
        }

        auto const provider = persistedToolProvider(call.provider());
        auto const projectRegistrationHash = provider.projectRegistrationHash
            ? std::optional{provider.projectRegistrationHash->hex()}
            : std::nullopt;
        auto const parentIdentity       = call.parentIdentity().hex();
        auto const observationReference = call.observationReference()
            ? std::optional{call.observationReference()->hex()}
            : std::nullopt;

        UF_TRY_VALUE(
            positionQuery,
            prepareToolCallCoordinateQuery(database, call)
        );
        auto const positionQueryResult = sqlite3_step(positionQuery.get());
        if (positionQueryResult == SQLITE_ROW)
        {
            if (
                auto const diverged = divergedToolCallField(
                    positionQuery.get(),
                    call
                ))
            {
                // The run stops here, and the mark has to outlive the refusal.
                // This transaction is otherwise read-only, so its rollback
                // would discard the only record that the run was stopped --
                // hence the write and the commit before the failure rather
                // than after it.
                auto message = toolCallDivergenceMessage(call, *diverged);
                UF_TRY(terminateToolRun(
                    database,
                    root.identity().hex(),
                    message
                ));
                UF_TRY(transaction.commit());
                return fail(
                    AutomationErrorKind::ActionRejected,
                    std::move(message)
                );
            }
            UF_TRY(ensureToolCallHistory(database, call));
            UF_TRY(transaction.commit());
            return StoredToolCallPosition{
                .callIdentity = call.identity(),
                .lookup       = ToolIdentityLookup::Existing,
            };
        }
        if (positionQueryResult != SQLITE_DONE)
        {
            return databaseFailure(database, "could not read Tool call position");
        }

        // A NEW position is new work, and a terminated run takes none. The
        // matched-coordinate branch above deliberately returns before this, so
        // a call the run already recorded still rejoins and still replays.
        UF_TRY(requireLiveToolRun(database, root.identity().hex()));

        // The parent coordinate must already be durable. A call the run's own
        // context issued names the root request, whose row was read and
        // matched above; anything else names a parent position, which is
        // looked up here. There is no third case and no absent case.
        //
        // This is the alternation foreign key SQLite cannot state, and it is
        // deliberately NOT the changed-parent divergence replayToolCall reports
        // on the same condition. The two ask different questions of it: a
        // WRITER is hanging a new position off a coordinate that does not
        // exist, which is an ordering mistake inside a live run, while a READER
        // is asking the record for a call under a parent the record never had,
        // which is a call arriving under a different parent.
        UF_TRY_VALUE(
            parentRecorded,
            toolCallParentIsRecorded(database, root, call)
        );
        if (!parentRecorded)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool call position requires its parent call to be persisted first"
            );
        }

        UF_TRY_VALUE(
            insert,
            prepare(
                database,
                "INSERT INTO tool_call_positions(call_identity, root_identity, "
                "parent_call_identity, call_sequence, run_identity, "
                "framework_release_identity, tool_runtime_protocol_identity, "
                "environment_identity, provider_kind, project_registration_hash, "
                "tool_catalog_hash, tool_name, tool_version, canonical_args, "
                "canonical_args_hash, observation_reference_hash) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, "
                "?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16)"
            )
        );
        auto const& execution = call.executionIdentity();
        UF_TRY(bindText(database, insert.get(), 1, call.identity().hex()));
        UF_TRY(bindText(database, insert.get(), 2, root.identity().hex()));
        UF_TRY(bindText(database, insert.get(), 3, parentIdentity));
        UF_TRY(bindInteger(database, insert.get(), 4, call.sequence()));
        UF_TRY(bindText(database, insert.get(), 5, execution.runIdentity.hex()));
        UF_TRY(bindText(
            database,
            insert.get(),
            6,
            execution.frameworkReleaseIdentity.hex()
        ));
        UF_TRY(bindText(
            database,
            insert.get(),
            7,
            execution.toolRuntimeProtocolIdentity.hex()
        ));
        UF_TRY(bindText(database, insert.get(), 8, execution.environmentIdentity.hex()));
        UF_TRY(bindText(database, insert.get(), 9, provider.kind));
        UF_TRY(bindOptionalText(database, insert.get(), 10, projectRegistrationHash));
        UF_TRY(bindText(database, insert.get(), 11, provider.toolCatalogHash.hex()));
        UF_TRY(bindText(database, insert.get(), 12, call.toolName()));
        UF_TRY(bindText(database, insert.get(), 13, call.toolVersion()));
        UF_TRY(bindText(database, insert.get(), 14, call.canonicalArgs()));
        UF_TRY(bindText(database, insert.get(), 15, call.canonicalArgsHash().hex()));
        UF_TRY(bindOptionalText(database, insert.get(), 16, observationReference));
        UF_TRY(expectDone(database, insert.get()));
        UF_TRY(ensureToolCallHistory(database, call));
        UF_TRY(transaction.commit());
        return StoredToolCallPosition{
            .callIdentity = call.identity(),
            .lookup       = ToolIdentityLookup::Created,
        };
    }

    auto OperatorCoordinator::admitToolCall(
        ToolAdmissionRequest const& request
    ) -> Result<ToolCallAdmission>
    {
        // Call-scoped borrows into the request, which outlives this call. They
        // exist so the request stays the one thing a producer builds while the
        // body below keeps naming the members it reasons about.
        auto const& controller = request.controller;
        auto const& lease      = request.lease;
        auto const& root       = request.root;
        auto const& call       = request.call;
        auto const& mutation = request.mutation;

        auto const requiredMutability = request.requiredMutability();

        auto const effects = mutation.has_value()
            ? std::span<ProposedEffect const>{mutation->effects}
            : std::span<ProposedEffect const>{};
        auto const approvals = mutation.has_value()
            ? std::span<ToolApprovalGrant const>{mutation->approvals}
            : std::span<ToolApprovalGrant const>{};

        // The catalog decides whether a Tool mutates, so the request cannot
        // state a mutability of its own; what it can get wrong is bringing a
        // mutation proposal to a read-only Tool, or bringing none to a mutating
        // one, and admitting the second would evaluate no policy at all.
        if ((requiredMutability == ToolMutability::Mutating) != mutation.has_value())
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                requiredMutability == ToolMutability::Mutating
                    ? "Mutating Tool admission requires a mutation proposal"
                    : "Read-only Tool admission cannot carry a mutation proposal"
            );
        }
        if (
            controller.sessionId() != lease.sessionId
            || controller.controllerId() != lease.controllerId
            || controller.controlledTargetId() != lease.controlledTargetId
            || controller.sessionEpoch() != lease.sessionEpoch
            || controller.capabilityProfileHash() != lease.capabilityProfileHash
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool admission binding and lease name different authority"
            );
        }

        UF_TRY(persistToolRootRequest(root));
        UF_TRY(persistToolCallPosition(root, call));

        auto* const database = m_impl->database.get();
        UF_TRY_VALUE(transaction, Transaction::begin(database));

        // A terminated run admits nothing, and the position persisted above is
        // deliberately not enough on its own: section 5.3's `proposed` recovery
        // says an unadmitted call repeats its admission, so a run that stopped
        // between the two has to be refused here rather than at the coordinate.
        UF_TRY(requireLiveToolRun(database, root.identity().hex()));
        UF_TRY(requireLiveBinding(database, controller));
        UF_TRY(requireLiveLease(
            database,
            lease,
            "Tool admission control lease was superseded"
        ));

        UF_TRY_VALUE(
            sessionQuery,
            prepare(
                database,
                "SELECT session.authenticated_controller_id, "
                "session.idempotency_namespace, session.project_registration_hash, "
                "session.controller_capabilities, session.controller_kind, "
                "session.controlled_target_id, policy.policy_hash, "
                "registration.registration_format, registration.plugin_identity_kind, "
                "session.capability_profile_hash, session.session_epoch "
                "FROM sessions session JOIN session_policies policy "
                "ON policy.session_id=session.session_id "
                "JOIN project_registrations registration ON "
                "registration.registration_hash=session.project_registration_hash "
                "WHERE session.session_id=?1 AND session.active=1"
            )
        );
        UF_TRY(bindText(database, sessionQuery.get(), 1, controller.sessionId()));
        if (sqlite3_step(sessionQuery.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool admission requires an active authenticated session"
            );
        }
        UF_TRY(requireExecutableRegistrationFormat(sessionQuery.get(), 7));
        UF_TRY_VALUE(
            storedKind,
            parseControllerKind(columnText(sessionQuery.get(), 4))
        );
        auto const sessionMatches =
            columnText(sessionQuery.get(), 0) == controller.controllerId()
            && columnText(sessionQuery.get(), 5)
                == controller.controlledTargetId()
            && columnText(sessionQuery.get(), 9)
                == controller.capabilityProfileHash().hex()
            && static_cast<uint64>(sqlite3_column_int64(sessionQuery.get(), 10))
                == controller.sessionEpoch()
            && storedKind == controller.kind();
        if (!sessionMatches)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool admission session no longer matches its controller binding"
            );
        }
        if (
            root.callerNamespace().value()
            != columnText(sessionQuery.get(), 1)
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool root namespace does not match the authenticated session"
            );
        }
        auto const projectRegistrationHash = columnText(sessionQuery.get(), 2);

        UF_TRY_VALUE(liveMutationChain, admittedToolAncestors(database, request));
        auto executionPrincipalId = controller.controllerId();
        auto executionPrincipalKind =
            std::string{controllerKindWireName(controller.kind())};

        if (requiredMutability == ToolMutability::Mutating)
        {
            UF_TRY(requireNoActiveToolMutation(
                database,
                controller.controlledTargetId(),
                liveMutationChain
            ));
        }
        if (auto const* projectProvider = std::get_if<ProjectToolProvider>(
                &call.provider()
            );
            projectProvider != nullptr
            && projectProvider->projectRegistrationHash.hex()
                != projectRegistrationHash)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool call was minted for a different ProjectRegistration"
            );
        }
        auto const policyHash = columnText(sessionQuery.get(), 6);
        if (
            request.policyAuthority.projectRegistrationHash().hex()
                != projectRegistrationHash
            || request.policyAuthority.policyHash().hex() != policyHash
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool admission authority differs from the active session"
            );
        }
        // The controller profile bounds its direct vocabulary. A registered
        // handler may use the machine vocabulary internally, but every such
        // child still needs the Operator's explicit grant and ancestor bounds.
        // A Project's surface label is a declaration, never a permission.
        if (
            request.isRootPositioned()
            && !toolSurfaceAllowed(controller.profile(), call.descriptor().surface)
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Controller profile does not admit this Tool surface"
            );
        }
        if (
            call.descriptor().surface == ToolSurface::Privileged
            && !request.policyAuthority.m_policy.grantsPrivilegedSurface(
                call.toolName()
            )
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Operator policy grants no Privileged surface to tool "
                    + call.toolName()
            );
        }
        UF_TRY_VALUE(
            heldCapabilities,
            readNameArray(columnText(sessionQuery.get(), 3))
        );
        if (auto const missing = missingRequiredToolCapability(
                heldCapabilities,
                call.descriptor().requiredCapabilities
            ))
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Controller does not hold required capability '" + *missing
                    + "' for tool " + call.toolName()
            );
        }
        auto effectEnvelope    = std::optional<EffectiveEffectEnvelope>{};
        auto requiredApprovals = std::vector<std::string>{};
        auto approvalTokens    = std::vector<std::string>{};
        auto approvalExpiry    = std::optional<uint64>{};
        if (requiredMutability == ToolMutability::Mutating)
        {
            UF_TRY_VALUE(
                evaluated,
                evaluateToolMutation(
                    request.policyAuthority.m_policy,
                    call.descriptor(),
                    call.toolName(),
                    effects,
                    heldCapabilities
                )
            );
            effectEnvelope    = std::move(evaluated.envelope);
            requiredApprovals = std::move(evaluated.requiredApprovals);
            for (auto const& approval : approvals)
            {
                UF_TRY(requireName(approval.token, "Tool approval token"));
                UF_TRY(requireName(
                    approval.authorityDecisionId.value(),
                    "Tool approval authority_decision_id"
                ));
                approvalTokens.emplace_back(approval.token);
            }
            std::ranges::sort(approvalTokens);
            if (
                std::ranges::adjacent_find(approvalTokens)
                != approvalTokens.end()
            )
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Mutating Tool admission repeats one approval token"
                );
            }
            if (requiredApprovals.empty() != approvals.empty())
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    requiredApprovals.empty()
                        ? "Mutating Tool policy allows no approval tokens"
                        : "Mutating Tool policy requires approval before admission"
                );
            }
            if (approvals.size() != requiredApprovals.size())
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Mutating Tool admission requires one token for every "
                    "policy approval capability"
                );
            }
        }

        if (call.sequence() > 1U)
        {
            UF_TRY_VALUE(
                predecessorQuery,
                prepare(
                    database,
                    "SELECT history.state FROM tool_call_positions position "
                    "JOIN tool_call_history history "
                    "ON history.call_identity=position.call_identity "
                    "WHERE position.root_identity=?1 AND "
                    "position.parent_call_identity=?3 AND position.call_sequence=?2"
                )
            );
            UF_TRY(bindText(
                database,
                predecessorQuery.get(),
                1,
                root.identity().hex()
            ));
            UF_TRY(bindInteger(
                database,
                predecessorQuery.get(),
                2,
                call.sequence() - 1U
            ));
            UF_TRY(bindText(
                database,
                predecessorQuery.get(),
                3,
                call.parentIdentity().hex()
            ));
            if (sqlite3_step(predecessorQuery.get()) != SQLITE_ROW)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Tool call sequence has no durable predecessor"
                );
            }
            UF_TRY_VALUE(
                predecessorState,
                parseToolCallState(columnText(predecessorQuery.get(), 0))
            );
            auto const predecessorFinished =
                predecessorState == ToolCallState::Confirmed
                || predecessorState == ToolCallState::ProvenAbsent
                || predecessorState == ToolCallState::TerminalFailure;
            if (!predecessorFinished)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Tool call sequence predecessor has no deterministic terminal outcome"
                );
            }
        }

        UF_TRY_VALUE(budget, readAgentBudget(database, controller.sessionId()));
        UF_TRY(requireWithinAgentDeadline(budget));
        UF_TRY_VALUE(budgetSnapshot, toolBudgetSnapshot(budget));

        auto const& execution = call.executionIdentity();
        auto const kindName   = controllerKindWireName(controller.kind());
        UF_TRY_VALUE(
            runQuery,
            prepare(
                database,
                "SELECT origin_principal_id, origin_principal_kind, "
                "controlled_target_id, project_registration_hash, run_identity, "
                "framework_release_identity, tool_runtime_protocol_identity, "
                "environment_identity FROM tool_runs WHERE root_identity=?1"
            )
        );
        UF_TRY(bindText(database, runQuery.get(), 1, root.identity().hex()));
        auto const runResult = sqlite3_step(runQuery.get());
        if (runResult == SQLITE_ROW)
        {
            auto const exactRun =
                columnText(runQuery.get(), 0) == controller.controllerId()
                && columnText(runQuery.get(), 1) == kindName
                && columnText(runQuery.get(), 2)
                    == controller.controlledTargetId()
                && columnText(runQuery.get(), 3) == projectRegistrationHash
                && columnText(runQuery.get(), 4) == execution.runIdentity.hex()
                && columnText(runQuery.get(), 5)
                    == execution.frameworkReleaseIdentity.hex()
                && columnText(runQuery.get(), 6)
                    == execution.toolRuntimeProtocolIdentity.hex()
                && columnText(runQuery.get(), 7)
                    == execution.environmentIdentity.hex();
            if (!exactRun)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Tool continuation would change its durable run authority"
                );
            }
        }
        else if (runResult == SQLITE_DONE)
        {
            UF_TRY_VALUE(
                runInsert,
                prepare(
                    database,
                    "INSERT INTO tool_runs(root_identity, origin_principal_id, "
                    "origin_principal_kind, controlled_target_id, "
                    "project_registration_hash, run_identity, "
                    "framework_release_identity, tool_runtime_protocol_identity, "
                    "environment_identity) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)"
                )
            );
            UF_TRY(bindText(database, runInsert.get(), 1, root.identity().hex()));
            UF_TRY(bindText(database, runInsert.get(), 2, controller.controllerId()));
            UF_TRY(bindText(database, runInsert.get(), 3, kindName));
            UF_TRY(bindText(
                database,
                runInsert.get(),
                4,
                controller.controlledTargetId()
            ));
            UF_TRY(bindText(database, runInsert.get(), 5, projectRegistrationHash));
            UF_TRY(bindText(database, runInsert.get(), 6, execution.runIdentity.hex()));
            UF_TRY(bindText(
                database,
                runInsert.get(),
                7,
                execution.frameworkReleaseIdentity.hex()
            ));
            UF_TRY(bindText(
                database,
                runInsert.get(),
                8,
                execution.toolRuntimeProtocolIdentity.hex()
            ));
            UF_TRY(bindText(
                database,
                runInsert.get(),
                9,
                execution.environmentIdentity.hex()
            ));
            UF_TRY(expectDone(database, runInsert.get()));
        }
        else
        {
            return databaseFailure(database, "could not read durable Tool run");
        }

        UF_TRY_VALUE(
            historyQuery,
            prepare(
                database,
                "SELECT state, revision, active_admission_attempt, mutating "
                "FROM tool_call_history WHERE call_identity=?1"
            )
        );
        UF_TRY(bindText(database, historyQuery.get(), 1, call.identity().hex()));
        if (sqlite3_step(historyQuery.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "Persisted Tool call has no history row"
            );
        }
        UF_TRY_VALUE(
            state,
            parseToolCallState(columnText(historyQuery.get(), 0))
        );
        auto const revision = static_cast<uint64>(
            sqlite3_column_int64(historyQuery.get(), 1)
        );
        auto const activeAttempt = static_cast<uint64>(
            sqlite3_column_int64(historyQuery.get(), 2)
        );
        auto const storedMutating =
            sqlite3_column_int(historyQuery.get(), 3) != 0;
        if (
            storedMutating
            != (requiredMutability == ToolMutability::Mutating)
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool admission mutability disagrees with persisted history"
            );
        }
        if (state != ToolCallState::Proposed && state != ToolCallState::Admitted)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool call state cannot enter admission"
            );
        }

        if (state == ToolCallState::Admitted)
        {
            UF_TRY_VALUE(
                latestQuery,
                prepare(
                    database,
                    "SELECT execution_principal_id, execution_principal_kind, "
                    "session_id, session_epoch, controlled_target_id, "
                    "project_registration_hash, policy_hash, "
                    "capability_profile_hash, lease_id, lease_revision, "
                    "fencing_token, effect_envelope, effect_envelope_hash, "
                    "required_approvals, approval_tokens "
                    "FROM tool_admission_attempts "
                    "WHERE call_identity=?1 AND attempt_number=?2"
                )
            );
            UF_TRY(bindText(database, latestQuery.get(), 1, call.identity().hex()));
            UF_TRY(bindInteger(database, latestQuery.get(), 2, activeAttempt));
            if (sqlite3_step(latestQuery.get()) != SQLITE_ROW)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "Admitted Tool call has no active admission row"
                );
            }
            auto const exactEffectAuthority =
                optionalColumnText(latestQuery.get(), 11)
                    == (effectEnvelope
                            ? std::optional<std::string>{
                                  effectEnvelope->canonicalJcs,
                              }
                            : std::nullopt)
                && optionalColumnText(latestQuery.get(), 12)
                    == (effectEnvelope
                            ? std::optional<std::string>{
                                  effectEnvelope->hash.hex(),
                              }
                            : std::nullopt)
                && optionalColumnText(latestQuery.get(), 13)
                    == (effectEnvelope
                            ? std::optional<std::string>{
                                  canonicalNameArray(requiredApprovals),
                              }
                            : std::nullopt);
            if (!exactEffectAuthority)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Tool continuation would change its durable effect authority"
                );
            }
            auto const exactAttempt =
                columnText(latestQuery.get(), 0) == executionPrincipalId
                && columnText(latestQuery.get(), 1) == executionPrincipalKind
                && columnText(latestQuery.get(), 2) == controller.sessionId()
                && static_cast<uint64>(sqlite3_column_int64(latestQuery.get(), 3))
                    == controller.sessionEpoch()
                && columnText(latestQuery.get(), 4)
                    == controller.controlledTargetId()
                && columnText(latestQuery.get(), 5) == projectRegistrationHash
                && columnText(latestQuery.get(), 6) == policyHash
                && columnText(latestQuery.get(), 7)
                    == controller.capabilityProfileHash().hex()
                && columnText(latestQuery.get(), 8) == lease.leaseId
                && static_cast<uint64>(sqlite3_column_int64(latestQuery.get(), 9))
                    == lease.revision
                && static_cast<uint64>(sqlite3_column_int64(latestQuery.get(), 10))
                    == lease.fencingToken
                && optionalColumnText(latestQuery.get(), 14)
                    == (effectEnvelope
                            ? std::optional<std::string>{
                                  canonicalNameArray(approvalTokens),
                              }
                            : std::nullopt);
            if (exactAttempt)
            {
                UF_TRY(transaction.commit());
                return ToolCallAdmission{
                    call.identity(),
                    activeAttempt,
                    revision,
                };
            }
        }

        UF_TRY_VALUE(
            nextRevision,
            checkedSqlIncrement(revision, "Tool call history revision")
        );

        if (
            activeAttempt
            == static_cast<uint64>(std::numeric_limits<sqlite3_int64>::max())
        )
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "Tool admission attempt sequence exhausted"
            );
        }
        auto const nextAttempt = activeAttempt + 1U;
        if (!requiredApprovals.empty())
        {
            UF_TRY_VALUE(currentUnixMillis, unixTimeMilliseconds());
            auto approvedCapabilities = std::vector<std::string>{};
            for (auto const& approval : approvals)
            {
                UF_TRY_VALUE(
                    approvalQuery,
                    prepare(
                        database,
                        "SELECT approver_capability, expires_at_unix_millis "
                        "FROM tool_approvals "
                        "WHERE token=?1 AND call_identity=?2 AND root_identity=?3 "
                        "AND session_id=?4 AND controller_id=?5 "
                        "AND controlled_target_id=?6 AND lease_id=?7 "
                        "AND lease_revision=?8 AND session_epoch=?9 "
                        "AND fencing_token=?10 AND project_registration_hash=?11 "
                        "AND policy_hash=?12 AND effect_envelope_hash=?13 "
                        "AND authority_decision_id=?14 AND consumed=0 "
                        "AND expires_at_unix_millis>?15"
                    )
                );
                UF_TRY(bindText(database, approvalQuery.get(), 1, approval.token));
                UF_TRY(bindText(
                    database,
                    approvalQuery.get(),
                    2,
                    call.identity().hex()
                ));
                UF_TRY(bindText(
                    database,
                    approvalQuery.get(),
                    3,
                    root.identity().hex()
                ));
                UF_TRY(bindText(
                    database,
                    approvalQuery.get(),
                    4,
                    controller.sessionId()
                ));
                UF_TRY(bindText(
                    database,
                    approvalQuery.get(),
                    5,
                    controller.controllerId()
                ));
                UF_TRY(bindText(
                    database,
                    approvalQuery.get(),
                    6,
                    controller.controlledTargetId()
                ));
                UF_TRY(bindText(database, approvalQuery.get(), 7, lease.leaseId));
                UF_TRY(bindInteger(
                    database,
                    approvalQuery.get(),
                    8,
                    lease.revision
                ));
                UF_TRY(bindInteger(
                    database,
                    approvalQuery.get(),
                    9,
                    lease.sessionEpoch
                ));
                UF_TRY(bindInteger(
                    database,
                    approvalQuery.get(),
                    10,
                    lease.fencingToken
                ));
                UF_TRY(bindText(
                    database,
                    approvalQuery.get(),
                    11,
                    projectRegistrationHash
                ));
                UF_TRY(bindText(database, approvalQuery.get(), 12, policyHash));
                UF_TRY(bindText(
                    database,
                    approvalQuery.get(),
                    13,
                    effectEnvelope->hash.hex()
                ));
                UF_TRY(bindText(
                    database,
                    approvalQuery.get(),
                    14,
                    approval.authorityDecisionId.value()
                ));
                UF_TRY(bindInteger(
                    database,
                    approvalQuery.get(),
                    15,
                    currentUnixMillis
                ));
                if (sqlite3_step(approvalQuery.get()) != SQLITE_ROW)
                {
                    return fail(
                        AutomationErrorKind::ActionRejected,
                        "Tool approval is stale, expired, mismatched, or already consumed"
                    );
                }
                approvedCapabilities.emplace_back(
                    columnText(approvalQuery.get(), 0)
                );
                auto const expiresAt = static_cast<uint64>(
                    sqlite3_column_int64(approvalQuery.get(), 1)
                );
                approvalExpiry = approvalExpiry
                    ? std::min(*approvalExpiry, expiresAt)
                    : expiresAt;
            }
            std::ranges::sort(approvedCapabilities);
            if (approvedCapabilities != requiredApprovals)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Tool approvals do not satisfy the policy capability set"
                );
            }
        }
        UF_TRY_VALUE(
            admissionInsert,
            prepare(
                database,
                "INSERT INTO tool_admission_attempts(call_identity, attempt_number, "
                "root_identity, origin_principal_id, origin_principal_kind, "
                "execution_principal_id, execution_principal_kind, session_id, "
                "session_epoch, controlled_target_id, project_registration_hash, "
                "policy_hash, capability_profile_hash, lease_id, lease_revision, "
                "fencing_token, budget_snapshot, budget_snapshot_hash, "
                "effect_envelope, effect_envelope_hash, required_approvals, "
                "approval_tokens, approval_expires_at_unix_millis) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, "
                "?13, ?14, ?15, ?16, ?17, ?18, ?19, ?20, ?21, ?22, ?23)"
            )
        );
        UF_TRY(bindText(database, admissionInsert.get(), 1, call.identity().hex()));
        UF_TRY(bindInteger(database, admissionInsert.get(), 2, nextAttempt));
        UF_TRY(bindText(database, admissionInsert.get(), 3, root.identity().hex()));
        UF_TRY(bindText(
            database,
            admissionInsert.get(),
            4,
            controller.controllerId()
        ));
        UF_TRY(bindText(database, admissionInsert.get(), 5, kindName));

        // Every call executes for the principal that originated the root.
        UF_TRY(bindText(
            database,
            admissionInsert.get(),
            6,
            executionPrincipalId
        ));
        UF_TRY(bindText(database, admissionInsert.get(), 7, executionPrincipalKind));
        UF_TRY(bindText(database, admissionInsert.get(), 8, controller.sessionId()));
        UF_TRY(bindInteger(
            database,
            admissionInsert.get(),
            9,
            controller.sessionEpoch()
        ));
        UF_TRY(bindText(
            database,
            admissionInsert.get(),
            10,
            controller.controlledTargetId()
        ));
        UF_TRY(bindText(
            database,
            admissionInsert.get(),
            11,
            projectRegistrationHash
        ));
        UF_TRY(bindText(database, admissionInsert.get(), 12, policyHash));
        UF_TRY(bindText(
            database,
            admissionInsert.get(),
            13,
            controller.capabilityProfileHash().hex()
        ));
        UF_TRY(bindText(database, admissionInsert.get(), 14, lease.leaseId));
        UF_TRY(bindInteger(database, admissionInsert.get(), 15, lease.revision));
        UF_TRY(bindInteger(
            database,
            admissionInsert.get(),
            16,
            lease.fencingToken
        ));
        UF_TRY(bindText(
            database,
            admissionInsert.get(),
            17,
            budgetSnapshot.bytes
        ));
        UF_TRY(bindText(
            database,
            admissionInsert.get(),
            18,
            budgetSnapshot.hash.hex()
        ));
        UF_TRY(bindOptionalText(
            database,
            admissionInsert.get(),
            19,
            effectEnvelope
                ? std::optional<std::string_view>{effectEnvelope->canonicalJcs}
                : std::nullopt
        ));
        auto const effectEnvelopeHash = effectEnvelope
            ? std::optional<std::string>{effectEnvelope->hash.hex()}
            : std::nullopt;
        UF_TRY(bindOptionalText(
            database,
            admissionInsert.get(),
            20,
            effectEnvelopeHash
        ));
        auto const requiredApprovalList = effectEnvelope
            ? std::optional<std::string>{canonicalNameArray(requiredApprovals)}
            : std::nullopt;
        UF_TRY(bindOptionalText(
            database,
            admissionInsert.get(),
            21,
            requiredApprovalList
        ));
        auto const approvalTokenList = effectEnvelope
            ? std::optional<std::string>{canonicalNameArray(approvalTokens)}
            : std::nullopt;
        UF_TRY(bindOptionalText(
            database,
            admissionInsert.get(),
            22,
            approvalTokenList
        ));
        UF_TRY(bindOptionalInteger(
            database,
            admissionInsert.get(),
            23,
            approvalExpiry
        ));
        UF_TRY(expectDone(database, admissionInsert.get()));

        for (auto const& approval : approvals)
        {
            UF_TRY_VALUE(
                consumeApproval,
                prepare(
                    database,
                    "UPDATE tool_approvals SET consumed=1, "
                    "consumed_by_attempt=?2 WHERE token=?1 AND consumed=0"
                )
            );
            UF_TRY(bindText(
                database,
                consumeApproval.get(),
                1,
                approval.token
            ));
            UF_TRY(bindInteger(
                database,
                consumeApproval.get(),
                2,
                nextAttempt
            ));
            UF_TRY(expectDone(database, consumeApproval.get()));
            if (sqlite3_changes(database) != 1)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Tool approval consumption lost its single-use CAS"
                );
            }
        }

        UF_TRY(chargeAgentBudget(
            database,
            controller.sessionId(),
            "UPDATE agent_budgets SET remaining_tool_calls = "
            "remaining_tool_calls - ?2 WHERE session_id=?1",
            1U,
            "session tool-call budget is exhausted"
        ));
        if (requiredMutability == ToolMutability::Mutating)
        {
            UF_TRY(chargeAgentBudget(
                database,
                controller.sessionId(),
                "UPDATE agent_budgets SET remaining_mutations = "
                "remaining_mutations - ?2 WHERE session_id=?1",
                1U,
                "session mutation budget is exhausted"
            ));
        }
        if (
            std::holds_alternative<FrameworkToolProvider>(call.provider())
            && call.toolName() == "framework.screen.capture"
        )
        {
            UF_TRY(chargeAgentBudget(
                database,
                controller.sessionId(),
                "UPDATE agent_budgets SET remaining_observations = "
                "remaining_observations - ?2 WHERE session_id=?1",
                1U,
                "session observation budget is exhausted"
            ));
        }

        UF_TRY_VALUE(
            historyUpdate,
            prepare(
                database,
                "UPDATE tool_call_history SET state='admitted', revision=?2, "
                "active_admission_attempt=?3 WHERE call_identity=?1 "
                "AND state=?4 AND revision=?5 AND active_admission_attempt=?6"
            )
        );
        UF_TRY(bindText(database, historyUpdate.get(), 1, call.identity().hex()));
        UF_TRY(bindInteger(database, historyUpdate.get(), 2, nextRevision));
        UF_TRY(bindInteger(database, historyUpdate.get(), 3, nextAttempt));
        UF_TRY(bindText(
            database,
            historyUpdate.get(),
            4,
            toolCallStateWireName(state)
        ));
        UF_TRY(bindInteger(database, historyUpdate.get(), 5, revision));
        UF_TRY(bindInteger(database, historyUpdate.get(), 6, activeAttempt));
        UF_TRY(expectDone(database, historyUpdate.get()));
        if (sqlite3_changes(database) != 1)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool admission lost its history CAS"
            );
        }
        UF_TRY(transaction.commit());
        return ToolCallAdmission{
            call.identity(),
            nextAttempt,
            nextRevision,
        };
    }

    auto OperatorCoordinator::issueToolApproval(
        ControllerBinding const& controller,
        ControlLease const& lease,
        ControllerBinding const& approver,
        ToolRootRequestIdentity const& root,
        ToolCallPositionIdentity const& call,
        OperatorPolicyAuthority const& policyAuthority,
        std::span<ProposedEffect const> effects,
        ToolApprovalRequest const& request,
        AuthorityDecisionId const& authorityDecisionId
    ) -> Result<ToolApprovalGrant>
    {
        if (call.parentIdentity() != root.identity())
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool approval requires a root-positioned mutation"
            );
        }
        if (call.descriptor().mutability != ToolMutability::Mutating)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool approval requires a mutating descriptor"
            );
        }
        if (
            controller.sessionId() != lease.sessionId
            || controller.controllerId() != lease.controllerId
            || controller.controlledTargetId() != lease.controlledTargetId
            || controller.sessionEpoch() != lease.sessionEpoch
            || controller.capabilityProfileHash() != lease.capabilityProfileHash
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool approval binding and lease name different authority"
            );
        }
        if (
            approver.kind() != ControllerKind::Human
            || approver.controlledTargetId() != controller.controlledTargetId()
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool approval requires a human binding on the same target"
            );
        }
        UF_TRY(requireName(authorityDecisionId.value(), "authority_decision_id"));
        UF_TRY(requireName(request.approverCapability, "approver_capability"));
        UF_TRY_VALUE(currentUnixMillis, unixTimeMilliseconds());
        if (request.expiresAtUnixMillis <= currentUnixMillis)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Tool approval expiry must be in the future"
            );
        }

        UF_TRY(persistToolRootRequest(root));
        UF_TRY(persistToolCallPosition(root, call));
        auto* const database = m_impl->database.get();
        UF_TRY_VALUE(transaction, Transaction::begin(database));
        UF_TRY(requireLiveBinding(database, controller));
        UF_TRY(requireLiveBinding(database, approver));
        UF_TRY(requireLiveLease(
            database,
            lease,
            "Tool approval control lease was superseded"
        ));
        UF_TRY_VALUE(
            sessionQuery,
            prepare(
                database,
                "SELECT session.idempotency_namespace, "
                "session.project_registration_hash, "
                "session.controller_capabilities, policy.policy_hash, "
                "session.authenticated_controller_id, session.controller_kind, "
                "session.controlled_target_id, session.capability_profile_hash, "
                "session.session_epoch, registration.registration_format, "
                "registration.plugin_identity_kind "
                "FROM sessions session JOIN session_policies policy "
                "ON policy.session_id=session.session_id "
                "JOIN project_registrations registration ON "
                "registration.registration_hash=session.project_registration_hash "
                "WHERE session.session_id=?1 AND session.active=1"
            )
        );
        UF_TRY(bindText(database, sessionQuery.get(), 1, controller.sessionId()));
        if (sqlite3_step(sessionQuery.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool approval requires an active authenticated session"
            );
        }
        UF_TRY(requireExecutableRegistrationFormat(sessionQuery.get(), 9));
        UF_TRY_VALUE(
            storedKind,
            parseControllerKind(columnText(sessionQuery.get(), 5))
        );
        if (
            columnText(sessionQuery.get(), 4) != controller.controllerId()
            || columnText(sessionQuery.get(), 6)
                != controller.controlledTargetId()
            || columnText(sessionQuery.get(), 7)
                != controller.capabilityProfileHash().hex()
            || static_cast<uint64>(sqlite3_column_int64(sessionQuery.get(), 8))
                != controller.sessionEpoch()
            || storedKind != controller.kind()
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool approval session no longer matches its controller binding"
            );
        }
        if (root.callerNamespace().value() != columnText(sessionQuery.get(), 0))
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool approval root namespace differs from the active session"
            );
        }
        auto const projectRegistrationHash = columnText(sessionQuery.get(), 1);
        auto const policyHash               = columnText(sessionQuery.get(), 3);
        if (
            policyAuthority.projectRegistrationHash().hex()
                    != projectRegistrationHash
            || policyAuthority.policyHash().hex() != policyHash
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool approval authority differs from the active session"
            );
        }
        if (auto const* projectProvider = std::get_if<ProjectToolProvider>(
                &call.provider()
            );
            projectProvider != nullptr
            && projectProvider->projectRegistrationHash.hex()
                != projectRegistrationHash)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool approval call belongs to another ProjectRegistration"
            );
        }
        if (!toolSurfaceAllowed(controller.profile(), call.descriptor().surface))
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Controller profile does not admit this Tool approval surface"
            );
        }
        UF_TRY_VALUE(
            heldCapabilities,
            readNameArray(columnText(sessionQuery.get(), 2))
        );
        if (auto const missing = missingRequiredToolCapability(
                heldCapabilities,
                call.descriptor().requiredCapabilities
            ))
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Controller does not hold required capability '" + *missing
                    + "' for tool " + call.toolName()
            );
        }
        UF_TRY_VALUE(
            evaluated,
            evaluateToolMutation(
                policyAuthority.m_policy,
                call.descriptor(),
                call.toolName(),
                effects,
                heldCapabilities
            )
        );
        if (
            !std::ranges::contains(
                evaluated.requiredApprovals,
                request.approverCapability
            )
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool policy does not require approver capability "
                    + request.approverCapability
            );
        }
        UF_TRY_VALUE(
            approverQuery,
            prepare(
                database,
                "SELECT session.controller_capabilities, "
                "session.project_registration_hash, session.controlled_target_id, "
                "policy.policy_hash FROM sessions session "
                "JOIN session_policies policy ON policy.session_id=session.session_id "
                "WHERE session.session_id=?1 AND session.active=1"
            )
        );
        UF_TRY(bindText(database, approverQuery.get(), 1, approver.sessionId()));
        if (sqlite3_step(approverQuery.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool approver session is no longer active"
            );
        }
        if (
            columnText(approverQuery.get(), 1) != projectRegistrationHash
            || columnText(approverQuery.get(), 2)
                != controller.controlledTargetId()
            || columnText(approverQuery.get(), 3) != policyHash
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool approver is bound to another target, Project, or policy"
            );
        }
        UF_TRY_VALUE(
            approverCapabilities,
            readNameArray(columnText(approverQuery.get(), 0))
        );
        if (!std::ranges::contains(
                approverCapabilities,
                request.approverCapability
            ))
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Authenticated Tool approver does not hold capability "
                    + request.approverCapability
            );
        }

        UF_TRY_VALUE(
            historyQuery,
            prepare(
                database,
                "SELECT state, mutating FROM tool_call_history "
                "WHERE call_identity=?1"
            )
        );
        UF_TRY(bindText(database, historyQuery.get(), 1, call.identity().hex()));
        if (
            sqlite3_step(historyQuery.get()) != SQLITE_ROW
            || sqlite3_column_int(historyQuery.get(), 1) == 0
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool approval found no durable mutating call"
            );
        }
        UF_TRY_VALUE(
            state,
            parseToolCallState(columnText(historyQuery.get(), 0))
        );
        if (state != ToolCallState::Proposed && state != ToolCallState::Admitted)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool approval call has already crossed dispatch"
            );
        }

        UF_TRY_VALUE(token, randomToken(database, k_opaqueTokenBytes));
        UF_TRY_VALUE(
            insert,
            prepare(
                database,
                "INSERT INTO tool_approvals(token, call_identity, root_identity, "
                "session_id, controller_id, controlled_target_id, lease_id, "
                "lease_revision, session_epoch, fencing_token, "
                "project_registration_hash, policy_hash, effect_envelope_hash, "
                "approver_principal, approver_capability, authority_decision_id, "
                "expires_at_unix_millis) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, "
                "?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17)"
            )
        );
        UF_TRY(bindText(database, insert.get(), 1, token));
        UF_TRY(bindText(database, insert.get(), 2, call.identity().hex()));
        UF_TRY(bindText(database, insert.get(), 3, root.identity().hex()));
        UF_TRY(bindText(database, insert.get(), 4, controller.sessionId()));
        UF_TRY(bindText(database, insert.get(), 5, controller.controllerId()));
        UF_TRY(bindText(
            database,
            insert.get(),
            6,
            controller.controlledTargetId()
        ));
        UF_TRY(bindText(database, insert.get(), 7, lease.leaseId));
        UF_TRY(bindInteger(database, insert.get(), 8, lease.revision));
        UF_TRY(bindInteger(database, insert.get(), 9, lease.sessionEpoch));
        UF_TRY(bindInteger(database, insert.get(), 10, lease.fencingToken));
        UF_TRY(bindText(database, insert.get(), 11, projectRegistrationHash));
        UF_TRY(bindText(database, insert.get(), 12, policyHash));
        UF_TRY(bindText(
            database,
            insert.get(),
            13,
            evaluated.envelope.hash.hex()
        ));
        UF_TRY(bindText(
            database,
            insert.get(),
            14,
            approver.controllerId()
        ));
        UF_TRY(bindText(
            database,
            insert.get(),
            15,
            request.approverCapability
        ));
        UF_TRY(bindText(
            database,
            insert.get(),
            16,
            authorityDecisionId.value()
        ));
        UF_TRY(bindInteger(
            database,
            insert.get(),
            17,
            request.expiresAtUnixMillis
        ));
        UF_TRY(expectDone(database, insert.get()));
        UF_TRY(transaction.commit());
        return ToolApprovalGrant{
            .token               = std::move(token),
            .authorityDecisionId = authorityDecisionId,
        };
    }

    // The session and lease are re-read against the run's ORIGIN principal,
    // never the executing one. A Project Tool handler is a principal, not a
    // session: it holds no binding and no lease of its own.
    auto OperatorCoordinator::beginToolCallDispatch(
        ToolCallAdmission const& admission
    ) -> Result<ToolCallDispatch>
    {
        auto* const database = m_impl->database.get();
        UF_TRY_VALUE(transaction, Transaction::begin(database));
        UF_TRY_VALUE(
            authorityQuery,
            prepare(
                database,
                "SELECT attempt.session_id, attempt.session_epoch, "
                "attempt.controlled_target_id, attempt.origin_principal_id, "
                "attempt.origin_principal_kind, attempt.capability_profile_hash, "
                "attempt.lease_id, attempt.lease_revision, attempt.fencing_token, "
                "attempt.approval_expires_at_unix_millis "
                "FROM tool_admission_attempts attempt "
                "JOIN sessions session ON session.session_id=attempt.session_id "
                "JOIN session_policies policy ON policy.session_id=attempt.session_id "
                "JOIN control_leases lease ON "
                "lease.controlled_target_id=attempt.controlled_target_id "
                "WHERE attempt.call_identity=?1 AND attempt.attempt_number=?2 "
                "AND session.active=1 AND session.session_epoch=attempt.session_epoch "
                "AND session.authenticated_controller_id=attempt.origin_principal_id "
                "AND session.controller_kind=attempt.origin_principal_kind "
                "AND session.controlled_target_id=attempt.controlled_target_id "
                "AND session.project_registration_hash="
                "attempt.project_registration_hash "
                "AND session.capability_profile_hash=attempt.capability_profile_hash "
                "AND policy.policy_hash=attempt.policy_hash "
                "AND lease.session_id=attempt.session_id "
                "AND lease.controller_id=attempt.origin_principal_id "
                "AND lease.session_epoch=attempt.session_epoch "
                "AND lease.lease_id=attempt.lease_id "
                "AND lease.revision=attempt.lease_revision "
                "AND lease.fencing_token=attempt.fencing_token "
                "AND lease.capability_profile_hash=attempt.capability_profile_hash"
            )
        );
        UF_TRY(bindText(
            database,
            authorityQuery.get(),
            1,
            admission.callIdentity().hex()
        ));
        UF_TRY(bindInteger(
            database,
            authorityQuery.get(),
            2,
            admission.attemptNumber()
        ));
        if (sqlite3_step(authorityQuery.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool dispatch admission authority is no longer live"
            );
        }
        if (sqlite3_column_type(authorityQuery.get(), 9) != SQLITE_NULL)
        {
            UF_TRY_VALUE(currentUnixMillis, unixTimeMilliseconds());
            if (
                currentUnixMillis
                >= static_cast<uint64>(sqlite3_column_int64(
                    authorityQuery.get(),
                    9
                ))
            )
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Tool dispatch approval expired after admission"
                );
            }
        }
        UF_TRY_VALUE(
            budget,
            readAgentBudget(database, columnText(authorityQuery.get(), 0))
        );
        UF_TRY(requireWithinAgentDeadline(budget));
        UF_TRY_VALUE(
            nextRevision,
            checkedSqlIncrement(
                admission.historyRevision(),
                "Tool call history revision"
            )
        );

        UF_TRY_VALUE(
            update,
            prepare(
                database,
                "UPDATE tool_call_history SET state='dispatching', revision=?2 "
                "WHERE call_identity=?1 AND state='admitted' AND revision=?3 "
                "AND active_admission_attempt=?4"
            )
        );
        UF_TRY(bindText(database, update.get(), 1, admission.callIdentity().hex()));
        UF_TRY(bindInteger(
            database,
            update.get(),
            2,
            nextRevision
        ));
        UF_TRY(bindInteger(
            database,
            update.get(),
            3,
            admission.historyRevision()
        ));
        UF_TRY(bindInteger(
            database,
            update.get(),
            4,
            admission.attemptNumber()
        ));
        UF_TRY(expectDone(database, update.get()));
        if (sqlite3_changes(database) != 1)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool dispatch lost its admitted-state CAS"
            );
        }
        UF_TRY(transaction.commit());
        return ToolCallDispatch{
            admission.callIdentity(),
            admission.attemptNumber(),
            nextRevision,
        };
    }

    auto OperatorCoordinator::reenterToolCallDispatch(
        ToolAdmissionRequest const& request
    ) -> Result<ToolCallDispatch>
    {
        auto const& controller = request.controller;
        auto const& lease      = request.lease;
        auto const& root       = request.root;
        auto const& call       = request.call;
        if (
            toolCallEffectMayBeUnrecorded(
                toolEffectComposition(call.provider()),
                call.descriptor().mutability
            )
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool call " + call.toolName()
                    + " is a mutating leaf answered directly by a provider, so "
                      "an interrupted dispatch may have moved the world with no "
                      "record of it; that row is classified uncertain rather "
                      "than re-entered"
            );
        }
        if (
            controller.sessionId() != lease.sessionId
            || controller.controllerId() != lease.controllerId
            || controller.controlledTargetId() != lease.controlledTargetId
            || controller.sessionEpoch() != lease.sessionEpoch
            || controller.capabilityProfileHash() != lease.capabilityProfileHash
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool dispatch re-entry binding and lease name different authority"
            );
        }

        // The coordinate and its stored attributes first, so a re-entry that is
        // really a divergence is named by the field that diverged rather than
        // by a missing history row.
        UF_TRY(persistToolCallPosition(root, call));

        auto* const database = m_impl->database.get();
        UF_TRY_VALUE(transaction, Transaction::begin(database));

        // Re-entry restarts a handler, which is new execution however much of
        // it is replay, so a terminated run refuses it. The coordinate rejoin
        // above cannot say this: it is what keeps an already-recorded call
        // readable after the run stopped.
        UF_TRY(requireLiveToolRun(database, root.identity().hex()));
        UF_TRY(requireLiveBinding(database, controller));
        UF_TRY(requireLiveLease(
            database,
            lease,
            "Tool dispatch re-entry control lease was superseded"
        ));
        UF_TRY(admittedToolAncestors(database, request));
        UF_TRY_VALUE(
            historyQuery,
            prepare(
                database,
                "SELECT state, revision, active_admission_attempt "
                "FROM tool_call_history WHERE call_identity=?1"
            )
        );
        UF_TRY(bindText(database, historyQuery.get(), 1, call.identity().hex()));
        if (sqlite3_step(historyQuery.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool dispatch re-entry names no durable call history"
            );
        }
        UF_TRY_VALUE(
            state,
            parseToolCallState(columnText(historyQuery.get(), 0))
        );
        if (state != ToolCallState::Dispatching)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "Tool dispatch re-entry requires a dispatching call; call {} is {}",
                    call.identity().hex(),
                    toolCallStateWireName(state)
                )
            );
        }
        auto const revision = static_cast<uint64>(
            sqlite3_column_int64(historyQuery.get(), 1)
        );
        auto const activeAttempt = static_cast<uint64>(
            sqlite3_column_int64(historyQuery.get(), 2)
        );

        UF_TRY_VALUE(
            attemptQuery,
            prepare(
                database,
                "SELECT origin_principal_id, origin_principal_kind, "
                "controlled_target_id FROM tool_admission_attempts "
                "WHERE call_identity=?1 AND attempt_number=?2"
            )
        );
        UF_TRY(bindText(database, attemptQuery.get(), 1, call.identity().hex()));
        UF_TRY(bindInteger(database, attemptQuery.get(), 2, activeAttempt));
        if (sqlite3_step(attemptQuery.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "Dispatching Tool call has no active admission row"
            );
        }
        UF_TRY_VALUE(
            admittedKind,
            parseControllerKind(columnText(attemptQuery.get(), 1))
        );
        if (
            columnText(attemptQuery.get(), 0) != controller.controllerId()
            || admittedKind != controller.kind()
            || columnText(attemptQuery.get(), 2)
                != controller.controlledTargetId()
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool dispatch re-entry presents a different origin principal "
                "or controlled target than the admission it re-enters"
            );
        }
        UF_TRY_VALUE(
            nextRevision,
            checkedSqlIncrement(revision, "Tool call history revision")
        );
        UF_TRY_VALUE(
            update,
            prepare(
                database,
                "UPDATE tool_call_history SET revision=?2 WHERE call_identity=?1 "
                "AND state='dispatching' AND revision=?3 "
                "AND active_admission_attempt=?4"
            )
        );
        UF_TRY(bindText(database, update.get(), 1, call.identity().hex()));
        UF_TRY(bindInteger(database, update.get(), 2, nextRevision));
        UF_TRY(bindInteger(database, update.get(), 3, revision));
        UF_TRY(bindInteger(database, update.get(), 4, activeAttempt));
        UF_TRY(expectDone(database, update.get()));
        if (sqlite3_changes(database) != 1)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool dispatch re-entry lost its dispatching-state CAS"
            );
        }
        UF_TRY(transaction.commit());
        return ToolCallDispatch{
            call.identity(),
            activeAttempt,
            nextRevision,
        };
    }

    auto evidenceReceiptJson(EvidenceArtifactReceipt const& receipt)
        -> json::Value
    {
        auto members = std::vector<json::Member>{
            {"byte_count", receiptCounter(receipt.byteCount)},
            {"created_at_unix_ms", receiptCounter(receipt.createdAtUnixMillis)},
            {"frame_identity",
             json::Value::ofObject({
                 {"capture_session_id",
                  receiptCounter(receipt.frameIdentity.sessionId().value())},
                 {"frame_id",
                  receiptCounter(receipt.frameIdentity.frameId().value())},
                 {"target_generation",
                  receiptCounter(receipt.frameIdentity.targetGeneration().value())},
             })},
            {"height", json::Value::ofNumber(static_cast<double>(receipt.height))},
            {"media_type", json::Value::ofString(receipt.mediaType)},
            {std::string{k_screenshotSha256Member},
             json::Value::ofString(receipt.contentHash.hex())},
            {"width", json::Value::ofNumber(static_cast<double>(receipt.width))},
        };
        if (receipt.rectangle)
        {
            members.emplace_back(
                "rectangle",
                json::Value::ofObject({
                    {"height", receiptCounter(receipt.rectangle->height())},
                    {"width", receiptCounter(receipt.rectangle->width())},
                    {"x", receiptCounter(receipt.rectangle->x())},
                    {"y", receiptCounter(receipt.rectangle->y())},
                })
            );
        }
        return json::Value::ofObject(std::move(members));
    }

    auto committedScreenshotEvidence(
        std::optional<CanonicalJson> const& existing,
        EvidenceArtifactReceipt const& receipt
    ) -> Result<CanonicalJson>
    {
        auto members = std::vector<json::Member>{};
        if (existing)
        {
            // A completion that already carried evidence carried an object;
            // anything else is this file's own invariant broken rather than a
            // caller's mistake, so it is checked rather than accommodated.
            UF_CHECK(existing->value().kind() == json::ValueKind::Object);
            auto const carried = existing->value().members();
            members.assign(carried.begin(), carried.end());
        }
        members.emplace_back(
            std::string{k_committedScreenshotMember},
            evidenceReceiptJson(receipt)
        );
        return CanonicalJson::parseExact(
            json::canonicalBytes(json::Value::ofObject(std::move(members)))
        );
    }

    auto toolCallCompletionFor(task::HostDeliveryReport const& report)
        -> Result<ToolCallCompletion>
    {
        auto const outcome   = report.outcome();
        auto const delivered = outcome == task::DeliveryOutcome::Delivered;
        auto const verdict   = std::string{deliveryOutcomeWireName(outcome)};

        // Confirmed delivery carries the provider result. Every other terminal
        // delivery carries the published error object; the delivery value is
        // what distinguishes proven absence from uncertainty.
        UF_TRY_VALUE(
            payload,
            CanonicalJson::parseExact(
                json::canonicalBytes(
                    delivered
                        ? json::Value::ofObject({
                              {"delivered", json::Value::ofBoolean(true)},
                              {"reason",
                               json::Value::ofString(
                                   std::string{report.reason()}
                               )},
                              {"verdict", json::Value::ofString(verdict)},
                          })
                        : json::Value::ofObject({
                              {"code", json::Value::ofString(verdict)},
                              {"message",
                               json::Value::ofString(
                                   std::string{report.reason()}
                               )},
                              {"retryable", json::Value::ofBoolean(false)},
                          })
                )
            )
        );
        // The counters render as decimal strings: RFC 8785 numbers are
        // IEEE-754 doubles, and a Receipt ordinal above 2^53 would round
        // inside a durable Tool outcome.
        UF_TRY_VALUE(
            evidence,
            CanonicalJson::parseExact(
                json::canonicalBytes(json::Value::ofObject({
                    {"host_delivery", json::Value::ofString(verdict)},
                    {"posted_inputs",
                     json::Value::ofString(delivered ? "1" : "0")},
                    {"receipt_id",
                     json::Value::ofString(std::to_string(report.receiptId()))},
                }))
            )
        );
        switch (outcome)
        {
        case task::DeliveryOutcome::Delivered:
            return ToolCallCompletion::confirmed(
                std::move(payload),
                std::move(evidence)
            );
        case task::DeliveryOutcome::NotDelivered:
            return ToolCallCompletion::provenAbsent(
                std::move(payload),
                std::move(evidence)
            );
        case task::DeliveryOutcome::TransportUnknown:
            return ToolCallCompletion::possible(
                std::move(payload),
                std::move(evidence)
            );
        }
        UF_UNREACHABLE_MSG("Unknown task::DeliveryOutcome value");
    }

    auto OperatorCoordinator::reserveToolCallDispatch(
        ToolCallPositionIdentity const& call,
        ControlLease const& lease,
        GenerationId runtimeGeneration,
        std::string const& uiTarget
    ) -> Result<ToolCallDispatchReservation>
    {
        if (uiTarget.empty())
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "a Tool call input delivery must name the model target its "
                "observation resolved"
            );
        }
        auto* const database = m_impl->database.get();
        UF_TRY_VALUE(transaction, Transaction::begin(database));
        UF_TRY(requireLiveLease(
            database,
            lease,
            "Tool call input delivery lease was superseded"
        ));

        // The durable boundary, read rather than restated. `dispatching` is
        // exactly "beginToolCallDispatch committed and no terminal outcome has
        // been written", so the row is the proof and the active attempt is read
        // off it instead of being carried in by a caller.
        UF_TRY_VALUE(
            query,
            prepare(
                database,
                "SELECT history.active_admission_attempt, "
                "attempt.controlled_target_id "
                "FROM tool_call_history history "
                "JOIN tool_admission_attempts attempt "
                "ON attempt.call_identity=history.call_identity "
                "AND attempt.attempt_number=history.active_admission_attempt "
                "WHERE history.call_identity=?1 AND history.state='dispatching'"
            )
        );
        UF_TRY(bindText(database, query.get(), 1, call.identity().hex()));
        if (sqlite3_step(query.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool call input delivery names no dispatching Tool call"
            );
        }
        auto const attemptNumber = static_cast<uint64>(
            sqlite3_column_int64(query.get(), 0)
        );
        if (columnText(query.get(), 1) != lease.controlledTargetId)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool call input delivery lease does not own the controlled "
                "target the admission recorded"
            );
        }
        UF_TRY(transaction.commit());

        // Six of the eight members are the Host's own checks and are filled
        // from the live lease and the caller's generation. frozenPlanHash
        // carries the call identity, which is this path's frozen statement of
        // what the delivery was authorised to do -- the coordinate, the tool,
        // the exact arguments, the descriptor and the catalog, all inside one
        // hash -- and dispatchSequence carries the admission attempt, which is
        // what sequences one call's deliveries.
        return ToolCallDispatchReservation{
            .authority = task::DispatchAuthority{
                .controlledTargetId = lease.controlledTargetId,
                .uiTarget           = uiTarget,
                .leaseId            = lease.leaseId,
                .frozenPlanHash     = call.identity(),
                .runtimeGeneration  = runtimeGeneration,
                .sessionEpoch       = lease.sessionEpoch,
                .fencingToken       = lease.fencingToken,
                .dispatchSequence   = attemptNumber,
            },
        };
    }

    auto OperatorCoordinator::completeToolCallDispatch(
        ToolCallDispatch const& dispatch,
        ToolCallCompletion const& completion
    ) -> Result<StoredToolCallOutcome>
    {
        auto* const database     = m_impl->database.get();
        auto const terminalState = toolCallStateFor(completion.kind());
        auto const& payload      = completion.payload();
        auto const& evidence     = completion.evidence();
        UF_TRY_VALUE(transaction, Transaction::begin(database));
        UF_TRY_VALUE(
            unresolved,
            unresolvedToolDescendants(database, dispatch.callIdentity())
        );
        if (unresolved)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "A Project Tool cannot complete while a descendant remains unresolved"
            );
        }
        UF_TRY_VALUE(
            historyQuery,
            prepare(
                database,
                // The position row is joined for its provider_kind, because
                // the refusal below is keyed on how this call reaches the
                // world and not only on whether it may change it.
                "SELECT history.state, history.revision, "
                "history.active_admission_attempt, history.outcome_payload, "
                "history.outcome_payload_hash, history.evidence, "
                "history.evidence_hash, history.mutating, "
                "position.provider_kind, position.root_identity "
                "FROM tool_call_history history "
                "JOIN tool_call_positions position "
                "ON position.call_identity=history.call_identity "
                "WHERE history.call_identity=?1"
            )
        );
        UF_TRY(bindText(
            database,
            historyQuery.get(),
            1,
            dispatch.callIdentity().hex()
        ));
        if (sqlite3_step(historyQuery.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool dispatch names no durable call history"
            );
        }
        // A replay divergence terminates the whole root. No frame may turn
        // that hard refusal into a terminal value which an ancestor handler
        // could catch and replace with a successful result.
        UF_TRY(requireLiveToolRun(database, columnText(historyQuery.get(), 9)));
        UF_TRY_VALUE(
            state,
            parseToolCallState(columnText(historyQuery.get(), 0))
        );
        auto const revision = static_cast<uint64>(
            sqlite3_column_int64(historyQuery.get(), 1)
        );
        auto const activeAttempt = static_cast<uint64>(
            sqlite3_column_int64(historyQuery.get(), 2)
        );
        auto const mutating   = sqlite3_column_int(historyQuery.get(), 7) != 0;
        auto const mutability = mutating
            ? ToolMutability::Mutating
            : ToolMutability::ReadOnly;

        // The same predicate the restart's row filter is generated from and
        // the re-entry gate refuses on, asked here for the same reason: whether
        // this call could have reached the world without a durable row saying
        // so. Only for that shape is terminal failure a claim the ledger cannot
        // let a provider make, because "it failed" would be asserting an
        // absence nothing observed.
        //
        // A mutating COMPOSED call may report terminal failure, and must be
        // able to: its whole effect surface is children that each already carry
        // their own classification, so a frame refused by name delivered
        // nothing this ledger does not already know about. Converting that to
        // `possible` would set the target-wide mutation barrier over an effect
        // that was never the frame's.
        if (
            toolCallEffectMayBeUnrecorded(
                toolEffectComposition(columnText(historyQuery.get(), 8)),
                mutability
            )
            && terminalState == ToolCallState::TerminalFailure
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "A mutating Tool answered directly by a provider cannot report "
                "terminal failure after dispatch; it must report possible"
            );
        }

        // The dual refusal, and the reason no read-only call can ever need
        // reconciling. `possible` and `proven_absent` are both claims about
        // whether an external effect landed, and a read-only Tool declares
        // none: there is nothing for either to be about. A read-only provider
        // that could not answer has failed, and terminal failure is what a
        // failure is. Without this, a crash-free path could still mint a
        // read-only uncertainty that the reconciliation transition -- mutating
        // by construction, because only a mutation can be uncertain -- would
        // then be unable to resolve.
        if (
            !mutating
            && (terminalState == ToolCallState::Possible
                || terminalState == ToolCallState::ProvenAbsent)
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "A read-only Tool cannot report {}; it declares no effect "
                    "for a delivery classification to be about",
                    toolCallStateWireName(terminalState)
                )
            );
        }
        auto const evidenceBytes = evidence
            ? std::optional<std::string_view>{evidence->bytes()}
            : std::nullopt;
        auto const evidenceHash = evidence
            ? std::optional<std::string>{evidence->contentHash().hex()}
            : std::nullopt;
        if (state == terminalState)
        {
            auto const exactOutcome =
                columnText(historyQuery.get(), 3) == payload.bytes()
                && columnText(historyQuery.get(), 4)
                    == payload.contentHash().hex()
                && optionalColumnText(historyQuery.get(), 5)
                    == (evidence
                        ? std::optional<std::string>{evidence->bytes()}
                        : std::nullopt)
                && optionalColumnText(historyQuery.get(), 6) == evidenceHash;
            if (!exactOutcome)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Terminal Tool outcome is immutable"
                );
            }
            UF_TRY(transaction.commit());
            return StoredToolCallOutcome{
                .state    = state,
                .lookup   = ToolOutcomeLookup::Existing,
                .revision = revision,
            };
        }
        if (
            state != ToolCallState::Dispatching
            || revision != dispatch.historyRevision()
            || activeAttempt != dispatch.attemptNumber()
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool completion does not match the active dispatch"
            );
        }
        UF_TRY_VALUE(
            nextRevision,
            checkedSqlIncrement(revision, "Tool call history revision")
        );

        UF_TRY_VALUE(
            update,
            prepare(
                database,
                "UPDATE tool_call_history SET state=?2, revision=?3, "
                "outcome_payload=?4, outcome_payload_hash=?5, evidence=?6, "
                "evidence_hash=?7 WHERE call_identity=?1 "
                "AND state='dispatching' AND revision=?8 "
                "AND active_admission_attempt=?9"
            )
        );
        UF_TRY(bindText(database, update.get(), 1, dispatch.callIdentity().hex()));
        UF_TRY(bindText(database, update.get(), 2, toolCallStateWireName(terminalState)));
        UF_TRY(bindInteger(database, update.get(), 3, nextRevision));
        UF_TRY(bindText(database, update.get(), 4, payload.bytes()));
        UF_TRY(bindText(database, update.get(), 5, payload.contentHash().hex()));
        UF_TRY(bindOptionalText(database, update.get(), 6, evidenceBytes));
        UF_TRY(bindOptionalText(
            database,
            update.get(),
            7,
            evidenceHash
                ? std::optional<std::string_view>{*evidenceHash}
                : std::nullopt
        ));
        UF_TRY(bindInteger(database, update.get(), 8, revision));
        UF_TRY(bindInteger(database, update.get(), 9, activeAttempt));
        UF_TRY(expectDone(database, update.get()));
        if (sqlite3_changes(database) != 1)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool completion lost its dispatch CAS"
            );
        }
        UF_TRY(transaction.commit());
        return StoredToolCallOutcome{
            .state    = terminalState,
            .lookup   = ToolOutcomeLookup::Created,
            .revision = nextRevision,
        };
    }

    auto OperatorCoordinator::issueToolChild(
        ToolCallIssuingContext& issuing,
        ValidatedToolInvocation const& invocation,
        SnapshotObservationAuthority const& observations
    ) -> Result<ToolCallPositionIdentity>
    {
        auto* const database = m_impl->database.get();
        UF_TRY_VALUE(
            query,
            prepare(
                database,
                "SELECT observation_reference_hash FROM tool_call_positions "
                "WHERE root_identity=?1 AND parent_call_identity=?2 AND call_sequence=?3"
            )
        );
        UF_TRY(bindText(database, query.get(), 1, issuing.rootIdentity().hex()));
        UF_TRY(bindText(database, query.get(), 2, issuing.parent().identity().hex()));
        UF_TRY(bindInteger(database, query.get(), 3, uint64{issuing.issuedCalls()} + 1U));
        auto const step = sqlite3_step(query.get());
        if (step == SQLITE_ROW)
        {
            auto observation = std::optional<ContentHash>{};
            if (sqlite3_column_type(query.get(), 0) != SQLITE_NULL)
            {
                UF_TRY_VALUE(hash, parseHashColumn(columnText(query.get(), 0)));
                observation = hash;
            }
            return issuing.issueNext(invocation, observation);
        }
        if (step != SQLITE_DONE)
        {
            return databaseFailure(database, "could not inspect the next Tool child");
        }
        UF_TRY_VALUE(presented, observations.presented(invocation.canonicalArgs()));
        return presented
            ? issuing.issueAgainstObservation(invocation, *presented)
            : issuing.issue(invocation);
    }

    auto OperatorCoordinator::validateToolCallChildren(
        ToolRootRequestIdentity const& root,
        ToolCallPositionIdentity const& call,
        uint64 consumedChildren
    ) -> Status
    {
        if (call.rootIdentity() != root.identity())
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool child replay belongs to a different root"
            );
        }
        auto* const database = m_impl->database.get();
        UF_TRY_VALUE(
            query,
            prepare(
                database,
                "SELECT COUNT(*), COALESCE(MAX(call_sequence),0) "
                "FROM tool_call_positions WHERE root_identity=?1 AND parent_call_identity=?2"
            )
        );
        UF_TRY(bindText(database, query.get(), 1, root.identity().hex()));
        UF_TRY(bindText(database, query.get(), 2, call.identity().hex()));
        if (sqlite3_step(query.get()) != SQLITE_ROW)
        {
            return databaseFailure(database, "could not inspect recorded Tool children");
        }
        auto const recorded = static_cast<uint64>(sqlite3_column_int64(query.get(), 0));
        auto const last = static_cast<uint64>(sqlite3_column_int64(query.get(), 1));
        if (recorded != consumedChildren || last != consumedChildren)
        {
            auto const reason = std::format(
                "Tool handler child replay diverged: consumed {} children, recorded {} through ordinal {}",
                consumedChildren,
                recorded,
                last
            );
            UF_TRY(terminateToolRun(database, root.identity().hex(), reason));
            return fail(AutomationErrorKind::ActionRejected, reason);
        }
        return ok();
    }

    auto OperatorCoordinator::hasUnresolvedToolDescendants(
        ToolCallPositionIdentity const& call
    ) -> Result<bool>
    {
        return unresolvedToolDescendants(m_impl->database.get(), call.identity());
    }

    auto OperatorCoordinator::ensureToolRunIsLive(
        ToolCallPositionIdentity const& call
    ) -> Status
    {
        return requireLiveToolRun(m_impl->database.get(), call.rootIdentity().hex());
    }

    auto OperatorCoordinator::replayToolCall(
        ToolRootRequestIdentity const& root,
        ToolCallPositionIdentity const& call
    ) -> Result<ToolCallReplay>
    {
        if (call.rootIdentity() != root.identity())
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool replay call belongs to a different root"
            );
        }
        auto* const database = m_impl->database.get();
        UF_TRY_VALUE(
            rootQuery,
            prepare(
                database,
                "SELECT caller_namespace, request_key, request_preimage, "
                "request_preimage_hash FROM tool_root_requests WHERE root_identity=?1"
            )
        );
        UF_TRY(bindText(database, rootQuery.get(), 1, root.identity().hex()));
        if (sqlite3_step(rootQuery.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool replay root is not durable"
            );
        }
        if (
            columnText(rootQuery.get(), 0) != root.callerNamespace().value()
            || columnText(rootQuery.get(), 1) != root.requestKey().value()
            || columnText(rootQuery.get(), 2) != root.requestPreimage().bytes()
            || columnText(rootQuery.get(), 3)
                != root.requestPreimage().contentHash().hex()
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool replay root canonical material was changed"
            );
        }

        UF_TRY_VALUE(callQuery, prepareToolCallCoordinateQuery(database, call));
        if (sqlite3_step(callQuery.get()) != SQLITE_ROW)
        {
            UF_TRY_VALUE(
                parentRecorded,
                toolCallParentIsRecorded(database, root, call)
            );
            if (!parentRecorded)
            {
                auto message = toolCallChangedParentMessage(call);
                UF_TRY(terminateToolRun(
                    database,
                    root.identity().hex(),
                    message
                ));
                return fail(
                    AutomationErrorKind::ActionRejected,
                    std::move(message)
                );
            }
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "Tool replay has no recorded call at {}",
                    toolCallCoordinateName(call)
                )
            );
        }
        if (auto const diverged = divergedToolCallField(callQuery.get(), call))
        {
            auto message = toolCallDivergenceMessage(call, *diverged);
            UF_TRY(terminateToolRun(database, root.identity().hex(), message));
            return fail(AutomationErrorKind::ActionRejected, std::move(message));
        }

        UF_TRY_VALUE(
            historyQuery,
            prepare(
                database,
                "SELECT state, revision, active_admission_attempt, "
                "outcome_payload, outcome_payload_hash, evidence, evidence_hash, "
                "mutating FROM tool_call_history WHERE call_identity=?1"
            )
        );
        UF_TRY(bindText(database, historyQuery.get(), 1, call.identity().hex()));
        if (sqlite3_step(historyQuery.get()) != SQLITE_ROW)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool replay call has no durable history"
            );
        }
        auto const expectedMutating =
            call.descriptor().mutability == ToolMutability::Mutating;
        if (
            (sqlite3_column_int(historyQuery.get(), 7) != 0)
            != expectedMutating
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool replay mutability disagrees with its pinned catalog"
            );
        }
        UF_TRY_VALUE(
            state,
            parseToolCallState(columnText(historyQuery.get(), 0))
        );
        auto replay = ToolCallReplay{
            .state = state,
            .revision = static_cast<uint64>(
                sqlite3_column_int64(historyQuery.get(), 1)
            ),
            .activeAdmissionAttempt = static_cast<uint64>(
                sqlite3_column_int64(historyQuery.get(), 2)
            ),
        };
        auto const payloadBytes = optionalColumnText(historyQuery.get(), 3);
        auto const payloadHash  = optionalColumnText(historyQuery.get(), 4);
        if (payloadBytes.has_value() != payloadHash.has_value())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Stored Tool outcome payload pair is incomplete"
            );
        }
        auto const activeAttempt = replay.activeAdmissionAttempt;
        if (
            payloadBytes.has_value() != toolCallStateHasOutcome(state)
            || (state == ToolCallState::Proposed && activeAttempt != 0U)
            || (state != ToolCallState::Proposed && activeAttempt == 0U)
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Stored Tool call state and outcome shape disagree"
            );
        }
        if (payloadBytes)
        {
            UF_TRY_VALUE(
                payload,
                restoreCanonicalJson(
                    *payloadBytes,
                    *payloadHash,
                    "Tool outcome payload"
                )
            );
            replay.payload.emplace(std::move(payload));
        }
        auto const evidenceBytes = optionalColumnText(historyQuery.get(), 5);
        auto const evidenceHash  = optionalColumnText(historyQuery.get(), 6);
        if (evidenceBytes.has_value() != evidenceHash.has_value())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Stored Tool outcome evidence pair is incomplete"
            );
        }
        if (evidenceBytes && !toolCallStateHasOutcome(state))
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Non-terminal Tool call carries outcome evidence"
            );
        }
        if (evidenceBytes)
        {
            UF_TRY_VALUE(
                evidence,
                restoreCanonicalJson(
                    *evidenceBytes,
                    *evidenceHash,
                    "Tool outcome evidence"
                )
            );
            replay.evidence.emplace(std::move(evidence));
        }
        return replay;
    }

    auto OperatorCoordinator::reconcileMutatingToolCall(
        ControllerBinding const& controller,
        ControlLease const& lease,
        ToolRootRequestIdentity const& root,
        ToolCallPositionIdentity const& call,
        ToolReconciliationQuery const& query
    ) -> Result<ToolCallReplay>
    {
        if (!query)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Tool reconciliation requires a trusted provider query"
            );
        }
        if (call.descriptor().mutability != ToolMutability::Mutating)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool reconciliation requires a mutating call"
            );
        }
        if (
            controller.sessionId() != lease.sessionId
            || controller.controllerId() != lease.controllerId
            || controller.controlledTargetId() != lease.controlledTargetId
            || controller.sessionEpoch() != lease.sessionEpoch
            || controller.capabilityProfileHash() != lease.capabilityProfileHash
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool reconciliation binding and lease name different authority"
            );
        }
        UF_TRY_VALUE(uncertain, replayToolCall(root, call));
        if (uncertain.state != ToolCallState::Possible)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "Only a possible mutating Tool call may be reconciled; call "
                    "{} is {}",
                    call.identity().hex(),
                    toolCallStateWireName(uncertain.state)
                )
            );
        }
        UF_TRY(requireDurableRunAuthority(
            m_impl->database.get(),
            "Tool reconciliation",
            controller,
            lease,
            root.identity().hex()
        ));

        // The query runs here, outside any ledger transaction and only after
        // the row has been proven uncertain and the authority proven live. It
        // is the whole of the seam: nothing else can turn a possible delivery
        // into a resolved one, and a startup path cannot reach it, so
        // proven_absent is never inferred from a process crash. What makes it
        // durable is its answer: every reconciliation kind carries mandatory
        // evidence, and that evidence is stored beside the outcome it produced.
        UF_TRY_VALUE(reconciliation, query(call));

        auto* const database    = m_impl->database.get();
        auto const terminalState = toolCallStateFor(reconciliation.kind());
        UF_TRY_VALUE(transaction, Transaction::begin(database));
        UF_TRY(requireLiveBinding(database, controller));
        UF_TRY(requireLiveLease(
            database,
            lease,
            "Tool reconciliation control lease was superseded"
        ));
        UF_TRY_VALUE(
            nextRevision,
            checkedSqlIncrement(uncertain.revision, "Tool call history revision")
        );
        UF_TRY_VALUE(
            update,
            prepare(
                database,
                "UPDATE tool_call_history SET state=?2, revision=?3, "
                "outcome_payload=?4, outcome_payload_hash=?5, evidence=?6, "
                "evidence_hash=?7 WHERE call_identity=?1 AND state='possible' "
                "AND revision=?8 AND active_admission_attempt=?9 AND mutating=1"
            )
        );
        UF_TRY(bindText(database, update.get(), 1, call.identity().hex()));
        UF_TRY(bindText(database, update.get(), 2, toolCallStateWireName(terminalState)));
        UF_TRY(bindInteger(database, update.get(), 3, nextRevision));
        UF_TRY(bindText(database, update.get(), 4, reconciliation.payload().bytes()));
        UF_TRY(bindText(
            database,
            update.get(),
            5,
            reconciliation.payload().contentHash().hex()
        ));
        UF_TRY(bindText(database, update.get(), 6, reconciliation.evidence().bytes()));
        UF_TRY(bindText(
            database,
            update.get(),
            7,
            reconciliation.evidence().contentHash().hex()
        ));
        UF_TRY(bindInteger(database, update.get(), 8, uncertain.revision));
        UF_TRY(bindInteger(
            database,
            update.get(),
            9,
            uncertain.activeAdmissionAttempt
        ));
        UF_TRY(expectDone(database, update.get()));
        if (sqlite3_changes(database) != 1)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool reconciliation lost its possible-state CAS"
            );
        }
        UF_TRY(transaction.commit());
        return replayToolCall(root, call);
    }

    auto OperatorCoordinator::recordExternalInput(
        ControllerBinding const& reporter,
        ExternalInputReport const& report
    ) -> Result<RecordedExternalInput>
    {
        UF_TRY(requireName(report.reason, "external input reason"));
        if (!reporter.profile().mayReportExternalInput)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "This controller kind may not report external input about a third party"
            );
        }
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY(requireLiveBinding(m_impl->database.get(), reporter));

        auto const& target = reporter.controlledTargetId();
        auto const epoch   = reporter.sessionEpoch();

        // Read before this finding's own append, so a finding never claims to
        // have been detected after itself.
        UF_TRY_VALUE(cursor, currentEventCursor(m_impl->database.get()));

        UF_TRY_VALUE(
            revisionQuery,
            prepare(
                m_impl->database.get(),
                "SELECT COALESCE(MAX(s.snapshot_revision), 0) FROM snapshots s "
                "JOIN sessions session ON session.session_id=s.session_id "
                "WHERE session.controlled_target_id=?1 AND s.session_epoch=?2"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), revisionQuery.get(), 1, target));
        UF_TRY(bindInteger(m_impl->database.get(), revisionQuery.get(), 2, epoch));
        if (sqlite3_step(revisionQuery.get()) != SQLITE_ROW)
        {
            return databaseFailure(
                m_impl->database.get(),
                "could not read the snapshot revision an external input invalidates"
            );
        }
        auto const invalidatedRevision = static_cast<uint64>(
            sqlite3_column_int64(revisionQuery.get(), 0)
        );

        UF_TRY_VALUE(findingId, randomToken(m_impl->database.get(), k_opaqueTokenBytes));
        UF_TRY_VALUE(
            insert,
            prepare(
                m_impl->database.get(),
                "INSERT INTO external_input_findings(finding_id, controlled_target_id, "
                "session_epoch, reporter_session_id, detected_after_cursor, "
                "invalidated_snapshot_revision, required_action, reason) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 1, findingId));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 2, target));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 3, epoch));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            4,
            reporter.sessionId()
        ));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 5, cursor));
        UF_TRY(bindInteger(m_impl->database.get(), insert.get(), 6, invalidatedRevision));
        UF_TRY(bindText(
            m_impl->database.get(),
            insert.get(),
            7,
            externalInputActionWireName(report.requiredAction)
        ));
        UF_TRY(bindText(m_impl->database.get(), insert.get(), 8, report.reason));
        UF_TRY(expectDone(m_impl->database.get(), insert.get()));

        UF_TRY(appendLedgerEvent(
            m_impl->database.get(),
            epoch,
            target,
            LedgerEventKind::ExternalInputDetected,
            findingId
        ));

        // Read back inside the transaction rather than returning the locals
        // that were bound. A caller who is told what the Operator computed
        // learns nothing about what the Operator stored, and a finding whose
        // stored cursor disagreed with the reported one would be invisible --
        // the audit reads the row, not the return value.
        UF_TRY_VALUE(
            stored,
            prepare(
                m_impl->database.get(),
                "SELECT detected_after_cursor, invalidated_snapshot_revision "
                "FROM external_input_findings WHERE finding_id=?1"
            )
        );
        UF_TRY(bindText(m_impl->database.get(), stored.get(), 1, findingId));
        if (sqlite3_step(stored.get()) != SQLITE_ROW)
        {
            return databaseFailure(
                m_impl->database.get(),
                "could not read back the recorded external input finding"
            );
        }
        auto const storedCursor = static_cast<uint64>(
            sqlite3_column_int64(stored.get(), 0)
        );
        auto const storedRevision = static_cast<uint64>(
            sqlite3_column_int64(stored.get(), 1)
        );
        UF_TRY(transaction.commit());
        return RecordedExternalInput{
            .findingId                   = std::move(findingId),
            .detectedAfterCursor         = storedCursor,
            .invalidatedSnapshotRevision = storedRevision,
        };
    }

    auto OperatorCoordinator::subscribe(
        ControllerBinding const& controller,
        SubscriptionCursor after,
        uint32 maximumEvents
    ) -> Result<SubscriptionRead>
    {
        if (maximumEvents == 0U)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "A subscription read must ask for at least one event"
            );
        }

        // One transaction for the whole read, so the head, the oldest available
        // sequence and the rows are one consistent view. It is BEGIN IMMEDIATE
        // like every other path here rather than a lighter read transaction,
        // because one writer at a time is what makes the sequence commit-ordered
        // and a second kind of transaction would be a second set of rules.
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY(requireLiveBinding(m_impl->database.get(), controller));
        UF_TRY_VALUE(currentCursor, currentEventCursor(m_impl->database.get()));

        // Derived rather than assumed. Bounded retention moves this floor, and
        // MIN(sequence) - 1 is the last cursor that can still be answered
        // without hiding a deleted row.
        UF_TRY_VALUE(
            oldestQuery,
            prepare(
                m_impl->database.get(),
                "SELECT COALESCE(MIN(sequence) - 1, ?1) FROM ledger_events"
            )
        );
        UF_TRY(bindInteger(m_impl->database.get(), oldestQuery.get(), 1, currentCursor));
        if (sqlite3_step(oldestQuery.get()) != SQLITE_ROW)
        {
            return databaseFailure(
                m_impl->database.get(),
                "could not read the oldest available cursor"
            );
        }
        auto const oldestCursor = static_cast<uint64>(
            sqlite3_column_int64(oldestQuery.get(), 0)
        );

        // Past the head means another database or epoch; below the floor means
        // retention deleted part of the requested stream. Neither is an empty
        // batch, because both require a fresh snapshot before continuing.
        if (after.value > currentCursor || after.value < oldestCursor)
        {
            UF_TRY(transaction.commit());
            return SubscriptionRead{ResyncRequired{
                .requestedCursor       = after,
                .oldestAvailableCursor = SubscriptionCursor{oldestCursor},
                .currentCursor         = SubscriptionCursor{currentCursor},
            }};
        }

        // Scoped to the controlled target and not to the binding's own session:
        // a controller that could see only its own events could not notice that
        // a human took control away from it, which is the one thing it most
        // needs to notice.
        UF_TRY_VALUE(
            events,
            prepare(
                m_impl->database.get(),
                "SELECT sequence, kind, controlled_target_id, subject_id "
                "FROM ledger_events WHERE controlled_target_id=?1 AND sequence>?2 "
                "ORDER BY sequence LIMIT ?3"
            )
        );
        UF_TRY(bindText(
            m_impl->database.get(),
            events.get(),
            1,
            controller.controlledTargetId()
        ));
        UF_TRY(bindInteger(m_impl->database.get(), events.get(), 2, after.value));
        UF_TRY(bindInteger(m_impl->database.get(), events.get(), 3, maximumEvents));

        auto batch = SubscriptionBatch{.events = {}, .nextCursor = after};
        auto step  = sqlite3_step(events.get());
        while (step == SQLITE_ROW)
        {
            UF_TRY_VALUE(kind, parseLedgerEventKind(columnText(events.get(), 1)));
            auto const sequence = static_cast<uint64>(
                sqlite3_column_int64(events.get(), 0)
            );
            batch.events.emplace_back(LedgerEvent{
                .sequence           = SubscriptionCursor{sequence},
                .kind               = kind,
                .controlledTargetId = columnText(events.get(), 2),
                .subjectId          = columnText(events.get(), 3),
            });

            // The cursor follows what was delivered, never the head: a batch cut
            // short by maximumEvents whose cursor named the head would silently
            // skip every event the cut left behind.
            batch.nextCursor = SubscriptionCursor{sequence};
            step             = sqlite3_step(events.get());
        }
        if (step != SQLITE_DONE)
        {
            return databaseFailure(m_impl->database.get(), "could not read the event stream");
        }
        UF_TRY(transaction.commit());
        return SubscriptionRead{std::move(batch)};
    }

    auto OperatorCoordinator::remainingBudget(
        ControllerBinding const& controller
    ) -> Result<AgentBudgetRemaining>
    {
        UF_TRY_VALUE(transaction, Transaction::begin(m_impl->database.get()));
        UF_TRY(requireLiveBinding(m_impl->database.get(), controller));
        UF_TRY_VALUE(
            budget,
            readAgentBudget(m_impl->database.get(), controller.sessionId())
        );
        UF_TRY_VALUE(now, steadyMillisecondsNow());
        UF_TRY(transaction.commit());
        return AgentBudgetRemaining{
            .toolCalls    = budget.remainingToolCalls,
            .mutations    = budget.remainingMutations,
            .observations = budget.remainingObservations,
            .riskUnits    = budget.remainingRiskUnits,
            .elapsedMillisRemaining     = now > budget.deadlineSteadyMillis
                ? uint64{0}
                : budget.deadlineSteadyMillis - now,
            .consecutiveNoProgressSteps = budget.consecutiveNoProgressSteps,
        };
    }

    auto toolRuntimeDurableRecordMaterial() -> std::string
    {
        // Named by their table, so a table renamed moves this material even
        // when its columns do not, and rendered in a fixed order so the
        // material is a function of the DDL and not of a container's ordering.
        constexpr auto k_toolRuntimeTables = std::array{
            std::pair{
                std::string_view{"tool_admission_attempts"},
                k_toolAdmissionAttemptsDdl,
            },
            std::pair{std::string_view{"tool_approvals"}, k_toolApprovalsDdl},
            std::pair{
                std::string_view{"tool_call_history"},
                k_toolCallHistoryDdl,
            },
            std::pair{
                std::string_view{"tool_call_positions"},
                k_toolCallPositionsDdl,
            },
            std::pair{
                std::string_view{"tool_root_requests"},
                k_toolRootRequestsDdl,
            },
            std::pair{std::string_view{"tool_runs"}, k_toolRunsDdl},
        };

        auto rows = std::vector<json::Value>{};
        rows.reserve(k_toolRuntimeTables.size());
        for (auto const& [name, ddl] : k_toolRuntimeTables)
        {
            rows.emplace_back(json::Value::ofObject({
                {"ddl", json::Value::ofString(std::string{ddl})},
                {"table", json::Value::ofString(std::string{name})},
            }));
        }
        return json::canonicalBytes(json::Value::ofObject({
            {"tool_runtime_tables", json::Value::ofArray(std::move(rows))},
        }));
    }
}
