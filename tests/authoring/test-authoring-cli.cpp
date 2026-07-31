// The colour-key crops the match cases need are the same two real screen
// captures the annotation join test measures, and they live with the workbench
// tests because the authoring end measured them first. Reaching across rather
// than copying four kilobytes of PNG is what keeps the three ends honest about
// the same pixels.
#include "../workbench/colour-key-fixture.hpp"

#include <command-runner.hpp>
#include <command.hpp>

#include <preview.hpp>
#include <project-persistence.hpp>
#include <run.hpp>

#include <annotation/authoring-compiler.hpp>
#include <annotation/authoring-document.hpp>
#include <annotation/catalog.hpp>
#include <annotation/runtime-manifest.hpp>

#include <core/error/result.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <image/png.hpp>

#include <doctest/doctest.h>

#include <algorithm>
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

        // The mask object on its own, so a count read out of it cannot be
        // answered by the rectangle the element echoes above it.
        [[nodiscard]]
        auto maskSection(std::string const& json) -> std::string
        {
            auto const at = json.find("\"mask\":");
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
                    "add",
                    project.text(),
                    "menu",
                    "unkeyed_menu",
                    "--capability",
                    "identify",
                    "--source",
                    blue.string(),
                    "--rect",
                    wholeCrop
                )
            );

            return KeyedFixture{.blueFrame = blue, .purpleFrame = purple};
        }

        // The one appearance every element this CLI draws carries. An element
        // authored here has exactly one, so a test that finds another number is
        // looking at a document the CLI did not write.
        [[nodiscard]]
        auto soleVariant(
            annotation::Element const& element
        ) -> annotation::Variant
        {
            REQUIRE(element.variants().size() == 1U);
            return element.variants().front();
        }

        [[nodiscard]]
        auto elementNamed(
            annotation::AuthoringDocument const& document,
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
        }

        [[nodiscard]]
        auto pageNamed(
            annotation::AuthoringDocument const& document,
            std::string_view name
        ) -> annotation::PageSignature const*
        {
            for (auto const& page : document.catalog().pages())
            {
                if (page.name().value() == name)
                {
                    return &page;
                }
            }
            return nullptr;
        }

        // Two pages, and one clickable element drawn on the first. This is the
        // shape `page reference` exists for: the second page shows the same
        // control, and the model wants one element referenced twice rather than
        // two elements searched twice.
        [[nodiscard]]
        auto authorTwoPageProject(TemporaryProject const& project) -> KeyedFixture
        {
            auto const blue   = project.path() / "menu-over-blue.png";
            auto const purple = project.path() / "menu-over-purple.png";
            writePng(blue, fixture::k_menuOverBlueArtwork);
            writePng(purple, fixture::k_menuOverPurpleArtwork);

            requireOk(
                run(
                    "project",
                    "init",
                    project.text(),
                    "--project-id",
                    "personal.two_page_cli",
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
                    "menu_mark",
                    "--source",
                    blue.string(),
                    "--rect",
                    wholeCropRect()
                )
            );
            requireOk(
                run(
                    "page",
                    "create",
                    project.text(),
                    "sortie",
                    "sortie_mark",
                    "--source",
                    purple.string(),
                    "--rect",
                    wholeCropRect()
                )
            );
            requireOk(
                run(
                    "page",
                    "add",
                    project.text(),
                    "menu",
                    "menu_button",
                    "--capability",
                    "interact",
                    "--rect",
                    "0,0,60,30"
                )
            );
            // A clickable control in two steps, which is what a clickable
            // control with stable pixels now costs. The first draws no
            // template: interact says WHERE a click may land, so --rect is the
            // element's region and nothing about the pixels inside it is
            // claimed. The second says they ARE stable and cuts one, inside
            // that same region.
            requireOk(
                run(
                    "element",
                    "appearance",
                    project.text(),
                    "menu_button",
                    "default",
                    "--source",
                    blue.string(),
                    "--rect",
                    "10,4,20,12"
                )
            );

            return KeyedFixture{.blueFrame = blue, .purpleFrame = purple};
        }

        // Two pages whose anchors are the SAME rectangle of two captures of one
        // screen, one keyed to the white menu text and one not.
        //
        // The key is what makes them confusable, and deliberately so: keying on
        // the text drops the artwork under it, which is exactly what lets that
        // template match the other capture too. So "menu" is identified by a
        // mark that is present on the sortie screen as well -- a real defect, of
        // the kind an author cannot see by looking at either screen.
        [[nodiscard]]
        auto authorConfusableProject(TemporaryProject const& project) -> KeyedFixture
        {
            auto const blue   = project.path() / "menu-over-blue.png";
            auto const purple = project.path() / "menu-over-purple.png";
            writePng(blue, fixture::k_menuOverBlueArtwork);
            writePng(purple, fixture::k_menuOverPurpleArtwork);

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
                    "personal.confusable_cli",
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
                    wholeCropRect(),
                    "--key",
                    whiteText,
                    "--tolerance",
                    std::format("{}", k_tolerance)
                )
            );
            requireOk(
                run(
                    "page",
                    "create",
                    project.text(),
                    "sortie",
                    "sortie_mark",
                    "--source",
                    purple.string(),
                    "--rect",
                    wholeCropRect()
                )
            );

            return KeyedFixture{.blueFrame = blue, .purpleFrame = purple};
        }

        // The developer's own measured case, in miniature: one control at one
        // rectangle whose pixels are one thing on one screen and another on the
        // next. Two elements bound by a naming convention is what it replaces,
        // and what made the host's judgment the script author's problem.
        //
        // The pin is a parameter because the only test worth writing here pins
        // the WRONG appearance: pinning the right one and watching it match
        // proves nothing a search that ignored the pin would not also pass.
        [[nodiscard]]
        auto authorPinnedProject(
            TemporaryProject const& project,
            std::string_view pin
        ) -> KeyedFixture
        {
            auto const frames = authorTwoPageProject(project);

            requireOk(
                run(
                    "page",
                    "add",
                    project.text(),
                    "menu",
                    "back",
                    "--capability",
                    "interact",
                    "--rect",
                    wholeCropRect()
                )
            );
            requireOk(
                run(
                    "element",
                    "appearance",
                    project.text(),
                    "back",
                    "on_blue",
                    "--source",
                    frames.blueFrame.string(),
                    "--rect",
                    wholeCropRect()
                )
            );
            requireOk(
                run(
                    "element",
                    "appearance",
                    project.text(),
                    "back",
                    "on_purple",
                    "--source",
                    frames.purpleFrame.string(),
                    "--rect",
                    wholeCropRect()
                )
            );
            requireOk(
                run(
                    "page",
                    "reference",
                    project.text(),
                    "sortie",
                    "back",
                    "--variant",
                    std::string{pin}
                )
            );
            return frames;
        }

        // The findings array on its own, so an element named in a cell above it
        // cannot answer for one the verdict never reported.
        [[nodiscard]]
        auto findingsJson(std::string const& json) -> std::string
        {
            auto const at = json.find("\"findings\":[");
            REQUIRE(at != std::string::npos);
            return json.substr(at);
        }

        // The grid cells one element contributed, cut out of the check document,
        // so a field read out of one row cannot be answered by another element's.
        // Brace-counted rather than cut at the first "}", because a cell's last
        // member is a nested rectangle.
        [[nodiscard]]
        auto cellsForElement(
            std::string const& json,
            std::string_view element
        ) -> std::vector<std::string>
        {
            auto const marker = std::format("{{\"element\":\"{}\",", element);

            auto rows = std::vector<std::string>{};
            auto at   = json.find(marker);
            while (at != std::string::npos)
            {
                auto depth = std::size_t{0};
                auto end   = at;
                while (end < json.size())
                {
                    if (json[end] == '{')
                    {
                        ++depth;
                    }
                    else if (json[end] == '}')
                    {
                        --depth;
                        if (depth == 0U)
                        {
                            break;
                        }
                    }
                    ++end;
                }
                REQUIRE(end < json.size());
                rows.emplace_back(json.substr(at, end - at + 1U));
                at = json.find(marker, end);
            }
            return rows;
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
                    "add",
                    project.text(),
                    "menu",
                    "wide_label",
                    "--capability",
                    "identify",
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
                "add",
                project.text(),
                "menu",
                "menu_button",
                "--capability",
                "interact",
                "--rect",
                "0,0,60,30"
            )
        );
        // The appearance carries every pixel fact, which is the round trip
        // being asserted: the second drawing verb has to write a key, a
        // threshold and a template rectangle that come back unchanged, exactly
        // as the first one does.
        requireOk(
            run(
                "element",
                "appearance",
                project.text(),
                "menu_button",
                "default",
                "--source",
                frames.blueFrame.string(),
                "--rect",
                "10,4,20,12",
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

        auto const* p_keyed = elementNamed(document, "keyed_menu");
        REQUIRE(p_keyed != nullptr);
        CHECK(p_keyed->capabilities().hasIdentify());
        CHECK_FALSE(p_keyed->capabilities().hasInteract());

        auto const keyedVariant = soleVariant(*p_keyed);
        CHECK(
            keyedVariant.templateRect()
            == *PixelRect::create(0, 0, fixture::k_width, fixture::k_height)
        );
        REQUIRE(keyedVariant.colourKey().has_value());
        CHECK(keyedVariant.colourKey()->red() == fixture::k_textRed);
        CHECK(keyedVariant.colourKey()->green() == fixture::k_textGreen);
        CHECK(keyedVariant.colourKey()->blue() == fixture::k_textBlue);
        CHECK(keyedVariant.colourKey()->tolerance() == k_tolerance);

        auto const* p_unkeyed = elementNamed(document, "unkeyed_menu");
        REQUIRE(p_unkeyed != nullptr);
        // The control on the key checks above: an element authored without
        // --key must come back carrying none, so a CLI that invented a key
        // would fail here rather than pass everything.
        CHECK_FALSE(soleVariant(*p_unkeyed).colourKey().has_value());

        auto const* p_target = elementNamed(document, "menu_button");
        REQUIRE(p_target != nullptr);
        CHECK(p_target->capabilities().hasInteract());
        CHECK_FALSE(p_target->capabilities().hasIdentify());
        CHECK_FALSE(p_target->capabilities().hasRead());
        CHECK(p_target->searchRoi() == *PixelRect::create(0, 0, 60, 30));

        auto const targetVariant = soleVariant(*p_target);
        CHECK(targetVariant.templateRect() == *PixelRect::create(10, 4, 20, 12));
        CHECK(targetVariant.threshold().basisPoints() == 8500);
        REQUIRE(targetVariant.colourKey().has_value());
        CHECK(targetVariant.colourKey()->red() == 249);
        CHECK(targetVariant.colourKey()->tolerance() == 8);

        // Every page-side use is a reference now, and the two anchors reach the
        // page through one as well: their references are what the signature is
        // derived from. A CLI that drew an element and dropped its reference
        // would still load an element and fail only here.
        REQUIRE(document.references().size() == 3U);
        auto const pageId = document.catalog().pages().front().id();
        for (auto const& reference : document.references())
        {
            CHECK(reference.pageId == pageId);
            CHECK(reference.holding == annotation::Holding::Owned);
        }

        auto const* p_targetReference = document.catalog().findReference(
            pageId,
            p_target->id()
        );
        REQUIRE(p_targetReference != nullptr);
        CHECK(p_targetReference->exercised.hasInteract());
        CHECK_FALSE(p_targetReference->exercised.hasIdentify());
    }

    TEST_CASE("authoring CLI draws one element that both identifies a page and is clicked")
    {
        auto const project = TemporaryProject{"capability-set"};
        auto const frames  = authorTwoPageProject(project);

        // The case the capability set exists for: one row of text that names
        // the page AND can be clicked. Two flags, one rectangle, one element.
        requireOk(
            run(
                "page",
                "add",
                project.text(),
                "menu",
                "story_label",
                "--capability",
                "identify",
                "--capability",
                "interact",
                "--source",
                frames.blueFrame.string(),
                "--rect",
                "40,4,30,12"
            )
        );

        auto const loaded = workbench::loadAuthoringProject(project.path());
        REQUIRE(loaded.has_value());
        auto const& document = loaded->document;

        auto const* p_dual = elementNamed(document, "story_label");
        REQUIRE(p_dual != nullptr);
        CHECK(p_dual->capabilities().hasIdentify());
        CHECK(p_dual->capabilities().hasInteract());
        CHECK_FALSE(p_dual->capabilities().hasRead());

        // One element and one appearance, not two of either. A CLI that took
        // only the last --capability would still pass the has-interact check
        // above; a CLI that drew the rectangle once per capability would leave
        // two elements matched twice a cycle, which is the cost this replaces.
        CHECK(document.elements().size() == 4U);
        CHECK(soleVariant(*p_dual).templateRect() == *PixelRect::create(40, 4, 30, 12));

        auto const* p_menu = pageNamed(document, "menu");
        REQUIRE(p_menu != nullptr);
        auto const* p_reference = document.catalog().findReference(
            p_menu->id(),
            p_dual->id()
        );
        REQUIRE(p_reference != nullptr);
        CHECK(p_reference->exercised.hasIdentify());
        CHECK(p_reference->exercised.hasInteract());
        CHECK(p_reference->holding == annotation::Holding::Owned);

        // And the signature the page derives from that reference names it, so
        // the identify half is load-bearing rather than a recorded flag.
        CHECK(std::ranges::contains(p_menu->required(), p_dual->id()));
    }

    TEST_CASE("authoring CLI reads the signature role off the identify capability")
    {
        auto const project = TemporaryProject{"signature-role"};
        auto const frames  = authorTwoPageProject(project);

        requireOk(
            run(
                "page",
                "add",
                project.text(),
                "sortie",
                "must_be_absent",
                "--capability",
                "identify:forbidden",
                "--source",
                frames.purpleFrame.string(),
                "--rect",
                "40,4,30,12"
            )
        );
        // The control, drawn the same way on the same page: without the suffix
        // the role is required, so the two land in different vectors of one
        // signature and a CLI that ignored the suffix would put both in the same.
        requireOk(
            run(
                "page",
                "add",
                project.text(),
                "sortie",
                "must_be_present",
                "--capability",
                "identify",
                "--source",
                frames.purpleFrame.string(),
                "--rect",
                "70,4,25,12"
            )
        );

        auto const loaded = workbench::loadAuthoringProject(project.path());
        REQUIRE(loaded.has_value());
        auto const& document = loaded->document;

        auto const* p_forbidden = elementNamed(document, "must_be_absent");
        auto const* p_required  = elementNamed(document, "must_be_present");
        REQUIRE(p_forbidden != nullptr);
        REQUIRE(p_required != nullptr);

        auto const* p_sortie = pageNamed(document, "sortie");
        REQUIRE(p_sortie != nullptr);
        CHECK(std::ranges::contains(p_sortie->forbidden(), p_forbidden->id()));
        CHECK_FALSE(std::ranges::contains(p_sortie->required(), p_forbidden->id()));
        CHECK(std::ranges::contains(p_sortie->required(), p_required->id()));
    }

    TEST_CASE("authoring CLI puts an already-authored element on a second page")
    {
        auto const project = TemporaryProject{"reference"};
        auto const frames  = authorTwoPageProject(project);

        auto const before = workbench::loadAuthoringProject(project.path());
        REQUIRE(before.has_value());
        auto const elementsBefore = before->document.elements().size();

        auto const referenced = run(
            "page",
            "reference",
            project.text(),
            "sortie",
            "menu_button",
            "--search-roi",
            "0,0,80,40"
        );
        requireOk(referenced);
        CHECK(referenced.json.contains("\"holding\":\"referenced\""));
        CHECK(unsignedField(referenced.json, "pages_referencing") == 2U);

        auto const loaded = workbench::loadAuthoringProject(project.path());
        REQUIRE(loaded.has_value());
        auto const& document = loaded->document;

        // No copy was minted. This is the whole claim: one element, one
        // template, two pages -- so correcting the pixels later corrects both.
        // A CLI that duplicated the element would pass every holding check
        // below and fail only here.
        CHECK(document.elements().size() == elementsBefore);

        auto const* p_button = elementNamed(document, "menu_button");
        auto const* p_menu   = pageNamed(document, "menu");
        auto const* p_sortie = pageNamed(document, "sortie");
        REQUIRE(p_button != nullptr);
        REQUIRE(p_menu != nullptr);
        REQUIRE(p_sortie != nullptr);

        auto const* p_home = document.catalog().findReference(
            p_menu->id(),
            p_button->id()
        );
        auto const* p_borrowed = document.catalog().findReference(
            p_sortie->id(),
            p_button->id()
        );
        REQUIRE(p_home != nullptr);
        REQUIRE(p_borrowed != nullptr);

        // Holding says which page the pixels belong to, which is the question
        // the flag it replaced could not answer. The home page keeps Owned and
        // the borrower is Referenced; two owners is what the catalog refuses.
        CHECK(p_home->holding == annotation::Holding::Owned);
        CHECK(p_borrowed->holding == annotation::Holding::Referenced);
        CHECK(p_borrowed->exercised.hasInteract());

        // The borrowed reference carries the region --search-roi named, not the
        // element's own, so a per-page refinement reaches the document.
        REQUIRE(p_borrowed->searchRoi.has_value());
        CHECK(*p_borrowed->searchRoi == *PixelRect::create(0, 0, 80, 40));
        CHECK(p_button->searchRoi() == *PixelRect::create(0, 0, 60, 30));

        // Locating it is page-scoped now, and two pages click it, so match has
        // to be told which. Nothing but --page can choose between them.
        auto const ambiguous = run(
            "match",
            project.text(),
            "menu_button",
            "--frame",
            frames.blueFrame.string()
        );
        CHECK_FALSE(ambiguous.ok);
        CHECK(ambiguous.message.contains("--page has to name which"));

        auto const located = run(
            "match",
            project.text(),
            "menu_button",
            "--frame",
            frames.blueFrame.string(),
            "--page",
            "menu"
        );
        requireOk(located);
        CHECK(located.json.contains("\"hit\":true"));
        CHECK(located.json.contains("\"page\":\"menu\""));
    }

    TEST_CASE("authoring CLI lets a second page say how it exercises a borrowed element")
    {
        auto const project = TemporaryProject{"reference-capabilities"};
        auto const frames  = authorTwoPageProject(project);

        // One drawn rectangle that names the page it was drawn on and can be
        // clicked there.
        requireOk(
            run(
                "page",
                "add",
                project.text(),
                "menu",
                "shared_mark",
                "--capability",
                "identify",
                "--capability",
                "interact",
                "--source",
                frames.blueFrame.string(),
                "--rect",
                "40,4,30,12"
            )
        );

        // The second page takes it into its own signature as evidence FOR
        // itself, and asks to click it as well.
        auto const required = run(
            "page",
            "reference",
            project.text(),
            "sortie",
            "shared_mark",
            "--capability",
            "identify:required",
            "--capability",
            "interact"
        );
        requireOk(required);
        CHECK(required.json.contains("\"holding\":\"referenced\""));
        CHECK(required.json.contains("\"identify\":{\"role\":\"required\"}"));

        // And a mark the menu page owns is evidence AGAINST sortie, because
        // sortie says so. One element, two pages, opposite roles: the sentence
        // a role carried on the element could not say.
        auto const forbidden = run(
            "page",
            "reference",
            project.text(),
            "sortie",
            "menu_mark",
            "--capability",
            "identify:forbidden"
        );
        requireOk(forbidden);
        CHECK(forbidden.json.contains("\"identify\":{\"role\":\"forbidden\"}"));

        auto const loaded = workbench::loadAuthoringProject(project.path());
        REQUIRE(loaded.has_value());
        auto const& document = loaded->document;

        auto const* p_shared   = elementNamed(document, "shared_mark");
        auto const* p_menuMark = elementNamed(document, "menu_mark");
        auto const* p_menu     = pageNamed(document, "menu");
        auto const* p_sortie   = pageNamed(document, "sortie");
        REQUIRE(p_shared != nullptr);
        REQUIRE(p_menuMark != nullptr);
        REQUIRE(p_menu != nullptr);
        REQUIRE(p_sortie != nullptr);

        // The two roles reach one page's signature pointing opposite ways. A
        // CLI that read the role but dropped the suffix would put both in the
        // required vector and fail here.
        CHECK(std::ranges::contains(p_sortie->required(), p_shared->id()));
        CHECK(std::ranges::contains(p_sortie->forbidden(), p_menuMark->id()));
        CHECK_FALSE(std::ranges::contains(p_sortie->required(), p_menuMark->id()));

        // And menu_mark still identifies its own page positively, so the role
        // belongs to the reference rather than to the element.
        CHECK(std::ranges::contains(p_menu->required(), p_menuMark->id()));

        // No copy was minted for either borrowing: four elements, the same four
        // the two pages were authored with plus shared_mark.
        CHECK(document.elements().size() == 4U);

        auto const* p_borrowed = document.catalog().findReference(
            p_sortie->id(),
            p_shared->id()
        );
        REQUIRE(p_borrowed != nullptr);
        CHECK(p_borrowed->holding == annotation::Holding::Referenced);
        CHECK(p_borrowed->exercised.hasIdentify());
        CHECK(p_borrowed->exercised.hasInteract());
        CHECK_FALSE(p_borrowed->exercised.hasRead());

        // Nothing narrowed the search here, so the page refines nothing and
        // keeps following the element's own region when a later correction
        // moves it. A CLI that seeded a copy of that rectangle onto the
        // reference would pass every check above and fail here -- and would
        // make identify unreachable altogether, since a reference that
        // identifies may refine no region at all.
        CHECK_FALSE(p_borrowed->searchRoi.has_value());
    }

    TEST_CASE("authoring CLI refuses references and capability sets the model cannot hold")
    {
        auto const project = TemporaryProject{"refusals"};
        auto const frames  = authorTwoPageProject(project);

        SUBCASE("a page may exercise only what the element declares")
        {
            auto const undeclared = run(
                "page",
                "reference",
                project.text(),
                "sortie",
                "menu_button",
                "--capability",
                "identify:required"
            );
            CHECK_FALSE(undeclared.ok);
            CHECK(std::to_underlying(undeclared.exitCode) != 0);
            // Which page, which use, which element, and what that element
            // actually declares. "Invalid" would leave an author guessing which
            // of the four to change.
            CHECK(
                undeclared.message.contains(
                    "page \"sortie\" would exercise identify on \"menu_button\", "
                    "which declares interact; a page exercises only what the "
                    "element declares"
                )
            );

            // The control on the same element and the same page: the use it
            // does declare is taken, so the refusal is about the capability
            // rather than about the pair.
            requireOk(
                run(
                    "page",
                    "reference",
                    project.text(),
                    "sortie",
                    "menu_button",
                    "--capability",
                    "interact"
                )
            );
        }

        SUBCASE("a signature role the vocabulary does not have, on the reference verb")
        {
            auto const sideways = run(
                "page",
                "reference",
                project.text(),
                "sortie",
                "menu_mark",
                "--capability",
                "identify:sideways"
            );
            CHECK_FALSE(sideways.ok);
            CHECK(
                sideways.message.contains(
                    "--capability identify takes :required or :forbidden"
                )
            );
        }

        SUBCASE("identify and a refined search region cannot be asked for together")
        {
            auto const both = run(
                "page",
                "reference",
                project.text(),
                "sortie",
                "menu_mark",
                "--capability",
                "identify:forbidden",
                "--search-roi",
                "0,0,40,20"
            );
            CHECK_FALSE(both.ok);
            CHECK(
                both.message.contains(
                    "--capability identify and --search-roi cannot be combined"
                )
            );

            // The control: the same reference without the region is taken, so
            // the refusal names the combination rather than either half.
            requireOk(
                run(
                    "page",
                    "reference",
                    project.text(),
                    "sortie",
                    "menu_mark",
                    "--capability",
                    "identify:forbidden"
                )
            );
        }

        SUBCASE("an element or a page the project does not hold")
        {
            auto const noElement = run(
                "page",
                "reference",
                project.text(),
                "sortie",
                "ghost_mark",
                "--capability",
                "interact"
            );
            CHECK_FALSE(noElement.ok);
            CHECK(
                noElement.message.contains(
                    "no element named \"ghost_mark\" is part of this project"
                )
            );

            auto const noPage = run(
                "page",
                "reference",
                project.text(),
                "ghost_page",
                "menu_button",
                "--capability",
                "interact"
            );
            CHECK_FALSE(noPage.ok);
            CHECK(
                noPage.message.contains(
                    "no page named \"ghost_page\" is part of this project"
                )
            );
        }

        SUBCASE("an element that only identifies is not placed, it is signed")
        {
            auto const refused = run(
                "page",
                "reference",
                project.text(),
                "sortie",
                "menu_mark"
            );
            CHECK_FALSE(refused.ok);
            CHECK(std::to_underlying(refused.exitCode) != 0);
            CHECK(refused.message.contains("only identifies a page"));

            // The control: the clickable element on the same page is taken, so
            // the refusal is about what the element declares.
            requireOk(
                run(
                    "page",
                    "reference",
                    project.text(),
                    "sortie",
                    "menu_button"
                )
            );

            // And a second reference from the same page is refused rather than
            // recorded twice.
            auto const again = run(
                "page",
                "reference",
                project.text(),
                "sortie",
                "menu_button"
            );
            CHECK_FALSE(again.ok);
            CHECK(again.message.contains("already on that page"));
        }

        SUBCASE("a capability the vocabulary does not have")
        {
            auto const unknown = run(
                "page",
                "add",
                project.text(),
                "menu",
                "mystery",
                "--capability",
                "click",
                "--source",
                frames.blueFrame.string(),
                "--rect",
                "0,0,10,10"
            );
            CHECK_FALSE(unknown.ok);
            CHECK(std::to_underlying(unknown.exitCode) != 0);
            CHECK(
                unknown.message.contains(
                    "--capability expects identify, interact or read"
                )
            );
        }

        SUBCASE("a signature role on a capability that has none")
        {
            auto const roled = run(
                "page",
                "add",
                project.text(),
                "menu",
                "roled",
                "--capability",
                "interact:required",
                "--source",
                frames.blueFrame.string(),
                "--rect",
                "0,0,10,10"
            );
            CHECK_FALSE(roled.ok);
            CHECK(roled.message.contains("takes no \":role\""));
        }

        SUBCASE("no capability at all, and one stated twice")
        {
            auto const none = run(
                "page",
                "add",
                project.text(),
                "menu",
                "purposeless",
                "--source",
                frames.blueFrame.string(),
                "--rect",
                "0,0,10,10"
            );
            CHECK_FALSE(none.ok);
            CHECK(none.message.contains("needs at least one --capability"));

            auto const twice = run(
                "page",
                "add",
                project.text(),
                "menu",
                "doubled",
                "--capability",
                "interact",
                "--capability",
                "interact",
                "--source",
                frames.blueFrame.string(),
                "--rect",
                "0,0,10,10"
            );
            CHECK_FALSE(twice.ok);
            CHECK(twice.message.contains("was given twice"));
        }

        SUBCASE("a page's first mark identifies it, so create takes no capability")
        {
            auto const flagged = run(
                "page",
                "create",
                project.text(),
                "battle",
                "battle_mark",
                "--capability",
                "interact",
                "--source",
                frames.purpleFrame.string(),
                "--rect",
                "0,0,10,10"
            );
            CHECK_FALSE(flagged.ok);
            CHECK(flagged.message.contains("takes no --capability"));

            // The control: the same page without the flag is created, so the
            // refusal is about the flag rather than about the page.
            requireOk(
                run(
                    "page",
                    "create",
                    project.text(),
                    "battle",
                    "battle_mark",
                    "--source",
                    frames.purpleFrame.string(),
                    "--rect",
                    "0,0,10,10"
                )
            );
        }
    }

    TEST_CASE("authoring CLI authors a region the runtime reads rather than clicks")
    {
        auto const project = TemporaryProject{"read"};
        auto const frames  = authorTwoPageProject(project);

        requireOk(
            run(
                "page",
                "add",
                project.text(),
                "menu",
                "level_readout",
                "--capability",
                "read",
                "--rect",
                "70,20,25,12"
            )
        );

        auto const loaded = workbench::loadAuthoringProject(project.path());
        REQUIRE(loaded.has_value());
        auto const& document = loaded->document;

        auto const* p_readout = elementNamed(document, "level_readout");
        REQUIRE(p_readout != nullptr);
        CHECK(p_readout->capabilities().hasRead());
        CHECK_FALSE(p_readout->capabilities().hasIdentify());
        CHECK_FALSE(p_readout->capabilities().hasInteract());

        auto const* p_menu = pageNamed(document, "menu");
        REQUIRE(p_menu != nullptr);
        auto const* p_reference = document.catalog().findReference(
            p_menu->id(),
            p_readout->id()
        );
        REQUIRE(p_reference != nullptr);
        CHECK(p_reference->exercised.hasRead());
        CHECK_FALSE(p_reference->exercised.hasInteract());

        // A readout carries no template at all, which is the model's own rule:
        // sortie_level's value IS what is read, so a template of it would
        // require the level not to change in order to read the level.
        CHECK(p_readout->variants().empty());

        // Nothing to compare, so match refuses rather than answering with a
        // measurement it never made.
        auto const unmeasurable = run(
            "match",
            project.text(),
            "level_readout",
            "--frame",
            frames.blueFrame.string()
        );
        CHECK_FALSE(unmeasurable.ok);
        CHECK(unmeasurable.message.contains("declares no appearance"));

        // Give it one -- the model allows it, and it is what turns a rectangle
        // located by its page into one a find re-verifies -- and match refuses
        // for the other reason: reading happens inside a task that has already
        // resolved the page, so neither runtime entry point reaches it.
        requireOk(
            run(
                "element",
                "appearance",
                project.text(),
                "level_readout",
                "default",
                "--source",
                frames.blueFrame.string(),
                "--rect",
                "70,20,25,12"
            )
        );
        auto const matched = run(
            "match",
            project.text(),
            "level_readout",
            "--frame",
            frames.blueFrame.string()
        );
        CHECK_FALSE(matched.ok);
        CHECK(matched.message.contains("is only read"));
    }

    // A colour key has to select a FIGURE out of the rectangle it was drawn on.
    // The two ways it fails to are opposite in size and identical in effect, and
    // neither shows up at authoring time without this: too few pixels and the
    // mask matches any patch of that colour anywhere in the search region, too
    // many and it IS a patch of that colour. Both are warnings and not
    // refusals -- every element below is authored, saved and answers ok.
    TEST_CASE("the drawing verbs warn about a mask that cannot measure anything")
    {
        auto const project = TemporaryProject{"mask-warning"};
        auto const frames  = writeCrops(project);

        requireOk(
            run(
                "project",
                "init",
                project.text(),
                "--project-id",
                "personal.mask_warning",
                "--resolution",
                std::format("{}x{}", fixture::k_width, fixture::k_height)
            )
        );

        auto const glyph = run(
            "page",
            "create",
            project.text(),
            "menu",
            "glyph",
            "--source",
            frames.blueFrame.string(),
            "--rect",
            wholeCropRect(),
            "--key",
            "255,255,255",
            "--tolerance",
            std::format("{}", k_tolerance)
        );
        requireOk(glyph);
        auto const glyphMask = maskSection(glyph.json);

        SUBCASE("the mask is the number frames probe reports for the same key")
        {
            // Not a golden constant on either side: the point is that the two
            // documents cannot disagree, because the drawing verb answers out
            // of the function the probe answers out of. A second count written
            // here would be exactly the drift this asserts against.
            auto const probe = run(
                "frames",
                "probe",
                frames.blueFrame.string(),
                frames.purpleFrame.string(),
                "--rect",
                wholeCropRect(),
                "--key",
                "255,255,255",
                "--tolerance",
                std::format("{}", k_tolerance)
            );
            requireOk(probe);
            CHECK(
                unsignedField(glyphMask, "fully_selected_pixels")
                == unsignedField(probe.json, "fully_selected_pixels")
            );
            CHECK(
                unsignedField(glyphMask, "rect_pixels")
                == unsignedField(probe.json, "rect_pixels")
            );
        }

        SUBCASE("white menu text over artwork draws no warning")
        {
            // The shape every element in the author's real project has: a few
            // hundred glyph pixels carved out of a rectangle that is mostly
            // artwork. A warning that fires here is one an author stops
            // reading, which costs more than no warning at all.
            CHECK(unsignedField(glyphMask, "fully_selected_pixels") > 50U);
            CHECK(numberField(glyphMask, "selected_fraction") < 0.5);
            CHECK(glyphMask.contains("\"warning\":null"));
        }

        SUBCASE("a key that selects a handful of pixels warns")
        {
            // Twenty white pixels of one glyph's edge, measured on this crop.
            // It is stable, it is well keyed, and it locates nothing.
            // Drawn as a second appearance, because that verb draws too and a
            // key is exactly as easy to get wrong there. "Add one more template
            // until something matches" is the cheapest wrong move the
            // multi-appearance model makes available, so the warning has to
            // reach it.
            auto const tiny = run(
                "element",
                "appearance",
                project.text(),
                "glyph",
                "tiny",
                "--source",
                frames.blueFrame.string(),
                "--rect",
                "16,0,24,20",
                "--key",
                "255,255,255",
                "--tolerance",
                std::format("{}", k_tolerance)
            );
            requireOk(tiny);

            auto const mask = maskSection(tiny.json);
            CHECK(unsignedField(mask, "fully_selected_pixels") < 50U);

            // Under the floor by count while far under the share limit, so the
            // only rule that can be speaking here is the floor.
            CHECK(numberField(mask, "selected_fraction") < 0.5);
            CHECK(mask.contains("a mask this small"));

            // Authored, not refused. The gate is `check`, which measures this
            // element against screens it must not match; this verb only says
            // what it drew.
            auto const loaded = workbench::loadAuthoringProject(project.path());
            REQUIRE(loaded.has_value());
            auto const* p_glyph = elementNamed(loaded->document, "glyph");
            REQUIRE(p_glyph != nullptr);
            CHECK(p_glyph->findVariant(*annotation::ResourceName::create("tiny")) != nullptr);
        }

        SUBCASE("a key that takes most of the rectangle warns")
        {
            // The artwork under the glyphs rather than the glyphs: 2944 of the
            // crop's 4000 pixels, all within tolerance of one navy. Every count
            // reads as excellent and the glyph-shaped holes carry no weight, so
            // any patch of that navy the same size matches it.
            auto const fill = run(
                "page",
                "add",
                project.text(),
                "menu",
                "fill",
                "--capability",
                "identify",
                "--source",
                frames.blueFrame.string(),
                "--rect",
                wholeCropRect(),
                "--key",
                "26,39,73",
                "--tolerance",
                "30"
            );
            requireOk(fill);

            auto const mask = maskSection(fill.json);

            // Far above the floor, so the only rule that can be speaking here
            // is the share limit.
            CHECK(unsignedField(mask, "fully_selected_pixels") > 50U);
            CHECK(numberField(mask, "selected_fraction") > 0.5);
            CHECK(mask.contains("a mask this large"));

            auto const loaded = workbench::loadAuthoringProject(project.path());
            REQUIRE(loaded.has_value());
            CHECK(elementNamed(loaded->document, "fill") != nullptr);
        }

        SUBCASE("an unkeyed rectangle has no mask to measure")
        {
            // Every pixel counts, so a share of 1.0 is what an unkeyed template
            // means rather than a mask that took the whole rectangle. Reporting
            // one would warn about every unkeyed element in the project.
            auto const plain = run(
                "page",
                "add",
                project.text(),
                "menu",
                "plain",
                "--capability",
                "identify",
                "--source",
                frames.blueFrame.string(),
                "--rect",
                wholeCropRect()
            );
            requireOk(plain);
            CHECK(maskSection(plain.json).starts_with("\"mask\":null"));
        }
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
                "add",
                project.text(),
                "menu",
                "off_screen",
                "--capability",
                "identify",
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
                    "add",
                    project.text(),
                    "menu",
                    "on_screen",
                    "--capability",
                    "identify",
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
                "add",
                project.text(),
                "menu",
                "bad_colour",
                "--capability",
                "identify",
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
                "add",
                project.text(),
                "menu",
                "bad_channel",
                "--capability",
                "identify",
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
                    "add",
                    project.text(),
                    "menu",
                    "good_colour",
                    "--capability",
                    "identify",
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
                "add",
                project.text(),
                "no_such_page",
                "stray_anchor",
                "--capability",
                "identify",
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
                    "add",
                    project.text(),
                    "menu",
                    "stray_anchor",
                    "--capability",
                    "identify",
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
                "add",
                project.text(),
                "menu",
                "off_screen",
                "--capability",
                "identify",
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

    TEST_CASE("check answers with the whole matrix and accepts a model that holds")
    {
        auto const project = TemporaryProject{"check-accepts"};
        static_cast<void>(authorTwoPageProject(project));

        auto const outcome = run("check", project.text());
        requireOk(outcome);

        CHECK(outcome.json.find("\"command\":\"check\"") != std::string::npos);
        CHECK(outcome.json.find("\"accepted\":true") != std::string::npos);
        CHECK(outcome.json.find("\"findings\":[]") != std::string::npos);

        // Three elements over two screens, measured on and off the screen each
        // belongs to. The off-diagonal cells are half the grid and the only half
        // that carries information.
        CHECK(occurrences(outcome.json, "\"subject\":\"element\"") == 6U);

        // The three statements the model can make, counted apart. Each mark is
        // required by its own page and stands in the way of the other's on the
        // other screen, so two cells read absent; menu_button takes part in no
        // signature, so nothing is claimed of it on the screen its page does
        // not own. That last cell is why this is not a bool: under absent a hit
        // is a defect to repair, under unclaimed a hit is the control genuinely
        // being on a screen no page's identity rests on, and there is nothing
        // to do. Collapsed to "expected_hit":false the two read alike.
        CHECK(occurrences(outcome.json, "\"expectation\":\"match\"") == 3U);
        CHECK(occurrences(outcome.json, "\"expectation\":\"absent\"") == 2U);
        CHECK(occurrences(outcome.json, "\"expectation\":\"unclaimed\"") == 1U);

        // The factor the separation rule was applied at travels with the answer,
        // so a stored result stays interpretable after the constant moves.
        CHECK(
            unsignedField(outcome.json, "separation_factor")
            == workbench::k_appearanceSeparationFactor
        );

        // Every screen resolved to the page recorded for it, which is the other
        // half of what the matrix measures.
        CHECK(occurrences(outcome.json, "\"outcome\":\"correct\"") == 2U);
    }

    TEST_CASE("check refuses a model whose mark matches a screen it does not belong to")
    {
        // The keyed mark drops the artwork under the menu text, so it matches
        // the OTHER capture of that screen too -- and the page it identifies is
        // therefore not identified. Nothing about either screen looks wrong on
        // its own; only searching the mark where it does not belong says so.
        auto const project = TemporaryProject{"check-refuses"};
        static_cast<void>(authorConfusableProject(project));

        auto const outcome = run("check", project.text());
        requireOk(outcome);

        CHECK(outcome.json.find("\"accepted\":false") != std::string::npos);

        auto const findings = findingsJson(outcome.json);
        CHECK(
            findings.find("\"kind\":\"wrong_outcome\",\"element\":\"keyed_menu\"")
            != std::string::npos
        );
        // The sound mark is not accused of anything.
        CHECK(findings.find("\"sortie_mark\"") == std::string::npos);
    }

    // The rectangle a battle screen's hand of cards occupies is a place a click
    // may land, and its contents are five different card faces one turn later.
    // Cutting a template of it states a stability it does not have, and pays for
    // the claim on every screen the falsification matrix searches. The model has
    // always been able to say so -- an element declaring no appearance is located
    // by the page being recognised -- and this is the command line reaching it.
    TEST_CASE("page add mints an appearance only where a capability needs pixels")
    {
        auto const project = TemporaryProject{"pixel-less"};
        auto const frames  = authorTwoPageProject(project);

        auto const drawn = run(
            "page",
            "add",
            project.text(),
            "menu",
            "hand_area",
            "--capability",
            "interact",
            "--rect",
            "0,20,60,20"
        );
        requireOk(drawn);

        // Nothing was cut and nothing was ingested, and the answer says both
        // rather than leaving them out: an agent reading this document finds
        // the same keys the drawing half answers with.
        CHECK(drawn.json.contains("\"variants\":[]"));
        CHECK(drawn.json.contains("\"mask\":null"));
        CHECK(drawn.json.contains("\"source_ingested\":false"));

        auto const loaded = workbench::loadAuthoringProject(project.path());
        REQUIRE(loaded.has_value());
        auto const& document = loaded->document;

        auto const* p_area = elementNamed(document, "hand_area");
        REQUIRE(p_area != nullptr);
        CHECK(p_area->variants().empty());

        // The rectangle landed on the ELEMENT's region rather than on this
        // page's reference. A reference's region is optional and means "this
        // page narrows the element's", so it needs one to narrow; leaving the
        // element's at the whole screen would say these pixels may be anywhere,
        // and any page referencing it without a refinement of its own would
        // locate it at the screen's centre.
        CHECK(p_area->searchRoi() == *PixelRect::create(0, 20, 60, 20));

        auto const* p_menu = pageNamed(document, "menu");
        REQUIRE(p_menu != nullptr);
        auto const* p_reference = document.catalog().findReference(
            p_menu->id(),
            p_area->id()
        );
        REQUIRE(p_reference != nullptr);
        CHECK_FALSE(p_reference->searchRoi.has_value());
        CHECK(p_reference->exercised.hasInteract());

        SUBCASE("every flag that describes a template is refused, and named")
        {
            auto const refused = run(
                "page",
                "add",
                project.text(),
                "menu",
                "another_area",
                "--capability",
                "interact",
                "--source",
                frames.blueFrame.string(),
                "--rect",
                "0,0,20,20",
                "--min-similarity-bp",
                "8500"
            );
            CHECK_FALSE(refused.ok);
            CHECK(refused.message.contains("--source"));
            CHECK(refused.message.contains("--min-similarity-bp"));
            CHECK(refused.message.contains("--rect is the whole of it"));

            // Refused rather than accepted and dropped, so nothing was written.
            auto const after = workbench::loadAuthoringProject(project.path());
            REQUIRE(after.has_value());
            CHECK(elementNamed(after->document, "another_area") == nullptr);
        }

        SUBCASE("match refuses it, because there is nothing to compare")
        {
            auto const matched = run(
                "match",
                project.text(),
                "hand_area",
                "--frame",
                frames.blueFrame.string(),
                "--page",
                "menu"
            );
            CHECK_FALSE(matched.ok);
            CHECK(matched.message.contains("declares no appearance"));
            CHECK(matched.message.contains("nothing to match"));
        }

        SUBCASE("check measures nothing about it, and says nothing about it")
        {
            auto const outcome = run("check", project.text());
            requireOk(outcome);
            CHECK(outcome.json.contains("\"accepted\":true"));
            CHECK(outcome.json.contains("\"findings\":[]"));

            // One row per screen, and every one of them empty of measurement:
            // no appearance answered and no score was taken. A template minted
            // here would put a name and a number in both, and would be searched
            // against every screen in the project to say something no capability
            // asked for.
            auto const rows = cellsForElement(outcome.json, "hand_area");
            CHECK(rows.size() == 2U);
            for (auto const& row : rows)
            {
                CHECK(row.contains("\"appearance\":null"));
                CHECK(row.contains("\"sad_score\":null"));
            }
        }
    }

    TEST_CASE("a page pins the appearance it expects, and the pin is what is searched")
    {
        SUBCASE("the appearance this page pins is the one that answers")
        {
            auto const project = TemporaryProject{"pin-right"};
            auto const frames  = authorPinnedProject(project, "on_purple");

            auto const located = run(
                "match",
                project.text(),
                "back",
                "--frame",
                frames.purpleFrame.string(),
                "--page",
                "sortie"
            );
            requireOk(located);
            CHECK(located.json.contains("\"hit\":true"));
            CHECK(located.json.contains("\"variant\":\"on_purple\""));
            CHECK(unsignedField(located.json, "sad_score") < 1'000);
        }

        SUBCASE("pinning the other one makes the same frame miss")
        {
            // The falsification. Same element, same frame, same command; only
            // the pin differs, and the answer flips. A pin the search ignored
            // would fold across both appearances, find the one cut from this
            // very screen, and report a hit here as well -- which is what a
            // test that only ever pinned the right one could never see.
            auto const project = TemporaryProject{"pin-wrong"};
            auto const frames  = authorPinnedProject(project, "on_blue");

            auto const located = run(
                "match",
                project.text(),
                "back",
                "--frame",
                frames.purpleFrame.string(),
                "--page",
                "sortie"
            );
            requireOk(located);
            CHECK(located.json.contains("\"hit\":false"));
            CHECK(located.json.contains("\"variant\":\"on_blue\""));
            CHECK(unsignedField(located.json, "sad_score") > 200'000);
        }

        SUBCASE("the page that owns the element still folds across both")
        {
            // Stated rather than left to be discovered: `page add` mints the
            // owning page's reference in the same edit that draws the element,
            // and `page reference` refuses a page that already has one, so the
            // owning page has no way to pin. It folds, which is correct but
            // costs a search per appearance.
            auto const project = TemporaryProject{"pin-owner"};
            auto const frames  = authorPinnedProject(project, "on_purple");

            auto const refused = run(
                "page",
                "reference",
                project.text(),
                "menu",
                "back",
                "--variant",
                "on_blue"
            );
            CHECK_FALSE(refused.ok);
            CHECK(refused.message.contains("already on that page"));

            auto const folded = run(
                "match",
                project.text(),
                "back",
                "--frame",
                frames.blueFrame.string(),
                "--page",
                "menu"
            );
            requireOk(folded);
            CHECK(folded.json.contains("\"hit\":true"));
            CHECK(folded.json.contains("\"variant\":\"on_blue\""));
        }
    }

    TEST_CASE("the appearance verbs refuse what is not theirs to state")
    {
        auto const project = TemporaryProject{"appearance-refusals"};
        auto const frames  = authorTwoPageProject(project);

        SUBCASE("an element the project does not hold")
        {
            auto const refused = run(
                "element",
                "appearance",
                project.text(),
                "nothing_here",
                "second",
                "--source",
                frames.blueFrame.string(),
                "--rect",
                "10,4,20,12"
            );
            CHECK_FALSE(refused.ok);
            CHECK(refused.message.contains("no element named \"nothing_here\""));
        }

        SUBCASE("a name the element already uses")
        {
            auto const refused = run(
                "element",
                "appearance",
                project.text(),
                "menu_button",
                "default",
                "--source",
                frames.purpleFrame.string(),
                "--rect",
                "10,4,20,12"
            );
            CHECK_FALSE(refused.ok);
            CHECK(refused.message.contains("already has an appearance named"));
        }

        SUBCASE("a capability, which belongs to the element")
        {
            auto const refused = run(
                "element",
                "appearance",
                project.text(),
                "menu_button",
                "second",
                "--capability",
                "read",
                "--source",
                frames.blueFrame.string(),
                "--rect",
                "10,4,20,12"
            );
            CHECK_FALSE(refused.ok);
            CHECK(refused.message.contains("takes no --capability"));
        }

        SUBCASE("a search region, which every appearance of the element shares")
        {
            auto const refused = run(
                "element",
                "appearance",
                project.text(),
                "menu_button",
                "second",
                "--source",
                frames.blueFrame.string(),
                "--rect",
                "10,4,20,12",
                "--search-roi",
                "0,0,100,40"
            );
            CHECK_FALSE(refused.ok);
            CHECK(refused.message.contains("takes no --search-roi"));
        }

        SUBCASE("a template the region the element was drawn in cannot hold")
        {
            // menu_button's region is 60x30, and a template is only ever
            // searched inside it, so a 100x40 one could never be found. The
            // region is the element's and this verb cannot widen it, so the
            // refusal is the model's own -- an appearance is a second look at
            // the same patch of pixels, not a second place to look.
            auto const refused = run(
                "element",
                "appearance",
                project.text(),
                "menu_button",
                "too_large",
                "--source",
                frames.blueFrame.string(),
                "--rect",
                wholeCropRect()
            );
            CHECK_FALSE(refused.ok);
            CHECK(refused.message.contains("must fit inside the element search_roi"));
        }

        SUBCASE("a pin naming an appearance the element does not declare")
        {
            auto const refused = run(
                "page",
                "reference",
                project.text(),
                "sortie",
                "menu_button",
                "--variant",
                "on_purple"
            );
            CHECK_FALSE(refused.ok);
            CHECK(refused.message.contains("has no appearance named \"on_purple\""));
            // And what it could have been, so the correction is one edit rather
            // than one edit plus a look at the document.
            CHECK(refused.message.contains("it declares: default"));
        }

        SUBCASE("a pin on a page that only takes the mark into its signature")
        {
            // The anchor pass runs before any page is known and folds across
            // every appearance whatever a reference says, so a page exercising
            // identify and nothing else has no search for a pin to bind.
            auto const refused = run(
                "page",
                "reference",
                project.text(),
                "sortie",
                "menu_mark",
                "--capability",
                "identify:required",
                "--variant",
                "default"
            );
            CHECK_FALSE(refused.ok);
            CHECK(refused.message.contains("no other search on this page"));
        }
    }
}
