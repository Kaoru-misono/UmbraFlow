// The production read-only path, composed end to end over a recorded target.
//
// Every part below this verb is already covered somewhere else: tests/deployment
// covers the directory load, tests/operator covers the installed-generation CAS,
// and the conformance run covers the resolver. What is only covered here is that
// one entry runs them in order and stops -- so the two cases that matter are the
// ones that go red when it acts on the target, and when it answers a screen it
// could not read as a screen with nothing on it.
//
// The recorded seam is modules/conformance's own: an ObservationFrameSource over
// a decoded capture, declaring TargetWorld::Recorded, which is what
// EngineSession::create matches the sink against.

#include <cli/args.hpp>
#include <cli/cli-result.hpp>
#include <cli/observe.hpp>

#include <conformance/observation-fixture.hpp>

#include <operator/ledger.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/ids.hpp>
#include <domain/key.hpp>
#include <domain/space.hpp>

#include <engine/ports.hpp>

#include <ocr/engine.hpp>

#include "../json/repository-path.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::cli
{
    namespace
    {
        // The geometry this recorded world presents. It is the extent and DPI
        // examples/umbraflow/runtime/artifact/runtime-model.toml declares, which
        // is what a live target would have to present for the engine to accept
        // the capture at all; the conformance run is what holds the model and
        // the capture beside it to each other.
        constexpr auto k_recordedWidth  = uint32{3};
        constexpr auto k_recordedHeight = uint32{1};
        constexpr auto k_recordedDpi    = uint32{96};

        [[nodiscard]]
        auto why(Result<ObservedState> const& outcome) -> std::string
        {
            return outcome.has_value()
                ? std::string{"<the observation succeeded>"}
                : formatError(outcome.error());
        }

        // Counts every verb the engine was asked to post, through a counter the
        // test owns rather than one the sink holds.
        //
        // EngineSession takes ownership of the sink and destroys it inside the
        // call under test, so conformance::ObservationActionSink -- which
        // answers clicks() off itself -- could only be read through a pointer
        // that is dangling exactly where the assertion needs it. The recorded
        // world is still the fixture's: this differs from it only in where the
        // count lives, and it counts all six verbs rather than clicks alone,
        // because "delivered nothing" is a claim about the whole sink.
        class RecordedActionSink final : public engine::IActionSink
        {
            std::shared_ptr<uint32> m_delivered;

        public:
            explicit RecordedActionSink(std::shared_ptr<uint32> delivered) noexcept
                : m_delivered{std::move(delivered)}
            {
            }

            [[nodiscard]]
            auto click(Point<ClientSpace>, ObservationLease const&) -> Status override
            {
                ++*m_delivered;
                return ok();
            }

            [[nodiscard]] auto pressKey(KeyName, TargetGeneration) -> Status override
            {
                ++*m_delivered;
                return ok();
            }

            [[nodiscard]] auto scroll(int32, ObservationLease const&) -> Status override
            {
                ++*m_delivered;
                return ok();
            }

            [[nodiscard]]
            auto longPress(
                Point<ClientSpace>,
                MonotonicInstant::Duration,
                ObservationLease const&
            ) -> Status override
            {
                ++*m_delivered;
                return ok();
            }

            [[nodiscard]]
            auto drag(
                Point<ClientSpace>,
                Point<ClientSpace>,
                MonotonicInstant::Duration,
                ObservationLease const&
            ) -> Status override
            {
                ++*m_delivered;
                return ok();
            }

            [[nodiscard]]
            auto movePointer(
                Point<ClientSpace>,
                ObservationLease const&
            ) -> Status override
            {
                ++*m_delivered;
                return ok();
            }

            [[nodiscard]] auto targetWorld() const noexcept -> TargetWorld override
            {
                return TargetWorld::Recorded;
            }
        };

        // An engine that is present and answers nothing. It stands for a bound
        // OCR adapter and not for a missing one: the case that removes the
        // adapter passes a null pointer, which is the state
        // platform::bindOcrEngine produces from an absent directory.
        class PresentReader final : public ocr::IOcrEngine
        {
        public:
            [[nodiscard]] auto identity() const noexcept -> std::string_view override
            {
                return "test-present-reader";
            }

            [[nodiscard]]
            auto read(
                BgraImage const&,
                ocr::ReadSpec const&
            ) -> Result<ocr::Readout> override
            {
                return ocr::Readout{};
            }
        };

        // The exemplar copied out of the repository, its RuntimeArtifact
        // installed into a production Operator root beside it, and the capture
        // the project published.
        //
        // Installing here rather than in the verb is the shape under test: a
        // release reaches the CAS through a deployment act, and what the verb
        // does is open what that act left behind.
        class RecordedWorld final
        {
            std::filesystem::path  m_root{};
            std::filesystem::path  m_project{};
            std::filesystem::path  m_runtime{};
            std::vector<std::byte> m_probe{};

        public:
            RecordedWorld()
            {
                m_root = (
                    std::filesystem::temp_directory_path()
                    / std::filesystem::path{
                        "uf-observe-" + std::to_string(std::random_device{}()),
                    }
                );
                m_project = m_root / "project";
                m_runtime = m_root / "production";
                std::filesystem::remove_all(m_root);
                std::filesystem::create_directories(m_root);
                std::filesystem::copy(
                    std::filesystem::path{UF_STAGED_UMBRAFLOW_PROJECT},
                    m_project,
                    std::filesystem::copy_options::recursive
                );

                auto stream = std::ifstream{
                    m_project / "runtime" / "probe-frame.png",
                    std::ios::binary,
                };
                REQUIRE(stream.good());
                auto const text = std::string{
                    std::istreambuf_iterator<char>{stream},
                    std::istreambuf_iterator<char>{},
                };
                m_probe.reserve(text.size());
                for (auto const value : text)
                {
                    m_probe.emplace_back(
                        static_cast<std::byte>(static_cast<unsigned char>(value))
                    );
                }

                auto const release = operator_runtime::conformance::observationRelease(
                    m_root / "handoff",
                    m_project / "runtime" / "artifact"
                );

                // Scoped, so the SQLite handle this test opened is closed before
                // the verb opens the same root.
                auto store = operator_runtime::OperatorCoordinator::open(m_runtime);
                REQUIRE(store.has_value());
                auto const installed = store->installRuntimeArtifact(
                    operator_runtime::RuntimeArtifactInstallRequest{
                        .handoffRoot                 = release.handoffRoot,
                        .expectedReleaseManifestHash = release.releaseManifestHash,
                        .expectedInstalledGeneration = 0U,
                    }
                );
                auto const installedWhy = installed.has_value()
                    ? std::string{}
                    : installed.error().message();
                REQUIRE_MESSAGE(
                    installed.has_value(),
                    installedWhy
                );
            }

            RecordedWorld(RecordedWorld const&)                    = delete;
            RecordedWorld(RecordedWorld&&)                         = delete;
            auto operator=(RecordedWorld const&) -> RecordedWorld& = delete;
            auto operator=(RecordedWorld&&) -> RecordedWorld&      = delete;

            ~RecordedWorld()
            {
                auto discarded = std::error_code{};
                std::filesystem::remove_all(m_root, discarded);
            }

            // Every byte the Operator holds. The coordinator this fixture
            // installed through was destroyed inside the constructor, so WAL
            // frames are already checkpointed into this file and any write a
            // later call commits and closes lands here too.
            [[nodiscard]] auto ledgerBytes() const -> std::string
            {
                auto stream = std::ifstream{
                    m_runtime / "operator-runtime.sqlite",
                    std::ios::binary,
                };
                REQUIRE(stream.good());
                return std::string{
                    std::istreambuf_iterator<char>{stream},
                    std::istreambuf_iterator<char>{},
                };
            }

            // One trace path per call: FileTraceSink refuses a file that already
            // carries evidence, so two observations in one case need two.
            [[nodiscard]] auto args(std::string_view trace) const -> ObserveArgs
            {
                return ObserveArgs{
                    .project      = m_project,
                    .windowHandle = 0,
                    .runtime      = m_runtime,
                    .ocrModels    = m_project,
                    .trace        = m_root / trace,
                };
            }

            [[nodiscard]]
            auto sources(
                std::shared_ptr<uint32> delivered,
                std::unique_ptr<ocr::IOcrEngine> ocrEngine
            ) const -> ObserveSources
            {
                auto const fingerprint = ProjectFingerprint::create(
                    k_recordedWidth,
                    k_recordedHeight,
                    k_recordedDpi,
                    k_recordedDpi
                );
                REQUIRE(fingerprint.has_value());
                return ObserveSources{
                    .frameSource = std::make_unique<
                        operator_runtime::conformance::ObservationFrameSource
                    >(
                        operator_runtime::conformance::observationFrame(
                            m_probe,
                            FrameId{4001}
                        )
                    ),
                    .actionSink      = std::make_unique<RecordedActionSink>(
                        std::move(delivered)
                    ),
                    .ocrEngine       = std::move(ocrEngine),
                    .liveFingerprint = *fingerprint,
                };
            }
        };
    }

    TEST_CASE(
        "bare production project reaches first observe without an external internal identifier"
    )
    {
        auto const world     = RecordedWorld{};
        auto const delivered = std::make_shared<uint32>();

        auto const observed = observeProject(
            world.args("resolved.jsonl"),
            world.sources(delivered, std::make_unique<PresentReader>())
        );
        REQUIRE_MESSAGE(
            observed.has_value(),
            "a bare production project must reach first observe without an "
            "external internal identifier: ",
            why(observed)
        );

        // The composition reached every stage: the directory registered a
        // plugin, the Operator answered for the artifact this project names,
        // and the Host resolved a state over the capture.
        CHECK(observed->pluginId == "fixture.alpha");
        CHECK(observed->installedGeneration > 0U);
        CHECK(observed->modelWidth == k_recordedWidth);
        CHECK(observed->liveWidth == k_recordedWidth);
        CHECK_FALSE(observed->artifactRootHash.empty());
        CHECK_MESSAGE(
            observed->stateResolution.contains(R"("kind":"resolved_state")"),
            observed->stateResolution
        );

        // What the verb prints has to let a reader see WHY: the ordered stack
        // is in the document, and so is one entry per Reader that reported.
        CHECK(observed->stateResolution.contains(R"("ordered_surface_stack")"));
        auto const report = formatObservedState(*observed);
        CHECK(report.contains(observed->stateResolution));
        CHECK(report.contains("model declares"));

        // The whole of the phase limit, in one number. A verb that planned,
        // minted a Receipt or dispatched would post here.
        CHECK(*delivered == 0U);
    }

    TEST_CASE("observe restarts through Coordinator and remains repeatable")
    {
        auto const world     = RecordedWorld{};
        auto const delivered = std::make_shared<uint32>();
        auto const first = observeProject(
            world.args("unchanged-first.jsonl"),
            world.sources(delivered, std::make_unique<PresentReader>())
        );
        REQUIRE_MESSAGE(first.has_value(), why(first));
        auto const afterFirst = world.ledgerBytes();
        CHECK_FALSE(afterFirst.empty());

        // Twice, because "read only" and "idempotent" are different claims and
        // a verb that wrote once on a first open would satisfy only the second.
        auto const second = observeProject(
            world.args("unchanged-second.jsonl"),
            world.sources(delivered, std::make_unique<PresentReader>())
        );
        REQUIRE_MESSAGE(second.has_value(), why(second));
        CHECK_FALSE(world.ledgerBytes().empty());
        CHECK(*delivered == 0U);
    }

    TEST_CASE("observe refuses a missing OCR engine rather than reading nothing")
    {
        auto const world     = RecordedWorld{};
        auto const delivered = std::make_shared<uint32>();

        auto const observed = observeProject(
            world.args("no-ocr.jsonl"),
            world.sources(delivered, nullptr)
        );
        REQUIRE_FALSE(observed.has_value());
        CHECK(
            automationErrorKind(observed.error())
            == AutomationErrorKind::UnsupportedCapability
        );
        CHECK_MESSAGE(
            why(observed).contains("--ocr-models"),
            why(observed)
        );
    }

    TEST_CASE("observe requires the OCR model directory on the command line")
    {
        auto const raw = std::vector<std::string>{
            "--project",
            "project",
            "--hwnd",
            "0x1",
            "--runtime",
            "production",
        };
        auto const parsed = parseObserveArguments(raw);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(formatError(parsed.error()).contains("--ocr-models"));
    }
}
