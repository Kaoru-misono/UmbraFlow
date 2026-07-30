#include "binding-fixture.hpp"

#include "../annotation/test-helpers.hpp"

#include <task/framework-bundle.hpp>
#include <task/operator-session.hpp>
#include <task/task-host.hpp>
#include <task/task-loader.hpp>

#include <annotation/authoring-compiler.hpp>
#include <annotation/authoring-document.hpp>
#include <annotation/content-hash.hpp>
#include <annotation/recognition.hpp>
#include <annotation/resource.hpp>

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
    // The event stream a resident host would subscribe to. task-host.hpp declares
    // this type and deliberately never defines it, because what a task event is
    // remains a P2 question. This is the program's single definition, and it
    // exists for one reason: subscribeEvents takes a reference to it, so proving
    // the verb reports UnsupportedCapability needs something to reference. It
    // carries no members precisely because inventing a payload here would be the
    // speculative design the incomplete declaration exists to avoid.
    class ITaskEventSink final
    {
    };

    namespace
    {
        namespace anno = annotation;

        constexpr auto k_sourceId = "00000000-0000-0000-0000-000000000301";
        constexpr auto k_anchorId = "00000000-0000-0000-0000-000000000021";
        constexpr auto k_actionId = "00000000-0000-0000-0000-000000000022";
        constexpr auto k_pageId   = "00000000-0000-0000-0000-000000000121";

        constexpr auto k_projectId = std::string_view{"personal.task_host"};

        // The task every run in this file drives: one observation cycle, one page
        // check, one action search, one click. It names both a page and a
        // recognizer, so the run.resources_validated line carries a non-empty
        // closure of each.
        constexpr auto k_taskSource = std::string_view{
            "local cycle = ctx:cycle_open()\n"
            "local page = ctx:cycle_page(cycle)\n"
            "if page == nil then ctx:cycle_close(cycle) return 0 end\n"
            "if not page:is(uf.pages.home) then ctx:cycle_close(cycle) return 0 end\n"
            "local hit = ctx:cycle_find(cycle, uf.recognizers.daily_button)\n"
            "if hit == nil then ctx:cycle_close(cycle) return 0 end\n"
            "ctx:cycle_click(cycle, hit)\n"
            "return 1\n"
        };

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
        // from. Each pixel carries a distinct value, so a frame painted with the
        // same pixels reproduces every crop exactly at its own position and
        // nowhere else: the page anchor matches at (0, 0) and the action target
        // at (1, 0), both with a zero sum of absolute differences.
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
        auto fixtureFingerprint() -> anno::ProjectFingerprint
        {
            return anno::test::fingerprint(3, 2, 96, 96);
        }

        // Publishes a project at `root` holding one page `home` (required anchor
        // `home_marker`) and one action target `daily_button` authorized on it,
        // then writes `tasks/<name>.luau`. This is the on-disk shape TaskHost
        // addresses: it never executes a loose-path script.
        auto publishProject(
            std::filesystem::path const& root,
            std::string_view taskName,
            std::string_view taskSource
        ) -> void
        {
            auto const fingerprint = fixtureFingerprint();
            auto const sourceId    = anno::test::sourceId(k_sourceId);
            auto pngBytes = sourcePixelsRgba();
            auto encoded  = image::encodeRgbaPng("task-host-source.png", 3, 2, pngBytes);
            REQUIRE(encoded.has_value());
            auto const sourceHash = anno::sha256(*encoded);
            REQUIRE(sourceHash.has_value());

            auto source = anno::AuthoringSource::create(
                anno::AuthoringSourceSpec{
                    .id          = sourceId,
                    .contentHash = *sourceHash,
                    .fingerprint = fingerprint,
                    .provenance  = anno::ImportedSourceProvenance{},
                }
            );
            REQUIRE(source.has_value());

            auto const anchorId = anno::test::elementId(k_anchorId);
            auto const actionId = anno::test::elementId(k_actionId);
            auto const pageId   = anno::test::pageId(k_pageId);
            auto const click    = anno::TemplateOffset::create(1, 1, 2, 2);
            REQUIRE(click.has_value());

            auto document = anno::AuthoringDocument::create(
                anno::test::projectId(std::string{k_projectId}),
                fingerprint,
                {*source},
                {
                    anno::test::anchorElement(
                        fingerprint,
                        anchorId,
                        "home_marker",
                        sourceId,
                        anno::test::pixelRect(0, 0, 1, 1),
                        anno::test::pixelRect(0, 0, 3, 2)
                    ),
                    anno::test::interactiveElement(
                        fingerprint,
                        actionId,
                        "daily_button",
                        sourceId,
                        anno::test::pixelRect(1, 0, 2, 2),
                        anno::test::pixelRect(0, 0, 3, 2),
                        *click
                    ),
                },
                {anno::test::page(pageId, "home", {anchorId})},
                {
                    anno::test::placement(
                        pageId,
                        actionId,
                        anno::test::pixelRect(0, 0, 3, 2)
                    ),
                },
                {}
            );
            REQUIRE(document.has_value());

            auto const asset = anno::AuthoringSourceAsset{
                .id       = sourceId,
                .pngBytes = *std::move(encoded),
            };
            auto const assets   = std::span{&asset, std::size_t{1}};
            auto const compiled = anno::compileAuthoringDocument(*document, assets);
            REQUIRE(compiled.has_value());

            writeText(
                root / "generated" / "annotations.runtime.toml",
                compiled->runtimeManifestToml
            );
            for (auto const& templateAsset : compiled->templateAssets)
            {
                writeFile(root / templateAsset.relativePath, templateAsset.pngBytes);
            }
            writeText(
                root / "tasks" / (std::string{taskName} + ".luau"),
                taskSource
            );
        }

        // A frame reproducing the authoring source pixel for pixel, in BGRA and
        // stamped with the current monotonic instant so the action's frame-age
        // check passes.
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

        // Every line the run wrote, with the documented non-golden `meta` member
        // stripped. Reading the file back rather than a recording sink is
        // deliberate: the host owns the sink, so the file is the only place the
        // stream is observable, and it is what an operator actually reads.
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

        // The stamped prefix every line of a run carries: schema, kind, and the
        // identity triple. Comparing prefixes pins the event order, the monotonic
        // sequence, and the single run/generation identity in one assertion.
        [[nodiscard]]
        auto stampedPrefix(std::string_view kind, std::size_t sequence) -> std::string
        {
            return std::format(
                R"({{"schema":"umbraflow-trace/v1","kind":"{}")"
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
        // existing error kind rather than by unimplemented machinery, and this
        // pins that they say so rather than doing something partial.
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

        auto const unknown = GenerationId{99};
        CHECK_FALSE(host.cancel(unknown).has_value());
        CHECK_FALSE(host.queryTask(unknown).has_value());
        CHECK_FALSE(
            host.startTask(
                unknown,
                "daily",
                runConfig(temp.path() / "unreachable.jsonl")
            )
            .has_value()
        );
        // A rejected run opens no trace file, so nothing was written for it.
        CHECK_FALSE(std::filesystem::exists(temp.path() / "unreachable.jsonl"));
    }

    TEST_CASE("a generation drives one front-end, whichever arrives first")
    {
        // The mutual exclusion, made structural. A generation holds the single-open-
        // cycle ledger, so two policy sources driving one generation would contend for
        // it. Both orders are checked, because "the first one wins" is only a rule if
        // it holds symmetrically -- and both are checked on the SAME host, so nothing
        // about process-level dispatch is doing the work.
        SUBCASE("a task run refuses an operator session afterwards")
        {
            auto const temp = TemporaryDir{"exclusion-task-first"};
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

            auto refused = host.startOperatorSession(
                *generation,
                runConfig(temp.path() / "operator.jsonl")
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(
                automationErrorKind(refused.error())
                == AutomationErrorKind::UnsupportedCapability
            );
            // A refused claim opens no trace file, so the operator session left no
            // evidence of a run that never happened.
            CHECK_FALSE(std::filesystem::exists(temp.path() / "operator.jsonl"));
        }

        SUBCASE("an operator session refuses a task run afterwards")
        {
            auto const temp = TemporaryDir{"exclusion-operator-first"};
            publishProject(temp.path(), "daily", k_taskSource);

            auto host             = TaskHost{};
            auto const generation = host.loadProject(temp.path());
            REQUIRE(generation.has_value());

            auto session = host.startOperatorSession(
                *generation,
                runConfig(temp.path() / "operator.jsonl")
            );
            REQUIRE(session.has_value());

            auto const refused = host.startTask(
                *generation,
                "daily",
                runConfig(temp.path() / "task.jsonl")
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(
                automationErrorKind(refused.error())
                == AutomationErrorKind::UnsupportedCapability
            );
            CHECK_FALSE(std::filesystem::exists(temp.path() / "task.jsonl"));
        }

        SUBCASE("the same front-end twice is allowed")
        {
            // A generation legitimately runs several tasks in sequence. What it must
            // never do is mix the two front-ends, so this half of the rule has to hold
            // or the exclusion would be a ban on reuse rather than on contention.
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

    TEST_CASE("an operator session's run bracket names the operator front-end")
    {
        auto const temp = TemporaryDir{"operator-bracket"};
        publishProject(temp.path(), "daily", k_taskSource);

        auto host             = TaskHost{};
        auto const generation = host.loadProject(temp.path());
        REQUIRE(generation.has_value());

        auto const tracePath = temp.path() / "operator.jsonl";
        auto session = host.startOperatorSession(*generation, runConfig(tracePath));
        REQUIRE(session.has_value());

        auto cycle = (*session)->cycleOpen();
        REQUIRE(cycle.has_value());
        auto const page = (*session)->cyclePage(*cycle);
        REQUIRE(page.has_value());
        REQUIRE(page->has_value());
        CHECK(**page == std::string{"home"});

        auto const report = (*session)->finish(std::nullopt);
        CHECK(report.outcome() == TaskRunOutcome::Completed);
        // An operator session names no task, so the report carries no task name: the
        // frontEnd member on every line is what explains the absence.
        CHECK(report.taskName.empty());

        auto const lines = traceLines(tracePath);
        REQUIRE_FALSE(lines.empty());
        for (auto const& line : lines)
        {
            CAPTURE(line);
            CHECK(line.contains("\"frontEnd\":\"operator\""));
        }
        CHECK(lines.front().contains("\"kind\":\"run.started\""));
        CHECK(lines.back().contains("\"kind\":\"run.finished\""));
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
        // Design section 9's rule 5, read back at the host boundary. The first
        // capture fails Cancelled, which latches the generation terminal; the
        // task catches the raise and returns normally. Nothing else marks the
        // run, so without the latch being folded into the report this would be
        // indistinguishable from a task that ran to completion.
        //
        // No stop token is armed anywhere: the VM interrupt never fires and the
        // engine would happily capture again, so the latch is the only thing
        // that can decide this run's outcome.
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

        // Two runs of one task must not share a seed. A constant seed would look
        // perfectly correct in a trace while making every run replay as the same
        // draw sequence, which is exactly the placeholder this replaced.
        CHECK(first->seed != second->seed);

        // The reported seed is the one the trace recorded, so a replay reads the
        // same number the caller was handed.
        CHECK(
            traceLines(firstPath).front().contains(
                std::format(R"("seed":{})", first->seed)
            )
        );
    }

    TEST_CASE("a TaskHost run writes one ordered umbraflow-trace/v1 run bracket")
    {
        // The acceptance criterion for the whole slice: one call drives the host
        // events, the engine's recognition and delivery events, and the script
        // layer's native calls into a single file, in order, under one sequence
        // and one run and generation identity -- and the run bracket that only
        // the host can write opens and closes it.
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
            "engine.observed",
            "task.native_call",
            "engine.page_resolved",
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
        // version, bundle hash and Luau compiler version are interpolated
        // because they legitimately change when the framework or the vendored
        // compiler is rebuilt, while their presence and position is exactly what
        // this pins; the seed is interpolated because it is drawn per run.
        CHECK(
            lines.front()
            == std::format(
                R"({{"schema":"umbraflow-trace/v1","kind":"run.started")"
                R"(,"seq":1,"runId":1,"generationId":1,"frontEnd":"task")"
                R"(,"projectId":"{}","taskName":"daily")"
                R"(,"sourceHash":"{}","frameworkVersion":"{}")"
                R"(,"frameworkHash":"{}","luauVersion":"{}","seed":{}}})",
                k_projectId,
                report->sourceHash,
                frameworkVersion(),
                frameworkBundleHash(),
                luauRuntimeVersion(),
                report->seed
            )
        );
        CHECK(
            lines[1]
            == R"({"schema":"umbraflow-trace/v1","kind":"run.resources_validated")"
               R"(,"seq":2,"runId":1,"generationId":1,"frontEnd":"task")"
               R"(,"recognizers":["daily_button"],"pages":["home"]})"
        );
        CHECK(
            lines.back()
            == R"({"schema":"umbraflow-trace/v1","kind":"run.finished")"
               R"(,"seq":13,"runId":1,"generationId":1,"frontEnd":"task","runOutcome":"Completed"})"
        );

        // The engine event that opened the observation and the one that
        // delivered the click name the same frame, which is what lets a reader
        // attribute the click to the evidence it was authorized against.
        CHECK(lines[2].contains(R"("frameId":91)"));
        CHECK(lines[9].contains(R"("frameId":91)"));
    }
}
