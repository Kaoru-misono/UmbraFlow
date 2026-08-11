#include <cli/args.hpp>
#include <cli/explore-protocol.hpp>
#include <cli/queue-cursor.hpp>

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <script/engine.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// The wire between an agent and an exploration session, plus the cursor that
// keeps a restart from running a chunk twice.
namespace uf::cli
{
    namespace
    {
        class TemporaryDirectory final
        {
            std::filesystem::path m_path;

        public:
            explicit TemporaryDirectory(std::string_view label)
                : m_path{std::filesystem::temp_directory_path() / label}
            {
                auto error = std::error_code{};
                std::filesystem::remove_all(m_path, error);
                REQUIRE(std::filesystem::create_directories(m_path, error));
            }

            TemporaryDirectory(TemporaryDirectory const&)                    = delete;
            TemporaryDirectory(TemporaryDirectory&&)                         = delete;
            auto operator=(TemporaryDirectory const&) -> TemporaryDirectory& = delete;
            auto operator=(TemporaryDirectory&&) -> TemporaryDirectory&      = delete;

            ~TemporaryDirectory()
            {
                auto error = std::error_code{};
                std::filesystem::remove_all(m_path, error);
            }

            [[nodiscard]] auto path() const noexcept -> std::filesystem::path const&
            {
                return m_path;
            }
        };

        auto writeText(
            std::filesystem::path const& path,
            std::string_view text
        ) -> void
        {
            auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};
            REQUIRE(stream.is_open());
            stream << text;
            REQUIRE(stream.good());
        }

        TEST_CASE("a queue line carries an id and a chunk and nothing else")
        {
            auto const parsed = parseExploreChunk(
                R"({"id":"step-3","chunk":"return 1"})"
            );
            REQUIRE(parsed.has_value());
            CHECK(parsed->id == "step-3");
            CHECK(parsed->chunk == "return 1");

            SUBCASE("member order does not matter")
            {
                auto const other = parseExploreChunk(
                    R"({"chunk":"return 2","id":"b"})"
                );
                REQUIRE(other.has_value());
                CHECK(other->id == "b");
                CHECK(other->chunk == "return 2");
            }

            SUBCASE("a chunk may carry newlines as escapes")
            {
                auto const multi = parseExploreChunk(
                    R"({"id":"a","chunk":"local x = 1\nreturn x"})"
                );
                REQUIRE(multi.has_value());
                CHECK(multi->chunk == "local x = 1\nreturn x");
            }
        }

        TEST_CASE("a queue line that is nearly right is refused rather than guessed at")
        {
            // Each of these runs code against a live target if read generously,
            // which is why the reader is strict.
            auto const bad = std::vector<std::string>{
                R"({"id":"a"})",
                R"({"chunk":"return 1"})",
                R"({"id":"","chunk":"return 1"})",
                R"({"id":"a","chunk":"return 1","extra":"x"})",
                R"({"id":"a","id":"b","chunk":"return 1"})",
                R"({"id":"a","chunk":1})",
                R"({"id":"a","chunk":"return 1"} trailing)",
                R"({"id":"a","chunk":"return \q 1"})",
                R"({"id":"a","chunk":"return 1")",
                R"(not json at all)",
            };
            for (auto const& line : bad)
            {
                auto const parsed = parseExploreChunk(line);
                CHECK_MESSAGE(!parsed.has_value(), line);
            }

            // The control: the shape they each break is otherwise accepted.
            CHECK(parseExploreChunk(R"({"id":"a","chunk":"return 1"})").has_value());
        }

        TEST_CASE("an unknown member is named in the refusal")
        {
            auto const parsed = parseExploreChunk(
                R"({"id":"a","op":"cycle_open"})"
            );
            REQUIRE(!parsed.has_value());
            CHECK(parsed.error().message().contains("op"));

            // The refusal names what this protocol DOES take, so a line written
            // to some other command shape is corrected rather than ignored.
            CHECK(parsed.error().message().contains("id"));
        }

        // Fixed rather than measured: the rendering is under test, not the figures.
        constexpr auto k_lineHeap = script::HeapUsage{
            .usedBytes    = 1024,
            .ceilingBytes = 67'108'864,
            .peakBytes    = 4096,
        };

        constexpr auto k_lineHeapText =
            std::string_view{R"("heap":{"used":1024,"ceiling":67108864})"};

        TEST_CASE("an agent can say which line is its last")
        {
            // Absorbed from the retired operator front-end, whose `quit` was the
            // one thing it could do that this channel could not. Without it a
            // session ends only by falling silent, which spends the idle timeout
            // every time and dates run.finished a default later than the work.
            auto const ending = parseExploreChunk(
                R"({"id":"last","chunk":"return 1","end":true})"
            );
            REQUIRE(ending.has_value());
            CHECK(
                *ending
                == ExploreChunk{
                    .id          = "last",
                    .chunk       = "return 1",
                    .endsSession = true,
                }
            );

            // It is a modifier, not a command: a line without it still runs, and
            // spelling it false is the same as leaving it out.
            auto const ordinary =
                parseExploreChunk(R"({"id":"a","chunk":"return 1"})");
            REQUIRE(ordinary.has_value());
            CHECK_FALSE(ordinary->endsSession);

            auto const explicitFalse = parseExploreChunk(
                R"({"id":"a","chunk":"return 1","end":false})"
            );
            REQUIRE(explicitFalse.has_value());
            CHECK_FALSE(explicitFalse->endsSession);

            // A boolean, and only the JSON spelling of one. Taking "true" as
            // well would make a line's meaning depend on which spelling an agent
            // happened to pick.
            CHECK_FALSE(
                parseExploreChunk(R"({"id":"a","chunk":"return 1","end":"true"})")
                    .has_value()
            );
            CHECK_FALSE(
                parseExploreChunk(R"({"id":"a","chunk":"return 1","end":1})")
                    .has_value()
            );
            CHECK_FALSE(
                parseExploreChunk(
                    R"({"id":"a","chunk":"return 1","end":true,"end":true})"
                )
                    .has_value()
            );

            // Ending is not a way to send no chunk: every line still runs one.
            CHECK_FALSE(parseExploreChunk(R"({"id":"a","end":true})").has_value());

            // A results file is read without the queue beside it, so the ending
            // has to be legible in the answers themselves.
            CHECK(
                exploreSuccess("last", script::ScriptValue{1.0}, k_lineHeap, true)
                    .contains(R"("ended":true)")
            );
            CHECK_FALSE(
                exploreSuccess("a", script::ScriptValue{1.0}, k_lineHeap, false)
                    .contains("ended")
            );
        }

        TEST_CASE("a result line distinguishes the four things a chunk can return")
        {
            CHECK(
                exploreSuccess("a", script::ScriptValue{}, k_lineHeap, false)
                == R"({"id":"a","ok":true,"heap":{"used":1024,"ceiling":67108864}})"
            );
            CHECK(
                exploreSuccess("a", script::ScriptValue{false}, k_lineHeap, false)
                == R"({"id":"a","ok":true,"value":false,)"
                    R"("heap":{"used":1024,"ceiling":67108864}})"
            );
            CHECK(
                exploreSuccess(
                    "a",
                    script::ScriptValue{std::string{"home"}},
                    k_lineHeap,
                    false
                )
                == R"({"id":"a","ok":true,"value":"home",)"
                    R"("heap":{"used":1024,"ceiling":67108864}})"
            );

            auto const number =
                exploreSuccess("a", script::ScriptValue{3.0}, k_lineHeap, false);
            CHECK(number.starts_with(R"({"id":"a","ok":true,"value":3)"));

            // Returning nothing and returning false are different answers.
            CHECK(
                exploreSuccess("a", script::ScriptValue{}, k_lineHeap, false)
                != exploreSuccess("a", script::ScriptValue{false}, k_lineHeap, false)
            );
        }

        TEST_CASE("a chunk answering with inf or nan is refused, not written raw")
        {
            // `{:.17g}` renders these as the bare tokens inf/-inf/nan, which are
            // not JSON literals, so the agent's reader would throw on a line the
            // protocol promised was JSON.
            for (auto const value : std::array<double, 3>{
                std::numeric_limits<double>::infinity(),
                -std::numeric_limits<double>::infinity(),
                std::numeric_limits<double>::quiet_NaN(),
            })
            {
                auto const line =
                    exploreSuccess("a", script::ScriptValue{value}, k_lineHeap, false);
                CAPTURE(line);
                CHECK(line.contains(R"("ok":false)"));
                CHECK(!line.contains("inf"));
                CHECK(!line.contains("nan"));
            }
        }

        TEST_CASE("every answered chunk carries the heap against its ceiling")
        {
            // The ceiling is measured against garbage as well as live data, so a
            // figure carried only by the failing line would arrive a chunk late.
            CHECK(
                exploreSuccess("a", script::ScriptValue{}, k_lineHeap, false)
                    .contains(k_lineHeapText)
            );

            auto const error =
                fail(AutomationErrorKind::InvalidResource, "boom").error();
            CHECK(exploreFailure("a", error, k_lineHeap, false).contains(k_lineHeapText));

            // A line that answered no chunk never reached a VM, so it has none.
            auto const refused =
                fail(AutomationErrorKind::InvalidResource, "bad line").error();
            CHECK(!serializeExploreParseFailure(refused).contains("heap"));
        }

        TEST_CASE("a failed chunk reports the domain's own kind and the id it came from")
        {
            auto const error = fail(
                AutomationErrorKind::StaleObservation,
                "this observation cycle is no longer open"
            ).error();

            auto const line = exploreFailure("step-7", error, k_lineHeap, false);
            CHECK(line.contains(R"("id":"step-7")"));
            CHECK(line.contains(R"("ok":false)"));
            CHECK(line.contains(R"("error":"stale_observation")"));
            CHECK(line.contains("no longer open"));
        }

        TEST_CASE("a line that named no chunk is answered under no id")
        {
            auto const error =
                fail(AutomationErrorKind::InvalidResource, "bad line").error();
            auto const line = serializeExploreParseFailure(error);

            // Empty rather than invented: attributing the refusal to a chunk the
            // agent may not have sent would send it looking in the wrong place.
            CHECK(line.contains(R"("id":"")"));
            CHECK(line.contains(R"("ok":false)"));
        }

        TEST_CASE("a message with a control byte or a quote survives the line")
        {
            auto const error = fail(
                AutomationErrorKind::InvalidResource,
                "a \"quoted\" thing\nand a newline"
            ).error();
            auto const line = exploreFailure("a", error, k_lineHeap, false);

            CHECK(line.contains(R"(\"quoted\")"));
            CHECK(line.contains(R"(\n)"));
            CHECK(!line.contains('\n'));
        }

        TEST_CASE("a cursor round-trips and refuses a file it did not write whole")
        {
            auto const record = QueueCursorRecord{
                .queue    = std::filesystem::path{"/tmp/queue.jsonl"},
                .position = QueuePosition{
                    .consumedBytes = 128,
                    .consumedLines = 3,
                },
            };
            auto const text = serializeQueueCursor(record);

            auto const parsed =
                parseQueueCursor(text, std::filesystem::path{"cursor"});
            REQUIRE(parsed.has_value());
            CHECK(*parsed == record);

            SUBCASE("a file cut short mid-write is not believed")
            {
                auto truncated = text;
                truncated.resize(text.size() - 3U);
                CHECK(
                    !parseQueueCursor(
                        truncated,
                        std::filesystem::path{"cursor"}
                    ).has_value()
                );
            }

            SUBCASE("a missing field is a refusal rather than a default")
            {
                CHECK(
                    !parseQueueCursor(
                        "queue=/tmp/queue.jsonl\nconsumed-bytes=1\n",
                        std::filesystem::path{"cursor"}
                    ).has_value()
                );
            }

            SUBCASE("a field this build does not know is a refusal")
            {
                CHECK(
                    !parseQueueCursor(
                        text + "surprise=1\n",
                        std::filesystem::path{"cursor"}
                    ).has_value()
                );
            }
        }

        TEST_CASE("a cursor beside another queue is refused rather than believed")
        {
            auto const directory = TemporaryDirectory{"uf-explore-cursor-queue"};
            auto const queue     = directory.path() / "queue.jsonl";
            auto const other     = directory.path() / "other.jsonl";
            auto const cursor    = queueCursorPath(queue);

            writeText(
                cursor,
                serializeQueueCursor(
                    QueueCursorRecord{
                        .queue    = other,
                        .position = QueuePosition{.consumedBytes = 10},
                    }
                )
            );

            // Its offset would seek into a file it never described, skipping
            // every chunk before that point unseen.
            CHECK(!readQueueCursor(cursor, queue).has_value());
        }

        TEST_CASE("a restart resumes where the answers stopped")
        {
            auto const directory = TemporaryDirectory{"uf-explore-cursor-resume"};
            auto const queue     = directory.path() / "queue.jsonl";
            constexpr auto k_first  = std::string_view{
                "{\"id\":\"a\",\"chunk\":\"return 1\"}\n"
            };
            constexpr auto k_second = std::string_view{
                "{\"id\":\"b\",\"chunk\":\"return 2\"}\n"
            };
            writeText(queue, std::string{k_first} + std::string{k_second});

            auto const cursorPath = queueCursorPath(queue);
            auto cursor = QueueCursor::open(
                cursorPath,
                queue,
                QueuePosition{}
            );
            REQUIRE(cursor.has_value());
            REQUIRE(cursor->advance(k_first.size()).has_value());

            auto const recorded = readQueueCursor(cursorPath, queue);
            REQUIRE(recorded.has_value());
            REQUIRE(recorded->has_value());
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): REQUIRE above proved engagement.
            CHECK((*recorded)->consumedBytes == k_first.size());
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): REQUIRE above proved engagement.
            CHECK((*recorded)->consumedLines == 1U);

            auto const extent = measureQueueExtent(queue);
            REQUIRE(extent.has_value());
            CHECK(extent->framedLines == 2U);
            CHECK(extent->framedBytes == k_first.size() + k_second.size());

            auto const start = resolveQueueStart(*recorded, *extent, queue);
            REQUIRE(start.has_value());
            CHECK(start->consumedBytes == k_first.size());
        }

        TEST_CASE("a queue with chunks and no cursor is refused rather than replayed")
        {
            auto const directory = TemporaryDirectory{"uf-explore-cursor-ambiguous"};
            auto const queue     = directory.path() / "queue.jsonl";
            writeText(queue, "{\"id\":\"a\",\"chunk\":\"return 1\"}\n");

            auto const extent = measureQueueExtent(queue);
            REQUIRE(extent.has_value());

            // Running them could re-deliver clicks against a live target and
            // skipping them could drop work, so neither reading is taken.
            auto const start = resolveQueueStart(
                std::optional<QueuePosition>{},
                *extent,
                queue
            );
            CHECK(!start.has_value());

            SUBCASE("an empty queue with no cursor is the ordinary first session")
            {
                writeText(queue, "");
                auto const empty = measureQueueExtent(queue);
                REQUIRE(empty.has_value());
                auto const fresh = resolveQueueStart(
                    std::optional<QueuePosition>{},
                    *empty,
                    queue
                );
                REQUIRE(fresh.has_value());
                CHECK(fresh->consumedBytes == 0U);
            }
        }

        TEST_CASE("a cursor past the end of its queue is refused")
        {
            auto const directory = TemporaryDirectory{"uf-explore-cursor-shrank"};
            auto const queue     = directory.path() / "queue.jsonl";
            writeText(queue, "{\"id\":\"a\",\"chunk\":\"return 1\"}\n");

            auto const extent = measureQueueExtent(queue);
            REQUIRE(extent.has_value());

            // The queue was truncated or replaced, so the cursor records chunks
            // that are no longer there.
            auto const start = resolveQueueStart(
                std::optional<QueuePosition>{
                    QueuePosition{.consumedBytes = 10'000}
                },
                *extent,
                queue
            );
            CHECK(!start.has_value());
        }

        TEST_CASE("explore takes the four flags that name a session")
        {
            auto const raw = std::vector<std::string>{
                "--project", "proj",
                "--hwnd",    "0x504f2",
                "--queue",   "q.jsonl",
                "--results", "r.jsonl",
            };
            auto const parsed = parseExploreArguments(raw);
            REQUIRE(parsed.has_value());
            CHECK(parsed->project == std::filesystem::path{"proj"});
            CHECK(parsed->windowHandle == intptr{0x504f2});
            CHECK(parsed->budget == k_defaultPixelComparisonBudget);
            CHECK(!parsed->ocrModels.has_value());

            SUBCASE("a missing required flag is refused")
            {
                auto const partial = std::vector<std::string>{
                    "--project", "proj",
                    "--queue",   "q.jsonl",
                };
                CHECK(!parseExploreArguments(partial).has_value());
            }

            SUBCASE("an unknown flag is refused rather than ignored")
            {
                auto raw2 = raw;
                raw2.emplace_back("--chunk");
                raw2.emplace_back("return 1");
                CHECK(!parseExploreArguments(raw2).has_value());
            }
        }

        TEST_CASE("the usage text names explore among the modes")
        {
            CHECK(exploreUsageText().contains("umbra-flow explore"));
            CHECK(usageText().contains("umbra-flow explore"));
        }
    }
}
