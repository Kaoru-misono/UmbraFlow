#include "binding-fixture.hpp"

#include "../domain/test-helpers.hpp"

#include <task/exploration-session.hpp>
#include <task/framework-bundle.hpp>
#include <task/task-host.hpp>
#include <task/task-loader.hpp>

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <image/png.hpp>

#include <trace/event.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::task
{
    // The event stream a resident host would subscribe to. task-host.hpp
    // declares the type and never defines it, because what a task event is
    // remains a P2 question; this is the program's single definition and exists
    // only so subscribeEvents has something to reference. Members would be the
    // speculation the incomplete declaration avoids.
    class ITaskEventSink final
    {
    };

    namespace
    {
        constexpr auto k_sourceId = "00000000-0000-0000-0000-000000000301";
        constexpr auto k_anchorId = "00000000-0000-0000-0000-000000000021";
        constexpr auto k_actionId = "00000000-0000-0000-0000-000000000022";
        constexpr auto k_pageId   = "00000000-0000-0000-0000-000000000121";

        constexpr auto k_projectId = std::string_view{"personal.task_host"};

        // The task every run in this file drives: load a template, observe once,
        // match it, click it. It also NAMES a page and an element, so the
        // run.resources_validated line carries a non-empty closure of each,
        // resolved against page-model.toml by the pre-VM pass.
        constexpr auto k_taskSource = std::string_view{
            // The two literals sit in a function the run never calls: the pre-VM
            // pass reads them off the AST, and the `uf` root carries no name
            // tables to evaluate them against any more
            // (docs/plans/2026-07-31-script-owned-page-model.md 9).
            "local function names()\n"
            "    return uf.pages.home, uf.elements.daily_button\n"
            "end\n"
            "local template = ctx:template_load(ctx:project_read(\"assets/daily.png\"))\n"
            "local cycle = ctx:cycle_open()\n"
            "local hit = ctx:cycle_match(cycle, template, 0, 0, 3, 2)\n"
            "if hit == nil then ctx:cycle_close(cycle) return 0 end\n"
            "ctx:cycle_click(cycle, hit)\n"
            "return 1\n"
        };

        // The page model the host reads, and the whole of what it reads out of
        // it: the geometry rectangles were measured at, and the names a script
        // may spell. Everything else in the file is layer two's.
        constexpr auto k_pageModel = std::string_view{R"toml(
schema = "umbraflow-project/l2-v1"
base_resolution = [3, 2]
base_dpi = [96, 96]

[[element]]
name = "home_marker"
capabilities = ["identify"]
rect = [0, 0, 3, 2]

[[element]]
name = "daily_button"
capabilities = ["interact"]
rect = [0, 0, 3, 2]

[[appearance]]
element = "daily_button"
name = "default"
source = "assets/daily.png"
threshold = 10000

[[page]]
name = "home"

[[reference]]
page = "home"
element = "daily_button"
holding = "owned"
exercised = ["interact"]
)toml"};

        class TemporaryDir final
        {
            std::filesystem::path m_path{};

        public:
            explicit TemporaryDir(std::string_view role)
            {
                static auto s_sequence = std::atomic<uint64>{1U};
                auto const token = std::chrono::steady_clock::now()
                    .time_since_epoch()
                    .count();
                m_path = std::filesystem::temp_directory_path()
                    / std::format(
                        "umbraflow-task-host-{}-{}-{}",
                        role,
                        token,
                        s_sequence.fetch_add(1U, std::memory_order_relaxed)
                    );
                auto error         = std::error_code{};
                auto const created = std::filesystem::create_directory(m_path, error);
                REQUIRE(created);
                REQUIRE_FALSE(error);
            }

            TemporaryDir(TemporaryDir const&) = delete;
            TemporaryDir(TemporaryDir&&) = delete;
            auto operator=(TemporaryDir const&) -> TemporaryDir& = delete;
            auto operator=(TemporaryDir&&) -> TemporaryDir& = delete;

            ~TemporaryDir() noexcept
            {
                try
                {
                    auto error = std::error_code{};
                    static_cast<void>(std::filesystem::remove_all(m_path, error));
                }
                catch (...)
                {
                }
            }

            [[nodiscard]]
            auto path() const -> std::filesystem::path
            {
                return m_path;
            }
        };

        auto writeFile(
            std::filesystem::path const& path,
            std::span<std::byte const> bytes
        ) -> void
        {
            auto error = std::error_code{};
            std::filesystem::create_directories(path.parent_path(), error);
            REQUIRE_FALSE(error);
            auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};
            REQUIRE(stream.is_open());
            for (auto const byte : bytes)
            {
                stream.put(std::to_integer<char>(byte));
            }
            stream.flush();
            REQUIRE(stream.good());
        }

        auto writeText(std::filesystem::path const& path, std::string_view text) -> void
        {
            writeFile(path, std::as_bytes(std::span{text}));
        }

        // The three-by-two authoring source every fixture project is compiled
        // from. Each pixel is distinct, so a frame painted with the same pixels
        // reproduces every crop at its own position and nowhere else, with a
        // zero sum of absolute differences.
        [[nodiscard]]
        auto sourcePixelsRgba() -> std::vector<std::byte>
        {
            return std::vector<std::byte>{
                asByte(1), asByte(2), asByte(3), asByte(255),
                asByte(4), asByte(5), asByte(6), asByte(255),
                asByte(7), asByte(8), asByte(9), asByte(255),
                asByte(10), asByte(11), asByte(12), asByte(255),
                asByte(13), asByte(14), asByte(15), asByte(255),
                asByte(16), asByte(17), asByte(18), asByte(255),
            };
        }

        [[nodiscard]]
        auto fixtureFingerprint() -> ProjectFingerprint
        {
            return test::fingerprint(3, 2, 96, 96);
        }

        // Publishes a project at `root`: the page model, the one template the
        // task searches for, and `tasks/<name>.luau`. TaskHost never executes a
        // loose-path script and reads no generated/annotations.runtime.toml any
        // more (docs/plans/2026-07-31-script-owned-page-model.md 9).
        auto publishProject(
            std::filesystem::path const& root,
            std::string_view taskName,
            std::string_view taskSource
        ) -> void
        {
            writeText(root / "page-model.toml", k_pageModel);

            // A one-by-one template of the source frame's first pixel, so a match
            // over the whole frame lands at a known position.
            auto const rgba    = sourcePixelsRgba();
            auto const cropped = std::vector<std::byte>{
                rgba[0],
                rgba[1],
                rgba[2],
                rgba[3],
            };
            auto encoded = image::encodeRgbaPng("task-host-template.png", 1, 1, cropped);
            REQUIRE(encoded.has_value());
            writeFile(root / "assets" / "daily.png", *encoded);

            writeText(
                root / "tasks" / (std::string{taskName} + ".luau"),
                taskSource
            );
        }

        // A frame reproducing the authoring source pixel for pixel, in BGRA and
        // stamped now so the action's frame-age check passes.
        [[nodiscard]]
        auto sourceFrame(FrameId frameId) -> Frame
        {
            auto const fingerprint = fixtureFingerprint();
            auto const transform   = CoordinateTransform::create(
                Point<DesktopSpace>{0.0F, 0.0F},
                static_cast<float>(fingerprint.width()),
                static_cast<float>(fingerprint.height()),
                fingerprint.width(),
                fingerprint.height()
            );
            REQUIRE(transform.has_value());

            auto const rgba = sourcePixelsRgba();
            auto pixels     = std::vector<std::byte>{};
            pixels.reserve(rgba.size());
            for (auto index = std::size_t{0}; index < rgba.size(); index += 4U)
            {
                pixels.emplace_back(rgba[index + 2U]);
                pixels.emplace_back(rgba[index + 1U]);
                pixels.emplace_back(rgba[index]);
                pixels.emplace_back(rgba[index + 3U]);
            }

            auto const width = checkedCast<std::size_t>(fingerprint.width());
            REQUIRE(width.has_value());
            auto const stride = *width * bytesPerPixel(PixelFormat::Bgra8);
            auto const buffer = std::shared_ptr<FrameBuffer const>{
                std::make_shared<FrameBuffer>(std::move(pixels))
            };
            auto frame = Frame::create(
                frameId,
                CaptureSessionId{7},
                TargetGeneration::fromValue(3),
                MonotonicInstant::now(),
                fingerprint.width(),
                fingerprint.height(),
                stride,
                PixelFormat::Bgra8,
                buffer,
                *transform
            );
            REQUIRE(frame.has_value());
            return *std::move(frame);
        }

        [[nodiscard]]
        auto runConfig(std::filesystem::path const& tracePath) -> TaskRunConfig
        {
            auto frames = std::vector<Frame>{};
            frames.emplace_back(sourceFrame(FrameId{91}));
            return TaskRunConfig{
                .frameSource     = std::make_unique<FakeFrameSource>(std::move(frames)),
                .actionSink      = std::make_unique<CountingActionSink>(),
                .liveFingerprint = fixtureFingerprint(),
                .maximumPixelComparisons = 1'000,
                .recognitionTimeout      = std::chrono::duration_cast<
                    MonotonicInstant::Duration
                >(std::chrono::seconds{5}),
                .maxActionFrameAge = std::chrono::duration_cast<
                    MonotonicInstant::Duration
                >(std::chrono::seconds{5}),
                .tracePath = tracePath,
            };
        }

        // Every line the run wrote, with the non-golden `meta` member stripped.
        // The host owns the sink, so the file is the only place the stream is
        // observable -- and it is what an operator actually reads.
        [[nodiscard]]
        auto traceLines(std::filesystem::path const& path) -> std::vector<std::string>
        {
            auto stream = std::ifstream{path, std::ios::binary};
            REQUIRE(stream.is_open());
            auto lines = std::vector<std::string>{};
            auto line  = std::string{};
            while (std::getline(stream, line))
            {
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                lines.emplace_back(trace::stripNonGoldenFields(line));
            }
            return lines;
        }

        // The stamped prefix every line of a run carries. Comparing prefixes
        // pins the event order, the monotonic sequence and the single
        // run/generation identity in one assertion.
        [[nodiscard]]
        auto stampedPrefix(std::string_view kind, std::size_t sequence) -> std::string
        {
            return std::format(
                R"({{"schema":"umbraflow-trace/v4","kind":"{}")"
                R"(,"seq":{},"runId":1,"generationId":1,"frontEnd":"task")",
                kind,
                sequence
            );
        }

        [[nodiscard]]
        auto errorOfKind(AutomationErrorKind kind) -> Error
        {
            return fail(kind, "task host outcome test").error();
        }
    }

    TEST_CASE("TaskRunReport derives its outcome from the failure that ended the run")
    {
        // outcome() is the single classification the report and the run.finished
        // line both read, so a Failed-with-no-failure state is unrepresentable.
        SUBCASE("no failure is a completed run")
        {
            CHECK(TaskRunReport{}.outcome() == TaskRunOutcome::Completed);
        }

        SUBCASE("a cancellation spends the generation rather than failing it")
        {
            auto const report = TaskRunReport{
                .failure = errorOfKind(AutomationErrorKind::Cancelled),
            };
            CHECK(report.outcome() == TaskRunOutcome::Cancelled);
        }

        SUBCASE("every other automation kind is a failed run")
        {
            auto const report = TaskRunReport{
                .failure = errorOfKind(AutomationErrorKind::Timeout),
            };
            CHECK(report.outcome() == TaskRunOutcome::Failed);
        }

        SUBCASE("an error carrying no automation kind is still a failed run")
        {
            auto const report = TaskRunReport{
                .failure = Error{
                    std::make_error_code(std::errc::io_error),
                    "not an automation failure",
                },
            };
            CHECK(report.outcome() == TaskRunOutcome::Failed);
        }
    }

    TEST_CASE("TaskHost reports its P2 verbs as unsupported capabilities")
    {
        // The verb set is frozen from day one so the API surface does not change
        // between P0 and P2. These three are signature commitments backed by an
        // existing error kind, and this pins that they say so rather than half-do.
        auto host = TaskHost{};
        auto sink = ITaskEventSink{};

        auto const pauseStatus     = host.pause(GenerationId{1});
        auto const resumeStatus    = host.resume(GenerationId{1});
        auto const subscribeStatus = host.subscribeEvents(sink);

        REQUIRE_FALSE(pauseStatus.has_value());
        REQUIRE_FALSE(resumeStatus.has_value());
        REQUIRE_FALSE(subscribeStatus.has_value());
        CHECK(
            automationErrorKind(pauseStatus.error())
            == AutomationErrorKind::UnsupportedCapability
        );
        CHECK(
            automationErrorKind(resumeStatus.error())
            == AutomationErrorKind::UnsupportedCapability
        );
        CHECK(
            automationErrorKind(subscribeStatus.error())
            == AutomationErrorKind::UnsupportedCapability
        );
    }

    TEST_CASE("TaskHost addresses every verb by generation and rejects an unknown one")
    {
        auto const temp = TemporaryDir{"generations"};
        publishProject(temp.path(), "daily", k_taskSource);

        auto host = TaskHost{};
        auto const generation = host.loadProject(temp.path());
        REQUIRE(generation.has_value());
        CHECK(*generation == GenerationId{1});

        // Every verb that takes a generation asks the same question first and
        // refuses in the same sentence, so a ninth verb that forgets the guard is
        // a missing line here rather than nothing at all.
        auto const unknown = GenerationId{99};
        auto const refuses = [](auto const& result, std::string_view verb)
        {
            INFO("verb ", verb);
            REQUIRE_FALSE(result.has_value());
            CHECK(
                automationErrorKind(result.error())
                == AutomationErrorKind::InvalidResource
            );
            CHECK(
                result.error().message() == "no loaded project for generation 99"
            );
        };

        constexpr auto routine = FrameworkRoutine{
            .name   = "unreachable",
            .source = "return 0\n",
        };
        auto const tracePath = temp.path() / "unreachable.jsonl";

        refuses(host.cancel(unknown), "cancel");
        refuses(host.queryTask(unknown), "queryTask");
        refuses(host.projectFingerprint(unknown), "projectFingerprint");
        refuses(host.projectElementCount(unknown), "projectElementCount");
        refuses(
            host.startTask(unknown, "daily", runConfig(tracePath)),
            "startTask"
        );
        refuses(
            host.runFrameworkRoutine(unknown, routine, runConfig(tracePath)),
            "runFrameworkRoutine"
        );
        refuses(
            host.startExplorationSession(unknown, runConfig(tracePath)),
            "startExplorationSession"
        );

        // A rejected run opens no trace file, so nothing was written for it.
        CHECK_FALSE(std::filesystem::exists(tracePath));
    }

    TEST_CASE("what a task returns reaches the report that describes its run")
    {
        // The four facts in a report are the host's, and the trace records native
        // calls rather than the task's account of them. Coercing the return to a
        // number left a task with no way to say where it got to, which is why the
        // first real daily writes its own log file every step.
        SUBCASE("a sentence is carried whole")
        {
            auto const temp = TemporaryDir{"return-sentence"};
            publishProject(
                temp.path(),
                "daily",
                "return \"stopped at step 176: node_map went away before the click\"\n"
            );

            auto host             = TaskHost{};
            auto const generation = host.loadProject(temp.path());
            REQUIRE(generation.has_value());

            auto const report = host.startTask(
                *generation,
                "daily",
                runConfig(temp.path() / "task.jsonl")
            );
            REQUIRE(report.has_value());
            CHECK_FALSE(report->failure.has_value());
            CHECK(
                report->returned
                == "stopped at step 176: node_map went away before the click"
            );
        }

        SUBCASE("a task that returns nothing says nothing")
        {
            auto const temp = TemporaryDir{"return-nothing"};
            publishProject(temp.path(), "daily", "local quiet = 1\n");

            auto host             = TaskHost{};
            auto const generation = host.loadProject(temp.path());
            REQUIRE(generation.has_value());

            auto const report = host.startTask(
                *generation,
                "daily",
                runConfig(temp.path() / "task.jsonl")
            );
            REQUIRE(report.has_value());
            CHECK_FALSE(report->failure.has_value());
            CHECK(report->returned.empty());
        }

        SUBCASE("a return no line can carry fails the run rather than vanishing")
        {
            // Under the coercion this replaces, a returned table was silently 0.
            auto const temp = TemporaryDir{"return-table"};
            publishProject(temp.path(), "daily", "return { done = true }\n");

            auto host             = TaskHost{};
            auto const generation = host.loadProject(temp.path());
            REQUIRE(generation.has_value());

            auto const report = host.startTask(
                *generation,
                "daily",
                runConfig(temp.path() / "task.jsonl")
            );
            REQUIRE(report.has_value());
            REQUIRE(report->failure.has_value());
            CHECK(
                automationErrorKind(*report->failure)
                == AutomationErrorKind::InvalidResource
            );
            CHECK(report->outcome() == TaskRunOutcome::Failed);
        }
    }

    TEST_CASE("a generation drives one front-end, whichever arrives first")
    {
        // A generation holds the single-open-cycle ledger, so two policy sources
        // driving one would contend for it. Both orders are checked, because
        // "the first one wins" is only a rule if it holds symmetrically, and
        // both on the SAME host so no process-level dispatch is doing the work.
        SUBCASE("a task run refuses an exploration session afterwards")
        {
            // The third front-end is checked on its own rather than inferred: an
            // agent session holds the same ledger a task does, so admitting one
            // beside a task would be two policy sources over one frame.
            auto const temp = TemporaryDir{"exclusion-task-then-agent"};
            publishProject(temp.path(), "daily", k_taskSource);

            auto host             = TaskHost{};
            auto const generation = host.loadProject(temp.path());
            REQUIRE(generation.has_value());

            auto const report = host.startTask(
                *generation,
                "daily",
                runConfig(temp.path() / "task.jsonl")
            );
            REQUIRE(report.has_value());

            auto refused = host.startExplorationSession(
                *generation,
                runConfig(temp.path() / "explore.jsonl")
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(
                automationErrorKind(refused.error())
                == AutomationErrorKind::UnsupportedCapability
            );
            CHECK_FALSE(std::filesystem::exists(temp.path() / "explore.jsonl"));
        }

        SUBCASE("an exploration session refuses a task run and an operator afterwards")
        {
            auto const temp = TemporaryDir{"exclusion-agent-first"};
            publishProject(temp.path(), "daily", k_taskSource);

            auto host             = TaskHost{};
            auto const generation = host.loadProject(temp.path());
            REQUIRE(generation.has_value());

            auto session = host.startExplorationSession(
                *generation,
                runConfig(temp.path() / "explore.jsonl")
            );
            REQUIRE(session.has_value());

            auto const refusedTask = host.startTask(
                *generation,
                "daily",
                runConfig(temp.path() / "task.jsonl")
            );
            REQUIRE_FALSE(refusedTask.has_value());
            CHECK_FALSE(std::filesystem::exists(temp.path() / "task.jsonl"));
        }

        SUBCASE("the same front-end twice is allowed")
        {
            // A generation legitimately runs several tasks in sequence; without
            // this half the exclusion bans reuse rather than contention.
            auto const temp = TemporaryDir{"exclusion-same"};
            publishProject(temp.path(), "daily", k_taskSource);

            auto host             = TaskHost{};
            auto const generation = host.loadProject(temp.path());
            REQUIRE(generation.has_value());

            auto const first = host.startTask(
                *generation,
                "daily",
                runConfig(temp.path() / "first.jsonl")
            );
            REQUIRE(first.has_value());
            auto const second = host.startTask(
                *generation,
                "daily",
                runConfig(temp.path() / "second.jsonl")
            );
            CHECK(second.has_value());
        }
    }

    TEST_CASE("what the framework minted is released once nothing can present it")
    {
        auto const temp = TemporaryDir{"minting-heap"};
        publishProject(temp.path(), "daily", k_taskSource);

        auto host             = TaskHost{};
        auto const generation = host.loadProject(temp.path());
        REQUIRE(generation.has_value());

        auto const tracePath = temp.path() / "minting-heap.jsonl";
        auto session = host.startExplorationSession(*generation, runConfig(tracePath));
        REQUIRE(session.has_value());

        // Every constructor records what it minted so a look-alike can be
        // refused later. Recording it is not the same as keeping it: a project
        // model is minted once per load and dropped, and holding those entries
        // strongly is what let one exploration session walk into the VM's
        // memory ceiling after roughly 150 loads.
        constexpr auto k_mintFiveHundred = std::string_view{
            R"lua(
                for index = 1, 500 do
                    model.Element.new({
                        name          = 'element' .. index,
                        capabilities  = { 'read' },
                        rect          = { x = 0, y = 0, width = 10, height = 10 },
                        expected_text = 'text',
                    })
                end
                return 1
            )lua"
        };

        // The first round pays costs that are one-time rather than per-element
        // -- the allocator's own growth, the strings this chunk interns -- so
        // the baseline is taken after it and not before.
        REQUIRE((*session)->evaluate(k_mintFiveHundred, "warm").has_value());
        auto const baseline = (*session)->heapUsage().usedBytes;

        for (auto round = 0; round < 4; ++round)
        {
            REQUIRE((*session)->evaluate(k_mintFiveHundred, "round").has_value());
        }
        auto const settled = (*session)->heapUsage().usedBytes;

        // Two thousand further elements. Measured at about a kilobyte each when
        // the registries held them, which is some two megabytes; released, the
        // reading moves only by the allocator's page granularity, and moves
        // DOWN as often as up.
        CAPTURE(baseline);
        CAPTURE(settled);
        CHECK(settled < baseline + (256U * 1024U));
    }

    TEST_CASE("an exploration session runs one chunk at a time under one bracket")
    {
        auto const temp = TemporaryDir{"exploration-bracket"};
        publishProject(temp.path(), "daily", k_taskSource);

        auto host             = TaskHost{};
        auto const generation = host.loadProject(temp.path());
        REQUIRE(generation.has_value());

        auto const tracePath = temp.path() / "explore.jsonl";
        auto session = host.startExplorationSession(*generation, runConfig(tracePath));
        REQUIRE(session.has_value());

        // Each chunk runs under a project environment built fresh for it, so a
        // global one chunk writes never reaches the next; what the host owns does.
        auto const first = (*session)->evaluate("carried = 7 return 1", "one");
        REQUIRE(first.has_value());
        CHECK(first->number() == 1.0);

        auto const second = (*session)->evaluate(
            "return carried == nil and 'gone' or 'leaked'",
            "two"
        );
        REQUIRE(second.has_value());
        REQUIRE(second->text() != nullptr);
        CHECK(*second->text() == "gone");

        // A chunk that leaves a cycle open costs its own line and nothing more:
        // the host sweeps the bracket, so the next chunk opens a cycle of its
        // own rather than meeting a framework-bug refusal.
        auto const leaky = (*session)->evaluate("ctx:cycle_open() return true", "three");
        REQUIRE(leaky.has_value());
        auto const after = (*session)->evaluate(
            "local t = ctx:cycle_open() ctx:cycle_close(t) return true",
            "four"
        );
        REQUIRE(after.has_value());
        CHECK(after->boolean() == true);
        CHECK_FALSE((*session)->terminalKind().has_value());

        // A chunk that raises is an ordinary outcome; the session goes on.
        auto const raised = (*session)->evaluate("error('deliberate')", "five");
        CHECK_FALSE(raised.has_value());
        auto const recovered = (*session)->evaluate("return 'still here'", "six");
        REQUIRE(recovered.has_value());

        // A value no result line can carry is refused by TYPE rather than
        // rendered as something plausible.
        auto const table = (*session)->evaluate("return {}", "seven");
        CHECK_FALSE(table.has_value());

        auto const report = (*session)->finish(std::nullopt);
        CHECK(report.outcome() == TaskRunOutcome::Completed);
        CHECK(report.taskName.empty());
        CHECK(report.seed != 0U);

        auto const lines = traceLines(tracePath);
        REQUIRE_FALSE(lines.empty());
        for (auto const& line : lines)
        {
            CAPTURE(line);
            CHECK(line.contains("\"frontEnd\":\"annotation\""));
        }
        CHECK(lines.front().contains("\"kind\":\"run.started\""));
        CHECK(lines.back().contains("\"kind\":\"run.finished\""));

        // A framework DID run here, unlike on an operator stream, so the run
        // identity names the build the agent's chunks executed against.
        CHECK(lines.front().contains("\"frameworkHash\""));
    }

    TEST_CASE("a chunk is a reclamation point, so what it allocated dies with it")
    {
        auto const temp = TemporaryDir{"exploration-reclaim"};
        publishProject(temp.path(), "daily", k_taskSource);

        auto host             = TaskHost{};
        auto const generation = host.loadProject(temp.path());
        REQUIRE(generation.has_value());

        auto session = host.startExplorationSession(
            *generation,
            runConfig(temp.path() / "explore.jsonl")
        );
        REQUIRE(session.has_value());

        auto const idle = (*session)->heapUsage();
        REQUIRE(idle.ceilingBytes > 0);
        REQUIRE(idle.usedBytes > 0);

        // Six megabytes still REACHABLE when the chunk returns, so the
        // incremental collector cannot have taken them mid-run; the ceiling is
        // measured against garbage as well as live data, so the chunk boundary
        // is where the host has to reclaim. The index is concatenated on because
        // Luau interns EVERY string, long ones included: ninety-six copies of
        // one string.rep would be one object and measure nothing.
        auto const ran = (*session)->evaluate(
            "local t = {} for i = 1, 96 do"
            " t[i] = string.rep('x', 65536) .. tostring(i) end"
            " return #t",
            "garbage"
        );
        REQUIRE(ran.has_value());
        CHECK(ran->number() == 96.0);

        auto const after = (*session)->heapUsage();

        // The control, without which this case would pass against a chunk that
        // allocated nothing: the six megabytes were really there.
        CHECK(after.peakBytes > idle.usedBytes + (uint64{5} * 1024 * 1024));

        // And none of them are still charged to the ledger.
        CHECK(after.usedBytes < idle.usedBytes + (uint64{1} * 1024 * 1024));

        auto const report = (*session)->finish(std::nullopt);
        CHECK(report.outcome() == TaskRunOutcome::Completed);
    }

    TEST_CASE("a chunk that ran the ledger into the ceiling names both figures")
    {
        auto const temp = TemporaryDir{"exploration-ceiling"};
        publishProject(temp.path(), "daily", k_taskSource);

        auto host             = TaskHost{};
        auto const generation = host.loadProject(temp.path());
        REQUIRE(generation.has_value());

        auto session = host.startExplorationSession(
            *generation,
            runConfig(temp.path() / "explore.jsonl")
        );
        REQUIRE(session.has_value());

        auto const ceiling = (*session)->heapUsage().ceilingBytes;
        REQUIRE(ceiling > 0);

        // Unbounded LIVE growth, so the ceiling is reached with nothing to
        // reclaim and the allocator refuses. Each string is made distinct
        // because Luau interns them and a loop reallocating one object would
        // spin rather than fill.
        auto const failed = (*session)->evaluate(
            "local t = {} local n = 0 while true do n = n + 1"
            " t[n] = string.rep('x', 65536) .. tostring(n) end",
            "ceiling"
        );
        REQUIRE_FALSE(failed.has_value());

        auto const message = std::string{failed.error().message()};
        CAPTURE(message);

        // Luau's own sentence survives verbatim with the reading added behind
        // it: without the second half an agent reads "not enough memory" and
        // cannot tell an exhausted ceiling from a chunk that asked for
        // something absurd.
        CHECK(message.contains("not enough memory"));
        CHECK(message.contains("memory ledger"));
        CHECK(message.contains(std::to_string(ceiling)));

        // A chunk that failed for its own reasons keeps its message untouched:
        // the reading is added because the ledger was against the ceiling.
        auto const ordinary = (*session)->evaluate("error('deliberate')", "plain");
        REQUIRE_FALSE(ordinary.has_value());
        CHECK(!std::string{ordinary.error().message()}.contains("memory ledger"));

        auto const report = (*session)->finish(std::nullopt);
        CHECK(report.outcome() == TaskRunOutcome::Completed);
    }

    TEST_CASE("a framework refusal points at the script that got it wrong")
    {
        auto const temp = TemporaryDir{"error-level"};
        publishProject(temp.path(), "daily", k_taskSource);

        auto host             = TaskHost{};
        auto const generation = host.loadProject(temp.path());
        REQUIRE(generation.has_value());

        auto session = host.startExplorationSession(
            *generation,
            runConfig(temp.path() / "explore.jsonl")
        );
        REQUIRE(session.has_value());

        // Luau stamps the position the raise BLAMES, so the message says whose
        // mistake it was. A helper that raises at level 2 blames the public verb
        // it sits under -- a line inside the framework -- and the author reads a
        // traceback into code they cannot edit.
        auto const refused = (*session)->evaluate(
            "observe.resolve_page(nil, nil, nil)",
            "callersfault"
        );
        REQUIRE_FALSE(refused.has_value());

        auto const message = std::string{refused.error().message()};
        CAPTURE(message);
        CHECK(message.contains("callersfault"));
        CHECK_FALSE(message.contains("observe.luau"));
        CHECK_FALSE(message.contains("[string \"observe\"]"));
    }

    TEST_CASE("a chunk broken with no host call still ends the exploration session")
    {
        auto const temp = TemporaryDir{"exploration-latch"};
        publishProject(temp.path(), "daily", k_taskSource);

        auto host             = TaskHost{};
        auto const generation = host.loadProject(temp.path());
        REQUIRE(generation.has_value());

        auto session = host.startExplorationSession(
            *generation,
            runConfig(temp.path() / "explore.jsonl")
        );
        REQUIRE(session.has_value());
        REQUIRE_FALSE((*session)->terminalKind().has_value());

        REQUIRE(host.cancel(*generation).has_value());

        // A PURE SPIN, and that is the whole case: it reaches no host call, so
        // nothing writes the context latch the session used to be the only thing
        // the session asked. The interrupt breaks the thread and only
        // script::Engine records it. Delete the generationSpent() disjunct from
        // ExplorationSession::terminalKind and this goes red while every other
        // cancellation case stays green, because those all cancel THROUGH a verb.
        auto const broken = (*session)->evaluate("while true do end", "spin");
        REQUIRE_FALSE(broken.has_value());

        auto const terminal = (*session)->terminalKind();
        REQUIRE(terminal.has_value());
        CHECK(*terminal == AutomationErrorKind::Cancelled);

        // And the closing report says so. finish() destroys the VM, so it has to
        // ask before it does; asking after loses the only latch that was set.
        auto const report = (*session)->finish(std::nullopt);
        CHECK(report.outcome() == TaskRunOutcome::Cancelled);
    }

    TEST_CASE("TaskHost cancel spends the generation and queryTask reports it")
    {
        auto const temp = TemporaryDir{"cancel"};
        publishProject(temp.path(), "daily", k_taskSource);

        auto host = TaskHost{};
        auto const generation = host.loadProject(temp.path());
        REQUIRE(generation.has_value());

        auto const before = host.queryTask(*generation);
        REQUIRE(before.has_value());
        CHECK_FALSE(before->cancellationRequested);
        CHECK_FALSE(before->lastOutcome.has_value());
        CHECK(before->taskName.empty());

        REQUIRE(host.cancel(*generation).has_value());

        auto const after = host.queryTask(*generation);
        REQUIRE(after.has_value());
        CHECK(after->cancellationRequested);

        // The cancelled generation is spent: the engine refuses to observe, so
        // the run reports Cancelled rather than completing.
        auto const report = host.startTask(
            *generation,
            "daily",
            runConfig(temp.path() / "cancelled.jsonl")
        );
        REQUIRE(report.has_value());
        CHECK(report->outcome() == TaskRunOutcome::Cancelled);

        auto const status = host.queryTask(*generation);
        REQUIRE(status.has_value());
        CHECK(status->lastOutcome == TaskRunOutcome::Cancelled);
        CHECK(status->taskName == "daily");
    }

    TEST_CASE("TaskHost composes an external stop token with its own cancel source")
    {
        auto const temp = TemporaryDir{"external-stop"};
        publishProject(temp.path(), "daily", k_taskSource);

        auto stop = std::stop_source{};
        auto host = TaskHost{};
        auto const generation = host.loadProject(
            temp.path(),
            TaskHostConfig{.externalCancellation = stop.get_token()}
        );
        REQUIRE(generation.has_value());

        auto const before = host.queryTask(*generation);
        REQUIRE(before.has_value());
        CHECK_FALSE(before->cancellationRequested);

        stop.request_stop();

        auto const after = host.queryTask(*generation);
        REQUIRE(after.has_value());
        CHECK(after->cancellationRequested);
    }

    TEST_CASE("TaskHost reports a run that failed rather than failing the call")
    {
        auto const temp = TemporaryDir{"failed-run"};
        publishProject(temp.path(), "boom", "error('deliberate task failure')\n");

        auto host = TaskHost{};
        auto const generation = host.loadProject(temp.path());
        REQUIRE(generation.has_value());

        auto const tracePath = temp.path() / "failed.jsonl";
        auto const report = host.startTask(
            *generation,
            "boom",
            runConfig(tracePath)
        );

        // The run started, so it is describable: a report, not a Result failure.
        REQUIRE(report.has_value());
        CHECK(report->outcome() == TaskRunOutcome::Failed);
        REQUIRE(report->failure.has_value());
        CHECK(report->taskName == "boom");

        auto const lines = traceLines(tracePath);
        REQUIRE_FALSE(lines.empty());
        CHECK(lines.back().contains(R"("kind":"run.finished")"));
        CHECK(lines.back().contains(R"("runOutcome":"Failed")"));
    }

    TEST_CASE("TaskHost reports a run whose terminal verdict the script swallowed")
    {
        // The first capture fails Cancelled, latching the generation terminal,
        // and the task catches the raise and returns normally: without the latch
        // folded into the report this is indistinguishable from a task that ran
        // to completion. No stop token is armed anywhere -- the VM interrupt
        // never fires and the engine would happily capture again -- so the latch
        // is the only thing that can decide this run's outcome.
        auto const temp = TemporaryDir{"swallowed-terminal"};
        publishProject(
            temp.path(),
            "swallow",
            "local ok = pcall(function() return ctx:cycle_open() end)\n"
            "if ok then return 0 end\n"
            "return 1\n"
        );

        auto host = TaskHost{};
        auto const generation = host.loadProject(temp.path());
        REQUIRE(generation.has_value());

        auto config = runConfig(temp.path() / "swallowed.jsonl");
        config.frameSource =
            std::make_unique<CancelOnceFrameSource>(sourceFrame(FrameId{92}));

        auto const report = host.startTask(*generation, "swallow", std::move(config));
        REQUIRE(report.has_value());
        CHECK(report->outcome() == TaskRunOutcome::Cancelled);
        REQUIRE(report->failure.has_value());
        CHECK(
            automationErrorKind(*report->failure) == AutomationErrorKind::Cancelled
        );

        auto const lines = traceLines(temp.path() / "swallowed.jsonl");
        REQUIRE_FALSE(lines.empty());
        CHECK(lines.back().contains(R"("runOutcome":"Cancelled")"));
    }

    TEST_CASE("TaskHost rejects a missing task before opening a trace file")
    {
        auto const temp = TemporaryDir{"missing-task"};
        publishProject(temp.path(), "daily", k_taskSource);

        auto host = TaskHost{};
        auto const generation = host.loadProject(temp.path());
        REQUIRE(generation.has_value());

        auto const tracePath = temp.path() / "never-opened.jsonl";
        auto const report = host.startTask(
            *generation,
            "nightly",
            runConfig(tracePath)
        );
        REQUIRE_FALSE(report.has_value());
        CHECK(
            automationErrorKind(report.error())
            == AutomationErrorKind::InvalidResource
        );
        CHECK_FALSE(std::filesystem::exists(tracePath));
    }

    TEST_CASE("TaskHost draws a fresh seed for every run and stamps it into the trace")
    {
        auto const temp = TemporaryDir{"seed"};
        publishProject(temp.path(), "daily", k_taskSource);

        auto host = TaskHost{};
        auto const generation = host.loadProject(temp.path());
        REQUIRE(generation.has_value());

        auto const firstPath = temp.path() / "first.jsonl";
        auto const first = host.startTask(
            *generation,
            "daily",
            runConfig(firstPath)
        );
        REQUIRE(first.has_value());
        REQUIRE(first->outcome() == TaskRunOutcome::Completed);

        auto const second = host.startTask(
            *generation,
            "daily",
            runConfig(temp.path() / "second.jsonl")
        );
        REQUIRE(second.has_value());
        REQUIRE(second->outcome() == TaskRunOutcome::Completed);

        // Two runs of one task must not share a seed: a constant seed looks
        // perfectly correct in a trace while making every run replay the same
        // draw sequence.
        CHECK(first->seed != second->seed);

        // The reported seed is the one the trace recorded, so a replay reads the
        // same number the caller was handed.
        CHECK(
            traceLines(firstPath).front().contains(
                std::format(R"("seed":{})", first->seed)
            )
        );
    }

    TEST_CASE("a TaskHost run writes one ordered umbraflow-trace/v4 run bracket")
    {
        // One call drives the host events, the engine's recognition and delivery
        // events and the script layer's native calls into a single file, in
        // order, under one sequence and one run and generation identity, inside
        // the run bracket only the host can write.
        auto const temp = TemporaryDir{"bracket"};
        publishProject(temp.path(), "daily", k_taskSource);

        auto host = TaskHost{};
        auto const generation = host.loadProject(temp.path());
        REQUIRE(generation.has_value());

        auto const tracePath = temp.path() / "run.jsonl";
        auto const report = host.startTask(
            *generation,
            "daily",
            runConfig(tracePath)
        );
        REQUIRE(report.has_value());
        CHECK(report->outcome() == TaskRunOutcome::Completed);
        CHECK_FALSE(report->failure.has_value());
        CHECK(report->taskName == "daily");
        CHECK(report->tracePath == tracePath);

        auto const loaded = loadTask(temp.path(), "daily");
        REQUIRE(loaded.has_value());
        CHECK(report->sourceHash == loaded->hash.hex());

        auto const lines = traceLines(tracePath);

        auto const expectedKinds = std::vector<std::string_view>{
            "run.started",
            "run.resources_validated",
            // project_read and template_load reach no engine verb, so each is a
            // task.native_call standing on its own before the first capture.
            "task.native_call",
            "task.native_call",
            "engine.observed",
            "task.native_call",
            "engine.action_found",
            "task.native_call",
            "engine.action_authorized",
            "engine.action_delivered",
            "engine.observation_invalidated",
            "task.native_call",
            "run.finished",
        };
        REQUIRE(lines.size() == expectedKinds.size());
        for (auto index = std::size_t{0}; index < expectedKinds.size(); ++index)
        {
            CHECK(lines[index].starts_with(stampedPrefix(expectedKinds[index], index + 1U)));
        }

        // The three host-authoritative lines are pinned whole. The framework
        // version, bundle hash, Luau version and per-run seed are interpolated
        // because they change legitimately; their presence and position is what
        // this pins.
        CHECK(
            lines.front()
            == std::format(
                R"({{"schema":"umbraflow-trace/v4","kind":"run.started")"
                R"(,"seq":1,"runId":1,"generationId":1,"frontEnd":"task")"
                R"(,"projectId":"{}","taskName":"daily")"
                R"(,"sourceHash":"{}","frameworkVersion":"{}")"
                R"(,"frameworkHash":"{}","luauVersion":"{}","seed":{}}})",
                // The project's own directory name is the whole of what
                // identifies a project on the wire; no manifest carries an id
                // (docs/plans/2026-07-31-script-owned-page-model.md 9).
                temp.path().filename().string(),
                report->sourceHash,
                frameworkVersion(),
                frameworkBundleHash(),
                luauRuntimeVersion(),
                report->seed
            )
        );
        CHECK(
            lines[1]
            == R"({"schema":"umbraflow-trace/v4","kind":"run.resources_validated")"
               R"(,"seq":2,"runId":1,"generationId":1,"frontEnd":"task")"
               R"(,"elements":["daily_button"],"pages":["home"]})"
        );
        CHECK(
            lines.back()
            == R"({"schema":"umbraflow-trace/v4","kind":"run.finished")"
               R"(,"seq":13,"runId":1,"generationId":1,"frontEnd":"task","runOutcome":"Completed"})"
        );

        // The observation and the delivery name the same frame, which lets a
        // reader attribute the click to the evidence it was authorized against.
        CHECK(lines[4].contains(R"("frameId":91)"));
        CHECK(lines[9].contains(R"("frameId":91)"));
    }
}
