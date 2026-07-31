#include <annotation.hpp>
#include <drive.hpp>
#include <ocr-text-reader.hpp>
#include <protocol.hpp>
#include <path-validation.hpp>
#include <text-reader.hpp>

#include <controller/discovery.hpp>
#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>
#include <core/utility/scope-exit.hpp>
#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>
#include <ocr/text.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <expected>
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

        // The scripted frame has area rather than being one pixel, because the
        // read verb's own fence is a rectangle against this extent: a 1x1
        // observation would leave nothing but the empty rect the parser already
        // refuses.
        constexpr auto k_frameWidth = uint32{40};
        constexpr auto k_frameHeight = uint32{20};

        [[nodiscard]]
        auto scriptedFrame(uint64 id) -> Frame
        {
            auto const transform = CoordinateTransform::create(
                Point<DesktopSpace>{0.0F, 0.0F},
                1.0F,
                1.0F,
                k_frameWidth,
                k_frameHeight
            );
            REQUIRE(transform.has_value());
            auto const stride = std::size_t{k_frameWidth} * 4U;
            auto const pixels = std::make_shared<FrameBuffer const>(
                std::vector<std::byte>(stride * k_frameHeight)
            );
            auto frame = Frame::create(
                FrameId{id},
                CaptureSessionId{1},
                TargetGeneration{},
                MonotonicInstant::now(),
                k_frameWidth,
                k_frameHeight,
                stride,
                PixelFormat::Bgra8,
                pixels,
                *transform
            );
            REQUIRE(frame.has_value());
            return *std::move(frame);
        }

        [[nodiscard]]
        auto rectOf(uint32 x, uint32 y, uint32 width, uint32 height) -> PixelRect
        {
            auto rect = PixelRect::create(x, y, width, height);
            REQUIRE(rect.has_value());
            return *rect;
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

        // How the scripted reader below answers a read. Described rather than
        // stored as a TextReadOutcome, for ScriptedAnswer's reason: an Error is
        // move-only, so a reader asked twice has to produce a second answer of
        // its own.
        struct ScriptedRead final
        {
            std::optional<AutomationErrorKind> refusal{};
            std::string                        message{};
            bool                               readerUnavailable{};

            std::vector<ocr::TextLine> lines{};
        };

        // A reader that answers from a script instead of from a model, and
        // records the rectangles the annotation layer handed it -- which is how
        // "the fence ran before the reader was asked" is asserted without 20 MB
        // of weights on disk.
        class ScriptedTextReader final : public IInputAgentTextReader
        {
            ScriptedRead m_answer;

            std::vector<PixelRect> m_asked{};

        public:
            explicit ScriptedTextReader(ScriptedRead answer)
                : m_answer{std::move(answer)}
            {
            }

            [[nodiscard]]
            auto read(Frame const&, PixelRect rect) -> TextReadOutcome override
            {
                m_asked.emplace_back(rect);
                if (m_answer.refusal.has_value())
                {
                    return TextReadOutcome{
                        .lines = fail(
                            *m_answer.refusal,
                            m_answer.message
                        ),
                        .readerUnavailable = m_answer.readerUnavailable,
                    };
                }
                return TextReadOutcome{
                    .lines             = m_answer.lines,
                    .readerUnavailable = false,
                };
            }

            [[nodiscard]]
            auto asked() const noexcept UF_LIFETIME_BOUND
                -> std::vector<PixelRect> const&
            {
                return m_asked;
            }
        };

        // The session under test plus the observing pointers a case reads the
        // scripted collaborators back through. Both are taken before their
        // owners are moved into the session, and the session owns them for the
        // whole case.
        struct SessionUnderTest final
        {
            ScriptedDrive*                     p_drive{};
            ScriptedTextReader*                p_reader{};
            std::unique_ptr<AnnotationSession> session{};
        };

        [[nodiscard]]
        auto buildSession(
            SessionFiles const& files,
            ScriptedAnswer answer,
            ScriptedRead readAnswer = ScriptedRead{}
        ) -> SessionUnderTest
        {
            auto drive = std::make_unique<ScriptedDrive>(
                files.outputDirectory / "before.png",
                std::move(answer)
            );
            auto reader = std::make_unique<ScriptedTextReader>(
                std::move(readAnswer)
            );
            auto* p_drive  = drive.get();
            auto* p_reader = reader.get();
            return SessionUnderTest{
                .p_drive  = p_drive,
                .p_reader = p_reader,
                .session  = std::make_unique<AnnotationSession>(
                    std::move(drive),
                    std::move(reader),
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

    // The read verb's whole contract, and the reason it is three cases rather
    // than one: an operator that cannot tell a reader which never came up from a
    // rectangle that was refused from a region that really holds no text has no
    // idea which of the three things to go and fix.
    TEST_CASE("annotation session answers a read with the text and its confidence")
    {
        auto const files = createSessionFiles("annotation-read");
        auto const cleanup = scopeExit(
            [cleanupPath = files.root]() noexcept
            {
                removeAllBestEffort(cleanupPath);
            }
        );

        auto under = buildSession(
            files,
            ScriptedAnswer{},
            ScriptedRead{
                .lines = {
                    ocr::TextLine{
                        .text         = R"(621/922 "x")",
                        .bounds       = rectOf(2, 3, 10, 5),
                        .confidenceBp = 9871U,
                    },
                },
            }
        );
        auto const outcome = under.session->execute(
            parsed(
                R"({"op":"read","rect_x":2,"rect_y":3,)"
                R"("rect_width":10,"rect_height":5})"
            )
        );

        CHECK(outcome.resultLine.contains(R"("op":"read")"));
        CHECK(outcome.resultLine.contains(R"("ok":true)"));
        CHECK(outcome.resultLine.contains(R"("reader_ready":true)"));
        CHECK(outcome.resultLine.contains(R"("frame_id":1)"));
        CHECK(outcome.resultLine.contains(R"("error":null)"));

        // The text is escaped as JSON, and the confidence travels with it: a
        // bare joined string would leave an author with no way to tell a read
        // worth trusting from one that is not.
        CHECK(
            outcome.resultLine.contains(
                R"({"text":"621/922 \"x\"","confidence_bp":9871,)"
                R"("bounds":{"x":2,"y":3,"width":10,"height":5}})"
            )
        );

        // A read brackets nothing and writes nothing, so the drive observes once
        // and is never asked to deliver.
        CHECK(under.p_drive->captures() == 1U);
        CHECK(under.p_drive->asked().empty());
        CHECK(under.p_reader->asked() == std::vector<PixelRect>{rectOf(2, 3, 10, 5)});
        CHECK_FALSE(outcome.stopAgent);
    }

    TEST_CASE("annotation session reports a region with no text as an answer")
    {
        auto const files = createSessionFiles("annotation-read-empty");
        auto const cleanup = scopeExit(
            [cleanupPath = files.root]() noexcept
            {
                removeAllBestEffort(cleanupPath);
            }
        );

        // The engine ran and found nothing. That is a fact about the screen --
        // the popup is not showing -- and reporting it as a failure would make
        // it indistinguishable from a model that would not load.
        auto under = buildSession(files, ScriptedAnswer{}, ScriptedRead{});
        auto const outcome = under.session->execute(
            parsed(
                R"({"op":"read","rect_x":0,"rect_y":0,)"
                R"("rect_width":4,"rect_height":4})"
            )
        );

        CHECK(outcome.resultLine.contains(R"("ok":true)"));
        CHECK(outcome.resultLine.contains(R"("reader_ready":true)"));
        CHECK(outcome.resultLine.contains(R"("lines":[])"));
        CHECK(outcome.resultLine.contains(R"("error":null)"));
        CHECK(under.p_reader->asked().size() == 1U);
    }

    TEST_CASE("annotation session separates a missing reader from a refused read")
    {
        auto const refused = createSessionFiles("annotation-read-refused");
        auto const refusedCleanup = scopeExit(
            [cleanupPath = refused.root]() noexcept
            {
                removeAllBestEffort(cleanupPath);
            }
        );

        auto const readCommand = std::string{
            R"({"op":"read","rect_x":0,"rect_y":0,)"
            R"("rect_width":4,"rect_height":4})"
        };

        // This one read failed. The reader is fine, so the next read may well
        // succeed and the operator's move is to look at the rectangle.
        auto refusedRead = buildSession(
            refused,
            ScriptedAnswer{},
            ScriptedRead{
                .refusal = AutomationErrorKind::InvalidResource,
                .message = "the ocr rect does not fit inside the image",
            }
        );
        auto const refusal = refusedRead.session->execute(parsed(readCommand));
        CHECK(refusal.resultLine.contains(R"("ok":false)"));
        CHECK(refusal.resultLine.contains(R"("reader_ready":true)"));
        CHECK(refusal.resultLine.contains(R"("lines":null)"));
        CHECK(refusal.resultLine.contains("does not fit inside the image"));
        CHECK_FALSE(refusal.stopAgent);

        auto const missing = createSessionFiles("annotation-read-unavailable");
        auto const missingCleanup = scopeExit(
            [cleanupPath = missing.root]() noexcept
            {
                removeAllBestEffort(cleanupPath);
            }
        );

        // The reader never came up. Every read in this run will answer the same
        // way until the payload beside the binary is fixed -- and yet the run
        // goes on, because capture and the input verbs are unaffected.
        auto unavailable = buildSession(
            missing,
            ScriptedAnswer{},
            ScriptedRead{
                .refusal           = AutomationErrorKind::InvalidResource,
                .message           = "the ocr model file 'inference.onnx' is missing",
                .readerUnavailable = true,
            }
        );
        auto const absent = unavailable.session->execute(parsed(readCommand));
        CHECK(absent.resultLine.contains(R"("ok":false)"));
        CHECK(absent.resultLine.contains(R"("reader_ready":false)"));
        CHECK(absent.resultLine.contains(R"("lines":null)"));
        CHECK(absent.resultLine.contains("is missing"));
        CHECK_FALSE(absent.stopAgent);
    }

    TEST_CASE("annotation session refuses a rect outside the observation")
    {
        auto const files = createSessionFiles("annotation-read-outside");
        auto const cleanup = scopeExit(
            [cleanupPath = files.root]() noexcept
            {
                removeAllBestEffort(cleanupPath);
            }
        );

        // The fence is this layer's rather than the engine's, and it has to run
        // before the reader is touched: a rectangle that was never going to be
        // readable must not be what brings a 20 MB model up.
        auto under = buildSession(files, ScriptedAnswer{}, ScriptedRead{});
        auto const outcome = under.session->execute(
            parsed(
                std::format(
                    R"({{"op":"read","rect_x":0,"rect_y":0,)"
                    R"("rect_width":{},"rect_height":{}}})",
                    k_frameWidth + 1U,
                    k_frameHeight
                )
            )
        );

        CHECK(outcome.resultLine.contains(R"("ok":false)"));
        CHECK(outcome.resultLine.contains(R"("reader_ready":true)"));
        CHECK(outcome.resultLine.contains(R"("lines":null)"));
        CHECK(outcome.resultLine.contains(R"("frame_id":1)"));
        CHECK(under.p_reader->asked().empty());
    }

    TEST_CASE("the ocr reader reports an absent payload as the reader, not the rect")
    {
        // The one failure mode of the live reader that needs no model to prove:
        // pointed at a directory with no weights in it, it must answer that the
        // reader is unavailable rather than that this rectangle was bad, and it
        // must do so without the agent having failed to start.
        auto const files = createSessionFiles("annotation-read-payload");
        auto const cleanup = scopeExit(
            [cleanupPath = files.root]() noexcept
            {
                removeAllBestEffort(cleanupPath);
            }
        );

        auto reader = OcrTextReader{files.root / "absent-models"};
        auto const outcome = reader.read(scriptedFrame(1), rectOf(0, 0, 4, 4));

        REQUIRE_FALSE(outcome.lines.has_value());
        CHECK(outcome.readerUnavailable);
        CHECK(outcome.lines.error().message().contains("inference.onnx"));
    }

    TEST_CASE("the shipped reader finds the payload staged beside the binary")
    {
        // The one thing about the live read path that no scripted reader can
        // stand in for: that `models/ppocr-v6-small-rec` really is beside the
        // executable, that createOcrTextReader resolves that directory the way
        // a detached agent has to, and that ONNX Runtime brings the recognition
        // model up from it. The test binary lands in the same directory the
        // agent does, so it resolves the same payload the agent will.
        //
        // What is read is deliberately not asserted: a blank frame is whatever
        // the model decides it is, and the accuracy of a real frame is
        // tests/ocr/test-ocr-real.cpp's subject rather than this one's.
        auto reader = createOcrTextReader();
        REQUIRE(reader.has_value());

        auto const outcome = (*reader)->read(
            scriptedFrame(1),
            rectOf(0, 0, k_frameWidth, k_frameHeight)
        );
        auto const reason = outcome.lines
            ? std::string{}
            : std::string{outcome.lines.error().message()};
        REQUIRE_MESSAGE(outcome.lines.has_value(), reason);
        CHECK_FALSE(outcome.readerUnavailable);
    }

    TEST_CASE("a frame is read as the BGRA plane it was captured as")
    {
        auto const frame = scriptedFrame(1);
        auto const image = frameAsBgraImage(frame);
        REQUIRE(image.has_value());
        CHECK(image->width() == k_frameWidth);
        CHECK(image->height() == k_frameHeight);
        CHECK(image->stride() == std::size_t{k_frameWidth} * 4U);

        // A greyscale frame would be recognised as confident nonsense rather
        // than failing, because the model reads whatever bytes it is handed as
        // three channels; refusing it here is the only place that can tell.
        auto const transform = CoordinateTransform::create(
            Point<DesktopSpace>{0.0F, 0.0F},
            1.0F,
            1.0F,
            2,
            2
        );
        REQUIRE(transform.has_value());
        auto grey = Frame::create(
            FrameId{2},
            CaptureSessionId{1},
            TargetGeneration{},
            MonotonicInstant::now(),
            2,
            2,
            2,
            PixelFormat::Gray8,
            std::make_shared<FrameBuffer const>(std::vector<std::byte>(4)),
            *transform
        );
        REQUIRE(grey.has_value());
        auto const refused = frameAsBgraImage(*grey);
        REQUIRE_FALSE(refused.has_value());
        CHECK(
            automationErrorKind(refused.error())
            == AutomationErrorKind::UnsupportedCapability
        );
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
