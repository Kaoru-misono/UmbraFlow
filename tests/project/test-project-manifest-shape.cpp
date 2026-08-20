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
// The shape now lives in schema/umbraflow-project-v2.schema.json and reaches
// both through the framework schema catalog. Every case below hands one
// document to both commands and requires not only the same verdict but the
// same sentence: the refusal each produces is compared for equality from the
// schema label onwards, so a second reading appearing in either of them --
// with a different bound, a different message, or none -- goes red here.
#include <project/project-kit.hpp>

#include <deployment/project-directory.hpp>

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
            "schema/umbraflow-project-v2.schema.json"
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
        [[nodiscard]]
        auto schemaComplaint(std::string_view message) -> std::string
        {
            auto const at = message.find(k_schemaLabel);
            return at == std::string_view::npos
                ? std::string{}
                : std::string{message.substr(at)};
        }

        // One deployment block, with the three members under test spliced in.
        // Everything else is a manifest whose shape the loader accepts.
        [[nodiscard]]
        auto deploymentBlock(
            std::string_view plugin,
            std::string_view authoring,
            std::string_view justification
        ) -> std::string
        {
            auto block = std::string{R"json({"name":"dream",)json"};
            block += R"json("plugin_id":"chaos.dream",)json";
            block += R"json("baseline_event_type":"project.baseline_created",)json";
            block += R"json("plugin":{"entry":"main","modules":[{"name":"main","path":")json";
            block += plugin;
            block += R"json("}]},)json";
            block += R"json("plugin_authoring":")json";
            block += authoring;
            block += R"json(",)json";
            block += justification;
            block += R"json("project_state_schema":"schema/state.json",)json";
            block += R"json("project_observation_schema":"schema/observation.json",)json";
            block += R"json("tool_precondition_schema":"schema/precondition.json",)json";
            block += R"json("reconcile_schema":"schema/reconcile.json",)json";
            block += R"json("tool_catalog":"schema/catalog.json",)json";
            block += R"json("journal_event_schema_manifest":"schema/journal.json",)json";
            block += R"json("reconcile_manifest":"schema/reconcile-manifest.json",)json";
            block += R"json("journal_payload_schemas":["schema/journal-0.json"],)json";
            block += R"json("effect_payload_schemas":[],)json";
            block += R"json("observed_instance_identity_schemas":[],)json";
            block += R"json("resources":[]})json";
            return block;
        }

        [[nodiscard]]
        auto manifestOf(std::string_view deployments) -> std::string
        {
            auto document = std::string{R"json({"schema":"umbraflow-project/v2",)json"};
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
                R"json(Reduce returns.",)json";
        }

        [[nodiscard]]
        auto acceptedManifest() -> std::string
        {
            return oneDeployment(
                "plugin/dream.luau",
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
        // `project check` over a source tree whose only declared input is a
        // file the manifest has nothing to do with, so what this measures is
        // the manifest and not the input list.
        [[nodiscard]]
        auto kitVerdict(std::string_view manifest) -> std::string
        {
            auto const workspace = TemporaryDirectory{"uf-manifest-shape-kit"};
            auto const source    = workspace.path() / "source";
            auto const build     = workspace.path() / "build";
            writeFile(source / "dummy.txt", "dummy\n");
            writeFile(source / "plugin/dream.luau", "return {}\n");
            writeFile(source / "umbraflow-project.json", manifest);

            auto inputs = std::vector<std::filesystem::path>{"dummy.txt"};
            if (manifest.contains("generated/adapters/acme.tool/do-work.luau"))
            {
                auto const declaration = std::filesystem::path{
                    "declarative-tools/acme.tool/do-work.json"
                };
                writeFile(source / declaration, generatedWorkflowDeclaration());
                inputs.emplace_back(declaration);
            }

            auto const initialized = initProject(ProjectInitSpec{
                .sourceDirectory = source,
                .buildDirectory  = build,
                .inputs          = std::move(inputs),
            });
            if (!initialized)
            {
                return messageOf(initialized);
            }

            auto const spec = ProjectBuildSpec{
                .sourceDirectory = source,
                .buildDirectory  = build,
                .toolCatalogs    = {},
            };
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
            auto const numericName = std::string{
                R"json([{"name":7,"plugin_id":"chaos.dream",)json"
                R"json("baseline_event_type":"project.baseline_created",)json"
                R"json("plugin":{"entry":"main","modules":[{"name":"main","path":"plugin/dream.luau"}]},)json"
                R"json("plugin_authoring":"hand-written",)json"
            }
                + statedJustification()
                + R"json("project_state_schema":"schema/state.json",)json"
                R"json("project_observation_schema":"schema/observation.json",)json"
                R"json("tool_precondition_schema":"schema/precondition.json",)json"
                R"json("reconcile_schema":"schema/reconcile.json",)json"
                R"json("tool_catalog":"schema/catalog.json",)json"
                R"json("journal_event_schema_manifest":"schema/journal.json",)json"
                R"json("reconcile_manifest":"schema/reconcile-manifest.json",)json"
                R"json("journal_payload_schemas":["schema/journal-0.json"],)json"
                R"json("effect_payload_schemas":[],)json"
                R"json("observed_instance_identity_schemas":[],)json"
                R"json("resources":[]}])json";

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
                        "generated/adapters/acme.tool/do-work.luau",
                        "generated",
                        ""
                    ),
                    .shapeAccepted = true,
                },
                ShapeCase{
                    .label    = "a hand-written plugin stating none",
                    .manifest = oneDeployment(
                        "plugin/dream.luau",
                        "hand-written",
                        ""
                    ),
                    .shapeAccepted = false,
                },
                ShapeCase{
                    .label    = "a generated adapter stating one",
                    .manifest = oneDeployment(
                        "generated/adapters/acme.tool/do-work.luau",
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
    }

    // What the pattern refuses is ASCII whitespace and nothing else, which is
    // what schema/umbraflow-project-v2.schema.json says it refuses. A
    // justification of one NO-BREAK SPACE is accepted by both readers -- it is
    // a review finding at plugin acceptance rather than a gate finding, on the
    // same terms as a justification that is present and false.
    //
    // This case exists because the two readers previously claimed, in a comment
    // and in a $comment, to refuse any whitespace-only value, and neither did.
    TEST_CASE("the justification pattern refuses ASCII whitespace and says so")
    {
        auto const blank = oneDeployment(
            "plugin/dream.luau",
            "hand-written",
            R"json("plugin_justification":" \t\n ",)json"
        );
        // Written as the JSON escape rather than as the byte pair, so what
        // the document carries is unambiguous in this source file too.
        auto const noBreakSpace = oneDeployment(
            "plugin/dream.luau",
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
