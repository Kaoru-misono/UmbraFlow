#include "binding-fixture.hpp"

#include <task/framework-bundle.hpp>
#include <task/script-bindings.hpp>
#include <task/task-context.hpp>

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/space.hpp>

#include <engine/session.hpp>

#include <image/png.hpp>

#include <script/engine.hpp>

#include <trace/recorder.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// The layer-two authoring loop: measure a region of a live frame, put the crop
// in the project as a template asset, build the element that searches for it,
// claim what it should do, save, reload, and let the falsification matrix
// measure it.
//
// The round trip is the test: each step in isolation proves almost nothing, and
// the property is that what an agent authored through this module is a project
// the RUNTIME reads back and the matrix accepts. So a case authors, saves, drops
// the model, loads it from disk, and runs regress.check over the reloaded one.
namespace uf::task
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

            [[nodiscard]] auto path() const noexcept UF_LIFETIME_BOUND
                -> std::filesystem::path const&
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

            auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};
            REQUIRE(stream.is_open());
            // Byte by byte rather than one write of a recast pointer: a test is
            // not a place to open a cast boundary, and a PNG template is small.
            for (auto const value : bytes)
            {
                stream.put(static_cast<char>(value));
            }
            REQUIRE(stream.good());
        }

        // The screen the matrix measures against: the fixture's own three grey
        // pixels, stored under the content hash the oracle derives its path from.
        // A screen is a FILE the project holds, so the project file's `hash` and
        // the file's own bytes have to agree -- hence naming it by its hash.
        [[nodiscard]]
        auto seedScreen(std::filesystem::path const& root) -> std::string
        {
            auto const rgba = std::vector<std::byte>{
                asByte(k_targetAnchorGray),
                asByte(k_targetAnchorGray),
                asByte(k_targetAnchorGray),
                asByte(255),
                asByte(k_targetActionGray),
                asByte(k_targetActionGray),
                asByte(k_targetActionGray),
                asByte(255),
                asByte(0),
                asByte(0),
                asByte(0),
                asByte(255),
            };
            auto encoded = image::encodeRgbaPng("screen.png", 3, 1, rgba);
            REQUIRE(encoded.has_value());
            auto const hash = sha256(*encoded);
            REQUIRE(hash.has_value());

            auto const hex = hash->hex();
            writeFile(
                root / "assets" / "screens" / (hex + ".png"),
                std::span<std::byte const>{*encoded}
            );
            return hex;
        }

        // A project holding one screen, one element and one claim, so a case has
        // something to ADD to rather than something to invent whole. The existing
        // element is the anchor grey; the agent authors a second one.
        // `clickable` gives the anchor the interact capability and a page row
        // that exercises it, which is what makes the project have a rectangle a
        // click can press at all. Off by default because every other test here
        // is about identify.
        auto seedProject(
            std::filesystem::path const& root,
            std::string const& screenHash,
            bool clickable = false
        ) -> void
        {
            auto const anchor = encodedTemplate(k_targetAnchorGray);
            writeFile(
                root / "assets" / "templates" / (anchor.hash.hex() + ".png"),
                std::span<std::byte const>{anchor.pngBytes}
            );

            auto const model = std::string{}
                + "schema = \"umbraflow-project/l2-v1\"\n"
                + "\n"
                + "[[element]]\n"
                + "name = \"anchor\"\n"
                + (clickable
                       ? "capabilities = [\"identify\", \"interact\"]\n"
                       : "capabilities = [\"identify\"]\n")
                + "rect = [0, 0, 1, 1]\n"
                + "\n"
                + "[[appearance]]\n"
                + "element = \"anchor\"\n"
                + "source = \"assets/templates/" + anchor.hash.hex() + ".png\"\n"
                + "threshold = 10000\n"
                + "\n"
                + "[[page]]\n"
                + "name = \"home\"\n"
                + "\n"
                + "[[reference]]\n"
                + "page = \"home\"\n"
                + "element = \"anchor\"\n"
                + "holding = \"owned\"\n"
                + (clickable
                       ? "exercised = [\"identify\", \"interact\"]\n"
                       : "exercised = [\"identify\"]\n")
                + "identify = \"required\"\n"
                + "\n"
                + "[[screen]]\n"
                + "name = \"home_screen\"\n"
                + "hash = \"" + screenHash + "\"\n"
                + "\n"
                + "[[expect]]\n"
                + "screen = \"home_screen\"\n"
                + "element = \"anchor\"\n"
                + "state = \"match\"\n";

            writeFile(
                root / "page-model.toml",
                std::as_bytes(std::span{std::string_view{model}})
            );
        }

        // A project with two pages and a real edge in each direction, so a case
        // has something to REBUILD and something to remap. Everything is already
        // in the writer's canonical order, so the file is its own round-trip
        // baseline. The unknown keys are half the point: `mood` on the page and
        // `note` on its first reference are lines this build's parser does not
        // understand, and adding a reference rebuilds the page and every one of
        // its rows through constructors that were never handed them -- which is
        // exactly where they would be lost, silently.
        auto seedGraphProject(
            std::filesystem::path const& root,
            std::string const&           screenHash
        ) -> void
        {
            auto const anchor = encodedTemplate(k_targetAnchorGray);
            auto const action = encodedTemplate(k_targetActionGray);
            writeFile(
                root / "assets" / "templates" / (anchor.hash.hex() + ".png"),
                std::span<std::byte const>{anchor.pngBytes}
            );
            writeFile(
                root / "assets" / "templates" / (action.hash.hex() + ".png"),
                std::span<std::byte const>{action.pngBytes}
            );

            auto const model = std::string{}
                + "schema = \"umbraflow-project/l2-v1\"\n"
                + "\n"
                + "[[element]]\n"
                + "name = \"action\"\n"
                + "capabilities = [\"identify\", \"interact\"]\n"
                + "rect = [1, 0, 1, 1]\n"
                + "\n"
                + "[[appearance]]\n"
                + "element = \"action\"\n"
                + "source = \"assets/templates/" + action.hash.hex() + ".png\"\n"
                + "threshold = 10000\n"
                + "\n"
                + "[[element]]\n"
                + "name = \"anchor\"\n"
                + "capabilities = [\"identify\"]\n"
                + "rect = [0, 0, 1, 1]\n"
                + "\n"
                + "[[appearance]]\n"
                + "element = \"anchor\"\n"
                + "source = \"assets/templates/" + anchor.hash.hex() + ".png\"\n"
                + "threshold = 10000\n"
                + "\n"
                + "[[page]]\n"
                + "name = \"detail\"\n"
                + "\n"
                + "[[reference]]\n"
                + "page = \"detail\"\n"
                + "element = \"action\"\n"
                + "holding = \"referenced\"\n"
                + "exercised = [\"identify\"]\n"
                + "identify = \"required\"\n"
                + "\n"
                + "[[page]]\n"
                + "name = \"home\"\n"
                + "mood = \"cheerful\"\n"
                + "\n"
                + "[page.extra]\n"
                + "owner = \"kaoru\"\n"
                + "\n"
                + "[[reference]]\n"
                + "page = \"home\"\n"
                + "element = \"anchor\"\n"
                + "holding = \"owned\"\n"
                + "exercised = [\"identify\"]\n"
                + "identify = \"required\"\n"
                + "note = \"the top-left corner\"\n"
                + "\n"
                + "[reference.extra]\n"
                + "lane = 3\n"
                + "\n"
                + "[[reference]]\n"
                + "page = \"home\"\n"
                + "element = \"action\"\n"
                + "holding = \"owned\"\n"
                + "exercised = [\"identify\", \"interact\"]\n"
                + "identify = \"required\"\n"
                + "\n"
                + "[[edge]]\n"
                + "from = \"detail\"\n"
                + "to = [\"home\"]\n"
                + "via = \"key\"\n"
                + "via_key = \"Escape\"\n"
                + "kind = \"navigate\"\n"
                + "\n"
                + "[[edge]]\n"
                + "from = \"home\"\n"
                + "to = [\"detail\"]\n"
                + "via = \"click\"\n"
                + "via_element = \"action\"\n"
                + "kind = \"navigate\"\n"
                + "depth = 2\n"
                + "\n"
                + "[edge.extra]\n"
                + "hint = \"the tile\"\n"
                + "\n"
                + "[[screen]]\n"
                + "name = \"home_screen\"\n"
                + "hash = \"" + screenHash + "\"\n"
                + "\n"
                + "[[expect]]\n"
                + "screen = \"home_screen\"\n"
                + "element = \"anchor\"\n"
                + "state = \"match\"\n";

            writeFile(
                root / "page-model.toml",
                std::as_bytes(std::span{std::string_view{model}})
            );
        }

        // A project that says which schema it is and nothing else: the file a
        // target nobody has annotated yet starts life as. The single line is also
        // the round-trip baseline -- everything the case below compares against
        // came out of the agent's own save.
        //
        // The empty template directory is the host's rule rather than the
        // schema's: `project_write` refuses a name whose parent directory does not
        // exist, because proving a write stays inside the project means
        // canonicalising a parent that is really there. Laying out the skeleton is
        // therefore the CLI's job.
        auto seedEmptyProject(std::filesystem::path const& root) -> void
        {
            auto error = std::error_code{};
            std::filesystem::create_directories(
                root / "assets" / "templates",
                error
            );

            auto const model =
                std::string{"schema = \"umbraflow-project/l2-v1\"\n"};

            writeFile(
                root / "page-model.toml",
                std::as_bytes(std::span{std::string_view{model}})
            );
        }

        // A session over the fixture frame, on an ANNOTATION stream because a
        // crop writes an annotation.* line and no other stream may hold one.
        struct Harness final
        {
            std::unique_ptr<trace::TraceRecorder> recorder;
            Result<engine::EngineSession>         session;
        };

        [[nodiscard]]
        auto buildHarness() -> Harness
        {
            auto recorder = std::make_unique<trace::TraceRecorder>(
                std::make_unique<DiscardingTraceSink>(),
                k_fixtureRunId,
                k_fixtureGenerationId,
                trace::FrontEnd::Annotation
            );
            auto frames = std::vector<Frame>{};
            for (auto id = uint64{700}; id < 720U; ++id)
            {
                frames.emplace_back(
                    grayFrame(fixtureFingerprint(), resolvingPixels(), FrameId{id})
                );
            }
            auto session = engine::EngineSession::create(
                std::make_unique<FakeFrameSource>(std::move(frames)),
                std::make_unique<CountingActionSink>(),
                *recorder,
                baseConfig(fixtureFingerprint())
            );
            return Harness{
                .recorder = std::move(recorder),
                .session  = std::move(session),
            };
        }

        [[nodiscard]]
        auto runExploration(TaskContext& context, std::string_view source)
            -> Result<double>
        {
            auto engine = script::Engine::create(explorationVmConfig(context));
            REQUIRE(engine.has_value());
            auto answer = engine->runNumber(source, "annotation-routines");
            if (!answer)
            {
                MESSAGE("luau: ", std::string{answer.error().message()});
            }
            return answer;
        }

        TEST_CASE("An agent authors an element from a crop and the matrix accepts it")
        {
            auto const directory = TemporaryDirectory{"uf-scribe-round-trip"};
            auto const screenHash = seedScreen(directory.path());
            seedProject(directory.path(), screenHash);

            auto harness = buildHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{
                *std::move(harness.session),
                *harness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            // The whole loop as an agent drives it through the explore channel:
            // measure, author, claim, save. The threshold is the CALLER's -- 10000
            // basis points, an exact match -- because nothing in the framework is
            // entitled to decide what a one-solid-colour crop is worth.
            constexpr std::string_view author = R"lua(
                local built = project.load_project(ctx)

                local ticket = ctx:cycle_open()
                local measured = scribe.measure(
                    ctx,
                    ticket,
                    { x = 1, y = 0, width = 1, height = 1 }
                )
                ctx:cycle_close(ticket)

                -- The measurement is what the decision is made FROM, and it is
                -- visible at the call site rather than buried in the routine.
                if measured.stats.rect_pixels ~= 1 then return 0 end
                if measured.stats.distinct_colours ~= 1 then return 0 end
                if string.sub(measured.source, 1, 17) ~= "assets/templates/" then
                    return 0
                end

                local element = scribe.author_element(ctx, measured, {
                    name         = "action",
                    capabilities = { "identify", "interact" },
                    threshold    = 10000,
                })
                scribe.add_element(built, element)
                scribe.claim(built, "home_screen", element, "match")
                scribe.save(ctx, built)
                return 1
            )lua";

            auto const authored = runExploration(context, author);
            REQUIRE(authored.has_value());
            CHECK(*authored == doctest::Approx(1.0));

            // The template asset is a real file under the name its own hash gave
            // it, which is what a later load reads back.
            auto entries = std::vector<std::filesystem::path>{};
            for (
                auto const& entry :
                std::filesystem::directory_iterator{
                    directory.path() / "assets" / "templates"
                }
            )
            {
                entries.emplace_back(entry.path());
            }
            CHECK(entries.size() == 2U);

            // A FRESH VM over a FRESH session, so nothing the authoring run held
            // can carry the answer: everything below comes off the disk.
            auto reloadHarness = buildHarness();
            REQUIRE(reloadHarness.session.has_value());
            TaskContext reloaded{
                *std::move(reloadHarness.session),
                *reloadHarness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            constexpr std::string_view verify = R"lua(
                local built = project.load_project(ctx)

                local element = built.element_by_name["action"]
                if element == nil then return 0 end
                if #element.appearances ~= 1 then return 0 end
                if element.appearances[1].threshold ~= 10000 then return 0 end
                if not element.capabilities.interact then return 0 end
                if element.rect.x ~= 1 or element.rect.width ~= 1 then return 0 end

                -- The claim survived the save as well, which is what gives the
                -- matrix a sentence it can contradict.
                if #built.claims.expectations ~= 2 then return 0 end

                local verdict = regress.check(ctx, built)
                if not verdict.accepted then return 0 end
                if verdict.elements ~= 2 then return 0 end
                if verdict.claimed ~= 2 then return 0 end
                return 1
            )lua";

            auto const verified = runExploration(reloaded, verify);
            REQUIRE(verified.has_value());
            CHECK(*verified == doctest::Approx(1.0));
        }

        TEST_CASE("Writing one row keeps every field the rows beside it carried")
        {
            // The silent-deletion failure this whole file format exists to
            // prevent, at the one place it can still happen: `add_reference`
            // REBUILDS every row of the page it writes to, so a field the
            // rebuild does not carry is erased from a page nobody touched. It
            // has happened once -- six fields landed across the model in one
            // afternoon and none of them reached `scribe`.
            auto const directory = TemporaryDirectory{"uf-scribe-carries-fields"};
            auto const screenHash = seedScreen(directory.path());
            seedProject(directory.path(), screenHash);

            auto harness = buildHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{
                *std::move(harness.session),
                *harness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const result = runExploration(context, R"lua(
                local built = project.load_project(ctx)

                -- The fixture project already declares an element with a
                -- template; reusing its appearance keeps this case about the
                -- rebuild rather than about authoring pixels.
                local seeded = built.element_by_name["anchor"]
                local mark = model.Element.new{
                    name         = "mark",
                    capabilities = { "identify", "interact" },
                    rect         = { x = 0, y = 0, width = 2, height = 1 },
                    appearances  = {
                        {
                            name      = "lit",
                            source    = seeded.appearances[1].source,
                            template  = seeded.appearances[1].template,
                            threshold = 9000,
                        },
                    },
                }
                local caption = model.Element.new{
                    name               = "caption",
                    capabilities       = { "identify", "read" },
                    rect               = { x = 0, y = 1, width = 2, height = 1 },
                    expected_fragments = { "max HP", "up" },
                }
                scribe.add_element(built, mark)
                scribe.add_element(built, caption)

                -- One page carrying every field the four phase-B commits added,
                -- built directly so the rebuild below is the only thing that can
                -- lose one.
                scribe.add_page(built, {
                    name      = "fate",
                    overlay   = true,
                    catch_all = true,
                    over      = { "battle", "route" },
                    references = {
                        {
                            element           = mark,
                            holding           = "owned",
                            exercised         = { "identify", "interact" },
                            identify          = "required",
                            interact_requires = "lit",
                            pinned_appearance = "lit",
                        },
                    },
                })

                -- Writing a SECOND row is what rebuilds the first.
                scribe.add_reference(built, "fate", caption, {
                    holding           = "owned",
                    exercised         = { "identify" },
                    identify          = "required",
                    expected_presence = true,
                })

                local page = built.page_by_name["fate"]
                if page.catch_all ~= true then return 0 end
                if page.over == nil or #page.over ~= 2 then return 0 end
                if page.over[1] ~= "battle" then return 0 end

                local kept = model.Page.reference_for(page, mark)
                if kept == nil then return 0 end
                if kept.interact_requires ~= "lit" then return 0 end

                local added = model.Page.reference_for(page, caption)
                if added == nil then return 0 end
                if added.expected_presence ~= true then return 0 end

                -- The element the row points at is rebuilt too.
                if built.element_by_name["caption"].expected_fragments == nil then
                    return 0
                end
                if #built.element_by_name["caption"].expected_fragments ~= 2 then
                    return 0
                end

                -- And a `state` page, which the same rebuild path carries on a
                -- page that is not an overlay.
                scribe.add_page(built, {
                    name  = "map",
                    state = "route",
                    references = {
                        {
                            element   = mark,
                            holding   = "referenced",
                            exercised = { "identify" },
                            identify  = "required",
                        },
                    },
                })
                scribe.add_reference(built, "map", caption, {
                    holding   = "referenced",
                    exercised = { "identify" },
                    identify  = "required",
                })
                if built.page_by_name["map"].state ~= "route" then return 0 end
                return 1
            )lua");
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("A wake point survives the file and is refused where it presses")
        {
            // Waking a target no page can be recognised on is the one delivery
            // with no receipt behind it, so what stands in for the receipt is
            // that the coordinate provably does nothing. This is where "provably"
            // is made good: the claim is data in the file, and the matrix is what
            // contradicts it.
            auto const directory  = TemporaryDirectory{"uf-wake-point"};
            auto const screenHash = seedScreen(directory.path());
            seedProject(directory.path(), screenHash, true);

            auto harness = buildHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{
                *std::move(harness.session),
                *harness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            // The seeded project's one clickable rectangle is the anchor at
            // [0, 0, 1, 1], so (0, 0) presses it and (40, 40) does not. Written
            // as literals: deriving them from the model would move the test with
            // any change to the fixture and assert nothing about either verdict.
            constexpr std::string_view exercise = R"lua(
                local built = project.load_project(ctx)

                -- A project that declares none is not checked, and cannot wake.
                if built.wake_point ~= nil then return -1 end
                -- Opened outside the pcall and closed after it: a raise between
                -- the open and the close would strand the cycle, and a
                -- generation holds at most one.
                local ticket = ctx:cycle_open()
                local ok = pcall(observe.wake, ctx, ticket, built)
                ctx:cycle_close(ticket)
                if ok then return -2 end

                -- Authored onto the model and written back out. The file is the
                -- only place it can live: a wake point supplied per invocation
                -- would be neither reviewable nor checkable.
                local safe = table.clone(built)
                safe.wake_point = { x = 40, y = 40 }
                ctx:project_write(project.file_name, project.encode(safe))

                local reloaded = project.load_project(ctx)
                if reloaded.wake_point == nil then return -3 end
                if reloaded.wake_point.x ~= 40 then return -4 end
                if reloaded.wake_point.y ~= 40 then return -5 end

                -- Canonical bytes round-trip, so the new key does not make every
                -- later save a diff.
                if project.encode(reloaded) ~= ctx:project_read(project.file_name) then
                    return -6
                end

                -- The other half of the safety argument, and the one a project
                -- file cannot state: a wake is refused whenever the screen is
                -- legible, because then the caller has an ordinary path and a
                -- poke would be a click nothing authorised. The seeded frame
                -- carries the anchor, so `home` resolves.
                local live = ctx:cycle_open()
                local delivered = observe.wake(ctx, live, reloaded)
                ctx:cycle_close(live)
                if delivered then return -12 end

                -- Off every clickable rectangle: the matrix has nothing to say.
                local clean = regress.check(ctx, reloaded)
                for _, finding in clean.findings do
                    if finding.kind == "wake_point_presses_something" then
                        return -7
                    end
                end

                -- On top of the one element a page clicks: refused, by name.
                local pressing = table.clone(reloaded)
                pressing.wake_point = { x = 0, y = 0 }
                local verdict = regress.check(ctx, pressing)
                if verdict.accepted then return -8 end

                local named = false
                for _, finding in verdict.findings do
                    if finding.kind == "wake_point_presses_something" then
                        named = true
                        if finding.element ~= "anchor" then return -9 end
                        if finding.page ~= "home" then return -10 end
                    end
                end
                if not named then return -11 end
                return 1
            )lua";

            auto const result = runExploration(context, exercise);
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("An authoring routine states no threshold of its own")
        {
            auto const directory = TemporaryDirectory{"uf-scribe-threshold"};
            auto const screenHash = seedScreen(directory.path());
            seedProject(directory.path(), screenHash);

            auto harness = buildHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{
                *std::move(harness.session),
                *harness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            // A default here would be a decision nobody could argue with, because
            // nobody would see it in the project file or at the call site.
            constexpr std::string_view source = R"lua(
                local ticket = ctx:cycle_open()
                local measured = scribe.measure(
                    ctx,
                    ticket,
                    { x = 0, y = 0, width = 1, height = 1 }
                )
                ctx:cycle_close(ticket)

                local ok, err = pcall(function()
                    return scribe.author_element(ctx, measured, {
                        name         = "nameless",
                        capabilities = { "identify" },
                    })
                end)
                if ok then return 0 end
                if string.find(tostring(err), "threshold", 1, true) == nil then
                    return 0
                end
                return 1
            )lua";

            auto const result = runExploration(context, source);
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("A template asset is named by the bytes it holds")
        {
            auto const directory = TemporaryDirectory{"uf-scribe-naming"};
            auto const screenHash = seedScreen(directory.path());
            seedProject(directory.path(), screenHash);

            auto harness = buildHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{
                *std::move(harness.session),
                *harness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            // Two measurements of the same pixels name one file, so re-measuring
            // an element cannot litter the project with duplicates that differ in
            // nothing; two measurements of different pixels never collide.
            constexpr std::string_view source = R"lua(
                local ticket = ctx:cycle_open()
                local first  = scribe.measure(
                    ctx, ticket, { x = 1, y = 0, width = 1, height = 1 }
                )
                local again  = scribe.measure(
                    ctx, ticket, { x = 1, y = 0, width = 1, height = 1 }
                )
                local other  = scribe.measure(
                    ctx, ticket, { x = 0, y = 0, width = 1, height = 1 }
                )
                ctx:cycle_close(ticket)

                if first.source ~= again.source then return 0 end
                if first.source == other.source then return 0 end

                -- And a name that is not a content hash is refused outright.
                local ok = pcall(function()
                    return scribe.template_path("not-a-hash")
                end)
                if ok then return 0 end
                return 1
            )lua";

            auto const result = runExploration(context, source);
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("An agent cannot claim an element on a screen the project lacks")
        {
            auto const directory = TemporaryDirectory{"uf-scribe-claim"};
            auto const screenHash = seedScreen(directory.path());
            seedProject(directory.path(), screenHash);

            auto harness = buildHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{
                *std::move(harness.session),
                *harness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            constexpr std::string_view source = R"lua(
                local built  = project.load_project(ctx)
                local ticket = ctx:cycle_open()
                local measured = scribe.measure(
                    ctx, ticket, { x = 1, y = 0, width = 1, height = 1 }
                )
                ctx:cycle_close(ticket)

                local element = scribe.author_element(ctx, measured, {
                    name         = "action",
                    capabilities = { "identify" },
                    threshold    = 10000,
                })
                scribe.add_element(built, element)

                local ok, err = pcall(function()
                    scribe.claim(built, "no_such_screen", element, "match")
                end)
                if ok then return 0 end
                if string.find(tostring(err), "no_such_screen", 1, true) == nil then
                    return 0
                end

                -- And an element name the project already holds is refused while
                -- the caller can still rename it.
                local twiceOk = pcall(function()
                    scribe.add_element(built, element)
                end)
                if twiceOk then return 0 end
                return 1
            )lua";

            auto const result = runExploration(context, source);
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        // `scribe.add_screen` turns a name and the hash a capture already wrote
        // into a screen the rest of this module can address. Without it, pixels
        // already written to `assets/screens/<hash>.png` stay invisible to
        // `claim`/`claim_text` until somebody types a `[[screen]]` block by hand.
        TEST_CASE("An agent registers a captured screen and claims text about it")
        {
            auto const directory  = TemporaryDirectory{"uf-scribe-add-screen"};
            auto const screenHash = seedScreen(directory.path());
            seedEmptyProject(directory.path());

            auto harness = buildHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{
                *std::move(harness.session),
                *harness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            // The hash is spliced in as a Lua local up front, so the rest of the
            // chunk reads exactly as an agent would write it against a hash
            // `explore.crop` or a full-frame capture handed back.
            auto const author = std::string{"local screenHash = \""} + screenHash
                + "\"\n" + R"lua(
                local built = project.load_project(ctx)
                if #built.claims.screens ~= 0 then return 0 end

                -- No measurement and no ctx: this element verifies itself by
                -- what it reads, so there is nothing to cut out of the frame.
                local title = scribe.author_text_element({
                    name         = "title",
                    capabilities = { "identify", "read" },
                    rect         = { x = 0, y = 0, width = 2, height = 1 },
                })
                scribe.add_element(built, title)

                local screen = scribe.add_screen(built, {
                    name = "captured",
                    hash = screenHash,
                })
                if screen.name ~= "captured" then return 0 end
                if screen.hash ~= screenHash then return 0 end
                if screen.path ~= "assets/screens/" .. screenHash .. ".png" then
                    return 0
                end
                if built.claims.screen_by_name["captured"] ~= screen then
                    return 0
                end
                if #built.claims.screens ~= 1 then return 0 end

                scribe.claim_text(built, "captured", title, "match", "Sortie")
                if #built.claims.expectations ~= 1 then return 0 end

                scribe.save(ctx, built)
                return 1
            )lua";

            auto const authored = runExploration(context, author);
            REQUIRE(authored.has_value());
            CHECK(*authored == doctest::Approx(1.0));

            // A FRESH VM over a FRESH session: everything below comes off disk.
            auto reloadHarness = buildHarness();
            REQUIRE(reloadHarness.session.has_value());
            TaskContext reloaded{
                *std::move(reloadHarness.session),
                *reloadHarness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            auto const verify = std::string{"local screenHash = \""} + screenHash
                + "\"\n" + R"lua(
                local built = project.load_project(ctx)

                -- Byte-stable: what the agent saved is what this build writes
                -- again from what it read back.
                if project.encode(built) ~= ctx:project_read(project.file_name) then
                    return 0
                end

                local screen = built.claims.screen_by_name["captured"]
                if screen == nil then return 0 end
                if screen.hash ~= screenHash then return 0 end

                if oracle.Claims.text_for(built.claims, "captured", "title")
                    ~= "Sortie"
                then
                    return 0
                end
                if
                    oracle.Claims.state_for(built.claims, "captured", "title")
                    ~= oracle.state_match
                then
                    return 0
                end
                return 1
            )lua";

            auto const verified = runExploration(reloaded, verify);
            REQUIRE(verified.has_value());
            CHECK(*verified == doctest::Approx(1.0));
        }

        TEST_CASE("A duplicate screen name is refused before anything is touched")
        {
            auto const directory  = TemporaryDirectory{"uf-scribe-screen-refusal"};
            auto const screenHash = seedScreen(directory.path());
            seedGraphProject(directory.path(), screenHash);

            auto harness = buildHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{
                *std::move(harness.session),
                *harness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            constexpr std::string_view source = R"lua(
                local built = project.load_project(ctx)
                local claims = built.claims

                -- `home_screen` is the fixture's own, refused here while the
                -- caller can still choose another name -- `Claims.new` would
                -- catch it too, but only as a complaint about a list.
                local ok, err = pcall(function()
                    scribe.add_screen(built, {
                        name = "home_screen",
                        hash = string.rep("0", 64),
                    })
                end)
                if ok then return 0 end
                local taken = "already declares a screen named 'home_screen'"
                if string.find(tostring(err), taken, 1, true) == nil then
                    return 0
                end

                -- Nothing changed: the refusal lands before the model is
                -- touched, so the object the caller is holding is still current.
                if built.claims ~= claims then return 0 end
                if #built.claims.screens ~= 1 then return 0 end
                return 1
            )lua";

            auto const result = runExploration(context, source);
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        // The other door into a model, and the same refusal `project.build` makes
        // for a hand-edited file. `oracle.Screen.new` is handed a screen's own two
        // facts and can see no pages, so it cannot ask whether the page a screen
        // says it is of exists. Guarding only the file would leave a hole shaped
        // like this verb: a declaration `recognition` has no page to resolve, so
        // the screen's claim cannot be measured while every matrix cell passes.
        TEST_CASE("A screen naming a page the project does not declare is refused")
        {
            auto const directory  = TemporaryDirectory{"uf-scribe-screen-page"};
            auto const screenHash = seedScreen(directory.path());
            seedGraphProject(directory.path(), screenHash);

            auto harness = buildHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{
                *std::move(harness.session),
                *harness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            constexpr std::string_view source = R"lua(
                local built = project.load_project(ctx)
                local claims = built.claims

                local ok, err = pcall(function()
                    scribe.add_screen(built, {
                        name = "captured",
                        hash = string.rep("1", 64),
                        page = "no_such_page",
                    })
                end)
                if ok then return 0 end
                local missing = "declares no page named 'no_such_page'"
                if string.find(tostring(err), missing, 1, true) == nil then
                    return 0
                end
                -- The sentence tells an agent what to do, and it is holding the
                -- session that can do it.
                if string.find(tostring(err), "add the page first", 1, true) == nil then
                    return 0
                end

                -- Nothing changed: the refusal lands before the model is
                -- touched, so the object the caller is holding is still current.
                if built.claims ~= claims then return 0 end

                -- The same call naming a page the file DOES carry is accepted,
                -- and the screen comes back holding the name rather than the
                -- page: writing a reference rebuilds a page, and a name is what
                -- always resolves to the one the model holds.
                local screen = scribe.add_screen(built, {
                    name = "captured",
                    hash = string.rep("1", 64),
                    page = "home",
                })
                if screen.page ~= "home" then return 0 end
                if built.claims.screen_by_name["captured"].page ~= "home" then
                    return 0
                end

                -- And a screen that says nothing about its page is still a
                -- screen: a capture of a page nobody has annotated yet is the
                -- first thing a session obtains.
                local plain = scribe.add_screen(built, {
                    name = "unlabelled",
                    hash = string.rep("2", 64),
                })
                if plain.page ~= nil then return 0 end
                if #built.claims.screens ~= 3 then return 0 end
                return 1
            )lua";

            auto const result = runExploration(context, source);
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        // `add_screen` does not read the PNG. `Screen.new` checks the hash's SHAPE
        // only, and a hand-edited `[[screen]]` block is trusted the same way by
        // `project.build`; reading the file here too would make a screen's
        // measurability depend on HOW it was declared, and `regress.check` has to
        // re-check on every walk regardless, because a file present today can be
        // deleted before the next one. So a screen with no backing file registers
        // without complaint and is refused the first time anything tries to MEASURE
        // it, by the existence proof in `regress.luau`.
        TEST_CASE("A screen with no captured PNG is refused when the walk measures it")
        {
            auto const directory  = TemporaryDirectory{"uf-scribe-screen-no-png"};
            auto const screenHash = seedScreen(directory.path());
            seedGraphProject(directory.path(), screenHash);

            auto harness = buildHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{
                *std::move(harness.session),
                *harness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            constexpr std::string_view source = R"lua(
                local built = project.load_project(ctx)

                -- Shaped like a real content hash, but no capture ever wrote
                -- these bytes. add_screen restates none of Screen.new's
                -- rulings, and Screen.new checks only the hash's SHAPE, so this
                -- call succeeds: registering a screen nothing backs is
                -- deliberately not this file's refusal to make.
                scribe.add_screen(built, {
                    name = "ghost",
                    hash = string.rep("0", 64),
                })

                -- Uncaught on purpose: the missing file is refused by the WALK,
                -- the one place already responsible for reading it, and this
                -- case is about which of the two verbs says so.
                regress.check(ctx, built)
                return 0
            )lua";

            auto const result = runExploration(context, source);
            REQUIRE_FALSE(result.has_value());

            // Not "assets/screens/" with a forward slash: the store reports the
            // resolved OS path, so the separator is platform-native. "screens"
            // and the hash itself are what stay stable across platforms.
            auto const message = std::string{result.error().message()};
            CAPTURE(message);
            CHECK(message.find("screens") != std::string::npos);
            CHECK(message.find(std::string(64, '0')) != std::string::npos);
            CHECK(message.find("cannot inspect project file") != std::string::npos);
        }

        // The remap is the property. A page is frozen, so writing a reference
        // rebuilds it and every edge that named the old page is left naming an
        // object the model no longer has. That is invisible in the saved file -- a
        // stale edge still carries the right page NAME -- and visible only by
        // identity: `observe.walk_edge` resolves `edge.from` itself, so it would
        // keep resolving the superseded page and keep failing to find the element
        // the agent just annotated. The assertions below are identity assertions,
        // and they are what a rebuild that forgets the remap fails.
        TEST_CASE("A written reference carries the graph's edges with it")
        {
            auto const directory  = TemporaryDirectory{"uf-scribe-reference"};
            auto const screenHash = seedScreen(directory.path());
            seedGraphProject(directory.path(), screenHash);

            auto harness = buildHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{
                *std::move(harness.session),
                *harness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            constexpr std::string_view author = R"lua(
                local built = project.load_project(ctx)

                local ticket = ctx:cycle_open()
                local measured = scribe.measure(
                    ctx,
                    ticket,
                    { x = 1, y = 0, width = 1, height = 1 }
                )
                ctx:cycle_close(ticket)

                local enter = scribe.author_element(ctx, measured, {
                    name         = "enter",
                    capabilities = { "identify", "interact" },
                    threshold    = 10000,
                })
                scribe.add_element(built, enter)

                local stale = built.page_by_name["home"]
                local home  = scribe.add_reference(built, "home", enter, {
                    holding   = "owned",
                    exercised = { "identify", "interact" },
                    identify  = "required",
                })

                -- The page that came back is the page the model now holds, and
                -- it is not the one the caller was holding a line ago.
                if home == stale then return 0 end
                if built.page_by_name["home"] ~= home then return 0 end
                if built.graph.page_by_name["home"] ~= home then return 0 end
                if #home.references ~= 3 then return 0 end
                if #stale.references ~= 2 then return 0 end

                -- Both seeded edges name `home` -- one leaves it, one lands on
                -- it -- so a missing remap strands one of them either way.
                for _, edge in built.graph.edges do
                    if edge.from ~= built.graph.page_by_name[edge.from.name] then
                        return 0
                    end
                    for _, target in edge.to or {} do
                        if target ~= built.graph.page_by_name[target.name] then
                            return 0
                        end
                    end
                    -- A click edge resolved its row at construction, so the row
                    -- has to belong to the page the graph holds NOW.
                    if edge.via_reference ~= nil then
                        local holder = built.graph.page_by_name[edge.from.name]
                        local held   = false
                        for _, row in holder.references do
                            if row == edge.via_reference then held = true end
                        end
                        if not held then return 0 end
                    end
                    -- An edge rebuilt against the new page is still the edge it
                    -- was: a remap that minted a fresh one and dropped what the
                    -- old one carried would be the format's silent deletion
                    -- happening in memory instead of on disk.
                    if edge.from.name == "home" then
                        if edge.extra.hint ~= "the tile" then return 0 end
                        if #edge.residual ~= 1 then return 0 end
                        if edge.residual[1] ~= "depth = 2" then return 0 end
                    end
                end

                -- An edge drawn after the rebuild leaves the SAME page object,
                -- which is what `walk_edge` demands of the graph it was given.
                local edge = scribe.add_edge(built, {
                    from        = "home",
                    to          = { "detail" },
                    via         = "click",
                    via_element = "enter",
                    kind        = "navigate",
                })
                if edge.from ~= built.page_by_name["home"] then return 0 end
                if edge.to[1] ~= built.page_by_name["detail"] then return 0 end
                if not navigation.Graph.has_edge(built.graph, edge) then
                    return 0
                end
                if #built.graph.edges ~= 3 then return 0 end
                local row = model.Page.reference_for(
                    built.page_by_name["home"],
                    enter
                )
                if edge.via_reference ~= row then return 0 end

                scribe.save(ctx, built)
                return 1
            )lua";

            auto const authored = runExploration(context, author);
            REQUIRE(authored.has_value());
            CHECK(*authored == doctest::Approx(1.0));

            // A FRESH VM over a FRESH session: everything below comes off disk.
            auto reloadHarness = buildHarness();
            REQUIRE(reloadHarness.session.has_value());
            TaskContext reloaded{
                *std::move(reloadHarness.session),
                *reloadHarness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            constexpr std::string_view verify = R"lua(
                local built = project.load_project(ctx)

                -- Byte-stable: what this build wrote is what it writes again
                -- from what it read back.
                if project.encode(built) ~= ctx:project_read(project.file_name) then
                    return 0
                end

                local home = built.page_by_name["home"]
                if home == nil then return 0 end
                if #home.references ~= 3 then return 0 end

                local row = model.Page.reference_for(
                    home,
                    built.element_by_name["enter"]
                )
                if row == nil then return 0 end
                if row.holding ~= "owned" then return 0 end
                if not row.exercised.identify then return 0 end
                if not row.exercised.interact then return 0 end
                if row.identify ~= "required" then return 0 end
                if row.page ~= "home" then return 0 end

                -- The page's own unknown line and its project-owned field came
                -- back through the rebuild, and so did its first row's.
                if home.extra.owner ~= "kaoru" then return 0 end
                if #home.residual ~= 1 then return 0 end
                if home.residual[1] ~= 'mood = "cheerful"' then return 0 end

                local anchored = model.Page.reference_for(
                    home,
                    built.element_by_name["anchor"]
                )
                if anchored == nil then return 0 end
                if anchored.extra.lane ~= 3 then return 0 end
                if #anchored.residual ~= 1 then return 0 end
                if anchored.residual[1] ~= 'note = "the top-left corner"' then
                    return 0
                end

                -- Three edges, every one of them naming pages this reloaded
                -- graph holds.
                if #built.graph.edges ~= 3 then return 0 end
                local rebuilt = nil
                for _, edge in built.graph.edges do
                    if edge.from ~= built.graph.page_by_name[edge.from.name] then
                        return 0
                    end
                    for _, target in edge.to or {} do
                        if target ~= built.graph.page_by_name[target.name] then
                            return 0
                        end
                    end
                    if
                        edge.via_element ~= nil
                        and edge.via_element.name == "action"
                    then
                        rebuilt = edge
                    end
                end

                -- The edge the rebuild passed through wrote itself back whole.
                if rebuilt == nil then return 0 end
                if rebuilt.extra.hint ~= "the tile" then return 0 end
                if #rebuilt.residual ~= 1 then return 0 end
                if rebuilt.residual[1] ~= "depth = 2" then return 0 end
                return 1
            )lua";

            auto const verified = runExploration(reloaded, verify);
            REQUIRE(verified.has_value());
            CHECK(*verified == doctest::Approx(1.0));
        }

        // The page whose only signature is its own printed name. Every screen in
        // this target writes its name in the same top-left box, so the cheapest
        // honest annotation of a new page is one shared element with no template
        // plus a row per page saying what that box reads there. The SECOND row
        // written onto the same page is what rebuilds every row already on it, and
        // is therefore where the first row's own text would quietly be lost.
        TEST_CASE("An agent annotates a page by the name its title box reads")
        {
            auto const directory  = TemporaryDirectory{"uf-scribe-text-identify"};
            auto const screenHash = seedScreen(directory.path());
            seedGraphProject(directory.path(), screenHash);

            auto harness = buildHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{
                *std::move(harness.session),
                *harness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            constexpr std::string_view author = R"lua(
                local built = project.load_project(ctx)

                -- No measurement and no ctx: there is nothing to cut out of the
                -- frame, because this element's evidence is what it reads.
                local title = scribe.author_text_element({
                    name         = "title",
                    capabilities = { "identify", "read" },
                    rect         = { x = 0, y = 0, width = 2, height = 1 },
                })
                if #title.appearances ~= 0 then return 0 end
                if not title.capabilities.identify then return 0 end
                scribe.add_element(built, title)

                -- An element that searches for pixels is a different verb, and
                -- saying so at the call site beats a mode flag nobody reads.
                local ok = pcall(function()
                    return scribe.author_text_element({
                        name         = "wrong",
                        capabilities = { "identify", "read" },
                        rect         = { x = 0, y = 0, width = 1, height = 1 },
                        appearances  = {},
                    })
                end)
                if ok then return 0 end

                scribe.add_reference(built, "detail", title, {
                    holding       = "owned",
                    exercised     = { "identify" },
                    identify      = "required",
                    expected_text = "Detail",
                })

                -- The second row, which rebuilds the first one.
                scribe.add_reference(built, "detail", built.element_by_name["anchor"], {
                    holding   = "referenced",
                    exercised = { "identify" },
                    identify  = "required",
                })

                local page = built.page_by_name["detail"]
                local row  = model.Page.reference_for(page, title)
                if row == nil then return 0 end
                if row.expected_text ~= "Detail" then return 0 end

                -- THE LAST STEP, AND THE ONE THIS ELEMENT COULD NOT TAKE. A
                -- claim is what the matrix contradicts, and a region verified by
                -- what it reads is claimed by the reading rather than by a
                -- template score -- so the verb says which, exactly as
                -- `author_text_element` did at the other end of the session.
                scribe.claim_text(built, "home_screen", title, "match", "出擊")

                -- The template verb on the same element says nothing measurable:
                -- there is no template to search for and no score to be read
                -- against, so the model refuses it rather than filing a cell that
                -- is permanently green.
                local plainOk, plainErr = pcall(function()
                    scribe.claim(built, "home_screen", title, "match")
                end)
                if plainOk then return 0 end
                local unmeasurable = "no measurement this claim could ever be read against"
                if string.find(tostring(plainErr), unmeasurable, 1, true) == nil then
                    return 0
                end

                -- And the reading verb on an element that HAS pixels is refused
                -- from the other side, so neither verb is a superset of the other.
                local anchor = built.element_by_name["anchor"]
                local textOk, textErr = pcall(function()
                    scribe.claim_text(built, "home_screen", anchor, "match", "Detail")
                end)
                if textOk then return 0 end
                local byPixels = "verifies itself by its template pixels"
                if string.find(tostring(textErr), byPixels, 1, true) == nil then
                    return 0
                end

                -- A screen nobody captured is nothing to measure against, and the
                -- refusal is this file's own because only the model holds the
                -- screen inventory.
                local screenOk, screenErr = pcall(function()
                    scribe.claim_text(built, "nowhere", title, "match", "Detail")
                end)
                if screenOk then return 0 end
                if string.find(tostring(screenErr), "no screen named", 1, true) == nil then
                    return 0
                end

                -- Three refusals and one claim: the collection holds exactly the
                -- one that was accepted, because a rejected claim is rebuilt from
                -- nothing and never half applied.
                if #built.claims.expectations ~= 2 then return 0 end

                scribe.save(ctx, built)
                return 1
            )lua";

            auto const authored = runExploration(context, author);
            REQUIRE(authored.has_value());
            CHECK(*authored == doctest::Approx(1.0));

            // A FRESH VM over a FRESH session: everything below comes off disk.
            auto reloadHarness = buildHarness();
            REQUIRE(reloadHarness.session.has_value());
            TaskContext reloaded{
                *std::move(reloadHarness.session),
                *reloadHarness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            constexpr std::string_view verify = R"lua(
                local built = project.load_project(ctx)

                if project.encode(built) ~= ctx:project_read(project.file_name) then
                    return 0
                end

                local title = built.element_by_name["title"]
                if title == nil then return 0 end
                if #title.appearances ~= 0 then return 0 end
                if not title.capabilities.read then return 0 end
                if title.expected_text ~= nil then return 0 end

                local row = model.Page.reference_for(
                    built.page_by_name["detail"],
                    title
                )
                if row == nil then return 0 end
                if row.identify ~= "required" then return 0 end
                if row.expected_text ~= "Detail" then return 0 end
                if model.Reference.expected_text(row) ~= "Detail" then return 0 end

                -- The claim came back off disk under the cell the walk reads,
                -- with the multi-byte text the target actually prints intact.
                if oracle.Claims.text_for(built.claims, "home_screen", "title")
                    ~= "出擊"
                then
                    return 0
                end
                if
                    oracle.Claims.state_for(built.claims, "home_screen", "title")
                    ~= oracle.state_match
                then
                    return 0
                end

                return 1
            )lua";

            auto const verified = runExploration(reloaded, verify);
            REQUIRE(verified.has_value());
            CHECK(*verified == doctest::Approx(1.0));
        }

        // The shape that knows what it looks like and not where it is: a minimap
        // mark matched at coordinates the script works out per frame, a confirm
        // button that sits somewhere different on every screen showing it. Both
        // carry a template and neither carries a rectangle, which `author_element`
        // has no spelling for -- so one authored through that verb came out
        // carrying the rectangle it happened to be cropped from, which the model
        // then states as a fact and which is false.
        //
        // The refused page row mid-round-trip is the only thing that can tell "no
        // rectangle" from "the one it was cut from": an element carrying that
        // rectangle takes the row happily, loads, searches and matches, and is
        // wrong only in what it CLAIMS. So the case asks `Page.new` -- which
        // refuses a row that neither inherits a rectangle nor states one -- and
        // only then writes the row that says where.
        TEST_CASE("An agent authors a shape each row places for itself")
        {
            auto const directory  = TemporaryDirectory{"uf-scribe-unplaced"};
            auto const screenHash = seedScreen(directory.path());
            seedProject(directory.path(), screenHash);

            auto harness = buildHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{
                *std::move(harness.session),
                *harness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            constexpr std::string_view author = R"lua(
                local built = project.load_project(ctx)

                local ticket = ctx:cycle_open()
                local measured = scribe.measure(
                    ctx,
                    ticket,
                    { x = 1, y = 0, width = 1, height = 1 }
                )
                ctx:cycle_close(ticket)

                -- The same measurement, the same template asset and the same
                -- caller-chosen threshold `author_element` would have used. The
                -- one difference is the one the verb is named for.
                local shape = scribe.author_unplaced_element(ctx, measured, {
                    name         = "drifting",
                    capabilities = { "interact" },
                    threshold    = 10000,
                })
                if shape.rect ~= nil then return 0 end
                if #shape.appearances ~= 1 then return 0 end
                if shape.appearances[1].source ~= measured.source then return 0 end
                if shape.appearances[1].threshold ~= 10000 then return 0 end
                scribe.add_element(built, shape)

                -- A row that inherits no rectangle and states none has nowhere to
                -- look, and the sentence is `Page.new`'s. An element that had
                -- quietly kept the crop's rectangle would sail through here.
                local ok, err = pcall(function()
                    scribe.add_page(built, {
                        name       = "map",
                        references = {
                            {
                                element   = built.element_by_name["anchor"],
                                holding   = "referenced",
                                exercised = { "identify" },
                                identify  = "required",
                            },
                            {
                                element   = shape,
                                holding   = "owned",
                                exercised = { "interact" },
                            },
                        },
                    })
                end)
                if ok then return 0 end
                if string.find(tostring(err), "says no rect_override", 1, true) == nil then
                    return 0
                end
                if built.page_by_name["map"] ~= nil then return 0 end

                -- And the same page with the row that does say where.
                scribe.add_page(built, {
                    name       = "map",
                    references = {
                        {
                            element   = built.element_by_name["anchor"],
                            holding   = "referenced",
                            exercised = { "identify" },
                            identify  = "required",
                        },
                        {
                            element       = shape,
                            holding       = "owned",
                            exercised     = { "interact" },
                            rect_override = { x = 1, y = 0, width = 1, height = 1 },
                        },
                    },
                })

                -- Two claims about ONE element on ONE screen, which is legal only
                -- because the rectangle is part of the cell: the falsifiable form
                -- of "this shape is the one that drifts" is that it matches here
                -- and stays away from there.
                scribe.claim(built, "home_screen", shape, "match", nil, {
                    x = 1, y = 0, width = 1, height = 1,
                })
                scribe.claim(built, "home_screen", shape, "absent", nil, {
                    x = 0, y = 0, width = 1, height = 1,
                })

                scribe.save(ctx, built)
                return 1
            )lua";

            auto const authored = runExploration(context, author);
            REQUIRE(authored.has_value());
            CHECK(*authored == doctest::Approx(1.0));

            // The pixels went into the project under their own hash, exactly as
            // the placed verb stores them: what this element declines to state is
            // the position, never the crop.
            auto entries = std::vector<std::filesystem::path>{};
            for (
                auto const& entry :
                std::filesystem::directory_iterator{
                    directory.path() / "assets" / "templates"
                }
            )
            {
                entries.emplace_back(entry.path());
            }
            CHECK(entries.size() == 2U);

            // A FRESH VM over a FRESH session: everything below comes off disk.
            auto reloadHarness = buildHarness();
            REQUIRE(reloadHarness.session.has_value());
            TaskContext reloaded{
                *std::move(reloadHarness.session),
                *reloadHarness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            constexpr std::string_view verify = R"lua(
                local built = project.load_project(ctx)

                if project.encode(built) ~= ctx:project_read(project.file_name) then
                    return 0
                end

                -- The absence survived the file. A saved rectangle here would be
                -- the fact nothing can contradict, back again.
                local shape = built.element_by_name["drifting"]
                if shape == nil then return 0 end
                if shape.rect ~= nil then return 0 end
                if #shape.appearances ~= 1 then return 0 end
                if shape.appearances[1].threshold ~= 10000 then return 0 end

                local row = model.Page.reference_for(
                    built.page_by_name["map"],
                    shape
                )
                if row == nil then return 0 end
                if row.rect_override.x ~= 1 then return 0 end
                if row.rect_override.width ~= 1 then return 0 end

                local rects =
                    oracle.Claims.rects_for(built.claims, "home_screen", "drifting")
                if #rects ~= 2 then return 0 end

                local at = oracle.Claims.state_for
                if at(built.claims, "home_screen", "drifting", nil,
                    { x = 1, y = 0, width = 1, height = 1 }) ~= oracle.state_match
                then
                    return 0
                end
                if at(built.claims, "home_screen", "drifting", nil,
                    { x = 0, y = 0, width = 1, height = 1 }) ~= oracle.state_absent
                then
                    return 0
                end

                -- And the matrix measured it where each claim said, which is what
                -- makes the two sentences evidence rather than annotation.
                local verdict = regress.check(ctx, built)
                if not verdict.accepted then return 0 end
                if verdict.elements ~= 2 then return 0 end
                if verdict.claimed ~= 3 then return 0 end
                return 1
            )lua";

            auto const verified = runExploration(reloaded, verify);
            REQUIRE(verified.has_value());
            CHECK(*verified == doctest::Approx(1.0));
        }

        // The two sentences that keep the third verb from being the first one
        // with a field left out.
        TEST_CASE("The unplaced verb refuses a rectangle and still demands a threshold")
        {
            auto const directory  = TemporaryDirectory{"uf-scribe-unplaced-refusal"};
            auto const screenHash = seedScreen(directory.path());
            seedProject(directory.path(), screenHash);

            auto harness = buildHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{
                *std::move(harness.session),
                *harness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            constexpr std::string_view source = R"lua(
                local built = project.load_project(ctx)

                local ticket = ctx:cycle_open()
                local measured = scribe.measure(
                    ctx,
                    ticket,
                    { x = 1, y = 0, width = 1, height = 1 }
                )
                ctx:cycle_close(ticket)

                -- A caller that names a rectangle wanted the verb that USES one.
                -- Dropping it would author an element the line does not describe,
                -- and the line is the only place a reader ever looks.
                local ok, err = pcall(function()
                    return scribe.author_unplaced_element(ctx, measured, {
                        name         = "confused",
                        capabilities = { "interact" },
                        threshold    = 10000,
                        rect         = { x = 1, y = 0, width = 1, height = 1 },
                    })
                end)
                if ok then return 0 end
                local wrongVerb = "states no rectangle of its own, so its spec carries no rect"
                if string.find(tostring(err), wrongVerb, 1, true) == nil then
                    return 0
                end

                -- The threshold contract is the one this file already had, and
                -- the sentence names the verb that was actually called.
                local floorOk, floorErr = pcall(function()
                    return scribe.author_unplaced_element(ctx, measured, {
                        name         = "nameless",
                        capabilities = { "interact" },
                    })
                end)
                if floorOk then return 0 end
                local noDefault = "scribe.author_unplaced_element needs threshold"
                if string.find(tostring(floorErr), noDefault, 1, true) == nil then
                    return 0
                end

                -- Neither refusal reached the write: both land before the crop is
                -- stored, so a rejected line leaves the project as it was.
                if #built.elements ~= 1 then return 0 end
                return 1
            )lua";

            auto const refused = runExploration(context, source);
            REQUIRE(refused.has_value());
            CHECK(*refused == doctest::Approx(1.0));

            // The anchor's template and nothing else: a refused author writes no
            // asset, which is what makes the refusal free to retry.
            auto entries = std::vector<std::filesystem::path>{};
            for (
                auto const& entry :
                std::filesystem::directory_iterator{
                    directory.path() / "assets" / "templates"
                }
            )
            {
                entries.emplace_back(entry.path());
            }
            CHECK(entries.size() == 1U);
        }

        // The second face of one element, and the pages that have to follow it. An
        // element is frozen and a page's reference row holds the element ITSELF, so
        // adding an appearance rebuilds it and every page that named it has to be
        // re-pointed at the replacement. Skipping that half is invisible in the
        // file -- a row names its element by NAME -- and shows up only as a page
        // that goes on searching yesterday's appearance list.
        //
        // Hence an element built so its first appearance CANNOT match: the badge is
        // searched at the action-grey pixel and its first template was cut from the
        // black one, so a page still holding the old element finds NOTHING and a
        // page holding the new one finds the grey face by name. `#appearances == 2`
        // would pass with every page stranded; this cannot.
        TEST_CASE("A second appearance reaches every page that referenced the element")
        {
            auto const directory  = TemporaryDirectory{"uf-scribe-appearance"};
            auto const screenHash = seedScreen(directory.path());
            seedGraphProject(directory.path(), screenHash);

            auto harness = buildHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{
                *std::move(harness.session),
                *harness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            constexpr std::string_view author = R"lua(
                local built = project.load_project(ctx)

                -- Cut from the black pixel, searched at the grey one.
                local dark = ctx:cycle_open()
                local unseen = scribe.measure(
                    ctx,
                    dark,
                    { x = 2, y = 0, width = 1, height = 1 }
                )
                ctx:cycle_close(dark)

                local badge = scribe.author_element(ctx, unseen, {
                    name         = "badge",
                    capabilities = { "identify", "interact" },
                    rect         = { x = 1, y = 0, width = 1, height = 1 },
                    appearance   = "dark",
                    threshold    = 10000,
                })
                scribe.add_element(built, badge)
                scribe.add_reference(built, "home", badge, {
                    holding   = "owned",
                    exercised = { "interact" },
                })
                scribe.add_edge(built, {
                    from        = "home",
                    to          = { "detail" },
                    via         = "click",
                    via_element = "badge",
                    kind        = "navigate",
                })

                -- Nothing this element declares is where it is searched.
                local first = ctx:cycle_open()
                local before = observe.find(
                    ctx,
                    first,
                    built.page_by_name["home"],
                    built.element_by_name["badge"]
                )
                ctx:cycle_close(first)
                if before ~= nil then return 0 end

                local second = ctx:cycle_open()
                local seen = scribe.measure(
                    ctx,
                    second,
                    { x = 1, y = 0, width = 1, height = 1 }
                )
                ctx:cycle_close(second)

                local stale   = built.element_by_name["badge"]
                local rebuilt = scribe.add_appearance(ctx, built, "badge", seen, {
                    name      = "grey",
                    threshold = 10000,
                })

                -- The element that came back is the one the model now holds, and
                -- it is not the one the caller was holding a line ago.
                if rebuilt == stale then return 0 end
                if built.element_by_name["badge"] ~= rebuilt then return 0 end
                if #rebuilt.appearances ~= 2 then return 0 end
                if #stale.appearances ~= 1 then return 0 end
                if rebuilt.appearances[2].name ~= "grey" then return 0 end
                for _, element in built.elements do
                    if element.name == "badge" and element ~= rebuilt then
                        return 0
                    end
                end

                -- THE POINT. The page's own row holds the replacement, so the
                -- search the page authorises is over the appearance list the
                -- model now carries.
                local home = built.page_by_name["home"]
                local row  = model.Page.reference_for(home, rebuilt)
                if row == nil or row.element ~= rebuilt then return 0 end

                local third = ctx:cycle_open()
                local hit = observe.find(ctx, third, home, rebuilt)
                ctx:cycle_close(third)
                if hit == nil then return 0 end
                if hit.appearance ~= "grey" then return 0 end

                -- Every edge names the pages and the element this model holds
                -- NOW, and a click edge's row belongs to the page that holds it.
                for _, edge in built.graph.edges do
                    if edge.from ~= built.graph.page_by_name[edge.from.name] then
                        return 0
                    end
                    for _, target in edge.to or {} do
                        if target ~= built.graph.page_by_name[target.name] then
                            return 0
                        end
                    end
                    if edge.via_element ~= nil then
                        local named = built.element_by_name[edge.via_element.name]
                        if edge.via_element ~= named then return 0 end
                        local holder = built.graph.page_by_name[edge.from.name]
                        local held   = false
                        for _, held_row in holder.references do
                            if held_row == edge.via_reference then held = true end
                        end
                        if not held then return 0 end
                    end
                end

                -- The rebuild ran every row of the page through Page.new again,
                -- so this is where the page's own unknown line and its project
                -- field would have been lost.
                if home.extra.owner ~= "kaoru" then return 0 end
                if #home.residual ~= 1 then return 0 end
                if home.residual[1] ~= 'mood = "cheerful"' then return 0 end
                local anchored = model.Page.reference_for(
                    home,
                    built.element_by_name["anchor"]
                )
                if anchored == nil then return 0 end
                if anchored.extra.lane ~= 3 then return 0 end
                if anchored.residual[1] ~= 'note = "the top-left corner"' then
                    return 0
                end

                -- A claim about the face just added. Expectation.new resolves the
                -- appearance against the element it is handed, so this is a
                -- second reading on whether the caller holds the current one.
                scribe.claim(built, "home_screen", rebuilt, "match", "grey")
                scribe.save(ctx, built)
                return 1
            )lua";

            auto const authored = runExploration(context, author);
            REQUIRE(authored.has_value());
            CHECK(*authored == doctest::Approx(1.0));

            // A FRESH VM over a FRESH session: everything below comes off disk.
            auto reloadHarness = buildHarness();
            REQUIRE(reloadHarness.session.has_value());
            TaskContext reloaded{
                *std::move(reloadHarness.session),
                *reloadHarness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            constexpr std::string_view verify = R"lua(
                local built = project.load_project(ctx)

                -- Byte-stable: what this build wrote is what it writes again
                -- from what it read back.
                if project.encode(built) ~= ctx:project_read(project.file_name) then
                    return 0
                end

                local badge = built.element_by_name["badge"]
                if badge == nil then return 0 end
                if #badge.appearances ~= 2 then return 0 end
                if badge.appearances[1].name ~= "dark" then return 0 end
                if badge.appearances[2].name ~= "grey" then return 0 end
                if badge.appearances[2].threshold ~= 10000 then return 0 end
                if badge.rect.x ~= 1 then return 0 end

                local home = built.page_by_name["home"]
                local row  = model.Page.reference_for(home, badge)
                if row == nil or row.element ~= badge then return 0 end

                -- The claim naming this element still lines up with it, on the
                -- appearance that was added after the claim's own element was
                -- first written.
                local state = oracle.Claims.state_for(
                    built.claims,
                    "home_screen",
                    "badge",
                    "grey"
                )
                if state ~= oracle.state_match then return 0 end

                -- Reloaded from the file, the page still finds the second face.
                local ticket = ctx:cycle_open()
                local hit = observe.find(ctx, ticket, home, badge)
                ctx:cycle_close(ticket)
                if hit == nil then return 0 end
                if hit.appearance ~= "grey" then return 0 end

                local verdict = regress.check(ctx, built)
                if not verdict.accepted then return 0 end
                return 1
            )lua";

            auto const verified = runExploration(reloaded, verify);
            REQUIRE(verified.has_value());
            CHECK(*verified == doctest::Approx(1.0));
        }

        // An element may carry a nameless appearance -- `Element.new` validates a
        // name only when there is one -- and a claim that names no appearance is
        // about the FOLD. The two meet in the matrix: a nameless row has no cell
        // of its own, so reading the folded element's claim off it judges the row
        // against a sentence that was never about it.
        TEST_CASE("An unnamed appearance is judged against no claim of its own")
        {
            auto const directory  = TemporaryDirectory{"uf-regress-unnamed-hole"};
            auto const screenHash = seedScreen(directory.path());
            seedGraphProject(directory.path(), screenHash);

            auto harness = buildHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{
                *std::move(harness.session),
                *harness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            // The named face is cut from the pixel the element is searched at and
            // the nameless one from a pixel that is not there, so the fold hits
            // and the nameless row misses. Ask the nameless row for a state and
            // it reads the element's own "match", which nothing measured for it.
            constexpr std::string_view source = R"lua(
                local built = project.load_project(ctx)

                local seen = ctx:cycle_open()
                local grey = scribe.measure(
                    ctx,
                    seen,
                    { x = 1, y = 0, width = 1, height = 1 }
                )
                ctx:cycle_close(seen)

                local unseen = ctx:cycle_open()
                local dark = scribe.measure(
                    ctx,
                    unseen,
                    { x = 2, y = 0, width = 1, height = 1 }
                )
                ctx:cycle_close(unseen)

                -- Built through the constructor rather than through scribe:
                -- `scribe.add_appearance` requires a name, and a nameless one is
                -- exactly the state under test.
                local twin = model.Element.new{
                    name         = "twin",
                    capabilities = { "interact" },
                    rect         = { x = 1, y = 0, width = 1, height = 1 },
                    appearances  = {
                        {
                            name      = "grey",
                            source    = scribe.save_template(ctx, grey),
                            template  = ctx:template_load(grey.png),
                            threshold = 10000,
                        },
                        {
                            source    = scribe.save_template(ctx, dark),
                            template  = ctx:template_load(dark.png),
                            threshold = 10000,
                        },
                    },
                }
                scribe.add_element(built, twin)
                scribe.add_reference(built, "home", twin, {
                    holding   = "owned",
                    exercised = { "interact" },
                })
                -- One claim, on the FOLDED cell: no appearance names it.
                scribe.claim(built, "home_screen", twin, "match")

                local verdict = regress.check(ctx, built)
                if not verdict.accepted then return 0 end
                if #verdict.findings ~= 0 then return 0 end

                local folded = 0
                local nameless = 0
                for _, cell in verdict.cells do
                    if cell.element == "twin" then
                        if cell.subject == "element" then
                            folded += 1
                            if cell.verdict ~= "expected" then return 0 end
                        elseif cell.appearance == nil then
                            nameless += 1
                            if cell.verdict ~= "unclaimed" then return 0 end
                            if cell.outcome ~= "miss" then return 0 end
                        else
                            if cell.appearance ~= "grey" then return 0 end
                            if cell.verdict ~= "unclaimed" then return 0 end
                        end
                    end
                end
                if folded ~= 1 then return 0 end
                if nameless ~= 1 then return 0 end
                return 1
            )lua";

            auto const checked = runExploration(context, source);
            REQUIRE(checked.has_value());
            CHECK(*checked == doctest::Approx(1.0));
        }

        // The other half of the same fold: an "absent" claim the element
        // contradicts is ONE finding, about the element. Reading it off the
        // nameless row as well reports the same fact twice, under an appearance
        // name the author cannot look up.
        TEST_CASE("An unnamed appearance does not repeat the element's own misfire")
        {
            auto const directory  = TemporaryDirectory{"uf-regress-unnamed-misfire"};
            auto const screenHash = seedScreen(directory.path());
            seedGraphProject(directory.path(), screenHash);

            auto harness = buildHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{
                *std::move(harness.session),
                *harness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            constexpr std::string_view source = R"lua(
                local built = project.load_project(ctx)

                local seen = ctx:cycle_open()
                local grey = scribe.measure(
                    ctx,
                    seen,
                    { x = 1, y = 0, width = 1, height = 1 }
                )
                ctx:cycle_close(seen)

                local unseen = ctx:cycle_open()
                local dark = scribe.measure(
                    ctx,
                    unseen,
                    { x = 2, y = 0, width = 1, height = 1 }
                )
                ctx:cycle_close(unseen)

                -- The nameless face is the one that hits here, so the fold
                -- answers with it and the element contradicts its own claim.
                local twin = model.Element.new{
                    name         = "twin",
                    capabilities = { "interact" },
                    rect         = { x = 1, y = 0, width = 1, height = 1 },
                    appearances  = {
                        {
                            name      = "dark",
                            source    = scribe.save_template(ctx, dark),
                            template  = ctx:template_load(dark.png),
                            threshold = 10000,
                        },
                        {
                            source    = scribe.save_template(ctx, grey),
                            template  = ctx:template_load(grey.png),
                            threshold = 10000,
                        },
                    },
                }
                scribe.add_element(built, twin)
                scribe.add_reference(built, "home", twin, {
                    holding   = "owned",
                    exercised = { "interact" },
                })
                scribe.claim(built, "home_screen", twin, "absent")

                local verdict = regress.check(ctx, built)
                if verdict.accepted then return 0 end
                if #verdict.findings ~= 1 then return 0 end

                local finding = verdict.findings[1]
                if finding.kind ~= "misfire" then return 0 end
                if finding.element ~= "twin" then return 0 end
                if finding.appearance ~= nil then return 0 end
                return 1
            )lua";

            auto const checked = runExploration(context, source);
            REQUIRE(checked.has_value());
            CHECK(*checked == doctest::Approx(1.0));
        }

        // Adding an appearance hands the element's own fields back to
        // `Element.new`, so the rebuild is exactly where a project's `extra`, an
        // element's unknown line and an appearance's unknown line would fall out --
        // and the file would still parse, still load, and still match. So the seed
        // here carries one of each. Everything is already in the writer's canonical
        // order, so the file is its own round-trip baseline.
        TEST_CASE("A rebuilt element keeps the lines nobody was looking at")
        {
            auto const directory  = TemporaryDirectory{"uf-scribe-appearance-carry"};
            auto const screenHash = seedScreen(directory.path());

            auto const action = encodedTemplate(k_targetActionGray);
            writeFile(
                directory.path() / "assets" / "templates"
                    / (action.hash.hex() + ".png"),
                std::span<std::byte const>{action.pngBytes}
            );

            auto const seeded = std::string{}
                + "schema = \"umbraflow-project/l2-v1\"\n"
                + "\n"
                + "[[element]]\n"
                + "name = \"crest\"\n"
                + "capabilities = [\"identify\"]\n"
                + "rect = [1, 0, 1, 1]\n"
                + "hue = \"grey\"\n"
                + "\n"
                + "[element.extra]\n"
                + "lane = 3\n"
                + "\n"
                + "[[appearance]]\n"
                + "element = \"crest\"\n"
                + "name = \"first\"\n"
                + "source = \"assets/templates/" + action.hash.hex() + ".png\"\n"
                + "threshold = 10000\n"
                + "edge = \"soft\"\n"
                + "\n"
                + "[[page]]\n"
                + "name = \"home\"\n"
                + "\n"
                + "[[reference]]\n"
                + "page = \"home\"\n"
                + "element = \"crest\"\n"
                + "holding = \"owned\"\n"
                + "exercised = [\"identify\"]\n"
                + "identify = \"required\"\n"
                + "\n"
                + "[[screen]]\n"
                + "name = \"home_screen\"\n"
                + "hash = \"" + screenHash + "\"\n";

            writeFile(
                directory.path() / "page-model.toml",
                std::as_bytes(std::span{std::string_view{seeded}})
            );

            auto harness = buildHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{
                *std::move(harness.session),
                *harness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            constexpr std::string_view author = R"lua(
                local built = project.load_project(ctx)

                local ticket = ctx:cycle_open()
                local measured = scribe.measure(
                    ctx,
                    ticket,
                    { x = 2, y = 0, width = 1, height = 1 }
                )
                ctx:cycle_close(ticket)

                local rebuilt = scribe.add_appearance(ctx, built, "crest", measured, {
                    name      = "second",
                    threshold = 9000,
                })
                if #rebuilt.appearances ~= 2 then return 0 end

                -- The element's own project field and unknown line, and the
                -- unknown line on the appearance that was already there.
                if rebuilt.extra.lane ~= 3 then return 0 end
                if #rebuilt.residual ~= 1 then return 0 end
                if rebuilt.residual[1] ~= 'hue = "grey"' then return 0 end
                if #rebuilt.appearances[1].residual ~= 1 then return 0 end
                if rebuilt.appearances[1].residual[1] ~= 'edge = "soft"' then
                    return 0
                end
                -- And what the element already said about itself.
                if rebuilt.rect.x ~= 1 then return 0 end
                if not rebuilt.capabilities.identify then return 0 end

                scribe.save(ctx, built)
                return 1
            )lua";

            auto const authored = runExploration(context, author);
            REQUIRE(authored.has_value());
            CHECK(*authored == doctest::Approx(1.0));

            auto reloadHarness = buildHarness();
            REQUIRE(reloadHarness.session.has_value());
            TaskContext reloaded{
                *std::move(reloadHarness.session),
                *reloadHarness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            constexpr std::string_view verify = R"lua(
                local built = project.load_project(ctx)
                if project.encode(built) ~= ctx:project_read(project.file_name) then
                    return 0
                end

                local crest = built.element_by_name["crest"]
                if #crest.appearances ~= 2 then return 0 end
                if crest.appearances[2].threshold ~= 9000 then return 0 end
                if crest.extra.lane ~= 3 then return 0 end
                if crest.residual[1] ~= 'hue = "grey"' then return 0 end
                if crest.appearances[1].residual[1] ~= 'edge = "soft"' then
                    return 0
                end
                return 1
            )lua";

            auto const verified = runExploration(reloaded, verify);
            REQUIRE(verified.has_value());
            CHECK(*verified == doctest::Approx(1.0));
        }

        // Every way of adding an appearance nothing could address afterwards, as
        // one chunk returning a bitmask: a chunk that returned 0 for the first
        // failure would say a refusal is missing without saying which. Expected is
        // every bit set; a failure prints the number and the missing bit names the
        // rule.
        //
        //   1  an element name the project does not declare
        //   2  an appearance name the element already has
        //   4  no threshold, which this module never defaults
        //   8  no name, on the second face of an element
        //   16 a spec naming what the measurement settles
        //   32 an element with no appearances at all
        //   64 a superseded element handed back to add_reference
        //
        // The last is the other half of the re-pointing: `add_appearance` makes an
        // element stale, so a row written from one an agent was still holding is a
        // page searching an appearance list the file no longer carries -- and it
        // saves and loads correctly, so only identity sees it.
        TEST_CASE("Adding an appearance refuses what nothing could address")
        {
            auto const directory  = TemporaryDirectory{"uf-scribe-appearance-refusal"};
            auto const screenHash = seedScreen(directory.path());
            seedGraphProject(directory.path(), screenHash);

            auto harness = buildHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{
                *std::move(harness.session),
                *harness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            constexpr std::string_view source = R"lua(
                local built = project.load_project(ctx)

                local ticket = ctx:cycle_open()
                local measured = scribe.measure(
                    ctx,
                    ticket,
                    { x = 1, y = 0, width = 1, height = 1 }
                )
                ctx:cycle_close(ticket)

                -- A refusal only counts when it is the refusal that was meant,
                -- so each one is matched against a phrase only it says.
                local function refuses(bit, needle, body)
                    local ok, err = pcall(body)
                    if ok then return 0 end
                    if string.find(tostring(err), needle, 1, true) == nil then
                        return 0
                    end
                    return bit
                end

                local score = 0

                score += refuses(1, "declares no element named", function()
                    scribe.add_appearance(ctx, built, "nobody", measured, {
                        name      = "second",
                        threshold = 10000,
                    })
                end)

                score += refuses(2, "already has an appearance named", function()
                    scribe.add_appearance(ctx, built, "anchor", measured, {
                        name      = "plain",
                        threshold = 10000,
                    })
                    scribe.add_appearance(ctx, built, "anchor", measured, {
                        name      = "plain",
                        threshold = 10000,
                    })
                end)

                score += refuses(4, "it has no default", function()
                    scribe.add_appearance(ctx, built, "anchor", measured, {
                        name = "thresholdless",
                    })
                end)

                score += refuses(8, "unaddressable", function()
                    scribe.add_appearance(ctx, built, "anchor", measured, {
                        threshold = 10000,
                    })
                end)

                score += refuses(16, "someone else's picture", function()
                    scribe.add_appearance(ctx, built, "anchor", measured, {
                        name      = "captioned",
                        threshold = 10000,
                        source    = "assets/templates/elsewhere.png",
                    })
                end)

                -- An element positioned BY its page, which pixels would silently
                -- reposition everywhere it is used.
                local titled = scribe.author_text_element({
                    name          = "title",
                    capabilities  = { "read" },
                    expected_text = "Home",
                })
                scribe.add_element(built, titled)
                score += refuses(32, "declares no appearances", function()
                    scribe.add_appearance(ctx, built, "title", measured, {
                        name      = "printed",
                        threshold = 10000,
                    })
                end)

                -- The stale-element half: an element authored, filed, and then
                -- superseded by a successful add_appearance, still held in the
                -- variable the agent authored it into.
                local superseded = scribe.author_element(ctx, measured, {
                    name         = "mark",
                    capabilities = { "identify", "interact" },
                    appearance   = "first",
                    threshold    = 10000,
                })
                scribe.add_element(built, superseded)
                scribe.add_appearance(ctx, built, "mark", measured, {
                    name      = "second",
                    threshold = 10000,
                })
                score += refuses(64, "is not the one this project holds", function()
                    scribe.add_reference(built, "home", superseded, {
                        holding   = "referenced",
                        exercised = { "interact" },
                    })
                end)

                return score
            )lua";

            auto const refused = runExploration(context, source);
            REQUIRE(refused.has_value());
            CHECK(*refused == doctest::Approx(127.0));
        }

        TEST_CASE("A page row is refused before the project is touched")
        {
            auto const directory  = TemporaryDirectory{"uf-scribe-row-refusal"};
            auto const screenHash = seedScreen(directory.path());
            seedGraphProject(directory.path(), screenHash);

            auto harness = buildHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{
                *std::move(harness.session),
                *harness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            constexpr std::string_view source = R"lua(
                local built  = project.load_project(ctx)
                local home   = built.page_by_name["home"]
                local detail = built.page_by_name["detail"]
                local anchor = built.element_by_name["anchor"]

                -- One page uses one element in one way, so a second row for an
                -- element this page already references is refused.
                local ok, err = pcall(function()
                    scribe.add_reference(built, "home", anchor, {
                        holding   = "referenced",
                        exercised = { "identify" },
                        identify  = "forbidden",
                    })
                end)
                if ok then return 0 end
                local already = "already references element 'anchor'"
                if string.find(tostring(err), already, 1, true) == nil then
                    return 0
                end

                -- A page exercises a SUBSET of what an element declares, never
                -- more: `anchor` identifies and does nothing else.
                local capOk, capErr = pcall(function()
                    scribe.add_reference(built, "detail", anchor, {
                        holding   = "referenced",
                        exercised = { "identify", "interact" },
                        identify  = "required",
                    })
                end)
                if capOk then return 0 end
                if string.find(tostring(capErr), "does not declare it", 1, true) == nil then
                    return 0
                end

                -- A row with no page to sit on.
                local pageOk, pageErr = pcall(function()
                    scribe.add_reference(built, "no_such_page", anchor, {
                        holding   = "owned",
                        exercised = { "identify" },
                        identify  = "required",
                    })
                end)
                if pageOk then return 0 end
                if string.find(tostring(pageErr), "no_such_page", 1, true) == nil then
                    return 0
                end

                -- And a row naming an element the project does not carry, which
                -- would save a file that cannot be opened again.
                local stranger = model.Element.new({
                    name         = "stranger",
                    capabilities = { "interact" },
                    rect         = { x = 0, y = 0, width = 1, height = 1 },
                })
                local strangeOk, strangeErr = pcall(function()
                    scribe.add_reference(built, "detail", stranger, {
                        holding   = "referenced",
                        exercised = { "interact" },
                    })
                end)
                if strangeOk then return 0 end
                local missing = "declares no element named 'stranger'"
                if string.find(tostring(strangeErr), missing, 1, true) == nil then
                    return 0
                end

                -- Not one of the four changed anything: every refusal lands
                -- before the model is written to.
                if built.page_by_name["home"] ~= home then return 0 end
                if built.page_by_name["detail"] ~= detail then return 0 end
                if #home.references ~= 2 then return 0 end
                if #detail.references ~= 1 then return 0 end
                if #built.graph.edges ~= 2 then return 0 end
                return 1
            )lua";

            auto const result = runExploration(context, source);
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        TEST_CASE("An edge cannot leave by a click its page never authorised")
        {
            auto const directory  = TemporaryDirectory{"uf-scribe-edge-refusal"};
            auto const screenHash = seedScreen(directory.path());
            seedGraphProject(directory.path(), screenHash);

            auto harness = buildHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{
                *std::move(harness.session),
                *harness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            constexpr std::string_view source = R"lua(
                local built = project.load_project(ctx)
                local graph = built.graph

                -- `detail` does not reference `anchor` at all, so no edge
                -- leaving it can be taken by clicking one.
                local ok, err = pcall(function()
                    scribe.add_edge(built, {
                        from        = "detail",
                        to          = { "home" },
                        via         = "click",
                        via_element = "anchor",
                        kind        = "navigate",
                    })
                end)
                if ok then return 0 end
                if string.find(tostring(err), "does not reference", 1, true) == nil then
                    return 0
                end

                -- It does reference `action`, but without exercising interact,
                -- and a page that may not click an element cannot leave by it.
                local interactOk, interactErr = pcall(function()
                    scribe.add_edge(built, {
                        from        = "detail",
                        to          = { "home" },
                        via         = "click",
                        via_element = "action",
                        kind        = "navigate",
                    })
                end)
                if interactOk then return 0 end
                local unexercised = "without exercising interact"
                if string.find(tostring(interactErr), unexercised, 1, true) == nil then
                    return 0
                end

                -- A page name nothing declares, at either end.
                local fromOk = pcall(function()
                    scribe.add_edge(built, {
                        from = "nowhere",
                        to   = { "home" },
                        via  = "spontaneous",
                        kind = "navigate",
                    })
                end)
                if fromOk then return 0 end
                local toOk = pcall(function()
                    scribe.add_edge(built, {
                        from = "home",
                        to   = { "nowhere" },
                        via  = "spontaneous",
                        kind = "navigate",
                    })
                end)
                if toOk then return 0 end

                -- A page OBJECT rather than a name, which is the mistake the
                -- name-only spec exists to make unwritable: the object an agent
                -- is holding may already have been rebuilt out from under it.
                local objectOk, objectErr = pcall(function()
                    scribe.add_edge(built, {
                        from = built.page_by_name["home"],
                        to   = { "detail" },
                        via  = "spontaneous",
                        kind = "navigate",
                    })
                end)
                if objectOk then return 0 end
                if string.find(tostring(objectErr), "the NAME of a page", 1, true) == nil then
                    return 0
                end

                -- And a trigger `home` already leaves by, which only the
                -- collection can see.
                local twiceOk, twiceErr = pcall(function()
                    scribe.add_edge(built, {
                        from        = "home",
                        to          = { "detail" },
                        via         = "click",
                        via_element = "action",
                        kind        = "navigate",
                    })
                end)
                if twiceOk then return 0 end
                if string.find(tostring(twiceErr), "repeats a trigger", 1, true) == nil then
                    return 0
                end

                -- Nothing joined the graph, and the graph itself was not
                -- replaced by a half-built one.
                if built.graph ~= graph then return 0 end
                if #built.graph.edges ~= 2 then return 0 end
                return 1
            )lua";

            auto const result = runExploration(context, source);
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        // The first page. A page is born COMPLETE -- `Page.new` refuses one with no
        // required identify reference -- so the verb could not have been "make an
        // empty page and let `add_reference` fill it in"; there is no legal empty
        // page for that sequence to start from. So the case authors a whole model
        // from one schema line: three elements, two pages born with their rows, an
        // edge between them, and then a row written onto a page this run invented
        // -- which is where a page that reached the model without reaching the
        // graph, or the graph without the name index, is a red run.
        TEST_CASE("An agent authors the first page of a project that had none")
        {
            auto const directory = TemporaryDirectory{"uf-scribe-first-page"};
            seedEmptyProject(directory.path());

            auto harness = buildHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{
                *std::move(harness.session),
                *harness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            constexpr std::string_view author = R"lua(
                local built = project.load_project(ctx)
                if #built.elements ~= 0 then return 0 end
                if #built.pages ~= 0 then return 0 end
                if #built.graph.edges ~= 0 then return 0 end

                local ticket   = ctx:cycle_open()
                local measured = scribe.measure(
                    ctx,
                    ticket,
                    { x = 1, y = 0, width = 1, height = 1 }
                )
                ctx:cycle_close(ticket)

                local action = scribe.author_element(ctx, measured, {
                    name         = "action",
                    capabilities = { "identify", "interact" },
                    threshold    = 10000,
                })
                scribe.add_element(built, action)

                -- Two regions this target prints words in. Neither has pixels of
                -- its own, so what each one says is a fact about the PAGE and
                -- travels on the page's row rather than on the element.
                local title = scribe.author_text_element({
                    name         = "title",
                    capabilities = { "identify", "read" },
                    rect         = { x = 0, y = 0, width = 2, height = 1 },
                })
                local status = scribe.author_text_element({
                    name         = "status",
                    capabilities = { "identify", "read" },
                    rect         = { x = 0, y = 0, width = 3, height = 1 },
                })
                scribe.add_element(built, title)
                scribe.add_element(built, status)

                -- The page and its whole signature in one call, because half a
                -- signature is not a page this model will build.
                local home = scribe.add_page(built, {
                    name       = "home",
                    references = {
                        {
                            element       = title,
                            holding       = "owned",
                            exercised     = { "identify" },
                            identify      = "required",
                            expected_text = "Sortie",
                        },
                        {
                            element       = status,
                            holding       = "owned",
                            exercised     = { "identify" },
                            identify      = "required",
                            expected_text = "Ready",
                        },
                        {
                            element   = action,
                            holding   = "owned",
                            exercised = { "interact" },
                        },
                    },
                })

                -- All three places a model keeps its pages hold the page that
                -- came back, and the constructor stamped each row with it.
                if built.page_by_name["home"] ~= home then return 0 end
                if built.graph.page_by_name["home"] ~= home then return 0 end
                if #built.pages ~= 1 then return 0 end
                if #home.references ~= 3 then return 0 end
                for _, row in home.references do
                    if row.page ~= "home" then return 0 end
                end

                -- A second page, and `overlay` riding through this verb to the
                -- constructor: the push below is refused unless it arrives.
                local detail = scribe.add_page(built, {
                    name       = "detail",
                    overlay    = true,
                    references = {
                        {
                            element       = title,
                            holding       = "referenced",
                            exercised     = { "identify" },
                            identify      = "required",
                            expected_text = "Detail",
                        },
                    },
                })
                if not detail.overlay then return 0 end

                -- An edge over two pages that were invented a moment ago, taken
                -- by clicking a row one of them authorised. A page that never
                -- reached the name index fails at `from`, and one that never
                -- reached the graph fails inside `Graph.new`.
                local edge = scribe.add_edge(built, {
                    from        = "home",
                    to          = { "detail" },
                    via         = "click",
                    via_element = "action",
                    kind        = "push",
                })
                if edge.from ~= built.page_by_name["home"] then return 0 end
                if edge.to[1] ~= built.page_by_name["detail"] then return 0 end
                if edge.via_reference ~= model.Page.reference_for(home, action) then
                    return 0
                end

                -- A LATER row onto an invented page. It rebuilds that page, so
                -- the edge that already lands on it has to be carried across --
                -- the same remap the seeded projects exercise, over a page no
                -- file ever declared.
                local rebuilt = scribe.add_reference(built, "detail", action, {
                    holding   = "referenced",
                    exercised = { "interact" },
                })
                if rebuilt == detail then return 0 end
                if built.page_by_name["detail"] ~= rebuilt then return 0 end
                for _, existing in built.graph.edges do
                    local from = built.graph.page_by_name[existing.from.name]
                    if existing.from ~= from then return 0 end
                    for _, target in existing.to or {} do
                        if target ~= built.graph.page_by_name[target.name] then
                            return 0
                        end
                    end
                end

                scribe.save(ctx, built)
                return 1
            )lua";

            auto const authored = runExploration(context, author);
            REQUIRE(authored.has_value());
            CHECK(*authored == doctest::Approx(1.0));

            // A FRESH VM over a FRESH session: everything below comes off disk.
            auto reloadHarness = buildHarness();
            REQUIRE(reloadHarness.session.has_value());
            TaskContext reloaded{
                *std::move(reloadHarness.session),
                *reloadHarness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            constexpr std::string_view verify = R"lua(
                local built = project.load_project(ctx)

                -- Byte-stable: what the agent saved is what this build writes
                -- again from what it read back.
                if project.encode(built) ~= ctx:project_read(project.file_name) then
                    return 0
                end

                if #built.elements ~= 3 then return 0 end
                if #built.pages ~= 2 then return 0 end

                local home  = built.page_by_name["home"]
                local title = built.element_by_name["title"]
                if home == nil or title == nil then return 0 end
                if #home.references ~= 3 then return 0 end

                -- The row's own text came back, and the element still says
                -- nothing: that split is what lets one drawn rectangle serve
                -- every page in the target.
                local titled = model.Page.reference_for(home, title)
                if titled == nil then return 0 end
                if titled.expected_text ~= "Sortie" then return 0 end
                if model.Reference.expected_text(titled) ~= "Sortie" then
                    return 0
                end
                if title.expected_text ~= nil then return 0 end

                local ready = model.Page.reference_for(
                    home,
                    built.element_by_name["status"]
                )
                if ready == nil then return 0 end
                if ready.expected_text ~= "Ready" then return 0 end

                -- The interact row exercises what it was written with and
                -- nothing more, and carries no polarity it never asked for.
                local clicked = model.Page.reference_for(
                    home,
                    built.element_by_name["action"]
                )
                if clicked == nil then return 0 end
                if not clicked.exercised.interact then return 0 end
                if clicked.exercised.identify then return 0 end
                if clicked.identify ~= nil then return 0 end

                local detail = built.page_by_name["detail"]
                if detail == nil then return 0 end
                if not detail.overlay then return 0 end
                if #detail.references ~= 2 then return 0 end
                local elsewhere = model.Page.reference_for(detail, title)
                if elsewhere == nil then return 0 end
                if elsewhere.expected_text ~= "Detail" then return 0 end

                if #built.graph.edges ~= 1 then return 0 end
                local edge = built.graph.edges[1]
                if edge.from ~= built.graph.page_by_name["home"] then return 0 end
                if edge.to[1] ~= built.graph.page_by_name["detail"] then
                    return 0
                end
                if edge.kind ~= "push" then return 0 end
                return 1
            )lua";

            auto const verified = runExploration(reloaded, verify);
            REQUIRE(verified.has_value());
            CHECK(*verified == doctest::Approx(1.0));
        }

        TEST_CASE("An invented page is refused before it can join the model")
        {
            auto const directory  = TemporaryDirectory{"uf-scribe-page-refusal"};
            auto const screenHash = seedScreen(directory.path());
            seedGraphProject(directory.path(), screenHash);

            auto harness = buildHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{
                *std::move(harness.session),
                *harness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            constexpr std::string_view source = R"lua(
                local built  = project.load_project(ctx)
                local pages  = built.pages
                local graph  = built.graph
                local anchor = built.element_by_name["anchor"]
                local action = built.element_by_name["action"]

                local signature = {
                    element   = anchor,
                    holding   = "referenced",
                    exercised = { "identify" },
                    identify  = "required",
                }

                -- A name the project already declares, refused while the caller
                -- still has the page in hand and can choose another. `Graph.new`
                -- would catch it too, but only as a complaint about a list.
                local ok, err = pcall(function()
                    scribe.add_page(built, {
                        name       = "home",
                        references = { signature },
                    })
                end)
                if ok then return 0 end
                local taken = "already declares a page named 'home'"
                if string.find(tostring(err), taken, 1, true) == nil then
                    return 0
                end

                -- A row naming an element the file does not carry. It is minted,
                -- so the constructor is satisfied; only the loaded project knows
                -- that saving this would write a file nothing can open again.
                local stranger = model.Element.new({
                    name         = "stranger",
                    capabilities = { "interact" },
                    rect         = { x = 0, y = 0, width = 1, height = 1 },
                })
                local strangeOk, strangeErr = pcall(function()
                    scribe.add_page(built, {
                        name       = "invented",
                        references = {
                            signature,
                            {
                                element   = stranger,
                                holding   = "owned",
                                exercised = { "interact" },
                            },
                        },
                    })
                end)
                if strangeOk then return 0 end
                local missing = "declares no element named 'stranger'"
                if string.find(tostring(strangeErr), missing, 1, true) == nil then
                    return 0
                end

                -- A page whose rows say nothing about when it is on screen. The
                -- refusal is the CONSTRUCTOR's, in its own words, which is the
                -- whole reason this verb takes the rows at once.
                local blindOk, blindErr = pcall(function()
                    scribe.add_page(built, {
                        name       = "unsigned",
                        references = {
                            {
                                element   = action,
                                holding   = "owned",
                                exercised = { "interact" },
                            },
                        },
                    })
                end)
                if blindOk then return 0 end
                local unsigned = "has no required identify reference"
                if string.find(tostring(blindErr), unsigned, 1, true) == nil then
                    return 0
                end

                -- A field the constructor does not own rides through to it and
                -- is refused there rather than dropped on the way, which is what
                -- keeps a project's own key from vanishing into a typo.
                local keyOk, keyErr = pcall(function()
                    scribe.add_page(built, {
                        name       = "typo",
                        overlays   = true,
                        references = { signature },
                    })
                end)
                if keyOk then return 0 end
                local stray = 'does not have a field named "overlays"'
                if string.find(tostring(keyErr), stray, 1, true) == nil then
                    return 0
                end

                -- Not one of the four changed anything: every refusal lands
                -- before the model is written to.
                if built.pages ~= pages then return 0 end
                if built.graph ~= graph then return 0 end
                if #built.pages ~= 2 then return 0 end
                if built.page_by_name["invented"] ~= nil then return 0 end
                if built.page_by_name["unsigned"] ~= nil then return 0 end
                if built.page_by_name["typo"] ~= nil then return 0 end
                return 1
            )lua";

            auto const result = runExploration(context, source);
            REQUIRE(result.has_value());
            CHECK(*result == doctest::Approx(1.0));
        }

        // The first thing an agent meets in a project directory nobody has laid
        // out, and what it is told about it. The refusal is deliberate: proving a
        // write stayed inside the project means canonicalizing a parent that is
        // really there, so the store creates no directory and a missing one has to
        // fail. The sentence matters because an agent driving the explore channel
        // reads exactly this text on its result line and has no trace file open.
        TEST_CASE("a write into a directory nobody laid out says which one is missing")
        {
            auto const directory = TemporaryDirectory{"uf-scribe-missing-parent"};

            // Deliberately NOT seedEmptyProject: the point is a project
            // directory with no assets/templates in it at all.
            auto const model =
                std::string{"schema = \"umbraflow-project/l2-v1\"\n"};
            writeFile(
                directory.path() / "page-model.toml",
                std::as_bytes(std::span{std::string_view{model}})
            );

            auto harness = buildHarness();
            REQUIRE(harness.session.has_value());
            TaskContext context{
                *std::move(harness.session),
                *harness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };

            constexpr std::string_view source = R"lua(
                local built = project.load_project(ctx)

                local ticket   = ctx:cycle_open()
                local measured = scribe.measure(
                    ctx,
                    ticket,
                    { x = 1, y = 0, width = 1, height = 1 }
                )
                ctx:cycle_close(ticket)

                -- Uncaught on purpose: this case is about the sentence that
                -- reaches the caller, not about what a pcall could read.
                local element = scribe.author_element(ctx, measured, {
                    name         = "action",
                    capabilities = { "identify" },
                    threshold    = 10000,
                })
                return #element.appearances
            )lua";

            auto const refused = runExploration(context, source);
            REQUIRE_FALSE(refused.has_value());

            auto const message = std::string{refused.error().message()};
            CAPTURE(message);

            // The three things the line has to carry: what it could not do, where,
            // and that the where is what is missing.
            CHECK(message.find("assets/templates") != std::string::npos);
            CHECK(message.find("does not exist") != std::string::npos);
            CHECK(message.find("(non-string error value)") == std::string::npos);

            // Laying the directory out is the only thing that was missing, and it
            // is what the CLI does before an exploration session starts.
            auto error = std::error_code{};
            REQUIRE(
                std::filesystem::create_directories(
                    directory.path() / "assets" / "templates",
                    error
                )
            );

            auto reloadHarness = buildHarness();
            REQUIRE(reloadHarness.session.has_value());
            TaskContext laidOut{
                *std::move(reloadHarness.session),
                *reloadHarness.recorder,
                TaskContextConfig{.projectRoot = directory.path()},
            };
            auto const accepted = runExploration(laidOut, source);
            CHECK(accepted.has_value());
        }
    }
}
