#include "../domain/test-helpers.hpp"

#include <args.hpp>
#include <check.hpp>
#include <replay.hpp>
#include <file-frame-source.hpp>
#include <run.hpp>

#include <core/error/error.hpp>
#include <core/types/integer.hpp>
#include <core/utility/scope-exit.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>
#include <domain/space.hpp>

#include <image/png.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// The descriptor calls, under the names each host exports: MSVC deprecates the
// unprefixed spellings and this build compiles warnings as errors. They are here
// because a check REPORTS through Luau's `print`, which writes to the C standard
// output this binary shares with the VM -- so nothing a std::cout buffer swap
// can reach observes it, and the descriptor is what has to move.
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

// The falsification matrix, end to end through cli::checkProduct over a project
// written to disk: the claim is the crossing of three boundaries -- matrix as
// data in the project file, walk in trusted Luau, frames from the project's own
// screens -- and not the judging alone.
namespace uf::cli
{
    namespace
    {
        // Small screens and small search regions keep the measurement on what
        // the matrix DECIDES rather than on how fast a sum of absolute
        // differences runs. An authored search region is a real precondition
        // too: eight elements searched over a whole frame did not finish in ten
        // minutes and finished in 17.3 seconds once each had one
        // (docs/plans/2026-07-31-script-owned-page-model.md 7).
        constexpr auto k_screenSize = uint32{48};
        constexpr auto k_markSize   = uint32{8};
        constexpr auto k_rectSize   = uint32{12};
        constexpr auto k_columns    = uint32{5};

        // A grey no mark is drawn in, so a template only ever matches where its
        // own block was painted. Every mark grey below is far from it.
        constexpr auto k_background = uint8{250};

        // Exact-match thresholds would make every miss trivially far away. This
        // one accepts a mean error of about 2.55 grey levels over the template,
        // which is the band a real authored mark sits in.
        constexpr auto k_threshold = uint32{9900};

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

            [[nodiscard]] auto path() const -> std::filesystem::path const&
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
            auto stream = std::ofstream{path, std::ios::binary};
            REQUIRE(stream.is_open());
            for (auto const value : bytes)
            {
                stream.put(static_cast<char>(value));
            }
            REQUIRE(stream.good());
        }

        auto writeText(std::filesystem::path const& path, std::string_view text) -> void
        {
            writeFile(path, std::as_bytes(std::span{text}));
        }

        [[nodiscard]]
        auto fixtureFingerprint() -> ProjectFingerprint
        {
            return test::fingerprint(k_screenSize, k_screenSize, 96, 96);
        }

        // One way an element can look, and which screen carries it.
        struct SyntheticAppearance final
        {
            std::string name{};
            uint8       gray{};

            // The screen that paints this appearance's block, or absent when no
            // screen does -- which is how a mark that must miss everywhere is
            // expressed.
            std::optional<std::size_t> screen{};

            // The grey the screen is painted in when it differs from the
            // template's. Equal greys make every hit exact and every margin
            // infinite; a small difference puts a measurable distance between an
            // owner and its rival.
            std::optional<uint8> paintGray{};

            // Screens that carry this appearance's block WITHOUT owning it: how
            // a mark the model says is absent turns up there anyway, which is
            // the failure the matrix exists to catch.
            std::vector<std::size_t> alsoOn{};
        };

        struct SyntheticElement final
        {
            std::string                      name{};
            uint32                           column{};
            uint32                           row{};
            std::vector<SyntheticAppearance> appearances{};
        };

        struct SyntheticProject final
        {
            std::vector<SyntheticElement> elements{};
            std::size_t                   screens{};

            // When false only the cells an appearance OWNS are written down --
            // an author's natural habit, and what the rules that fire without an
            // expectation are aimed at.
            bool claimOffDiagonal{true};

            // How wide each element's search region is. The default leaves room
            // for the mark inside it so a search actually searches; the timing
            // case also builds one at exactly the mark's size, leaving a single
            // candidate position so the boundary crossing dominates.
            uint32 rectSize{k_rectSize};

            // Which page each screen says it IS, by index. An absent or empty
            // entry writes no `page` line -- a screen nobody annotated yet, not
            // a defect -- so the baseline project stays unchanged.
            std::vector<std::string> screenPages{};
        };

        [[nodiscard]]
        auto markInset(SyntheticProject const& project) -> uint32
        {
            return (project.rectSize - k_markSize) / 2U;
        }

        // The grid stride does not depend on the region size, so shrinking the
        // regions moves no element and the two timed projects differ in exactly
        // one thing.
        [[nodiscard]]
        auto elementRect(
            SyntheticProject const& project,
            SyntheticElement const& element
        ) -> PixelRect
        {
            return test::pixelRect(
                element.column * (k_rectSize - 3U),
                element.row * (k_rectSize - 3U),
                project.rectSize,
                project.rectSize
            );
        }

        // A solid square of one grey, as PNG bytes. A uniform block matches at
        // exactly one offset inside a region painted with anything else, and its
        // distance to every other grey is a number the test can reason about.
        [[nodiscard]]
        auto solidPng(uint32 size, uint8 gray) -> std::vector<std::byte>
        {
            auto pixels = std::vector<std::byte>{};
            pixels.reserve(std::size_t{size} * size * 4U);
            for (auto index = uint32{0}; index < size * size; ++index)
            {
                pixels.emplace_back(static_cast<std::byte>(gray));
                pixels.emplace_back(static_cast<std::byte>(gray));
                pixels.emplace_back(static_cast<std::byte>(gray));
                pixels.emplace_back(static_cast<std::byte>(uint8{255}));
            }
            auto encoded = image::encodeRgbaPng("solid", size, size, pixels);
            REQUIRE(encoded.has_value());
            return *std::move(encoded);
        }

        [[nodiscard]]
        auto markPng(uint8 gray) -> std::vector<std::byte>
        {
            return solidPng(k_markSize, gray);
        }

        [[nodiscard]]
        auto screenPng(
            SyntheticProject const& project,
            std::size_t screenIndex
        ) -> std::vector<std::byte>
        {
            auto pixels = std::vector<std::byte>(
                std::size_t{k_screenSize} * k_screenSize * 4U,
                static_cast<std::byte>(k_background)
            );
            for (auto index = std::size_t{3}; index < pixels.size(); index += 4U)
            {
                pixels[index] = static_cast<std::byte>(uint8{255});
            }

            for (auto const& element : project.elements)
            {
                auto const rect  = elementRect(project, element);
                auto const inset = markInset(project);
                for (auto const& appearance : element.appearances)
                {
                    auto const strays = std::ranges::contains(
                        appearance.alsoOn,
                        screenIndex
                    );
                    if (appearance.screen != screenIndex && !strays)
                    {
                        continue;
                    }
                    auto const painted =
                        appearance.paintGray.value_or(appearance.gray);
                    for (auto offsetY = uint32{0}; offsetY < k_markSize; ++offsetY)
                    {
                        for (auto offsetX = uint32{0}; offsetX < k_markSize; ++offsetX)
                        {
                            auto const x = rect.x() + inset + offsetX;
                            auto const y = rect.y() + inset + offsetY;
                            auto const base =
                                (std::size_t{y} * k_screenSize + x) * 4U;
                            pixels[base]      = static_cast<std::byte>(painted);
                            pixels[base + 1U] = static_cast<std::byte>(painted);
                            pixels[base + 2U] = static_cast<std::byte>(painted);
                        }
                    }
                }
            }

            auto encoded =
                image::encodeRgbaPng("screen", k_screenSize, k_screenSize, pixels);
            REQUIRE(encoded.has_value());
            return *std::move(encoded);
        }

        [[nodiscard]]
        auto markPath(
            SyntheticElement const& element,
            SyntheticAppearance const& appearance
        ) -> std::string
        {
            return std::format("assets/marks/{}-{}.png", element.name, appearance.name);
        }

        // Returns the hash of each screen PNG it writes, in screen order.
        [[nodiscard]]
        auto writeScreens(
            std::filesystem::path const& root,
            SyntheticProject const& project
        ) -> std::vector<std::string>
        {
            auto hashes = std::vector<std::string>{};
            for (auto index = std::size_t{0}; index < project.screens; ++index)
            {
                auto const bytes = screenPng(project, index);
                auto const hash  = sha256(bytes);
                REQUIRE(hash.has_value());
                auto const hex = hash->hex();
                writeFile(root / "assets" / "screens" / (hex + ".png"), bytes);
                hashes.emplace_back(hex);
            }
            return hashes;
        }

        // The layer-two project file: the elements, one page so the file is a
        // model rather than a bag of marks, the screen inventory, and what the
        // model claims about every cell.
        [[nodiscard]]
        auto pageModelToml(
            SyntheticProject const& project,
            std::vector<std::string> const& screenHashes
        ) -> std::string
        {
            auto text = std::string{"schema = \"umbraflow-project/l2-v2\"\n"};

            // The geometry every rectangle below is measured at. The host reads
            // it out of this file rather than out of a compiled manifest, so a
            // project file without it does not load at all.
            text += std::format(
                "base_resolution = [{}, {}]\nbase_dpi = [96, 96]\n",
                k_screenSize,
                k_screenSize
            );

            for (auto const& element : project.elements)
            {
                auto const rect = elementRect(project, element);
                text += std::format(
                    "\n[[element]]\nname = \"{}\"\ncapabilities = [\"identify\"]\n"
                    "rect = [{}, {}, {}, {}]\n",
                    element.name,
                    rect.x(),
                    rect.y(),
                    rect.width(),
                    rect.height()
                );
                for (auto const& appearance : element.appearances)
                {
                    text += std::format(
                        "\n[[appearance]]\nelement = \"{}\"\nname = \"{}\"\n"
                        "source = \"{}\"\nthreshold = {}\n",
                        element.name,
                        appearance.name,
                        markPath(element, appearance),
                        k_threshold
                    );
                }
            }

            REQUIRE(!project.elements.empty());
            text += std::format(
                "\n[[page]]\nname = \"only\"\n"
                "\n[[reference]]\npage = \"only\"\nelement = \"{}\"\n"
                "holding = \"owned\"\nexercised = [\"identify\"]\n"
                "identify = \"required\"\n",
                project.elements.front().name
            );

            for (auto index = std::size_t{0}; index < screenHashes.size(); ++index)
            {
                text += std::format(
                    "\n[[screen]]\nname = \"screen{}\"\nhash = \"{}\"\n",
                    index,
                    screenHashes[index]
                );
                if (
                    index < project.screenPages.size()
                    && !project.screenPages[index].empty()
                )
                {
                    text += std::format(
                        "page = \"{}\"\n",
                        project.screenPages[index]
                    );
                }
            }

            for (auto index = std::size_t{0}; index < project.screens; ++index)
            {
                for (auto const& element : project.elements)
                {
                    auto owned = false;
                    for (auto const& appearance : element.appearances)
                    {
                        auto const matches = appearance.screen == index;
                        owned               = owned || matches;
                        if (element.appearances.size() < 2U)
                        {
                            continue;
                        }
                        if (!matches && !project.claimOffDiagonal)
                        {
                            continue;
                        }
                        text += std::format(
                            "\n[[expect]]\nscreen = \"screen{}\"\nelement = \"{}\"\n"
                            "appearance = \"{}\"\nstate = \"{}\"\n",
                            index,
                            element.name,
                            appearance.name,
                            matches ? "match" : "absent"
                        );
                    }
                    if (!owned && !project.claimOffDiagonal)
                    {
                        continue;
                    }
                    text += std::format(
                        "\n[[expect]]\nscreen = \"screen{}\"\nelement = \"{}\"\n"
                        "state = \"{}\"\n",
                        index,
                        element.name,
                        owned ? "match" : "absent"
                    );
                }
            }

            return text;
        }

        // Lays the whole project out on disk. The page-model text it returns is
        // for the cases that rewrite that one file to make the project malformed
        // in exactly one way; every other case ignores it.
        auto layOutProject(
            std::filesystem::path const& root,
            SyntheticProject const& project
        ) -> std::string
        {
            for (auto const& element : project.elements)
            {
                for (auto const& appearance : element.appearances)
                {
                    writeFile(
                        root / markPath(element, appearance),
                        markPng(appearance.gray)
                    );
                }
            }

            auto const hashes = writeScreens(root, project);
            auto const text   = pageModelToml(project, hashes);
            writeText(root / "page-model.toml", text);
            return text;
        }

        // Twenty elements over four screens, each owning exactly one, in a
        // five-by-four grid of non-overlapping rectangles: the shape the
        // measurement is asked for on, and the baseline every rejection perturbs.
        [[nodiscard]]
        auto twentyElementProject() -> SyntheticProject
        {
            auto project = SyntheticProject{.screens = 4};
            for (auto index = uint32{0}; index < 20U; ++index)
            {
                project.elements.emplace_back(
                    SyntheticElement{
                        .name   = std::format("mark_{:02}", index),
                        .column = index % k_columns,
                        .row    = index / k_columns,
                        .appearances = {
                            SyntheticAppearance{
                                .name   = "lit",
                                .gray   = static_cast<uint8>(16U + index * 4U),
                                .screen = index % 4U,
                            },
                        },
                    }
                );
            }
            return project;
        }

        [[nodiscard]]
        auto checkArgs(
            std::filesystem::path const& root,
            std::filesystem::path const& trace
        ) -> CheckArgs
        {
            return CheckArgs{
                .project = root,
                .trace   = trace,
            };
        }

        // The one sentence a failing run is judged by, so a case that failed for
        // an unrelated reason cannot pass.
        [[nodiscard]]
        auto failureText(CheckReport const& report) -> std::string
        {
            if (!report.run.failure)
            {
                return std::string{};
            }
            return formatRunError(*report.run.failure);
        }

        [[nodiscard]]
        auto duplicatedStdout() -> int
        {
#if defined(_WIN32)
            return _dup(_fileno(stdout));
#else
            return dup(fileno(stdout));
#endif
        }

        [[nodiscard]]
        auto redirectStdoutTo(std::filesystem::path const& path) -> bool
        {
#if defined(_WIN32)
            std::FILE* p_reopened = nullptr;
            return freopen_s(&p_reopened, path.string().c_str(), "w", stdout) == 0;
#else
            return std::freopen(path.string().c_str(), "w", stdout) != nullptr;
#endif
        }

        auto restoreStdout(int saved) noexcept -> void
        {
            std::fflush(stdout);
#if defined(_WIN32)
            _dup2(saved, _fileno(stdout));
            _close(saved);
#else
            dup2(saved, fileno(stdout));
            close(saved);
#endif
        }

        // What one check produced, and what it PRINTED while producing it.
        struct CapturedCheck final
        {
            Result<CheckReport> report;
            std::string         printed{};
        };

        // Runs a check with the standard output redirected into `sink`. It is put
        // back on every exit path, because doctest reports through the same
        // descriptor once the case returns.
        [[nodiscard]]
        auto checkCapturingOutput(
            CheckArgs const& args,
            std::filesystem::path const& sink
        ) -> CapturedCheck
        {
            std::fflush(stdout);
            auto const saved = duplicatedStdout();
            REQUIRE(saved >= 0);
            auto restore = scopeExit(
                [saved]() noexcept
                {
                    restoreStdout(saved);
                }
            );
            REQUIRE(redirectStdoutTo(sink));

            auto report = checkProduct(args);
            restoreStdout(saved);
            restore.release();

            auto stream = std::ifstream{sink, std::ios::binary};
            REQUIRE(stream.is_open());
            return CapturedCheck{
                .report  = std::move(report),
                .printed = std::string{
                    std::istreambuf_iterator<char>{stream},
                    std::istreambuf_iterator<char>{},
                },
            };
        }
    }

    TEST_CASE("a model whose marks each own one screen is accepted")
    {
        auto const directory = TemporaryDirectory{"uf-check-accepted"};
        layOutProject(directory.path(), twentyElementProject());

        auto const report = checkProduct(
            checkArgs(directory.path(), directory.path() / "trace.jsonl")
        );
        REQUIRE(report.has_value());
        CHECK_MESSAGE(!report->run.failure.has_value(), failureText(*report));
        CHECK(report->findings == 0U);
        CHECK(exitCodeForCheck(*report) == ExitCode::Success);
    }

    TEST_CASE("a check writes its report to standard output")
    {
        // The product is the report, and nothing else here reads a line of it:
        // every other case judges `findings` and the exit code, which a routine
        // that printed nothing still answers. Delete the print loop from
        // k_checkRoutineBody and only this case goes red.
        //
        // The swept half pins the FLAG as well as the loop. Hard-code
        // `{ sweep_pages = false }` into the routine and the two co-resolution
        // blocks disappear while the clause that reads the flag stays satisfied,
        // so the sweep never runs and every other case still passes.
        auto const directory = TemporaryDirectory{"uf-check-report-lines"};
        layOutProject(directory.path(), twentyElementProject());

        auto sweeping       = checkArgs(directory.path(), directory.path() / "trace.jsonl");
        sweeping.sweepPages = true;

        auto const captured = checkCapturingOutput(
            sweeping,
            directory.path() / "report.jsonl"
        );
        REQUIRE(captured.report.has_value());
        CHECK_MESSAGE(
            !captured.report->run.failure.has_value(),
            failureText(*captured.report)
        );

        CHECK(captured.printed.find(R"({"check":"summary")") != std::string::npos);
        // Page `only` is identified by mark_00, which owns screen0, so a sweep
        // resolves it there and reports one row per declared page besides.
        CHECK(captured.printed.find(R"({"check":"resolution")") != std::string::npos);
        CHECK(
            captured.printed.find(R"({"check":"page_coverage")") != std::string::npos
        );
    }

    TEST_CASE("a screen that says which page it is has that page resolved on it")
    {
        // The only cell of the matrix that measures a whole page SIGNATURE
        // rather than one element: a conjunction over identify rows, resolved on
        // the screen's own observation, which no element-level cell walks end to
        // end.
        SUBCASE("the screen its page really is stays accepted")
        {
            auto const directory = TemporaryDirectory{"uf-check-page-declared"};
            auto project = twentyElementProject();
            // Page `only` is identified by mark_00, and mark_00 owns screen0.
            project.screenPages = {"only"};
            layOutProject(directory.path(), project);

            auto const report = checkProduct(
                checkArgs(directory.path(), directory.path() / "trace.jsonl")
            );
            REQUIRE(report.has_value());
            CHECK_MESSAGE(!report->run.failure.has_value(), failureText(*report));
            CHECK(report->findings == 0U);
            CHECK(exitCodeForCheck(*report) == ExitCode::Success);
        }

        SUBCASE("a screen that is not its declared page is rejected")
        {
            // The same declaration moved one screen along: every cell of the
            // matrix still holds, and the model is rejected anyway because
            // screen1 says it is a page whose required mark is not on it.
            auto const directory = TemporaryDirectory{"uf-check-page-wrong"};
            auto project        = twentyElementProject();
            project.screenPages = {"", "only"};
            layOutProject(directory.path(), project);

            auto const report = checkProduct(
                checkArgs(directory.path(), directory.path() / "trace.jsonl")
            );
            REQUIRE(report.has_value());
            CHECK_MESSAGE(!report->run.failure.has_value(), failureText(*report));
            CHECK(report->findings == 1U);
            CHECK(exitCodeForCheck(*report) == ExitCode::Failure);
        }

        SUBCASE("a page the file never declared is refused before any capture")
        {
            auto const directory = TemporaryDirectory{"uf-check-page-ghost"};
            auto project        = twentyElementProject();
            project.screenPages = {"nowhere"};
            layOutProject(directory.path(), project);

            auto const report = checkProduct(
                checkArgs(directory.path(), directory.path() / "trace.jsonl")
            );
            REQUIRE(report.has_value());
            REQUIRE(report->run.failure.has_value());
            // The FILE's own refusal, named precisely: `recognition` refuses the
            // same declaration again further in, so a fragment both sentences
            // share would pass on either door.
            auto const text = failureText(*report);
            CHECK(text.find("says it is page 'nowhere'") != std::string::npos);
            CHECK(
                text.find("which this project file does not declare")
                != std::string::npos
            );
            CHECK(exitCodeForCheck(*report) != ExitCode::Success);
        }
    }

    TEST_CASE("a mark that matches a screen the model says it is absent from is rejected")
    {
        // The file claims a mark is absent from three screens and one of them
        // carries it: nothing about the model is malformed, the measurement
        // contradicts what was written down. The element keeps its single
        // appearance, so no rule about a set of appearances can fire and the
        // per-cell judgement is the only thing that can reject this model.
        auto const directory = TemporaryDirectory{"uf-check-misfire"};
        auto project = twentyElementProject();
        project.elements.front().appearances.front().alsoOn = {std::size_t{1}};
        layOutProject(directory.path(), project);

        auto const report = checkProduct(
            checkArgs(directory.path(), directory.path() / "trace.jsonl")
        );
        REQUIRE(report.has_value());
        CHECK_MESSAGE(!report->run.failure.has_value(), failureText(*report));
        CHECK(report->findings > 0U);
        CHECK(exitCodeForCheck(*report) == ExitCode::Failure);
    }

    TEST_CASE("two appearances of one element may not match one screen")
    {
        // The rule 2026-07-31-annotation-model-capabilities 4-2.4 asks for, and
        // the one that fires without an expectation. The two greys are one level
        // apart -- a pair of speed icons under a mask drawn a little too
        // generously -- so both clear the threshold on either owner's screen and
        // the element tells nothing apart.
        auto const directory = TemporaryDirectory{"uf-check-confusable"};
        auto project = twentyElementProject();
        project.elements.front().appearances = {
            SyntheticAppearance{.name = "speed_1x", .gray = 100, .screen = std::size_t{0}},
            SyntheticAppearance{.name = "speed_2x", .gray = 101, .screen = std::size_t{1}},
        };

        SUBCASE("with every cell claimed, the off-diagonal misfires are reported")
        {
            layOutProject(directory.path(), project);

            auto const report = checkProduct(
                checkArgs(directory.path(), directory.path() / "trace.jsonl")
            );
            REQUIRE(report.has_value());
            CHECK_MESSAGE(!report->run.failure.has_value(), failureText(*report));
            CHECK(report->findings > 0U);
            CHECK(exitCodeForCheck(*report) == ExitCode::Failure);
        }

        SUBCASE("with only the diagonal claimed, the pairwise rule still rejects it")
        {
            // Every recorded expectation is satisfied -- the author measured each
            // appearance on its own screen -- so only the rule that fires without
            // an expectation can see that the element distinguishes nothing.
            project.claimOffDiagonal = false;
            layOutProject(directory.path(), project);

            auto const report = checkProduct(
                checkArgs(directory.path(), directory.path() / "trace.jsonl")
            );
            REQUIRE(report.has_value());
            CHECK_MESSAGE(!report->run.failure.has_value(), failureText(*report));
            CHECK(report->findings > 0U);
            CHECK(exitCodeForCheck(*report) == ExitCode::Failure);
        }
    }

    TEST_CASE("a well separated appearance set with only the diagonal claimed is accepted")
    {
        // The control for the case above: same file shape, same missing
        // off-diagonal claims, greys far enough apart that each appearance owns
        // its screen alone. Without it the pairwise rule could be rejecting every
        // multi-appearance element and the case above would still pass.
        auto const directory = TemporaryDirectory{"uf-check-separated"};
        auto project             = twentyElementProject();
        project.claimOffDiagonal = false;
        project.elements.front().appearances = {
            SyntheticAppearance{.name = "speed_1x", .gray = 40, .screen = std::size_t{0}},
            SyntheticAppearance{.name = "speed_2x", .gray = 140, .screen = std::size_t{1}},
        };
        layOutProject(directory.path(), project);

        auto const report = checkProduct(
            checkArgs(directory.path(), directory.path() / "trace.jsonl")
        );
        REQUIRE(report.has_value());
        CHECK_MESSAGE(!report->run.failure.has_value(), failureText(*report));
        CHECK(report->findings == 0U);
        CHECK(exitCodeForCheck(*report) == ExitCode::Success);
    }

    TEST_CASE("an owner that only just beats its rival is reported")
    {
        // P4 of docs/plans/2026-07-31-annotation-model-capabilities.md 2.3: both
        // outcomes are CORRECT here, so every recorded expectation holds and the
        // per-cell rules see nothing; only the separation factor does.
        //
        // The numbers are exact. An 8x8 template at 9900 basis points ceils at
        // 16320, so a hit needs a score of at most 163. On a screen painted grey
        // 100 the owner's grey 98 scores 64 x 2 = 128 and hits, the rival's 106
        // scores 64 x 6 = 384 and misses -- a lead of three times, under the
        // factor of four a healthy mark has been measured to deliver.
        auto const directory = TemporaryDirectory{"uf-check-thin"};
        auto project = twentyElementProject();
        project.elements.front().appearances = {
            SyntheticAppearance{
                .name      = "near",
                .gray      = 98,
                .screen    = std::size_t{0},
                .paintGray = uint8{100},
            },
            SyntheticAppearance{.name = "rival", .gray = 106},
        };
        layOutProject(directory.path(), project);

        auto const report = checkProduct(
            checkArgs(directory.path(), directory.path() / "trace.jsonl")
        );
        REQUIRE(report.has_value());
        CHECK_MESSAGE(!report->run.failure.has_value(), failureText(*report));
        CHECK(report->findings > 0U);
        CHECK(exitCodeForCheck(*report) == ExitCode::Failure);
    }

    TEST_CASE("the project file refuses a screen inventory it cannot measure")
    {
        auto const directory = TemporaryDirectory{"uf-check-file-refusals"};
        auto const baseline  = layOutProject(directory.path(), twentyElementProject());

        struct Refusal final
        {
            std::string_view label;
            std::string      appended;
            std::string_view fragment;
        };

        // Every row appends to a project that is otherwise valid, so exactly one
        // invariant can be the one that fires.
        auto const rows = std::vector<Refusal>{
            Refusal{
                .label    = "a hash that names no file",
                .appended = "\n[[screen]]\nname = \"bogus\"\nhash = \"not-a-hash\"\n",
                .fragment = "content hash of its PNG",
            },
            Refusal{
                .label    = "an expectation about a screen nothing declares",
                .appended = "\n[[expect]]\nscreen = \"ghost\"\nelement = \"mark_00\"\n"
                            "state = \"match\"\n",
                .fragment = "which this project file does not declare",
            },
            Refusal{
                .label    = "an expectation about an element nothing declares",
                .appended = "\n[[expect]]\nscreen = \"screen0\"\nelement = \"ghost\"\n"
                            "state = \"match\"\n",
                .fragment = "which this project file does not declare",
            },
            Refusal{
                .label    = "a state that is not one of the three",
                .appended = "\n[[expect]]\nscreen = \"screen0\"\nelement = \"mark_01\"\n"
                            "state = \"probably\"\n",
                .fragment = "needs state =",
            },
            Refusal{
                .label    = "an appearance the element does not have",
                .appended = "\n[[expect]]\nscreen = \"screen0\"\nelement = \"mark_01\"\n"
                            "appearance = \"dim\"\nstate = \"absent\"\n",
                .fragment = "does not declare",
            },
            Refusal{
                .label    = "one cell claimed twice",
                .appended = "\n[[expect]]\nscreen = \"screen0\"\nelement = \"mark_00\"\n"
                            "state = \"match\"\n",
                .fragment = "is claimed twice",
            },
            Refusal{
                .label    = "a claim about an element with no pixels of its own",
                .appended = "\n[[element]]\nname = \"slot\"\n"
                            "capabilities = [\"read\"]\nrect = [0, 0, 4, 4]\n"
                            "\n[[expect]]\nscreen = \"screen0\"\nelement = \"slot\"\n"
                            "state = \"match\"\n",
                .fragment = "no measurement this claim could ever be read against",
            },
            Refusal{
                .label    = "a text claimed of an element that has pixels",
                .appended = "\n[[expect]]\nscreen = \"screen0\"\nelement = \"mark_01\"\n"
                            "text = \"battle\"\nstate = \"match\"\n",
                .fragment = "verifies itself by its template pixels",
            },
            Refusal{
                .label    = "a cell claiming both a template and a text",
                .appended = "\n[[expect]]\nscreen = \"screen0\"\nelement = \"mark_01\"\n"
                            "appearance = \"lit\"\ntext = \"battle\"\n"
                            "state = \"match\"\n",
                .fragment = "names both an appearance and a text",
            },
        };

        for (auto const& row : rows)
        {
            CAPTURE(row.label);
            writeText(directory.path() / "page-model.toml", baseline + row.appended);

            auto const report = checkProduct(
                checkArgs(directory.path(), directory.path() / "trace.jsonl")
            );
            REQUIRE(report.has_value());
            REQUIRE(report->run.failure.has_value());
            CHECK(failureText(*report).find(row.fragment) != std::string::npos);
            CHECK(exitCodeForCheck(*report) != ExitCode::Success);
        }
    }

    TEST_CASE("a project claiming what a region reads refuses a check with no engine")
    {
        // An element with no templates identifies by the text its rectangle
        // reads, and measuring such a cell needs an OCR engine -- an optional
        // binding. Reporting those cells as "unclaimed" would be a green matrix
        // over exactly the cells nobody measured, indistinguishable from a
        // project that claimed nothing, so the routine refuses before the first
        // screen and NAMES THE FLAG.
        auto const directory = TemporaryDirectory{"uf-check-text-no-engine"};
        auto const baseline  = layOutProject(directory.path(), twentyElementProject());

        constexpr auto k_textClaim = std::string_view{
            "\n[[element]]\nname = \"title\"\n"
            "capabilities = [\"read\"]\nrect = [0, 0, 4, 4]\n"
            "\n[[expect]]\nscreen = \"screen0\"\nelement = \"title\"\n"
            "text = \"battle\"\nstate = \"match\"\n"
        };

        SUBCASE("without --ocr-models the check refuses and names the flag")
        {
            writeText(
                directory.path() / "page-model.toml",
                baseline + std::string{k_textClaim}
            );

            auto const report = checkProduct(
                checkArgs(directory.path(), directory.path() / "trace.jsonl")
            );
            REQUIRE(report.has_value());
            REQUIRE(report->run.failure.has_value());
            CHECK(failureText(*report).find("--ocr-models") != std::string::npos);
            CHECK(exitCodeForCheck(*report) != ExitCode::Success);
        }

        SUBCASE("a project claiming no reading needs no engine and is accepted")
        {
            // The control: same binary, same absent flag, every cell a template
            // distance -- so the refusal above is about what the project claims
            // and not about the flag being missing.
            auto const report = checkProduct(
                checkArgs(directory.path(), directory.path() / "trace.jsonl")
            );
            REQUIRE(report.has_value());
            CHECK_MESSAGE(!report->run.failure.has_value(), failureText(*report));
            CHECK(report->findings == 0U);
        }

        SUBCASE("a page a screen declares can need the engine on its own")
        {
            // `oracle.Claims.reads_text` answers "no" here and the check needs an
            // engine anyway, because screen0 says it is a page whose signature is
            // what a title box READS. A routine that asked only the claims would
            // fail inside a resolution instead, naming the engine and not the
            // project that needs it.
            auto const nested = TemporaryDirectory{"uf-check-page-text-no-engine"};
            auto project        = twentyElementProject();
            project.screenPages = {"only"};
            auto const baseText = layOutProject(nested.path(), project);
            writeText(
                nested.path() / "page-model.toml",
                baseText
                    + "\n[[element]]\nname = \"title\"\n"
                      "capabilities = [\"identify\", \"read\"]\n"
                      "rect = [0, 0, 4, 4]\n"
                      "\n[[reference]]\npage = \"only\"\nelement = \"title\"\n"
                      "holding = \"referenced\"\nexercised = [\"identify\"]\n"
                      "identify = \"required\"\nexpected_text = \"battle\"\n"
            );

            auto const report = checkProduct(
                checkArgs(nested.path(), nested.path() / "trace.jsonl")
            );
            REQUIRE(report.has_value());
            REQUIRE(report->run.failure.has_value());
            CHECK(failureText(*report).find("--ocr-models") != std::string::npos);
            CHECK(exitCodeForCheck(*report) != ExitCode::Success);
        }

        SUBCASE("a page NO screen names needs the engine only when the sweep runs")
        {
            // The clause that has to move with the sweep. Page `only` is
            // identified by what a title box reads and nothing declares it, so a
            // sweeping walk offers it to all four screens and reads there, while a
            // walk that does not sweep never resolves it at all. Ask about every
            // declared page unconditionally and the default run below is refused
            // over a flag that would change nothing about it, so this goes red.
            auto const nested = TemporaryDirectory{"uf-check-page-text-undeclared"};
            auto const baseText = layOutProject(nested.path(), twentyElementProject());
            writeText(
                nested.path() / "page-model.toml",
                baseText
                    + "\n[[element]]\nname = \"title\"\n"
                      "capabilities = [\"identify\", \"read\"]\n"
                      "rect = [0, 0, 4, 4]\n"
                      "\n[[reference]]\npage = \"only\"\nelement = \"title\"\n"
                      "holding = \"referenced\"\nexercised = [\"identify\"]\n"
                      "identify = \"required\"\nexpected_text = \"battle\"\n"
            );

            auto const quiet = checkProduct(
                checkArgs(nested.path(), nested.path() / "trace.jsonl")
            );
            REQUIRE(quiet.has_value());
            CHECK_MESSAGE(!quiet->run.failure.has_value(), failureText(*quiet));

            auto sweeping       = checkArgs(nested.path(), nested.path() / "swept.jsonl");
            sweeping.sweepPages = true;

            auto const swept = checkProduct(sweeping);
            REQUIRE(swept.has_value());
            REQUIRE(swept->run.failure.has_value());
            CHECK(failureText(*swept).find("--ocr-models") != std::string::npos);
        }

        SUBCASE("a --ocr-models directory that will not build an engine fails first")
        {
            // The one case that fails if `checkProduct` accepted the flag and
            // then ignored it: the engine is built before any screen is opened,
            // so a directory with no model behind it ends the check before it
            // starts rather than at the first read.
            auto const report = checkProduct(
                CheckArgs{
                    .project   = directory.path(),
                    .trace     = directory.path() / "trace.jsonl",
                    .ocrModels = directory.path() / "no-such-models",
                }
            );
            CHECK_FALSE(report.has_value());
        }
    }

    TEST_CASE("a screen claiming more text cells than any per-cycle default is checkable")
    {
        // One title box, drawn once and reused, reads a different name on every
        // page, so a project of fifty pages claims it fifty times -- normal rather
        // than a runaway. k_defaultMaximumReadsPerCycle (thirty-two) bounds a wait
        // loop's cycle against its observation lease and has nothing to do with a
        // matrix, which opens one observation per screen and measures the whole of
        // it; so a check runs with no per-cycle read ceiling at all. Size one from
        // any policy constant and the forty cells below end the run on a budget
        // refusal instead of the clause it should reach, so this goes red.
        auto const directory = TemporaryDirectory{"uf-check-wide-text-screen"};
        auto const baseline  = layOutProject(directory.path(), twentyElementProject());

        constexpr auto k_claimedCells = 40U;
        auto claims = std::string{};
        for (auto index = 0U; index < k_claimedCells; ++index)
        {
            // Elements with no appearances of their own -- what a region verified
            // by what it reads is. Each is claimed once, on one screen.
            claims += std::format(
                "\n[[element]]\nname = \"slot_{0}\"\ncapabilities = [\"read\"]\n"
                "rect = [0, 0, 4, 4]\n"
                "\n[[expect]]\nscreen = \"screen0\"\nelement = \"slot_{0}\"\n"
                "text = \"line {0}\"\nstate = \"match\"\n",
                index
            );
        }
        writeText(directory.path() / "page-model.toml", baseline + claims);

        auto const report = checkProduct(
            checkArgs(directory.path(), directory.path() / "trace.jsonl")
        );
        REQUIRE(report.has_value());
        REQUIRE(report->run.failure.has_value());

        // It stops on the engine it was not given, which is the only clause
        // between this project and its first capture.
        auto const text = failureText(*report);
        CHECK(text.find("--ocr-models") != std::string::npos);
        CHECK(text.find("read budget") == std::string::npos);
    }

    TEST_CASE("a declared screen with no pixels and a pixel file nothing declares both fail")
    {
        SUBCASE("the project declares fewer screens than the directory holds")
        {
            auto const directory = TemporaryDirectory{"uf-check-extra-screen"};
            auto const baseline  = layOutProject(directory.path(), twentyElementProject());

            // A PNG of the right geometry that no [[screen]] names. Left in
            // place, it would shift which capture answers for which screen from
            // its position in file-name order onward.
            auto project    = twentyElementProject();
            project.screens = 5;
            auto const stray = screenPng(project, 4);
            auto const hash  = sha256(stray);
            REQUIRE(hash.has_value());
            writeFile(
                directory.path() / "assets" / "screens" / (hash->hex() + ".png"),
                stray
            );

            auto const report = checkProduct(
                checkArgs(directory.path(), directory.path() / "trace.jsonl")
            );
            REQUIRE(report.has_value());
            REQUIRE(report->run.failure.has_value());
            CHECK(failureText(*report).find("have to be one set") != std::string::npos);
        }

        SUBCASE("a declared screen's PNG is missing")
        {
            auto const directory = TemporaryDirectory{"uf-check-missing-screen"};
            auto const baseline  = layOutProject(directory.path(), twentyElementProject());

            // Replace one screen file with a differently named one, so the count
            // still matches and one declared hash now has nothing behind it.
            auto const hashes = writeScreens(directory.path(), twentyElementProject());
            auto error        = std::error_code{};
            std::filesystem::rename(
                directory.path() / "assets" / "screens" / (hashes.front() + ".png"),
                directory.path() / "assets" / "screens" / "0123456789.png",
                error
            );
            REQUIRE(!error);

            auto const report = checkProduct(
                checkArgs(directory.path(), directory.path() / "trace.jsonl")
            );
            REQUIRE(report.has_value());
            REQUIRE(report->run.failure.has_value());
            CHECK(exitCodeForCheck(*report) != ExitCode::Success);
        }
    }

    TEST_CASE("the file frame source serves one screen per capture and then refuses")
    {
        auto const directory = TemporaryDirectory{"uf-check-frame-source"};
        layOutProject(directory.path(), twentyElementProject());

        auto source = FileFrameSource::create(
            directory.path() / "assets" / "screens",
            fixtureFingerprint()
        );
        REQUIRE(source.has_value());
        CHECK((*source)->fileCount() == 4U);

        auto const budget = engine::IFrameSource::CaptureBudget{
            .deadline = MonotonicInstant::now(),
        };
        auto seen = std::vector<uint64>{};
        for (auto index = 0; index < 4; ++index)
        {
            auto frame = (*source)->capture(budget);
            REQUIRE(frame.has_value());
            CHECK(frame->width() == k_screenSize);
            CHECK(frame->pixelFormat() == PixelFormat::Bgra8);
            seen.emplace_back(frame->id().value());
        }
        CHECK(seen == std::vector<uint64>{1, 2, 3, 4});

        // Running out is a failure and never a repeat of the last screen: a
        // source that repeated would let a routine which lost count keep
        // measuring one screen and report a matrix that passed.
        auto const exhausted = (*source)->capture(budget);
        REQUIRE(!exhausted.has_value());
        CHECK(
            automationErrorKind(exhausted.error())
            == AutomationErrorKind::CaptureUnavailable
        );
    }

    TEST_CASE("a screen captured at another geometry is refused by name")
    {
        // Every template was cut at the project's own geometry, so a screen of
        // another size measures one model's marks against another's pixels.
        // Refusing here names the file; letting it through would blame the marks.
        auto const directory = TemporaryDirectory{"uf-check-geometry"};
        writeFile(directory.path() / "screens" / "small.png", markPng(uint8{30}));

        auto source = FileFrameSource::create(
            directory.path() / "screens",
            fixtureFingerprint()
        );
        REQUIRE(source.has_value());

        auto const frame = (*source)->capture(
            engine::IFrameSource::CaptureBudget{.deadline = MonotonicInstant::now()}
        );
        REQUIRE(!frame.has_value());
        CHECK(
            automationErrorKind(frame.error())
            == AutomationErrorKind::TargetCompatibilityUnverified
        );
        CHECK(toString(frame.error()).find("small.png") != std::string::npos);
    }

    // docs/plans/2026-07-31-script-owned-page-model.md 10.5 leaves the batch
    // matching primitive open until the boundary cost of a whole matrix is
    // measured on a realistic project. This case is that measurement and asserts
    // nothing about the numbers it reports.
    TEST_CASE("the cost of a twenty-element four-screen matrix is measured")
    {
        // Two sizes, because most of a small matrix is fixed cost -- project
        // load, VM boot, four PNG decodes -- and none of that is what a batch
        // primitive would remove. The MARGINAL cost of a cell is, so the
        // difference in time is divided by the difference in cells.
        struct Measured final
        {
            std::size_t crossings{};
            long long   micros{};
        };

        auto measure = [](
            std::string_view label,
            std::size_t elements,
            uint32 rectSize
        ) -> Measured
        {
            auto const directory = TemporaryDirectory{label};
            auto project         = twentyElementProject();
            project.elements.resize(elements);
            project.rectSize = rectSize;
            layOutProject(directory.path(), project);

            // Three runs and the fastest kept: the slow ones are this machine's
            // scheduler and its filesystem cache, not this code.
            auto best = std::chrono::steady_clock::duration::max();
            for (auto attempt = 0; attempt < 3; ++attempt)
            {
                auto const started = std::chrono::steady_clock::now();
                auto const report  = checkProduct(
                    checkArgs(directory.path(), directory.path() / "trace.jsonl")
                );
                auto const elapsed = std::chrono::steady_clock::now() - started;
                REQUIRE(report.has_value());
                REQUIRE_MESSAGE(!report->run.failure.has_value(), failureText(*report));
                best = std::min(best, elapsed);
            }

            // One cycle_match per (screen, appearance), the crossing the open
            // question is about. Every element declares one appearance, so the
            // folded row reuses that measurement rather than searching twice.
            auto crossings = std::size_t{0};
            for (auto const& element : project.elements)
            {
                crossings += element.appearances.size();
            }
            return Measured{
                .crossings = crossings * project.screens,
                .micros    =
                    std::chrono::duration_cast<std::chrono::microseconds>(best).count(),
            };
        };

        auto const marginalPerCell = [](Measured const& small, Measured const& full)
        {
            REQUIRE(full.crossings > small.crossings);
            return (full.micros - small.micros)
                / static_cast<long long>(full.crossings - small.crossings);
        };

        // A 12x12 region for an 8x8 template: 25 candidate positions of 64
        // pixels, so 1600 comparisons per cell. That is the order a cell of a
        // real project with an authored search region costs.
        auto const searchingSmall = measure("uf-check-timing-small", 5U, k_rectSize);
        auto const searchingFull  = measure("uf-check-timing-full", 20U, k_rectSize);
        auto const searching      = marginalPerCell(searchingSmall, searchingFull);

        // The same matrix with the region narrowed to the mark: one candidate
        // position, 64 comparisons. What is left of the marginal cost is the
        // Luau-to-C++ crossing plus rounding error -- the number 10.5 left the
        // batch primitive question open on.
        auto const tightSmall = measure("uf-check-crossing-small", 5U, k_markSize);
        auto const tightFull  = measure("uf-check-crossing-full", 20U, k_markSize);
        auto const crossing   = marginalPerCell(tightSmall, tightFull);

        MESSAGE(
            std::format(
                "matrix cost: 20 elements x 4 screens = {} cells in {} us "
                "(5 elements: {} us), marginal {} us per cell at 1600 pixel "
                "comparisons. Narrowed to one candidate position (64 "
                "comparisons) the same matrix runs in {} us and the marginal "
                "cost is {} us per cell, which bounds the Luau-to-C++ crossing. "
                "Fixed cost -- project load, VM boot, four PNG decodes -- is "
                "{} us. Debug build, unoptimised matcher.",
                searchingFull.crossings,
                searchingFull.micros,
                searchingSmall.micros,
                searching,
                tightFull.micros,
                crossing,
                tightSmall.micros
                    - crossing * static_cast<long long>(tightSmall.crossings)
            )
        );
    }

    // ------------------------------------------------- the replay verb

    // These live beside the check cases because both verbs are offline products
    // over one project and this file owns the synthetic project they need.
    namespace
    {
        // One recorded run, written the way the recorder writes one. Only the
        // members the projection reads are spelled: a line carrying more is what
        // a real stream looks like, and one carrying less is what this pins.
        [[nodiscard]]
        auto traceText(
            std::string_view frontEnd,
            std::string_view modelHash,
            std::span<std::string const> pages
        ) -> std::string
        {
            auto text = std::format(
                R"({{"schema":"umbraflow-trace/v4","kind":"run.started","seq":1)"
                R"(,"frontEnd":"{}","projectId":"fixture","taskName":"daily")"
                R"(,"sourceHash":"aa","modelHash":"{}","seed":1}})"
                "\n",
                frontEnd,
                modelHash
            );
            auto seq = 1U;
            for (auto const& page : pages)
            {
                ++seq;
                text += std::format(
                    R"({{"kind":"framework.page_resolved","seq":{})"
                    R"(,"frontEnd":"{}","label":"{}"}})"
                    "\n",
                    seq,
                    frontEnd,
                    page
                );
            }
            return text;
        }

        [[nodiscard]]
        auto modelHashOf(std::string_view pageModelText) -> std::string
        {
            auto const hashed = sha256(std::as_bytes(std::span{pageModelText}));
            REQUIRE(hashed.has_value());
            return hashed->hex();
        }
    }

    TEST_CASE("a replay refuses a stream that is not a run")
    {
        // The exclusion that has to happen before a VM boots. `umbra-flow check`
        // resolves every page it cares about against ONE frame, so its
        // resolutions are a sweep; read as a walk they report a task that stood
        // on dozens of pages and delivered nothing. Measured on the reference
        // corpus: a real check trace replayed as a task produces 186 findings
        // and not one of them is about the model.
        auto const directory = TemporaryDirectory{"uf-replay-front-end"};
        auto project        = twentyElementProject();
        project.screenPages = {"only"};
        auto const text      = layOutProject(directory.path(), project);

        auto const trace = directory.path() / "check-run.jsonl";
        auto const pages = std::vector<std::string>{"only"};
        writeText(trace, traceText("check", modelHashOf(text), pages));

        auto const report = replayProduct(
            ReplayArgs{.project = directory.path(), .trace = trace}
        );
        REQUIRE_FALSE(report.has_value());
        CHECK(
            automationErrorKind(report.error())
            == AutomationErrorKind::InvalidResource
        );
        CHECK(std::string{report.error().message()}.contains("front end"));
    }

    TEST_CASE("a replay refuses a run recorded against another page model")
    {
        // Every move a replay could report is about edges, and a stream recorded
        // against a different file may never have had them. The hash never
        // reaches the script layer, so this is the only place the comparison can
        // happen.
        auto const directory = TemporaryDirectory{"uf-replay-model-hash"};
        auto project        = twentyElementProject();
        project.screenPages = {"only"};
        layOutProject(directory.path(), project);

        auto const trace = directory.path() / "other-model.jsonl";
        auto const pages = std::vector<std::string>{"only"};
        writeText(
            trace,
            traceText("task", std::string(64U, 'b'), pages)
        );

        auto const report = replayProduct(
            ReplayArgs{.project = directory.path(), .trace = trace}
        );
        REQUIRE_FALSE(report.has_value());
        CHECK(std::string{report.error().message()}.contains("page model"));
    }

    TEST_CASE("a replay of a run that made no move accepts and says so")
    {
        // The whole pipeline end to end: a trace read off a file, projected,
        // handed to the VM as data through `ctx:replay_steps`, judged against
        // the graph, and printed. One resolution is no move at all, so the
        // model is not contradicted -- and the report still says what it looked
        // at, which is what tells this apart from a routine that printed nothing.
        auto const directory = TemporaryDirectory{"uf-replay-accepted"};
        auto project        = twentyElementProject();
        project.screenPages = {"only"};
        auto const text      = layOutProject(directory.path(), project);

        auto const trace = directory.path() / "run.jsonl";
        auto const pages = std::vector<std::string>{"only", "only"};
        writeText(trace, traceText("task", modelHashOf(text), pages));

        // The routine prints through Lua's `print`, which writes the C stdout
        // and not std::cout, so the redirect is the same file-descriptor one the
        // check cases use.
        auto const sink = directory.path() / "report.jsonl";
        std::fflush(stdout);
        auto const saved = duplicatedStdout();
        REQUIRE(saved >= 0);
        auto restore = scopeExit([saved]() noexcept { restoreStdout(saved); });
        REQUIRE(redirectStdoutTo(sink));

        auto const report = replayProduct(
            ReplayArgs{.project = directory.path(), .trace = trace}
        );
        restoreStdout(saved);
        restore.release();

        REQUIRE(report.has_value());
        auto const failure = report->run.failure
            ? std::string{report->run.failure->message()}
            : std::string{};
        CHECK_MESSAGE(!report->run.failure.has_value(), failure);
        CHECK(report->findings == 0U);
        CHECK(exitCodeForReplay(*report) == ExitCode::Success);

        auto stream = std::ifstream{sink, std::ios::binary};
        REQUIRE(stream.is_open());
        auto const printed = std::string{
            std::istreambuf_iterator<char>{stream},
            std::istreambuf_iterator<char>{}
        };
        CHECK(printed.contains(R"({"replay":"summary")"));
        CHECK(printed.contains(R"("resolutions":2)"));
        CHECK(printed.contains(R"("transitions":0)"));
    }
}
