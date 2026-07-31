#include <annotation.hpp>
#include <drive.hpp>
#include <protocol.hpp>
#include <path-validation.hpp>

#include <controller/discovery.hpp>
#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>
#include <core/utility/scope-exit.hpp>
#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// The seam between the two halves of the agent. Everything below runs the
// annotation half against a scripted drive, which is what the split bought: the
// path confinement, the before/after framing and the results-line shape used to
// be reachable only through a resolved window, a DPI context and a live Windows
// Graphics Capture session.
namespace uf::input_agent
{
    namespace
    {
        constexpr auto k_client = ClientSize{800, 450};

        [[nodiscard]]
        auto scriptedFrame(uint64 id) -> Frame
        {
            auto const transform = CoordinateTransform::create(
                Point<DesktopSpace>{0.0F, 0.0F},
                1.0F,
                1.0F,
                1,
                1
            );
            REQUIRE(transform.has_value());
            auto const pixels = std::make_shared<FrameBuffer const>(
                std::vector<std::byte>(4)
            );
            auto frame = Frame::create(
                FrameId{id},
                CaptureSessionId{1},
                TargetGeneration{},
                MonotonicInstant::now(),
                1,
                1,
                4,
                PixelFormat::Bgra8,
                pixels,
                *transform
            );
            REQUIRE(frame.has_value());
            return *std::move(frame);
        }

        auto removeAllBestEffort(
            std::filesystem::path const& path
        ) noexcept -> void
        {
            try
            {
                auto error = std::error_code{};
                static_cast<void>(std::filesystem::remove_all(path, error));
            }
            catch (...)
            {
            }
        }

        // What one session works against: the canonical output directory and the
        // two IPC paths no output may alias. The IPC files sit outside the output
        // directory, exactly as runInputAgent requires of a real run.
        struct SessionFiles final
        {
            std::filesystem::path root{};
            std::filesystem::path outputDirectory{};
            std::filesystem::path queue{};
            std::filesystem::path results{};
        };

        [[nodiscard]]
        auto createSessionFiles(std::string_view role) -> SessionFiles
        {
            auto const token = std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();
            auto const root = std::filesystem::temp_directory_path()
                / std::format("umbraflow-{}-{}", role, token);
            auto error = std::error_code{};
            REQUIRE(std::filesystem::create_directory(root, error));
            REQUIRE_FALSE(error);
            REQUIRE(std::filesystem::create_directory(root / "out", error));
            REQUIRE_FALSE(error);

            auto files = SessionFiles{
                .root            = root,
                .outputDirectory = root / "out",
                .queue           = root / "queue.jsonl",
                .results         = root / "results.jsonl",
            };
            auto queued = canonicalizePathForComparison(
                files.queue,
                "queue"
            );
            // Both IPC paths are canonicalized the way runInputAgent does, so
            // aliasing is compared on the same footing a real run compares it.
            REQUIRE(queued.has_value());
            files.queue  = *queued;
            auto results = canonicalizePathForComparison(files.results, "results");
            REQUIRE(results.has_value());
            files.results  = *results;
            auto directory = canonicalizeOutputDirectory(files.outputDirectory);
            REQUIRE(directory.has_value());
            files.outputDirectory = *directory;
            return files;
        }

        // How the scripted drive below answers a delivery. The answer is
        // DESCRIBED rather than stored as a DriveOutcome, because an Error is
        // move-only: a stored outcome could be handed over once, and a drive
        // asked twice has to produce a second answer of its own.
        struct ScriptedAnswer final
        {
            std::optional<AutomationErrorKind> refusal{};
            std::string                        message{};
            bool                               targetReplaced{};
        };

        // A drive that answers from a script instead of from a window. It
        // records what the annotation layer asked of it and, at the moment of
        // delivery, how large the before-frame file was -- which is how the
        // observe->act window is asserted without a real capture session.
        class ScriptedDrive final : public IInputAgentDrive
        {
            std::filesystem::path m_beforePath;
            ScriptedAnswer        m_answer;

            std::vector<std::string> m_asked{};
            std::size_t              m_captures{};
            std::size_t              m_auditClears{};
            std::size_t              m_closes{};

            std::optional<uintmax> m_beforeBytesAtDelivery{};

            [[nodiscard]] auto answer(std::string verb) -> DriveOutcome
            {
                m_asked.emplace_back(std::move(verb));
                auto error = std::error_code{};
                auto const size = std::filesystem::file_size(m_beforePath, error);
                m_beforeBytesAtDelivery = error ? uintmax{} : size;
                if (!m_answer.refusal.has_value())
                {
                    return DriveOutcome{};
                }
                auto refused = fail(*m_answer.refusal, m_answer.message);
                return DriveOutcome{
                    .error          = std::move(refused).error(),
                    .targetReplaced = m_answer.targetReplaced,
                };
            }

        public:
            // beforePath is the file the next framed action reserves for its
            // before-frame; it is stated rather than discovered because a drive
            // never learns an output path in production and must not start.
            ScriptedDrive(
                std::filesystem::path beforePath,
                ScriptedAnswer answer
            )
                : m_beforePath{std::move(beforePath)}
                , m_answer{std::move(answer)}
            {
            }

            [[nodiscard]] auto clientSize() const noexcept -> ClientSize override
            {
                return k_client;
            }

            [[nodiscard]] auto capture() -> Result<Frame> override
            {
                ++m_captures;
                return scriptedFrame(m_captures);
            }

            [[nodiscard]]
            auto click(Frame const&, Point<ClientSpace>) -> DriveOutcome override
            {
                return answer("click");
            }

            [[nodiscard]]
            auto scroll(
                Frame const&,
                Point<ClientSpace>,
                WheelDelta
            ) -> DriveOutcome override
            {
                return answer("scroll");
            }

            [[nodiscard]]
            auto key(Frame const&, KeyInput) -> DriveOutcome override
            {
                return answer("key");
            }

            auto clearAudit() noexcept -> void override { ++m_auditClears; }

            [[nodiscard]] auto close() -> Status override
            {
                ++m_closes;
                return ok();
            }

            [[nodiscard]]
            auto asked() const noexcept UF_LIFETIME_BOUND
                -> std::vector<std::string> const&
            {
                return m_asked;
            }

            [[nodiscard]] auto captures() const noexcept -> std::size_t
            {
                return m_captures;
            }

            [[nodiscard]] auto auditClears() const noexcept -> std::size_t
            {
                return m_auditClears;
            }

            [[nodiscard]] auto closes() const noexcept -> std::size_t
            {
                return m_closes;
            }

            [[nodiscard]]
            auto beforeBytesAtDelivery() const noexcept -> std::optional<uintmax>
            {
                return m_beforeBytesAtDelivery;
            }
        };

        // The session under test plus the observing pointer a case reads the
        // scripted drive back through. The pointer is taken before the drive is
        // moved into the session, and the session owns it for the whole case.
        struct SessionUnderTest final
        {
            ScriptedDrive*                     p_drive{};
            std::unique_ptr<AnnotationSession> session{};
        };

        [[nodiscard]]
        auto buildSession(
            SessionFiles const& files,
            ScriptedAnswer answer
        ) -> SessionUnderTest
        {
            auto drive = std::make_unique<ScriptedDrive>(
                files.outputDirectory / "before.png",
                std::move(answer)
            );
            auto* p_drive = drive.get();
            return SessionUnderTest{
                .p_drive = p_drive,
                .session = std::make_unique<AnnotationSession>(
                    std::move(drive),
                    files.outputDirectory,
                    files.queue,
                    files.results
                ),
            };
        }

        [[nodiscard]]
        auto parsed(std::string_view line) -> InputAgentCommand
        {
            auto command = parseInputAgentCommand(line);
            REQUIRE(command.has_value());
            return *std::move(command);
        }

        [[nodiscard]] auto clickCommand() -> std::string
        {
            return std::string{
                R"({"op":"click","x":10,"y":20,)"
                R"("out_before":"before.png","out_after":"after.png",)"
                R"("settle_ms":0})"
            };
        }
    }

    TEST_CASE("annotation session refuses an unconfined output before observing")
    {
        // The confinement fence is the annotation layer's, and it has to run
        // before the drive is asked for anything: an escaping path that is
        // answered only after a capture has already touched the target has
        // spent an observation on a command that was never going to be served.
        auto const files = createSessionFiles("annotation-confine");
        auto const cleanup = scopeExit(
            [cleanupPath = files.root]() noexcept
            {
                removeAllBestEffort(cleanupPath);
            }
        );

        auto under = buildSession(files, ScriptedAnswer{});
        auto const outcome = under.session->execute(
            parsed(
                R"({"op":"click","x":10,"y":20,)"
                R"("out_before":"../escaped.png","out_after":"after.png"})"
            )
        );

        CHECK(outcome.resultLine.contains(R"("ok":false)"));
        CHECK(outcome.resultLine.contains(R"("delivered":false)"));
        CHECK_FALSE(outcome.stopAgent);
        CHECK(under.p_drive->captures() == 0U);
        CHECK(under.p_drive->asked().empty());
    }

    TEST_CASE("annotation session encodes the before-frame only after delivery")
    {
        // The observe->act window must hold nothing slow. Encoding the
        // before-frame and flushing it durably inside that window inflates the
        // observation's age against max_action_frame_age and produces a false
        // StaleObservation, so the file is reserved before the capture and
        // written only once the input has been delivered.
        auto const files = createSessionFiles("annotation-window");
        auto const cleanup = scopeExit(
            [cleanupPath = files.root]() noexcept
            {
                removeAllBestEffort(cleanupPath);
            }
        );

        auto under = buildSession(files, ScriptedAnswer{});
        auto const outcome = under.session->execute(parsed(clickCommand()));

        CHECK(outcome.resultLine.contains(R"("ok":true)"));
        CHECK(outcome.resultLine.contains(R"("delivered":true)"));
        REQUIRE(under.p_drive->asked() == std::vector<std::string>{"click"});

        // Reserved at delivery time, so the file exists and is still empty.
        REQUIRE(under.p_drive->beforeBytesAtDelivery().has_value());
        CHECK(*under.p_drive->beforeBytesAtDelivery() == uintmax{});
        CHECK(
            std::filesystem::file_size(files.outputDirectory / "before.png")
            > uintmax{}
        );
        CHECK(
            std::filesystem::file_size(files.outputDirectory / "after.png")
            > uintmax{}
        );

        // Two observations bracket the action, and the line names both.
        CHECK(under.p_drive->captures() == 2U);
        CHECK(outcome.resultLine.contains(R"("before_frame_id":1)"));
        CHECK(outcome.resultLine.contains(R"("after_frame_id":2)"));
    }

    TEST_CASE("annotation session ends the run only when the window was replaced")
    {
        auto const rejected = createSessionFiles("annotation-rejected");
        auto const rejectedCleanup = scopeExit(
            [cleanupPath = rejected.root]() noexcept
            {
                removeAllBestEffort(cleanupPath);
            }
        );

        // An ordinary refusal is this command's business and nothing more: the
        // line reports it and the queue behind it is still good.
        auto refusedDrive = buildSession(
            rejected,
            ScriptedAnswer{
                .refusal = AutomationErrorKind::ActionRejected,
                .message = "point outside the client area",
            }
        );
        auto const refused = refusedDrive.session->execute(parsed(clickCommand()));
        CHECK(refused.resultLine.contains(R"("ok":false)"));
        CHECK(refused.resultLine.contains(R"("delivered":false)"));
        CHECK(refused.resultLine.contains("point outside the client area"));
        CHECK_FALSE(refused.stopAgent);

        auto const replaced = createSessionFiles("annotation-replaced");
        auto const replacedCleanup = scopeExit(
            [cleanupPath = replaced.root]() noexcept
            {
                removeAllBestEffort(cleanupPath);
            }
        );

        // The window being gone invalidates everything queued behind this
        // command too, so it is the one failure that carries the stop up.
        auto goneDrive = buildSession(
            replaced,
            ScriptedAnswer{
                .refusal        = AutomationErrorKind::TargetUnavailable,
                .message        = "the capture target instance changed",
                .targetReplaced = true,
            }
        );
        auto const gone = goneDrive.session->execute(parsed(clickCommand()));
        CHECK(gone.resultLine.contains(R"("ok":false)"));
        CHECK(gone.resultLine.contains(R"("delivered":false)"));
        CHECK(gone.stopAgent);
    }

    TEST_CASE("annotation session reports the drive's own client size")
    {
        auto const files = createSessionFiles("annotation-capture");
        auto const cleanup = scopeExit(
            [cleanupPath = files.root]() noexcept
            {
                removeAllBestEffort(cleanupPath);
            }
        );

        auto under = buildSession(files, ScriptedAnswer{});
        auto const outcome = under.session->execute(
            parsed(R"({"op":"capture","out":"shot.png"})")
        );

        CHECK(outcome.resultLine.contains(R"("ok":true)"));
        CHECK(
            outcome.resultLine.contains(
                std::format(
                    R"("client_size":{{"width":{},"height":{}}})",
                    k_client.width(),
                    k_client.height()
                )
            )
        );
        CHECK_FALSE(outcome.stopAgent);
        CHECK(under.p_drive->captures() == 1U);
        // A capture delivers nothing, so the drive is never asked to.
        CHECK(under.p_drive->asked().empty());

        // Both lifecycle verbs the loop calls reach the drive rather than
        // stopping at the session.
        under.session->clearCommandAudit();
        CHECK(under.p_drive->auditClears() == 1U);
        CHECK(under.session->close().has_value());
        CHECK(under.p_drive->closes() == 1U);
    }
}
