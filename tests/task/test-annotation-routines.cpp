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
// WHY THE ROUND TRIP IS THE TEST. Each step in isolation proves almost nothing:
// an element built in memory that is never written cannot be reloaded, and a
// file written by hand proves nothing about the routine that would write it. The
// property the whole thing exists for is that what an agent authored through
// this module is a project the RUNTIME reads back and the matrix accepts -- so
// the case authors, saves, drops the model, loads it from disk, and runs
// regress.check over the reloaded one.
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
        //
        // A screen is a FILE the project holds, so the project file's `hash` and
        // the file's own bytes have to agree -- which is the point of naming it
        // by its hash rather than by a label someone typed.
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
            auto const hash = anno::sha256(*encoded);
            REQUIRE(hash.has_value());

            auto const hex = hash->hex();
            writeFile(
                root / "assets" / "screens" / (hex + ".png"),
                std::span<std::byte const>{*encoded}
            );
            return hex;
        }

        // A project holding one screen, one element and one claim, so the case
        // has something to ADD to rather than something to invent whole. The
        // existing element is the anchor grey; the agent authors a second one.
        auto seedProject(
            std::filesystem::path const& root,
            std::string const& screenHash
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
                + "capabilities = [\"identify\"]\n"
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
                + "exercised = [\"identify\"]\n"
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
        // has something to REBUILD and something to remap. Everything here is in
        // the writer's canonical order already, so the file is its own
        // round-trip baseline.
        //
        // THE UNKNOWN KEYS ARE THE POINT OF HALF OF IT. `mood` on the page and
        // `note` on its first reference are lines this build's parser does not
        // understand, and the two `extra` blocks are fields the project owns.
        // Adding a reference rebuilds the page and every one of its rows through
        // constructors that were never handed those lines, so a rebuild is
        // exactly where they would be lost -- silently, which is the failure the
        // format exists to prevent.
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
            return engine->runNumber(source, "annotation-routines");
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

            // The whole loop, as an agent would drive it through the explore
            // channel: measure, author, claim, save. The threshold is the
            // CALLER's -- 10000 basis points, an exact match -- because the
            // measurement below says the crop is one solid colour and nothing in
            // the framework is entitled to decide what that is worth.
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

        // THE REMAP IS THE PROPERTY THIS CASE EXISTS FOR.
        //
        // A page is frozen, so writing a reference rebuilds it, and every edge
        // that named the old page is left naming an object the model no longer
        // has. That is invisible in the saved file -- a stale edge still carries
        // the right page NAME, so the graph accepts it and the bytes are
        // correct. It is visible only by identity, and it is fatal to the one
        // thing the new row was written for: `observe.walk_edge` resolves
        // `edge.from` itself, so it would keep resolving the superseded page and
        // keep failing to find the element the agent just annotated.
        //
        // So the assertions below are identity assertions, and they are what a
        // rebuild that forgets the remap fails.
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
    }
}
