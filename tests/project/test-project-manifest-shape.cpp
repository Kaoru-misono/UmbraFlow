// umbraflow-project.json has one reader, and this is what says so.
//
// Two commands judge the document: the offline project kit's `project build`
// and `project check`, and the runtime loader's loadProductionProject. They
// live in modules that cannot link one another -- uf::deployment reaches
// uf::task, and the `project` executable links uf::project and uf::core alone
// -- so for a while each carried its own reading of the same document. The
// kit's was the weaker: it accepted an empty deployments array, an empty
// plugin path, a deployment with a numeric name and any unknown member, all of
// which the loader refused, and it named the wrong defect for a deployment
// that is not an object at all.
//
// The shape now lives in schema/umbraflow-project-v3.schema.json and reaches
// both through the framework schema catalog. Every case below hands one
// document to both commands and requires not only the same verdict but the
// same sentence: the refusal each produces is compared for equality from the
// schema label onwards, so a second reading appearing in either of them --
// with a different bound, a different message, or none -- goes red here.
#include <project/project-kit.hpp>

#include <deployment/project-directory.hpp>

#include <task/runtime-model-file.hpp>

#include <core/error/result.hpp>

#include <doctest/doctest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <ios>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::project
{
    namespace
    {
        // The label json::Schema prints in front of every refusal it makes,
        // which is the published path both readers compile the document from.
        // Comparing from here rather than from the front of the message drops
        // the two readers' own prefixes, which differ because one is naming a
        // source tree and the other a project directory.
        constexpr auto k_schemaLabel = std::string_view{
            "schema/umbraflow-project-v3.schema.json"
        };

        class TemporaryDirectory final
        {
            std::filesystem::path m_path;

        public:
            explicit TemporaryDirectory(std::string_view label)
                : m_path{
                      std::filesystem::temp_directory_path()
                      / (std::string{label} + "-"
                         + std::to_string(std::random_device{}()))
                  }
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
            std::string_view text
        ) -> void
        {
            auto error = std::error_code{};
            std::filesystem::create_directories(path.parent_path(), error);
            auto stream = std::ofstream{
                path,
                std::ios::binary | std::ios::trunc
            };
            REQUIRE(stream.is_open());
            stream << text;
            REQUIRE(stream.good());
        }

        // A verdict, as the reader stated it: empty means accepted. Both
        // readers answer in this one currency so that the comparison below is
        // between two sentences rather than between two result types.
        template <typename Value>
        [[nodiscard]]
        auto messageOf(Result<Value> const& result) -> std::string
        {
            return result.has_value()
                ? std::string{}
                : std::string{result.error().message()};
        }

        // What the one shape authority said, with everything either reader
        // wrapped around it removed. Empty means this reader did not refuse the
        // document's shape -- either because it accepted the document, or
        // because it got past the shape and failed at a later rule.
        //
        // The reason is ONE LINE, and the cut is at its end rather than at the
        // end of the message. What a reader prints after it is that reader's
        // own account of the reason -- the kit adds the complete member-set
        // arithmetic at the refused path, so an author editing a declaration
        // sees the whole edit at once -- and an account is not a second
        // verdict. Comparing whole messages here would make the two readers
        // disagree the moment one of them explained itself better.
        [[nodiscard]]
        auto schemaComplaint(std::string_view message) -> std::string
        {
            auto const at = message.find(k_schemaLabel);
            if (at == std::string_view::npos)
            {
                return std::string{};
            }
            auto const reason = message.substr(at);
            return std::string{reason.substr(0U, reason.find('\n'))};
        }

        // The module a case names is the deployment's one closure. There is no
        // second slot beside it: a deployment ships one program, and the two
        // authoring tiers differ only in who wrote the file it names.
        inline constexpr auto k_handWrittenTool = std::string_view{
            "plugin/dream.luau"
        };
        inline constexpr auto k_generatedTool = std::string_view{
            "generated/adapters/acme.tool/do-work/tool.luau"
        };

        // One deployment block, with the three members under test spliced in.
        // Everything else is a manifest whose shape the loader accepts. It
        // declares no Tool and binds none, which is the whole statement a
        // project shipping no handler makes.
        [[nodiscard]]
        auto deploymentBlock(
            std::string_view plugin,
            std::string_view authoring,
            std::string_view justification
        ) -> std::string
        {
            auto block = std::string{R"json({"name":"dream",)json"};
            block += R"json("plugin_id":"chaos.dream",)json";
            block += R"json("tool_closure":{"entry":"main",)json"
                R"json("exported_entry_points":[],)json"
                R"json("modules":[{"name":"main","path":")json";
            block += plugin;
            block += R"json("}]},)json";
            block += R"json("plugin_authoring":")json";
            block += authoring;
            block += R"json(",)json";
            block += justification;
            block += R"json("observed_instance_identity_schemas":[],)json";
            block += R"json("tools":[],)json";
            block += R"json("tool_bindings":[],)json";
            block += R"json("resources":[]})json";
            return block;
        }

        [[nodiscard]]
        auto manifestOf(std::string_view deployments) -> std::string
        {
            auto document = std::string{R"json({"schema":"umbraflow-project/v3",)json"};
            document += R"json("runtime_artifact":"runtime/artifact",)json";
            document += R"json("primary_deployment":"dream",)json";
            document += R"json("template_cuts":[],)json";
            document += R"json("deployments":)json";
            document += deployments;
            document += "}";
            return document;
        }

        [[nodiscard]]
        auto oneDeployment(
            std::string_view plugin,
            std::string_view authoring,
            std::string_view justification
        ) -> std::string
        {
            return manifestOf(
                "[" + deploymentBlock(plugin, authoring, justification) + "]"
            );
        }

        [[nodiscard]]
        auto statedJustification() -> std::string
        {
            return R"json("plugin_justification":"umbraflow-declarative-)json"
                R"json(workflow-tool/v1 has no member that decides what a )json"
                R"json(handler returns.",)json";
        }

        [[nodiscard]]
        auto acceptedManifest() -> std::string
        {
            return oneDeployment(
                k_handWrittenTool,
                "hand-written",
                statedJustification()
            );
        }

        [[nodiscard]]
        auto generatedWorkflowDeclaration() -> std::string_view
        {
            return R"json({
  "schema": "umbraflow-declarative-workflow-tool/v1",
  "tool_name": "acme.do_work",
  "target_argument": "observed_instance_id",
  "allowed_instance_kinds": ["acme.target"],
  "fresh_observation": {
    "required_surface": "acme.surface",
    "require_unambiguous": true
  },
  "ui_finding": {"kind": "observed_instance_absent"},
  "states": [
    {
      "state_key": "await-target",
      "kind": "wait",
      "observation_budget": 1,
      "timeout_ms": 1000
    }
  ],
  "steps": ["await-target"],
  "bounds": {
    "maximum_states": 1,
    "maximum_steps": 1,
    "maximum_dispatches": 0,
    "maximum_observations": 1,
    "maximum_waits": 1,
    "maximum_elapsed_ms": 1000
  }
})json";
        }

        // The kit's verdict on one document: `project build` then
        // `project check` over a source tree that holds exactly the files the
        // manifest names, so what this measures is the manifest.
        [[nodiscard]]
        auto kitVerdict(std::string_view manifest) -> std::string
        {
            auto const workspace = TemporaryDirectory{"uf-manifest-shape-kit"};
            auto const source    = workspace.path() / "source";
            auto const build     = workspace.path() / "build";
            writeFile(source / k_handWrittenTool, "return {}\n");
            writeFile(source / "umbraflow-project.json", manifest);

            // The RuntimeArtifact every document below declares. The kit runs
            // the trusted parser over it, so a source tree without one is
            // refused for a reason that has nothing to do with the manifest
            // shape these cases measure. H_genesis is the smallest artifact
            // that parses.
            auto const artifact = source / "runtime" / "artifact";
            auto const genesis  = task::genesisRuntimeArtifactManifestJcs();
            REQUIRE(genesis.has_value());
            writeFile(
                artifact / std::string{task::k_runtimeModelFileName},
                task::k_genesisRuntimeModelToml
            );
            writeFile(
                artifact / std::string{task::k_runtimeArtifactManifestFileName},
                *genesis
            );

            if (manifest.contains(k_generatedTool))
            {
                writeFile(
                    source
                        / std::filesystem::path{
                            "declarative-tools/acme.tool/do-work.json"
                        },
                    generatedWorkflowDeclaration()
                );
            }

            auto const spec = ProjectBuildSpec{
                .sourceDirectory = source,
                .buildDirectory  = build,
            };
            auto const initialized = initProject(spec);
            if (!initialized)
            {
                return messageOf(initialized);
            }

            // No resolver: every document below declares no template cut, so a
            // reachable resolver would answer nothing and prove nothing.
            auto built = messageOf(buildProject(spec, {}));
            if (!built.empty())
            {
                return built;
            }
            return messageOf(checkProject(spec, {}));
        }

        // The loader's verdict on the same document. The directory holds
        // nothing else, so a document whose shape is accepted fails later on a
        // file it names -- which is exactly the outcome the cases below tell
        // apart from a shape refusal.
        [[nodiscard]]
        auto loaderVerdict(std::string_view manifest) -> std::string
        {
            auto const directory = TemporaryDirectory{"uf-manifest-shape-loader"};
            writeFile(directory.path() / "umbraflow-project.json", manifest);
            return messageOf(
                deployment::loadProductionProject(directory.path(), {})
            );
        }

        struct ShapeCase final
        {
            std::string_view label{};
            std::string      manifest{};
            bool             shapeAccepted{};
        };

        [[nodiscard]]
        auto shapeCases() -> std::array<ShapeCase, 9>
        {
            // Everything but the name is the accepted block, so what either
            // reader refuses here is the number where a deployment name goes.
            constexpr auto k_statedName = std::string_view{R"json({"name":"dream",)json"};
            auto numericBlock           = deploymentBlock(
                k_handWrittenTool,
                "hand-written",
                statedJustification()
            );
            REQUIRE(numericBlock.starts_with(k_statedName));
            numericBlock.replace(0U, k_statedName.size(), R"json({"name":7,)json");
            auto const numericName = "[" + numericBlock + "]";

            auto unknownMember = acceptedManifest();
            unknownMember.insert(1U, R"json("invented_member":1,)json");

            return {
                // The positive control. Without it a pair of readers that
                // refused everything would agree on every case below and prove
                // nothing.
                ShapeCase{
                    .label         = "a hand-written plugin stating a justification",
                    .manifest      = acceptedManifest(),
                    .shapeAccepted = true,
                },
                // D1: a generated adapter IS the declarative tier and owes no
                // reason. The path is a generated one and the tier is stated in
                // the document, because the loader can read the document and
                // cannot read the kit's output tree.
                ShapeCase{
                    .label    = "a generated adapter stating none",
                    .manifest = oneDeployment(
                        k_generatedTool,
                        "generated",
                        ""
                    ),
                    .shapeAccepted = true,
                },
                ShapeCase{
                    .label    = "a hand-written plugin stating none",
                    .manifest = oneDeployment(
                        k_handWrittenTool,
                        "hand-written",
                        ""
                    ),
                    .shapeAccepted = false,
                },
                ShapeCase{
                    .label    = "a generated adapter stating one",
                    .manifest = oneDeployment(
                        k_generatedTool,
                        "generated",
                        statedJustification()
                    ),
                    .shapeAccepted = false,
                },
                // D3, sharpest first: an empty deployments array. The kit's own
                // reader walked the array and had nothing to say about it being
                // empty, while the schema has always carried minItems: 1.
                ShapeCase{
                    .label         = "no deployments at all",
                    .manifest      = manifestOf("[]"),
                    .shapeAccepted = false,
                },
                ShapeCase{
                    .label    = "a deployment naming an empty plugin path",
                    .manifest = oneDeployment(
                        "",
                        "hand-written",
                        statedJustification()
                    ),
                    .shapeAccepted = false,
                },
                ShapeCase{
                    .label         = "a deployment whose name is a number",
                    .manifest      = manifestOf(numericName),
                    .shapeAccepted = false,
                },
                ShapeCase{
                    .label         = "a member neither reader knows",
                    .manifest      = unknownMember,
                    .shapeAccepted = false,
                },
                // The one both readers already refused, for two different
                // reasons: the kit reported "names no plugin", which is the
                // wrong defect.
                ShapeCase{
                    .label         = "a deployment that is not an object",
                    .manifest      = manifestOf("[42]"),
                    .shapeAccepted = false,
                },
            };
        }
    }

    // Every divergence the two readers used to have, plus both directions of
    // the direct-plugin tier's admission gate, judged by both.
    TEST_CASE("umbraflow-project.json has one reader")
    {
        for (auto const& judged : shapeCases())
        {
            INFO(judged.label);
            auto const kit             = kitVerdict(judged.manifest);
            auto const loader          = loaderVerdict(judged.manifest);
            auto const kitComplaint    = schemaComplaint(kit);
            auto const loaderComplaint = schemaComplaint(loader);
            INFO("kit: ", kit);
            INFO("loader: ", loader);

            CHECK_MESSAGE(
                kitComplaint == loaderComplaint,
                "both readers must judge the same document by the same stated "
                "reason"
            );
            if (judged.shapeAccepted)
            {
                CHECK_MESSAGE(
                    kit.empty(),
                    "the project kit must accept this document"
                );
                CHECK_MESSAGE(
                    loaderComplaint.empty(),
                    "the loader must get past this document's shape"
                );
            }
            else
            {
                CHECK_MESSAGE(
                    !kit.empty(),
                    "the project kit must refuse this document"
                );
                CHECK_MESSAGE(
                    !loaderComplaint.empty(),
                    "the loader must refuse this document's shape"
                );
            }
        }

        // The kit's account of one refusal, on a deployment wrong in several
        // ways at once -- which is exactly what a declaration written against
        // an older release is.
        //
        // The evaluator short-circuits, so the stated reason names ONE of these
        // problems. The account names all of them at that one location, and
        // prints each missing member's own $comment out of the schema, so the
        // whole edit can be made from one run instead of six.
        SUBCASE("a declaration wrong in several ways at once")
        {
            constexpr auto stale = std::string_view{
                R"json([{"name":"dream","plugin_id":"chaos.dream",)json"
                R"json("plugin_authoring":"hand-written",)json"
                R"json("plugin_justification":"legacy",)json"
                R"json("plugin":{},"tool_catalog":"catalog.json",)json"
                R"json("resources":[]}])json"
            };
            auto const account = kitVerdict(manifestOf(stale));
            INFO("kit: ", account);

            for (auto const missing : {
                     "observed_instance_identity_schemas",
                     "tool_bindings",
                     "tool_closure",
                     "tools",
                 })
            {
                CHECK_MESSAGE(
                    account.find(missing) != std::string::npos,
                    "the account must name every required member the "
                    "deployment lacks"
                );
            }
            // Counted rather than searched for: the refusal line already names
            // the first undeclared member, so a search alone would pass on an
            // account that said nothing.
            CHECK_MESSAGE(
                account.find(
                    "must carry 4 member(s) it does not"
                ) != std::string::npos,
                "the account must count every required member the deployment "
                "lacks"
            );
            CHECK_MESSAGE(
                account.find(
                    "carries 2 member(s) this closed object does not declare"
                ) != std::string::npos,
                "the account must count every member this closed object does "
                "not declare"
            );
            CHECK_MESSAGE(
                account.find("tool_catalog") != std::string::npos,
                "the account must name the undeclared members the refusal "
                "line did not"
            );
            CHECK_MESSAGE(
                account.find("There is one slot because there is one program")
                    != std::string::npos,
                "each missing member must carry its own $comment out of the "
                "schema, because that is where the explanation already is"
            );
        }
    }

    // What the pattern refuses is ASCII whitespace and nothing else, which is
    // what schema/umbraflow-project-v3.schema.json says it refuses. A
    // justification of one NO-BREAK SPACE is accepted by both readers -- it is
    // a review finding at plugin acceptance rather than a gate finding, on the
    // same terms as a justification that is present and false.
    //
    // This case exists because the two readers previously claimed, in a comment
    // and in a $comment, to refuse any whitespace-only value, and neither did.
    TEST_CASE("the justification pattern refuses ASCII whitespace and says so")
    {
        auto const blank = oneDeployment(
            k_handWrittenTool,
            "hand-written",
            R"json("plugin_justification":" \t\n ",)json"
        );
        // Written as the JSON escape rather than as the byte pair, so what
        // the document carries is unambiguous in this source file too.
        auto const noBreakSpace = oneDeployment(
            k_handWrittenTool,
            "hand-written",
            R"json("plugin_justification":" ",)json"
        );

        auto const blankKit    = kitVerdict(blank);
        auto const blankLoader = schemaComplaint(loaderVerdict(blank));
        INFO(blankKit);
        CHECK_MESSAGE(
            !blankKit.empty(),
            "a justification of ASCII whitespace must be refused"
        );
        CHECK_MESSAGE(
            schemaComplaint(blankKit) == blankLoader,
            "both readers must refuse it by the same stated reason"
        );
        CHECK_MESSAGE(
            blankLoader.contains("pattern"),
            "the refusal must be the pattern rather than the required clause"
        );

        auto const spacedKit    = kitVerdict(noBreakSpace);
        auto const spacedLoader = schemaComplaint(loaderVerdict(noBreakSpace));
        INFO(spacedKit);
        CHECK_MESSAGE(
            spacedKit.empty(),
            "U+00A0 is not ASCII whitespace, and the schema says it is accepted"
        );
        CHECK_MESSAGE(
            spacedLoader.empty(),
            "the loader must accept the same document's shape"
        );
    }
}
