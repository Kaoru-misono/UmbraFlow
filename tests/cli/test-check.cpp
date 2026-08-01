#include "../annotation/test-helpers.hpp"

#include <args.hpp>
#include <check.hpp>
#include <file-frame-source.hpp>
#include <run.hpp>

#include <annotation/authoring-compiler.hpp>
#include <annotation/authoring-document.hpp>
#include <annotation/capabilities.hpp>
#include <annotation/content-hash.hpp>
#include <annotation/resource.hpp>

#include <core/error/error.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/space.hpp>

#include <image/png.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// The falsification matrix, end to end through the product verb that runs it.
//
// Every case here drives cli::checkProduct over a project written to disk,
// because that is the whole claim of this work: the matrix is data in the
// project file, the walk is trusted Luau, the frames come from the project's own
// screens, and the exit code says whether the model survived. A test that
// exercised the Luau in isolation would prove the judging and nothing about the
// three boundaries it has to cross to be useful.
namespace uf::cli
{
    namespace anno = annotation;

    namespace
    {
        // The synthetic geometry every project below is built at.
        //
        // A screen is small and a search region is small, and both are
        // deliberate: this file measures what the matrix DECIDES, and the cost
        // of the deciding, rather than how fast a sum of absolute differences
        // runs. An authored search region is a usability precondition for a real
        // project too -- eight elements searched over a whole frame did not
        // finish in ten minutes, and the same eight finished in 17.3 seconds
        // once each had one (docs/plans/2026-07-31-script-owned-page-model.md 7).
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
        auto fixtureFingerprint() -> anno::ProjectFingerprint
        {
            return anno::test::fingerprint(k_screenSize, k_screenSize, 96, 96);
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

            // The grey the screen is painted in, when it differs from the grey
            // the template holds. Equal greys make every hit exact and every
            // margin infinite; a small difference is what puts a measurable
            // distance between an owner and its rival.
            std::optional<uint8> paintGray{};

            // Screens that carry this appearance's block WITHOUT owning it. It
            // is how a mark that the model says is absent somewhere is made to
            // turn up there anyway, which is the failure the matrix exists to
            // catch and the only way to produce it with one appearance.
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

            // When false only the cells an appearance OWNS are written into the
            // project file, which is an author's natural habit: measure each mark
            // on its own screen and leave the rest blank. It is what the rules
            // that fire without an expectation are aimed at.
            bool claimOffDiagonal{true};

            // How wide each element's search region is. The default leaves room
            // for the mark to sit somewhere inside it, so a search actually
            // searches; the timing case also builds one at exactly the mark's
            // size, which leaves a single candidate position and so measures the
            // boundary crossing with almost no comparison work behind it.
            uint32 rectSize{k_rectSize};
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
            return anno::test::pixelRect(
                element.column * (k_rectSize - 3U),
                element.row * (k_rectSize - 3U),
                project.rectSize,
                project.rectSize
            );
        }

        // A solid square of one grey, as PNG bytes. A uniform block is the right
        // shape for a mark here: it is unambiguous, it matches at exactly one
        // offset inside a region painted with anything else, and the distance to
        // every other grey is a number the test can reason about.
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

        // Publishes the annotation project TaskHost::loadProject reads.
        //
        // It is deliberately minimal and shares nothing with the page model
        // below: what the matrix needs from it is the project's identity and its
        // fingerprint, because the screens being measured were captured at that
        // geometry. Everything the matrix actually walks lives in the layer-two
        // file.
        auto publishAnnotationProject(std::filesystem::path const& root) -> void
        {
            auto const fingerprint = fixtureFingerprint();
            auto const sourceId    = anno::test::sourceId(
                "00000000-0000-0000-0000-0000000000a1"
            );

            // The source is a whole screen at the project's own geometry,
            // because the compiler validates that an authoring source carries
            // the fingerprint the project declares.
            auto const sourceBytes = solidPng(k_screenSize, uint8{7});
            auto const sourceHash  = anno::sha256(sourceBytes);
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

            auto const anchorId =
                anno::test::elementId("00000000-0000-0000-0000-0000000000b1");
            auto const pageId =
                anno::test::pageId("00000000-0000-0000-0000-0000000000c1");

            auto document = anno::AuthoringDocument::create(
                anno::test::projectId("personal.check"),
                fingerprint,
                {*source},
                {
                    anno::test::element(
                        fingerprint,
                        anchorId,
                        "host_anchor",
                        anno::test::capabilities(anno::Identify{}),
                        anno::test::pixelRect(0, 0, k_markSize, k_markSize),
                        {
                            anno::test::appearance(
                                "default",
                                sourceId,
                                anno::test::pixelRect(0, 0, k_markSize, k_markSize)
                            ),
                        }
                    ),
                },
                {anno::test::page(pageId, "host_page")},
                {anno::test::reference(pageId, anchorId, anno::test::identifiesAs())},
                {}
            );
            REQUIRE(document.has_value());

            auto const asset = anno::AuthoringSourceAsset{
                .id       = sourceId,
                .pngBytes = sourceBytes,
            };
            auto const compiled = anno::compileAuthoringDocument(
                *document,
                std::span{&asset, std::size_t{1}}
            );
            REQUIRE(compiled.has_value());

            writeText(
                root / "generated" / "annotations.runtime.toml",
                compiled->runtimeManifestToml
            );
            for (auto const& templateAsset : compiled->templateAssets)
            {
                writeFile(root / templateAsset.relativePath, templateAsset.pngBytes);
            }
        }

        // Writes every screen PNG and returns the content hash of each, in screen
        // order.
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
                auto const hash  = anno::sha256(bytes);
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
            auto text = std::string{"schema = \"umbraflow-project/l2-v1\"\n"};

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

        // Lays the whole project out on disk. Its effect is the layout; the
        // page-model text it hands back is a convenience for the cases that
        // rewrite that one file to make the project malformed in exactly one way,
        // and is ignored by every other case.
        auto layOutProject(
            std::filesystem::path const& root,
            SyntheticProject const& project
        ) -> std::string
        {
            publishAnnotationProject(root);

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

        // Twenty elements over four screens, each owning exactly one screen, laid
        // out in a five-by-four grid of non-overlapping rectangles. This is the
        // shape the work order asks to be measured on and the baseline every
        // rejection case perturbs.
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

    TEST_CASE("a mark that matches a screen the model says it is absent from is rejected")
    {
        // The whole point of the matrix, in its simplest form: the file claims a
        // mark is absent from three screens, and one of them carries it. Nothing
        // about the model itself is malformed -- it is the measurement that
        // contradicts what was written down.
        //
        // The element keeps its single appearance, so no rule about a set of
        // appearances can fire and the per-cell judgement is the only thing that
        // can reject this model.
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
        // the one that does not need an expectation to fire.
        //
        // The two greys below are one level apart, which is what a pair of speed
        // icons differing by a single digit looks like to a mask drawn a little
        // too generously. Both clear the threshold on the screen either of them
        // owns, so the element tells nothing apart -- and the fold's answer is
        // whichever happened to score better on the day.
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
            // THIS IS THE CASE THE RULE EXISTS FOR. The author measured each
            // appearance on its own screen, wrote down what they saw, and every
            // recorded expectation is satisfied. Only the rule that fires without
            // an expectation -- no two appearances of one element may match one
            // screen -- can see that the element distinguishes nothing.
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
        // The control for the case above. Same file shape, same missing
        // off-diagonal claims, greys far enough apart that each appearance owns
        // its screen alone -- and the verdict is accepted. Without this, the
        // pairwise rule could be rejecting every multi-appearance element and
        // the case above would still pass.
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
        // outcomes are CORRECT here, and that is exactly what makes it worth
        // reporting rather than waiting for. The owning appearance clears its
        // threshold and the rival does not, so every recorded expectation holds
        // and the per-cell rules see nothing; only the separation factor does.
        //
        // The numbers are exact. An 8x8 template at 9900 basis points has a
        // ceiling of 16320, so a hit needs a score of at most 163. The screen is
        // painted grey 100: the owner's grey 98 scores 64 x 2 = 128 and hits, and
        // the rival's grey 106 scores 64 x 6 = 384 and misses -- a lead of three
        // times, under the factor of four a healthy mark has been measured to
        // deliver.
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
            auto const hash  = anno::sha256(stray);
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
        // Every template a project holds was cut at the project's own geometry,
        // so a screen of another size measures one model's marks against another
        // model's pixels. Refusing it here names the file; letting it through
        // would report a matrix of misses and blame the marks.
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

    // MEASURE, DO NOT OPTIMIZE. docs/plans/2026-07-31-script-owned-page-model.md
    // 10.5 leaves the question of a batch matching primitive open until the
    // boundary cost of a whole matrix has been measured on a realistic project,
    // and forbids guessing. This case is that measurement and asserts nothing
    // about the numbers: it reports the wall time of a twenty-element,
    // four-screen matrix and the cost of one Luau-to-C++ crossing within it, so
    // the decision is taken against data.
    TEST_CASE("the cost of a twenty-element four-screen matrix is measured")
    {
        // Two sizes rather than one, because a single number cannot answer the
        // question. Most of a small matrix is fixed cost -- loading the project,
        // booting the VM, decoding four PNGs -- and none of that is what a batch
        // primitive would remove. The MARGINAL cost of a cell is, so both sizes
        // are timed and the difference is divided by the difference in cells.
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

            // One cycle_match per (screen, appearance), which is the crossing the
            // open question is about. Every element here declares one appearance,
            // so the folded row reuses that measurement rather than paying for a
            // second search.
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
        // Luau-to-C++ crossing plus a rounding error, which is the number
        // docs/plans/2026-07-31-script-owned-page-model.md 10.5 left the batch
        // primitive question open on.
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
}
