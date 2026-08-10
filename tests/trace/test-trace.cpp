#include <trace/event.hpp>
#include <trace/file-sink.hpp>
#include <trace/recorder.hpp>
#include <trace/sink.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <concepts>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace uf::trace
{
    static_assert(
        !std::constructible_from<TraceScalar, std::vector<std::byte>>
    );
    static_assert(
        !std::constructible_from<TraceScalar, std::vector<TraceField>>
    );

    namespace
    {
        [[nodiscard]] auto hashOf(char digit) -> ContentHash
        {
            auto const encoded = "sha256:" + std::string(64U, digit);
            auto const hash    = ContentHash::parse(encoded);
            REQUIRE(hash.has_value());
            return *hash;
        }

        [[nodiscard]]
        auto streamSpec(
            std::string sessionId = "session-1",
            char manifestDigit = 'a',
            std::string producer = "operator.host"
        ) -> TraceStreamSpec
        {
            return TraceStreamSpec{
                .sessionId           = std::move(sessionId),
                .sessionManifestHash = hashOf(manifestDigit),
                .producer            = std::move(producer),
            };
        }

        [[nodiscard]]
        auto eventSpec(
            std::string eventType = "host.audit"
        ) -> TraceEventSpec
        {
            return TraceEventSpec{
                .eventType = std::move(eventType),
                .audit     = AuditMetadata{
                    .actor = "operator.agent",
                },
                .payload   = TypedTracePayload{
                    .schemaHash = hashOf('b'),
                },
            };
        }

        class CollectingSink final : public ITraceSink
        {
            std::vector<TraceEvent> m_events{};

        public:
            [[nodiscard]] auto append(TraceEvent const& event) -> Status override
            {
                m_events.emplace_back(event);
                return ok();
            }

            [[nodiscard]]
            auto events() const noexcept UF_LIFETIME_BOUND
                -> std::vector<TraceEvent> const&
            {
                return m_events;
            }
        };

        class FailingSink final : public ITraceSink
        {
            uint64 m_calls{};

        public:
            [[nodiscard]]
            auto append(TraceEvent const& /*event*/) -> Status override
            {
                ++m_calls;
                return fail(
                    std::make_error_code(std::errc::io_error),
                    "deliberate sink failure"
                );
            }

            [[nodiscard]] auto calls() const noexcept -> uint64
            {
                return m_calls;
            }
        };

        [[nodiscard]]
        auto readLines(
            std::filesystem::path const& path
        ) -> std::vector<std::string>
        {
            auto stream = std::ifstream{path, std::ios::binary};
            REQUIRE(stream.is_open());

            auto lines = std::vector<std::string>{};
            auto line  = std::string{};
            while (std::getline(stream, line))
            {
                lines.emplace_back(line);
            }
            return lines;
        }

        [[nodiscard]] auto uniqueTracePath() -> std::filesystem::path
        {
            static auto counter = uint64{};
            ++counter;
            auto const nonce = std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count();
            return std::filesystem::temp_directory_path()
                / std::format("uf-trace-v2-{}-{}.jsonl", nonce, counter);
        }
    }

    TEST_CASE("recorder owns stream identity, order, and typed serialization")
    {
        auto sink         = std::make_unique<CollectingSink>();
        auto sinkObserver = sink.get();
        auto recorder     = TraceRecorder::create(std::move(sink), streamSpec());
        REQUIRE(recorder.has_value());

        auto event = eventSpec("host.delivery");
        event.audit.references = {
            TraceReference{.type = "operation", .id = "op-1"},
            TraceReference{.type = "observation", .id = "obs-1"},
        };
        event.payload.fields = {
            TraceField{.name = "zeta", .value = uint64{7}},
            TraceField{.name = "alpha", .value = std::string{"a\"b\\c"}},
            TraceField{.name = "enabled", .value = true},
            TraceField{.name = "offset", .value = int64{-2}},
            TraceField{.name = "reason", .value = std::monostate{}},
        };
        REQUIRE(recorder->emit(event).has_value());
        REQUIRE(sinkObserver->events().size() == 1U);

        auto const& recorded = sinkObserver->events().front();
        CHECK(recorded.eventType() == "host.delivery");
        CHECK(recorded.sessionId() == "session-1");
        CHECK(recorded.sessionManifestHash() == hashOf('a'));
        CHECK(recorded.producer() == "operator.host");
        CHECK(recorded.sequence() == 1U);

        auto const expected = std::format(
            "{{\"schema\":\"umbraflow-trace/v2\",\"event_type\":\"host.delivery\""
            ",\"session_id\":\"session-1\""
            ",\"session_manifest_hash\":\"{}\",\"monotonic_sequence\":1"
            ",\"recorded_at_unix_millis\":{},\"audit\":{{\"actor\":"
            "\"operator.agent\",\"producer\":\"operator.host\",\"references\":["
            "{{\"type\":\"observation\",\"id\":\"obs-1\"}},"
            "{{\"type\":\"operation\",\"id\":\"op-1\"}}]}}"
            ",\"payload\":{{\"schema_hash\":\"{}\",\"fields\":["
            "{{\"name\":\"alpha\",\"type\":\"text\",\"value\":\"a\\\"b\\\\c\"}},"
            "{{\"name\":\"enabled\",\"type\":\"bool\",\"value\":true}},"
            "{{\"name\":\"offset\",\"type\":\"int64\",\"value\":-2}},"
            "{{\"name\":\"reason\",\"type\":\"null\"}},"
            "{{\"name\":\"zeta\",\"type\":\"uint64\",\"value\":7}}]}}}}",
            hashOf('a').hex(),
            recorded.recordedAtUnixMillis(),
            hashOf('b').hex()
        );
        CHECK(serializeTraceEvent(recorded) == expected);
    }

    TEST_CASE("invalid input does not consume sequence")
    {
        auto sink         = std::make_unique<CollectingSink>();
        auto sinkObserver = sink.get();
        auto recorder     = TraceRecorder::create(std::move(sink), streamSpec());
        REQUIRE(recorder.has_value());

        auto invalid      = eventSpec();
        invalid.eventType = "unnamespaced";
        CHECK_FALSE(recorder->emit(invalid).has_value());

        REQUIRE(recorder->emit(eventSpec("host.first")).has_value());
        REQUIRE(recorder->emit(eventSpec("host.second")).has_value());

        REQUIRE(sinkObserver->events().size() == 2U);
        CHECK(sinkObserver->events()[0].sequence() == 1U);
        CHECK(sinkObserver->events()[1].sequence() == 2U);
    }

    TEST_CASE("recorder rejects invalid stream identity before accepting events")
    {
        auto nullRecorder = TraceRecorder::create(
            std::unique_ptr<ITraceSink>{},
            streamSpec()
        );
        CHECK_FALSE(nullRecorder.has_value());

        auto invalidProducer     = streamSpec();
        invalidProducer.producer = "Operator.Host";
        auto invalidProducerRecorder = TraceRecorder::create(
            std::make_unique<CollectingSink>(),
            invalidProducer
        );
        CHECK_FALSE(invalidProducerRecorder.has_value());

        auto workspaceSession      = streamSpec();
        workspaceSession.sessionId = "E:/private/annotation-workspace.sqlite";
        auto workspaceRecorder = TraceRecorder::create(
            std::make_unique<CollectingSink>(),
            workspaceSession
        );
        CHECK_FALSE(workspaceRecorder.has_value());
    }

    TEST_CASE("production Trace rejects frame data and authoring workspace leaks")
    {
        auto sink     = std::make_unique<CollectingSink>();
        auto recorder = TraceRecorder::create(std::move(sink), streamSpec());
        REQUIRE(recorder.has_value());

        auto const forbiddenNames = std::vector<std::string>{
            "screenshot",
            "screen.shot",
            "audit.screenshot_data",
            "frame_bytes",
            "frame.bytes",
            "payload.frame_data",
            "pixel_bytes",
            "annotation_workspace_path",
            "annotation.workspace.path",
        };
        for (auto const& name : forbiddenNames)
        {
            auto event = eventSpec();
            event.payload.fields = {
                TraceField{.name = name, .value = std::string{"redacted"}},
            };
            CHECK_FALSE(recorder->emit(event).has_value());
        }

        auto dataUri = eventSpec();
        dataUri.payload.fields = {
            TraceField{
                .name  = "evidence",
                .value = std::string{"data:image/png;base64,iVBORw0KGgo="},
            },
        };
        CHECK_FALSE(recorder->emit(dataUri).has_value());

        auto embeddedDataUri = eventSpec();
        embeddedDataUri.payload.fields = {
            TraceField{
                .name  = "evidence",
                .value = std::string{"blocked: data:image/png;base64,AAAA"},
            },
        };
        CHECK_FALSE(recorder->emit(embeddedDataUri).has_value());

        auto encodedBytes = eventSpec();
        encodedBytes.payload.fields = {
            TraceField{.name = "evidence", .value = std::string(64U, 'A')},
        };
        CHECK_FALSE(recorder->emit(encodedBytes).has_value());

        auto spacedEncodedBytes = eventSpec();
        spacedEncodedBytes.payload.fields = {
            TraceField{
                .name  = "evidence",
                .value = std::string{" "} + std::string(64U, '_') + " ",
            },
        };
        CHECK_FALSE(recorder->emit(spacedEncodedBytes).has_value());

        auto controlText = eventSpec();
        controlText.payload.fields = {
            TraceField{
                .name  = "message",
                .value = std::string{"line\nbreak"},
            },
        };
        CHECK_FALSE(recorder->emit(controlText).has_value());

        auto invalidUtf8 = eventSpec();
        auto invalidText = std::string{};
        invalidText.push_back(static_cast<char>(0xC3));
        invalidUtf8.payload.fields = {
            TraceField{.name = "message", .value = std::move(invalidText)},
        };
        CHECK_FALSE(recorder->emit(invalidUtf8).has_value());

        auto workspace = eventSpec();
        workspace.payload.fields = {
            TraceField{
                .name  = "resource",
                .value = std::string{"E:/private/annotation-workspace.sqlite"},
            },
        };
        CHECK_FALSE(recorder->emit(workspace).has_value());

        auto obfuscatedWorkspace = eventSpec();
        obfuscatedWorkspace.payload.fields = {
            TraceField{
                .name  = "resource",
                .value = std::string{"annotation///workspace...sqlite"},
            },
        };
        CHECK_FALSE(recorder->emit(obfuscatedWorkspace).has_value());

        auto frameReference = eventSpec();
        frameReference.audit.references = {
            TraceReference{.type = "frame.bytes", .id = "evidence-1"},
        };
        CHECK_FALSE(recorder->emit(frameReference).has_value());

        auto oversized = eventSpec();
        oversized.payload.fields = {
            TraceField{
                .name  = "message",
                .value = std::string(4097U, 'x'),
            },
        };
        CHECK_FALSE(recorder->emit(oversized).has_value());
    }

    TEST_CASE("duplicate payload fields and references fail closed")
    {
        auto sink     = std::make_unique<CollectingSink>();
        auto recorder = TraceRecorder::create(std::move(sink), streamSpec());
        REQUIRE(recorder.has_value());

        auto fields = eventSpec();
        fields.payload.fields = {
            TraceField{.name = "value", .value = uint64{1}},
            TraceField{.name = "value", .value = uint64{2}},
        };
        CHECK_FALSE(recorder->emit(fields).has_value());

        auto references = eventSpec();
        references.audit.references = {
            TraceReference{.type = "operation", .id = "op-1"},
            TraceReference{.type = "operation", .id = "op-1"},
        };
        CHECK_FALSE(recorder->emit(references).has_value());
    }

    TEST_CASE("payload fields sort in JCS member order, not UTF-8 byte order")
    {
        // U+FFFD is EF BF BD in UTF-8 and the single UTF-16 unit FFFD; U+10000
        // is F0 90 80 80 and the surrogate pair D800 DC00. Byte order puts
        // U+FFFD first because EF < F0; UTF-16 code-unit order puts U+10000
        // first because D800 < FFFD. This pair is where the default string
        // comparison and the order RFC 8785 requires disagree, so it is the
        // only kind of input that can hold the recorder to the right one.
        auto const replacement   = std::string{"\xEF\xBF\xBD"};
        auto const supplementary = std::string{"\xF0\x90\x80\x80"};

        auto ascending = std::vector<TraceField>{
            TraceField{.name = replacement, .value = uint64{1}},
            TraceField{.name = supplementary, .value = uint64{2}},
        };
        sortTraceFieldsCanonically(ascending);
        CHECK(ascending[0].name == supplementary);
        CHECK(ascending[1].name == replacement);

        auto descending = std::vector<TraceField>{
            TraceField{.name = supplementary, .value = uint64{2}},
            TraceField{.name = replacement, .value = uint64{1}},
        };
        sortTraceFieldsCanonically(descending);
        CHECK(descending[0].name == supplementary);
        CHECK(descending[1].name == replacement);

        // The recorder rejects duplicates by reading adjacency after this
        // sort, so equal names must still come out next to each other.
        auto duplicates = std::vector<TraceField>{
            TraceField{.name = "beta", .value = uint64{1}},
            TraceField{.name = "alpha", .value = uint64{2}},
            TraceField{.name = "beta", .value = uint64{3}},
            TraceField{.name = "alpha", .value = uint64{4}},
        };
        sortTraceFieldsCanonically(duplicates);
        CHECK(duplicates[0].name == "alpha");
        CHECK(duplicates[1].name == "alpha");
        CHECK(duplicates[2].name == "beta");
        CHECK(duplicates[3].name == "beta");
    }

    TEST_CASE("no field name a recorder accepts can separate those two orders")
    {
        auto sink     = std::make_unique<CollectingSink>();
        auto recorder = TraceRecorder::create(std::move(sink), streamSpec());
        REQUIRE(recorder.has_value());

        // The name validator admits [a-z] at each segment start, [a-z0-9_-]
        // after it, and '.' between segments. Every byte it admits is ASCII,
        // where UTF-8 byte order and UTF-16 code-unit order are one order, so
        // the ordering above cannot be observed through emit(). That is a
        // property of the validator, not of the sort: relaxing it here is what
        // would make the sort's comparator start to matter end to end.
        auto supplementary = eventSpec();
        supplementary.payload.fields = {
            TraceField{.name = "\xF0\x90\x80\x80", .value = uint64{1}},
        };
        CHECK_FALSE(recorder->emit(supplementary).has_value());

        auto replacement = eventSpec();
        replacement.payload.fields = {
            TraceField{.name = "\xEF\xBF\xBD", .value = uint64{1}},
        };
        CHECK_FALSE(recorder->emit(replacement).has_value());
    }

    TEST_CASE("sink uncertainty permanently faults a recorder")
    {
        auto sink         = std::make_unique<FailingSink>();
        auto sinkObserver = sink.get();
        auto recorder     = TraceRecorder::create(std::move(sink), streamSpec());
        REQUIRE(recorder.has_value());

        CHECK_FALSE(recorder->emit(eventSpec()).has_value());
        CHECK_FALSE(recorder->emit(eventSpec()).has_value());
        CHECK(sinkObserver->calls() == 1U);
    }

    TEST_CASE("file sink appends new evidence and never truncates old bytes")
    {
        auto const path = uniqueTracePath();

        {
            auto sink = FileTraceSink::createNew(path);
            REQUIRE(sink.has_value());
            auto recorder = TraceRecorder::create(
                std::move(*sink),
                streamSpec()
            );
            REQUIRE(recorder.has_value());
            REQUIRE(recorder->emit(eventSpec("host.first")).has_value());
            auto const flushedPrefix = readLines(path);
            REQUIRE(flushedPrefix.size() == 1U);
            CHECK(flushedPrefix[0].contains("\"event_type\":\"host.first\""));
            REQUIRE(recorder->emit(eventSpec("host.second")).has_value());
        }

        auto const lines = readLines(path);
        REQUIRE(lines.size() == 2U);
        CHECK(lines[0].contains("\"event_type\":\"host.first\""));
        CHECK(lines[0].contains("\"monotonic_sequence\":1"));
        CHECK(lines[1].contains("\"event_type\":\"host.second\""));
        CHECK(lines[1].contains("\"monotonic_sequence\":2"));

        auto second = FileTraceSink::createNew(path);
        CHECK_FALSE(second.has_value());
        CHECK(readLines(path) == lines);

        auto error = std::error_code{};
        std::filesystem::remove(path, error);
        CHECK_FALSE(error);
    }
}
