// The colour-key crops the match cases need are the same two real screen
// captures the annotation join test measures, and they live with the workbench
// tests because the authoring end measured them first. Reaching across rather
// than copying four kilobytes of PNG is what keeps the three ends honest about
// the same pixels.
#include "../workbench/colour-key-fixture.hpp"

#include <command-runner.hpp>
#include <command.hpp>

#include <project-persistence.hpp>
#include <run.hpp>

#include <annotation/authoring-compiler.hpp>
#include <annotation/authoring-document.hpp>
#include <annotation/runtime-manifest.hpp>

#include <core/error/result.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <image/png.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::authoring
{
    namespace
    {
        namespace fixture = workbench::colour_key_fixture;

        // The tolerance colour-key-fixture.hpp took its measurement at, and the
        // authoring default this CLI applies, so the numbers below are the ones
        // tests/annotation/test-colour-key-join.cpp already measured.
        constexpr auto k_tolerance = uint32{12};

        class TemporaryProject final
        {
            std::filesystem::path m_path{};

        public:
            explicit TemporaryProject(std::string_view role)
            {
                static auto s_sequence = std::atomic<uint64>{1U};

                auto const now   = std::chrono::steady_clock::now();
                auto const token = now.time_since_epoch().count();

                auto const leafName = std::format(
                    "umbraflow-authoring-{}-{}-{}",
                    role,
                    token,
                    s_sequence.fetch_add(1U, std::memory_order_relaxed)
                );
                m_path = std::filesystem::temp_directory_path() / leafName;

                auto error = std::error_code{};
                auto const created = std::filesystem::create_directory(
                    m_path,
                    error
                );
                REQUIRE(created);
                REQUIRE_FALSE(error);
            }

            TemporaryProject(TemporaryProject const&) = delete;
            auto operator=(TemporaryProject const&) -> TemporaryProject& = delete;
            TemporaryProject(TemporaryProject&&) = delete;
            auto operator=(TemporaryProject&&) -> TemporaryProject& = delete;

            ~TemporaryProject() noexcept
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

            [[nodiscard]] auto path() const -> std::filesystem::path
            {
                return m_path;
            }

            [[nodiscard]] auto text() const -> std::string
            {
                return m_path.string();
            }
        };

        auto writePng(
            std::filesystem::path const& path,
            std::span<uint8 const> encoded
        ) -> void
        {
            auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};
            REQUIRE(stream.is_open());
            for (auto const value : encoded)
            {
                stream.put(static_cast<char>(value));
            }
            REQUIRE(stream.good());
        }

        [[nodiscard]]
        auto readText(std::filesystem::path const& path) -> std::string
        {
            auto stream = std::ifstream{path, std::ios::binary};
            REQUIRE(stream.is_open());
            return std::string{
                std::istreambuf_iterator<char>{stream},
                std::istreambuf_iterator<char>{},
            };
        }

        // Exactly what entry/authoring/main.cpp does with one command line, so a
        // test drives the surface an agent drives rather than a seam beneath it.
        struct CommandOutcome final
        {
            bool          ok{};
            std::string   json{};
            std::string   message{};
            cli::ExitCode exitCode{cli::ExitCode::Success};
        };

        [[nodiscard]]
        auto runArguments(
            std::vector<std::string> const& arguments
        ) -> CommandOutcome
        {
            auto const command = parseAuthoringCommand(arguments);
            if (!command)
            {
                return CommandOutcome{
                    .ok       = false,
                    .json     = authoringErrorJson(command.error()),
                    .message  = cli::formatRunError(command.error()),
                    .exitCode = cli::exitCodeForError(command.error(), false),
                };
            }

            auto output = runAuthoringCommand(*command);
            if (!output)
            {
                return CommandOutcome{
                    .ok       = false,
                    .json     = authoringErrorJson(output.error()),
                    .message  = cli::formatRunError(output.error()),
                    .exitCode = cli::exitCodeForError(output.error(), false),
                };
            }
            return CommandOutcome{
                .ok       = true,
                .json     = *std::move(output),
                .exitCode = cli::ExitCode::Success,
            };
        }

        // A command line written the way it is typed, one argument per wrapped
        // line. The pack is closed and file-local -- every argument below is a
        // string literal, a path string, or a std::string -- so it stays
        // unconstrained by the rule for implementation-only templates.
        template <typename... Arguments>
        [[nodiscard]]
        auto run(Arguments&&... arguments) -> CommandOutcome
        {
            return runArguments(
                std::vector<std::string>{
                    std::string{std::forward<Arguments>(arguments)}...,
                }
            );
        }

        auto requireOk(CommandOutcome const& outcome) -> void
        {
            INFO(outcome.message);
            REQUIRE(outcome.ok);
        }

        [[nodiscard]]
        auto unsignedField(
            std::string const& json,
            std::string_view key
        ) -> uint64
        {
            auto const marker = std::format("\"{}\":", key);
            auto const at     = json.find(marker);
            REQUIRE(at != std::string::npos);

            auto const tail   = std::string_view{json}.substr(at + marker.size());
            auto parsed       = uint64{};
            auto const result = std::from_chars(
                tail.data(),
                tail.data() + tail.size(),
                parsed
            );
            REQUIRE(result.ec == std::errc{});
            return parsed;
        }

        // The frames subcommands answer in means and fractions, so their
        // measurements are read back as numbers rather than matched as text.
        [[nodiscard]]
        auto numberField(
            std::string const& json,
            std::string_view key
        ) -> double
        {
            auto const marker = std::format("\"{}\":", key);
            auto const at     = json.find(marker);
            REQUIRE(at != std::string::npos);

            auto const tail   = std::string_view{json}.substr(at + marker.size());
            auto parsed       = double{};
            auto const result = std::from_chars(
                tail.data(),
                tail.data() + tail.size(),
                parsed
            );
            REQUIRE(result.ec == std::errc{});
            return parsed;
        }

        [[nodiscard]]
        auto occurrences(
            std::string const& json,
            std::string_view needle
        ) -> std::size_t
        {
            auto found = std::size_t{0};
            auto at    = json.find(needle);
            while (at != std::string::npos)
            {
                ++found;
                at = json.find(needle, at + needle.size());
            }
            return found;
        }

        // The regions array on its own, so a field read out of it cannot be
        // answered by the analysed rect the document echoes above it.
        [[nodiscard]]
        auto regionsJson(std::string const& json) -> std::string
        {
            auto const at = json.find("\"regions\":[");
            REQUIRE(at != std::string::npos);
            return json.substr(at);
        }

        // The whole of the crop, as every subcommand that takes a rectangle
        // spells it.
        [[nodiscard]]
        auto wholeCropRect() -> std::string
        {
            return std::format("0,0,{},{}", fixture::k_width, fixture::k_height);
        }

        // One project holding the same 100x40 rectangle twice on one page: once
        // keyed to the white menu text, once with no key at all. Everything else
        // about the two is identical, so the only thing that can separate them
        // against a frame drawn over other artwork is the key.
        struct KeyedFixture final
        {
            std::filesystem::path blueFrame{};
            std::filesystem::path purpleFrame{};
        };

        [[nodiscard]]
        auto authorKeyedProject(TemporaryProject const& project) -> KeyedFixture
        {
            auto const blue   = project.path() / "menu-over-blue.png";
            auto const purple = project.path() / "menu-over-purple.png";
            writePng(blue, fixture::k_menuOverBlueArtwork);
            writePng(purple, fixture::k_menuOverPurpleArtwork);

            auto const wholeCrop = wholeCropRect();
            auto const whiteText = std::format(
                "{},{},{}",
                fixture::k_textRed,
                fixture::k_textGreen,
                fixture::k_textBlue
            );

            requireOk(
                run(
                    "project",
                    "init",
                    project.text(),
                    "--project-id",
                    "personal.colour_key_cli",
                    "--resolution",
                    std::format("{}x{}", fixture::k_width, fixture::k_height)
                )
            );
            requireOk(
                run(
                    "page",
                    "create",
                    project.text(),
                    "menu",
                    "keyed_menu",
                    "--source",
                    blue.string(),
                    "--rect",
                    wholeCrop,
                    "--key",
                    whiteText,
                    "--tolerance",
                    std::format("{}", k_tolerance)
                )
            );
            requireOk(
                run(
                    "page",
                    "add-anchor",
                    project.text(),
                    "menu",
                    "unkeyed_menu",
                    "--source",
                    blue.string(),
                    "--rect",
                    wholeCrop
                )
            );

            return KeyedFixture{.blueFrame = blue, .purpleFrame = purple};
        }

        // The extent a project needs to hold both search regions the budget
        // cases author: 480 wide for the wider one and 160 tall for the two
        // stacked without overlapping.
        constexpr auto k_budgetWidth  = uint32{480};
        constexpr auto k_budgetHeight = uint32{160};

        // A deterministic grey plane written into all three colour channels, so
        // the matcher's BT.601 conversion reads back exactly these values. The
        // mixing wraps on purpose: it only has to scatter, and two seeds have to
        // produce planes with no 90x33 or 200x50 block in common so a template
        // cropped from one is genuinely absent from the other.
        [[nodiscard]]
        auto writeNoiseFrame(
            std::filesystem::path const& path,
            uint32 seed
        ) -> void
        {
            auto pixels = std::vector<std::byte>{};
            pixels.reserve(std::size_t{k_budgetWidth} * k_budgetHeight * 4U);
            for (auto y = uint32{0}; y < k_budgetHeight; ++y)
            {
                for (auto x = uint32{0}; x < k_budgetWidth; ++x)
                {
                    auto mixed = (x * 73'856'093U)
                        ^ (y * 19'349'663U)
                        ^ ((seed + 1U) * 83'492'791U);
                    mixed ^= mixed >> 13U;
                    mixed *= 2'654'435'761U;
                    mixed ^= mixed >> 16U;

                    auto const narrowed = checkedCast<uint8>(mixed & 0xFFU);
                    REQUIRE(narrowed.has_value());
                    auto const grey = std::byte{*narrowed};
                    pixels.emplace_back(grey);
                    pixels.emplace_back(grey);
                    pixels.emplace_back(grey);
                    pixels.emplace_back(std::byte{255});
                }
            }

            auto const written = image::writeRgbaPng(
                path,
                k_budgetWidth,
                k_budgetHeight,
                pixels
            );
            REQUIRE(written.has_value());
        }

        // One project holding the two search geometries a real annotation session
        // found the previous default budget too small for, as two page anchors on
        // one page. Both are anchors on purpose: evaluatePage shares one budget
        // across every page anchor in the catalog, so a match on either of them
        // spends the sum of the two searches, which is the figure the default has
        // to cover.
        struct BudgetFixture final
        {
            std::filesystem::path authoredFrame{};
            std::filesystem::path otherFrame{};
        };

        [[nodiscard]]
        auto authorBudgetProject(TemporaryProject const& project) -> BudgetFixture
        {
            auto const authored = project.path() / "authored.png";
            auto const other    = project.path() / "other.png";
            writeNoiseFrame(authored, 1);
            writeNoiseFrame(other, 2);

            requireOk(
                run(
                    "project",
                    "init",
                    project.text(),
                    "--project-id",
                    "personal.budget_cli",
                    "--resolution",
                    std::format("{}x{}", k_budgetWidth, k_budgetHeight)
                )
            );
            // Template 90x33 in a 180x70 region: 91 * 38 = 3,458 positions.
            requireOk(
                run(
                    "page",
                    "create",
                    project.text(),
                    "menu",
                    "narrow_label",
                    "--source",
                    authored.string(),
                    "--rect",
                    "0,0,90,33",
                    "--search-roi",
                    "0,0,180,70"
                )
            );
            // Template 200x50 in a 480x90 region: 281 * 41 = 11,521 positions.
            requireOk(
                run(
                    "page",
                    "add-anchor",
                    project.text(),
                    "menu",
                    "wide_label",
                    "--source",
                    authored.string(),
                    "--rect",
                    "0,70,200,50",
                    "--search-roi",
                    "0,70,480,90"
                )
            );

            return BudgetFixture{.authoredFrame = authored, .otherFrame = other};
        }

        // The two crops on disk with no project around them. A frames
        // subcommand reads PNGs and never opens a project root, so the
        // temporary directory here is scratch space rather than a project.
        [[nodiscard]]
        auto writeCrops(TemporaryProject const& scratch) -> KeyedFixture
        {
            auto const blue   = scratch.path() / "menu-over-blue.png";
            auto const purple = scratch.path() / "menu-over-purple.png";
            writePng(blue, fixture::k_menuOverBlueArtwork);
            writePng(purple, fixture::k_menuOverPurpleArtwork);
            return KeyedFixture{.blueFrame = blue, .purpleFrame = purple};
        }
    }

    TEST_CASE("authoring CLI writes a project that loads back with the same rects and keys")
    {
        auto const project = TemporaryProject{"round-trip"};
        auto const frames  = authorKeyedProject(project);

        requireOk(
            run(
                "page",
                "add-target",
                project.text(),
                "menu",
                "menu_button",
                "--source",
                frames.blueFrame.string(),
                "--rect",
                "10,4,20,12",
                "--search-roi",
                "0,0,60,30",
                "--key",
                "249,249,249",
                "--tolerance",
                "8",
                "--min-similarity-bp",
                "8500"
            )
        );

        auto const loaded = workbench::loadAuthoringProject(project.path());
        auto const loadedInfo = loaded
            ? std::string{"loaded"}
            : toString(loaded.error());
        INFO(loadedInfo);
        REQUIRE(loaded.has_value());

        auto const& document = loaded->document;
        CHECK(document.catalog().projectId().value() == "personal.colour_key_cli");
        REQUIRE(document.catalog().pages().size() == 1U);
        CHECK(document.catalog().pages().front().name().value() == "menu");

        // One screenshot named three times ingests once: the source is
        // identified by the canonical PNG's own content hash, so the second and
        // third --source of the same file resolve to the source already there.
        CHECK(document.sources().size() == 1U);

        REQUIRE(document.elements().size() == 3U);
        auto const elementNamed = [&document](
            std::string_view name
        ) -> annotation::Element const*
        {
            for (auto const& element : document.elements())
            {
                if (element.name().value() == name)
                {
                    return &element;
                }
            }
            return nullptr;
        };

        auto const* p_keyed = elementNamed("keyed_menu");
        REQUIRE(p_keyed != nullptr);
        CHECK(p_keyed->annotationType() == annotation::AnnotationType::PageAnchor);
        CHECK(
            p_keyed->templateRect()
            == *PixelRect::create(0, 0, fixture::k_width, fixture::k_height)
        );
        REQUIRE(p_keyed->colourKey().has_value());
        CHECK(p_keyed->colourKey()->red() == fixture::k_textRed);
        CHECK(p_keyed->colourKey()->green() == fixture::k_textGreen);
        CHECK(p_keyed->colourKey()->blue() == fixture::k_textBlue);
        CHECK(p_keyed->colourKey()->tolerance() == k_tolerance);

        auto const* p_unkeyed = elementNamed("unkeyed_menu");
        REQUIRE(p_unkeyed != nullptr);
        // The control on the key checks above: an element authored without
        // --key must come back carrying none, so a CLI that invented a key
        // would fail here rather than pass everything.
        CHECK_FALSE(p_unkeyed->colourKey().has_value());

        auto const* p_target = elementNamed("menu_button");
        REQUIRE(p_target != nullptr);
        CHECK(p_target->annotationType() == annotation::AnnotationType::ActionTarget);
        CHECK(p_target->templateRect() == *PixelRect::create(10, 4, 20, 12));
        CHECK(p_target->searchRoi() == *PixelRect::create(0, 0, 60, 30));
        CHECK(p_target->threshold().basisPoints() == 8500);
        REQUIRE(p_target->colourKey().has_value());
        CHECK(p_target->colourKey()->red() == 249);
        CHECK(p_target->colourKey()->tolerance() == 8);

        // An action target reaches its page through a placement, not through the
        // signature, so a CLI that added the element and dropped the link would
        // still load an element and fail only here.
        REQUIRE(document.placements().size() == 1U);
        CHECK(document.placements().front().elementId == p_target->id());
        CHECK(
            document.placements().front().pageId
            == document.catalog().pages().front().id()
        );
    }

    TEST_CASE("authoring CLI publishes a runtime manifest that parses and matches the compile")
    {
        auto const project = TemporaryProject{"generated"};
        static_cast<void>(authorKeyedProject(project));

        auto const loaded = workbench::loadAuthoringProject(project.path());
        REQUIRE(loaded.has_value());
        auto const compiled = annotation::compileAuthoringDocument(
            loaded->document,
            loaded->sources
        );
        REQUIRE(compiled.has_value());

        auto const runtimeToml = readText(
            project.path() / "generated" / "annotations.runtime.toml"
        );
        CHECK(runtimeToml == compiled->runtimeManifestToml);

        auto const parsed = annotation::parseRuntimeManifest(runtimeToml);
        auto const parsedInfo = parsed
            ? std::string{"parsed"}
            : toString(parsed.error());
        INFO(parsedInfo);
        REQUIRE(parsed.has_value());

        // A manifest that parses but describes an earlier version of the
        // document would pass the two checks above only by accident, so the
        // published closure is measured: both anchors, each with its own
        // template asset installed where the manifest says it is.
        CHECK(parsed->catalog().recognizers().size() == 2U);
        REQUIRE(parsed->assets().size() == 2U);
        for (auto const& asset : parsed->assets())
        {
            CHECK(
                std::filesystem::is_regular_file(
                    project.path() / asset.templatePath
                )
            );
        }

        // The keyed and unkeyed anchors cover the same rectangle of the same
        // screen, so their templates differ only by the baked mask. Identical
        // hashes here would mean the key never reached the compiler.
        CHECK(parsed->assets()[0].templateHash != parsed->assets()[1].templateHash);
    }

    TEST_CASE("authoring CLI match separates a keyed recognizer from an unkeyed one")
    {
        auto const project = TemporaryProject{"match"};
        auto const frames  = authorKeyedProject(project);

        auto const keyed = run(
            "match",
            project.text(),
            "keyed_menu",
            "--frame",
            frames.purpleFrame.string()
        );
        requireOk(keyed);

        auto const unkeyed = run(
            "match",
            project.text(),
            "unkeyed_menu",
            "--frame",
            frames.purpleFrame.string()
        );
        requireOk(unkeyed);

        // The whole point of the tool: the artwork under the glyphs changed
        // completely between the two captures, and only the recognizer whose
        // colour key selects the glyphs still matches. The two differ in
        // nothing else -- same source, same rectangle, same threshold, same
        // frame -- so this cannot pass for any other reason.
        CHECK(keyed.json.contains("\"hit\":true"));
        CHECK(unkeyed.json.contains("\"hit\":false"));

        // Both searched the same single candidate position, so the scores are a
        // measurement of the two images rather than of where the search landed.
        // The gap is what a mask selecting nothing could not produce: it would
        // score zero on both.
        auto const keyedScore   = unsignedField(keyed.json, "sad_score");
        auto const unkeyedScore = unsignedField(unkeyed.json, "sad_score");
        CHECK(keyedScore < 1'000);
        CHECK(unkeyedScore > 200'000);
        CHECK(keyed.json.contains("\"matched_rect\":{\"x\":0,\"y\":0"));

        // The frame the project was authored against still matches with the key
        // in place, so the miss above is about the changed artwork rather than
        // about a keyed template that matches nothing at all.
        auto const authored = run(
            "match",
            project.text(),
            "keyed_menu",
            "--frame",
            frames.blueFrame.string()
        );
        requireOk(authored);
        CHECK(authored.json.contains("\"hit\":true"));
    }

    TEST_CASE("authoring CLI match tells a search that did not finish from one that missed")
    {
        auto const project = TemporaryProject{"budget"};
        auto const frames  = authorBudgetProject(project);

        // Both ordinary search regions complete under the default budget, on a
        // frame that holds neither template, which is the case that walks every
        // candidate position instead of exiting early on an exact hit. A match on
        // one anchor evaluates both, so this one answer covers both geometries.
        auto const missed = run(
            "match",
            project.text(),
            "wide_label",
            "--frame",
            frames.otherFrame.string()
        );
        requireOk(missed);
        CHECK(missed.json.contains("\"hit\":false"));

        auto const spent = unsignedField(missed.json, "pixel_comparisons");
        CHECK(spent > 0);
        CHECK(spent <= cli::k_defaultPixelComparisonBudget);

        // The control on the miss: the frame the project was authored against
        // still hits, so the miss above is a measurement of two different images
        // rather than a project that can match nothing.
        auto const hit = run(
            "match",
            project.text(),
            "wide_label",
            "--frame",
            frames.authoredFrame.string()
        );
        requireOk(hit);
        CHECK(hit.json.contains("\"hit\":true"));

        // The guard is still real. A budget far below what the same search just
        // spent has to stop it: if it ran to completion anyway this would answer
        // ok with a miss, exactly like the run above.
        auto const tiny = uint64{1'000};
        REQUIRE(tiny < spent);
        auto const stopped = run(
            "match",
            project.text(),
            "wide_label",
            "--frame",
            frames.otherFrame.string(),
            "--budget",
            std::format("{}", tiny)
        );
        CHECK_FALSE(stopped.ok);
        CHECK(stopped.exitCode != cli::ExitCode::Success);
        CHECK(std::to_underlying(stopped.exitCode) != 0);

        // And the answer an agent reads is not the answer a miss produces. The
        // kind names a recognition that never finished, and the response names
        // the branch: observe again, rather than conclude the anchor is absent.
        // Both fields carry the wire spelling, which is the one every other JSON
        // surface answers with; the enumerator spelling appeared here alone.
        CHECK(stopped.json.contains("\"kind\":\"recognition_incomplete\""));
        CHECK(stopped.json.contains("\"response\":\"retry\""));
        CHECK(stopped.json.contains("budget exhausted"));
        CHECK_FALSE(stopped.json.contains("\"hit\":"));

        // The other half: the completed miss carries no failure classification at
        // all, so the two outcomes cannot be read as the same thing.
        CHECK_FALSE(missed.json.contains("recognition_incomplete"));
        CHECK_FALSE(missed.json.contains("\"response\":"));
        CHECK(missed.exitCode == cli::ExitCode::Success);
    }

    TEST_CASE("authoring CLI refuses invalid input and names what was wrong")
    {
        auto const project = TemporaryProject{"invalid"};
        auto const frames  = authorKeyedProject(project);

        SUBCASE("a rectangle that leaves the source")
        {
            auto const outside = run(
                "page",
                "add-anchor",
                project.text(),
                "menu",
                "off_screen",
                "--source",
                frames.blueFrame.string(),
                "--rect",
                "60,0,60,40"
            );
            CHECK_FALSE(outside.ok);
            CHECK(outside.exitCode != cli::ExitCode::Success);
            CHECK(std::to_underlying(outside.exitCode) != 0);
            CHECK(outside.message.contains("fit the project resolution"));

            // The control: the same command with a rectangle that does fit is
            // accepted, so the refusal is about the rectangle and not about the
            // command being unusable.
            requireOk(
                run(
                    "page",
                    "add-anchor",
                    project.text(),
                    "menu",
                    "on_screen",
                    "--source",
                    frames.blueFrame.string(),
                    "--rect",
                    "40,0,60,40"
                )
            );
        }

        SUBCASE("a malformed colour")
        {
            auto const truncated = run(
                "page",
                "add-anchor",
                project.text(),
                "menu",
                "bad_colour",
                "--source",
                frames.blueFrame.string(),
                "--rect",
                "0,0,10,10",
                "--key",
                "249,249"
            );
            CHECK_FALSE(truncated.ok);
            CHECK(std::to_underlying(truncated.exitCode) != 0);
            CHECK(truncated.message.contains("--key expects r,g,b"));

            auto const outOfRange = run(
                "page",
                "add-anchor",
                project.text(),
                "menu",
                "bad_channel",
                "--source",
                frames.blueFrame.string(),
                "--rect",
                "0,0,10,10",
                "--key",
                "249,300,249"
            );
            CHECK_FALSE(outOfRange.ok);
            CHECK(std::to_underlying(outOfRange.exitCode) != 0);
            CHECK(outOfRange.message.contains("colour key"));

            // The control: three channels in range are accepted.
            requireOk(
                run(
                    "page",
                    "add-anchor",
                    project.text(),
                    "menu",
                    "good_colour",
                    "--source",
                    frames.blueFrame.string(),
                    "--rect",
                    "0,0,10,10",
                    "--key",
                    "249,249,249"
                )
            );
        }

        SUBCASE("a page that does not exist")
        {
            auto const missing = run(
                "page",
                "add-anchor",
                project.text(),
                "no_such_page",
                "stray_anchor",
                "--source",
                frames.blueFrame.string(),
                "--rect",
                "0,0,10,10"
            );
            CHECK_FALSE(missing.ok);
            CHECK(std::to_underlying(missing.exitCode) != 0);
            CHECK(missing.message.contains("no page named \"no_such_page\""));

            // The control: the page the project does hold takes the same anchor.
            requireOk(
                run(
                    "page",
                    "add-anchor",
                    project.text(),
                    "menu",
                    "stray_anchor",
                    "--source",
                    frames.blueFrame.string(),
                    "--rect",
                    "0,0,10,10"
                )
            );
        }

        SUBCASE("a refused edit leaves the published project untouched")
        {
            auto const before = readText(
                project.path() / "generated" / "annotations.runtime.toml"
            );
            auto const refused = run(
                "page",
                "add-anchor",
                project.text(),
                "menu",
                "off_screen",
                "--source",
                frames.blueFrame.string(),
                "--rect",
                "60,0,60,40"
            );
            CHECK_FALSE(refused.ok);
            CHECK(
                readText(project.path() / "generated" / "annotations.runtime.toml")
                == before
            );
        }
    }

    TEST_CASE("frames probe reads a reliable anchor off two means")
    {
        auto const scratch = TemporaryProject{"frames-probe"};
        auto const frames  = writeCrops(scratch);

        auto const white = run(
            "frames",
            "probe",
            frames.blueFrame.string(),
            frames.purpleFrame.string(),
            "--rect",
            wholeCropRect(),
            "--key",
            "255,255,255"
        );
        requireOk(white);

        // The rect is about 90% artwork and the artwork changed completely
        // between the two captures, so the rect mean is the number the masked
        // mean has to be read against rather than a threshold of its own.
        auto const rectMean = numberField(white.json, "rect_mean_gray_spread");
        auto const whiteMean = numberField(
            white.json,
            "masked_mean_gray_spread"
        );
        CHECK(rectMean > 40.0);
        CHECK(whiteMean < 1.0);

        // A key that selects nothing reports a masked mean of zero and would
        // pass every check above, so what the key took is measured too.
        CHECK(unsignedField(white.json, "fully_selected_pixels") > 200);

        // The control, and the reverse answer: a key on the artwork under the
        // glyphs takes most of the rect and its masked mean lands above the
        // rect mean, because what it selected is exactly what moved. Both
        // probes report the same rect mean -- it does not depend on the key --
        // so the two masked means are comparable measurements of one rect.
        auto const artwork = run(
            "frames",
            "probe",
            frames.blueFrame.string(),
            frames.purpleFrame.string(),
            "--rect",
            wholeCropRect(),
            "--key",
            "26,39,73",
            "--tolerance",
            "60"
        );
        requireOk(artwork);
        CHECK(numberField(artwork.json, "rect_mean_gray_spread") == rectMean);
        CHECK(unsignedField(artwork.json, "fully_selected_pixels") > 2'000);
        CHECK(numberField(artwork.json, "masked_mean_gray_spread") > rectMean);
    }

    TEST_CASE("frames stability finds the glyphs and leaves the artwork out")
    {
        auto const scratch = TemporaryProject{"frames-stability"};
        auto const frames  = writeCrops(scratch);

        auto const whole = run(
            "frames",
            "stability",
            frames.blueFrame.string(),
            frames.purpleFrame.string()
        );
        requireOk(whole);

        // Without --rect the analysed rect is the whole first frame, which is
        // the question an author asks before they have a rectangle at all.
        CHECK(
            whole.json.contains(
                "\"rect\":{\"x\":0,\"y\":0,\"width\":100,\"height\":40}"
            )
        );
        CHECK(unsignedField(whole.json, "rect_pixels") == 4'000);

        // A little over a tenth of the rect holds still. Both bounds carry
        // weight: everything stable would mean the two captures were the same
        // image, and nothing stable would mean the text moved too.
        auto const stablePixels = unsignedField(whole.json, "stable_pixels");
        CHECK(stablePixels > 300);
        CHECK(stablePixels < 700);
        CHECK(numberField(whole.json, "mean_gray_spread") > 40.0);

        auto const regions = regionsJson(whole.json);
        auto const x       = unsignedField(regions, "x");
        auto const y       = unsignedField(regions, "y");
        auto const width   = unsignedField(regions, "width");
        auto const height  = unsignedField(regions, "height");

        // Bounds in frame coordinates, inside the analysed rect, which is what
        // makes a region readable straight back into --rect. Strictly inside on
        // both axes: a region that merely repeated the analysed rect would
        // answer nothing, and would pass a containment check alone.
        CHECK(x + width <= fixture::k_width);
        CHECK(y + height <= fixture::k_height);
        CHECK(width < fixture::k_width);
        CHECK(height < fixture::k_height);
        CHECK(unsignedField(regions, "stable_pixels") == stablePixels);

        // --gap cuts the one bounding box into the two glyphs, which is the
        // whole reason the flag exists: the merged box above spans the space
        // between them and is not a rectangle worth annotating.
        auto const cut = run(
            "frames",
            "stability",
            frames.blueFrame.string(),
            frames.purpleFrame.string(),
            "--gap",
            "3"
        );
        requireOk(cut);
        CHECK(occurrences(regionsJson(cut.json), "\"bounds\"") == 2U);
        CHECK(unsignedField(cut.json, "stable_pixels") == stablePixels);

        // The control on every number above: the same two frames over a strip
        // of pure artwork to the left of the glyphs hold nothing still at all.
        // Without it the counts above could be reporting two captures that were
        // simply alike.
        auto const artwork = run(
            "frames",
            "stability",
            frames.blueFrame.string(),
            frames.purpleFrame.string(),
            "--rect",
            "0,0,30,40"
        );
        requireOk(artwork);
        CHECK(unsignedField(artwork.json, "stable_pixels") == 0);
        CHECK(artwork.json.contains("\"regions\":[]"));
    }

    TEST_CASE("frames census names the colour the menu text is drawn in")
    {
        auto const scratch = TemporaryProject{"frames-census"};
        auto const frames  = writeCrops(scratch);

        auto const census = run(
            "frames",
            "census",
            frames.blueFrame.string(),
            "--rect",
            wholeCropRect(),
            "--top",
            "4"
        );
        requireOk(census);

        // The white the fixture measured its key at is the most frequent colour
        // in the crop, reported in the channel order --key takes. That is what
        // makes picking a key from this report work at all.
        CHECK(
            census.json.contains(
                std::format(
                    "\"dominant\":[{{\"red\":{},\"green\":{},\"blue\":{}",
                    fixture::k_textRed,
                    fixture::k_textGreen,
                    fixture::k_textBlue
                )
            )
        );
        CHECK(unsignedField(census.json, "distinct_colours") > 100);
        CHECK(occurrences(census.json, "\"count\":") == 4U);

        // The control: the same frame over a strip that holds no text does not
        // report white at all, so the entry above is a measurement of the rect
        // rather than of the file.
        auto const artwork = run(
            "frames",
            "census",
            frames.blueFrame.string(),
            "--rect",
            "0,0,20,40",
            "--top",
            "1"
        );
        requireOk(artwork);
        CHECK(occurrences(artwork.json, "\"count\":") == 1U);
        CHECK_FALSE(
            artwork.json.contains("\"red\":255,\"green\":255,\"blue\":255")
        );
    }

    TEST_CASE("frames refuses malformed input and names what was wrong")
    {
        auto const scratch = TemporaryProject{"frames-invalid"};
        auto const frames  = writeCrops(scratch);

        SUBCASE("a frame that is not there")
        {
            auto const absent  = scratch.path() / "no-such-capture.png";
            auto const refused = run(
                "frames",
                "stability",
                frames.blueFrame.string(),
                absent.string()
            );
            CHECK_FALSE(refused.ok);
            CHECK(std::to_underlying(refused.exitCode) != 0);
            CHECK(refused.message.contains("no-such-capture.png"));

            // The control: the same command over two frames that do exist is
            // accepted, so the refusal is about the missing file.
            requireOk(
                run(
                    "frames",
                    "stability",
                    frames.blueFrame.string(),
                    frames.purpleFrame.string()
                )
            );
        }

        SUBCASE("a rect the frames do not contain")
        {
            auto const outside = run(
                "frames",
                "stability",
                frames.blueFrame.string(),
                frames.purpleFrame.string(),
                "--rect",
                "0,0,200,40"
            );
            CHECK_FALSE(outside.ok);
            CHECK(std::to_underlying(outside.exitCode) != 0);
            CHECK(outside.message.contains("outside extent 100x40"));

            // The control: the widest rect the frames do contain is accepted.
            requireOk(
                run(
                    "frames",
                    "stability",
                    frames.blueFrame.string(),
                    frames.purpleFrame.string(),
                    "--rect",
                    wholeCropRect()
                )
            );
        }

        SUBCASE("one frame where a comparison needs two")
        {
            auto const single = run(
                "frames",
                "stability",
                frames.blueFrame.string()
            );
            CHECK_FALSE(single.ok);
            CHECK(std::to_underlying(single.exitCode) != 0);
            CHECK(single.message.contains("needs at least two"));

            auto const probe = run(
                "frames",
                "probe",
                frames.blueFrame.string(),
                "--rect",
                wholeCropRect(),
                "--key",
                "255,255,255"
            );
            CHECK_FALSE(probe.ok);
            CHECK(std::to_underlying(probe.exitCode) != 0);
            CHECK(probe.message.contains("needs at least two"));

            // The control: a census counts one frame's colours and takes the
            // single frame the two scans above refused, so the refusal is about
            // what a comparison needs rather than about one path being unusable.
            requireOk(
                run(
                    "frames",
                    "census",
                    frames.blueFrame.string(),
                    "--rect",
                    wholeCropRect()
                )
            );
        }
    }
}
