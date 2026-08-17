// What the deployment's validators and its two operator protocol readers refuse
// that the exemplars' substituted constants accepted, and what each of them
// still accepts.
//
// Every refusal below is paired with the acceptance it is the negation of. A
// validator that refused everything would satisfy the first half of each case
// and fail the second, which is the failure mode this file exists to exclude:
// the substituted constants it replaces were green precisely because nothing
// ever asked them to refuse.
//
// The two operator protocol readers are no longer exercised here. Each takes a
// ValidatedDocument, and only a ProjectSchemaOwner can mint one, so a crafted
// document cannot reach a reader from a file that holds no plugin. What was a
// reader refusal is asserted below against the validator that makes it now --
// the same two a running Operator applies before a reader is ever called. The
// readers themselves are read in tests/operator/test-ledger.cpp, on documents
// the fixture plugin produced and the schema owner stamped.

#include "arcana-expedition/project-schemas.hpp"
#include "umbraflow/project-schemas.hpp"

#include <core/safety/annotations.hpp>

#include <deployment/project-deployment.hpp>

#include <domain/content-hash.hpp>

#include <operator/project-plugin.hpp>

#include <schema/framework-schema-catalog.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::deployment
{
    namespace
    {
        namespace umbraflow = operator_runtime::test_support;
        namespace arcana    = operator_runtime::conformance::expedition;

        using operator_runtime::ProjectDocumentDirection;
        using operator_runtime::ProjectPluginFunction;

        [[nodiscard]]
        auto repositoryRoot() -> std::filesystem::path
        {
            auto source = std::filesystem::path{__FILE__};
            if (source.is_relative())
            {
                source = std::filesystem::absolute(source);
            }
            auto candidate = source.parent_path().parent_path().parent_path();
            if (std::filesystem::is_directory(candidate / "schema"))
            {
                return candidate;
            }

            candidate = std::filesystem::current_path();
            while (!candidate.empty())
            {
                if (std::filesystem::is_directory(candidate / "schema"))
                {
                    return candidate;
                }
                auto const parent = candidate.parent_path();
                if (parent == candidate)
                {
                    break;
                }
                candidate = parent;
            }

            FAIL("repository root containing schema/ was not found");
            return {};
        }

        [[nodiscard]]
        auto schemaDigest(std::string_view relativePath) -> std::string
        {
            auto stream = std::ifstream{
                repositoryRoot() / relativePath,
                std::ios::binary,
            };
            REQUIRE(stream.good());
            auto const bytes = std::string{
                std::istreambuf_iterator<char>{stream},
                std::istreambuf_iterator<char>{},
            };
            auto const digest = sha256(std::as_bytes(std::span{bytes}));
            REQUIRE(digest.has_value());
            return digest->hex();
        }

        // The recognizer both exemplars carried as their canonical validator
        // until this change: an ordered substring search and a closing brace.
        // It is restated here rather than cited, because the proof this file
        // owes is that a document it accepted is now refused, and a claim about
        // deleted code proves nothing.
        [[nodiscard]]
        auto oldOrderedMemberRecognizer(
            std::string_view exactJcs,
            std::span<std::string_view const> members
        ) -> bool
        {
            if (!exactJcs.starts_with(members.front()) || !exactJcs.ends_with('}'))
            {
                return false;
            }
            auto at = std::size_t{0};
            for (auto const member : members)
            {
                auto const found = exactJcs.find(member, at);
                if (found == std::string_view::npos)
                {
                    return false;
                }
                at = found + member.size();
            }
            return true;
        }

        [[nodiscard]]
        auto oldPlanEnvelopeRecognizer(std::string_view exactJcs) -> bool
        {
            constexpr auto members = std::array{
                std::string_view{"{\"canonical_args\":"},
                std::string_view{",\"project_observation\":"},
                std::string_view{",\"project_state\":"},
                std::string_view{",\"tool_name\":"},
                std::string_view{",\"tool_version\":"},
            };
            return oldOrderedMemberRecognizer(exactJcs, members);
        }

        // The umbraflow exemplar's reconcile branch, and the arcana exemplar's.
        [[nodiscard]]
        auto oldDispositionRecognizer(std::string_view exactJcs) -> bool
        {
            return exactJcs.starts_with("{\"disposition\":\"")
                && exactJcs.ends_with("\"}");
        }

        [[nodiscard]]
        auto oldVerdictRecognizer(std::string_view exactJcs) -> bool
        {
            return exactJcs.starts_with("{\"verdict\":\"")
                && exactJcs.ends_with("\"}");
        }

        // Every member name is present and in the order the recognizer walks,
        // and "a" sorts before all of them -- so this is a plan envelope to the
        // old validator and is not its own RFC 8785 form.
        constexpr auto k_unsortedPlanEnvelope = std::string_view{
            "{\"canonical_args\":0,\"project_observation\":0,\"project_state\":0,"
            "\"tool_name\":0,\"tool_version\":0,\"a\":0}"
        };

        // The same shape with a number no canonical document can spell: JCS
        // adopts ES6 Number::toString, which writes this one as 1.5.
        constexpr auto k_nonCanonicalNumberPlanEnvelope = std::string_view{
            "{\"canonical_args\":1.50,\"project_observation\":0,"
            "\"project_state\":0,\"tool_name\":0,\"tool_version\":0}"
        };

        constexpr auto k_provenance = std::string_view{
            "{\"kind\":\"observation\","
            "\"observation_ids\":[\"fixture-observation-1\"],"
            "\"principal_id\":null,\"source_hashes\":[]}"
        };

        [[nodiscard]]
        auto umbraflowDeployment(umbraflow::DeploymentBundle const& bundle)
            -> ProjectDeployment
        {
            auto deployed = ProjectDeployment::create(bundle.sources());
            auto const why = deployed.has_value()
                ? std::string{}
                : std::string{deployed.error().message()};
            INFO(why);
            REQUIRE(deployed.has_value());
            return *std::move(deployed);
        }

        [[nodiscard]]
        auto arcanaDeployment(arcana::DeploymentBundle const& bundle)
            -> ProjectDeployment
        {
            auto deployed = ProjectDeployment::create(bundle.sources());
            auto const why = deployed.has_value()
                ? std::string{}
                : std::string{deployed.error().message()};
            INFO(why);
            REQUIRE(deployed.has_value());
            return *std::move(deployed);
        }

        // Why one set of sources was refused, for a case whose subject is the
        // message rather than the outcome. A refusal naming a different link
        // satisfies CHECK_FALSE exactly as the intended one does, so a case
        // about which link broke has to read what the refusal said.
        [[nodiscard]]
        auto why(Result<ProjectDeployment> const& outcome) -> std::string
        {
            return outcome.has_value()
                ? std::string{"<the sources built a deployment>"}
                : std::string{outcome.error().message()};
        }

        // Engagement proved where the read happens rather than asserted a line
        // above it. A REQUIRE two lines up is invisible to the analyzer and to
        // anyone who later deletes it, so the check travels with the read.
        template <typename T>
        [[nodiscard]]
        auto valueOf(std::optional<T> const& value UF_LIFETIME_BOUND) -> T const&
        {
            if (!value.has_value())
            {
                throw std::logic_error{"read of a disengaged optional"};
            }
            return *value;
        }

        // One reduce envelope carrying one fixture.progress event.
        [[nodiscard]]
        auto reduceEnvelope(std::string_view payload) -> std::string
        {
            auto envelope = std::string{
                "{\"journal_events\":[{\"namespaced_event_type\":\"fixture.progress\","
                "\"opaque_project_payload\":"
            };
            envelope += payload;
            envelope += ",\"provenance\":";
            envelope += k_provenance;
            envelope += "}],\"prior_project_state\":{\"revision\":0}}";
            return envelope;
        }

        // One derive envelope whose ui_snapshot reports a single undecided
        // reading. Only the reason varies, so nothing but the reason vocabulary
        // can separate two of these.
        [[nodiscard]]
        auto deriveEnvelopeReading(std::string_view reason) -> std::string
        {
            auto envelope = std::string{
                "{\"pending_operation_transition\":null,"
                "\"pinned_project_artifact_identities\":[],"
                "\"prior_project_observation\":null,"
                "\"project_state\":{\"revision\":0},"
                "\"ui_snapshot\":{\"kind\":\"resolved_state\","
                "\"ordered_surface_stack\":[\"fixture.surface\"],"
                "\"readings\":[{\"kind\":\"unknown\",\"reader\":\"fixture.reader\","
                "\"reason\":\""
            };
            envelope += reason;
            envelope += "\",\"ui_target\":\"fixture.target\"}]}}";
            return envelope;
        }

        [[nodiscard]]
        auto planEnvelope(std::string_view canonicalArgs) -> std::string
        {
            // The plan input's project_observation is pinned to the framework's
            // final envelope, so an empty object would be refused before the
            // argument a case is about ever reached its definition. The member
            // order is RFC 8785 canonical, as the raw-validator case below
            // feeds this same string back into requireExactCanonical.
            auto envelope = std::string{"{\"canonical_args\":"};
            envelope += canonicalArgs;
            envelope += ",\"project_observation\":{\"canonical_opaque_payload\":{},"
                        "\"observed_instances\":[],\"project_tool_preconditions\":[],"
                        "\"schema\":\"umbraflow-project-observation/v1\"},"
                        "\"project_state\":{\"revision\":0},\"tool_name\":\"command-1\","
                        "\"tool_version\":\"1\"}";
            return envelope;
        }

        [[nodiscard]]
        auto planProposal(std::string_view payload, std::string_view schemaHex)
            -> std::string
        {
            auto proposal = std::string{
                "{\"allowed_ui_actions\":[\"fixture.step\"],"
                "\"canonical_args\":{\"value\":1},"
                "\"effects\":[{\"namespaced_type\":\"fixture.write\","
                "\"opaque_project_payload\":"
            };
            proposal += payload;
            proposal += ",\"payload_schema_hash\":\"";
            proposal += schemaHex;
            proposal += "\",\"risk\":\"low\",\"scope_key\":\"alpha\","
                        "\"scope_kind\":\"instance\"}],"
                        "\"tool_name\":\"command-1\",\"tool_version\":\"1\","
                        "\"workflow_limits\":{\"maximum_dispatches\":8,"
                        "\"maximum_elapsed_ms\":60000,\"maximum_observations\":16,"
                        "\"maximum_steps\":8,\"maximum_waits\":4}}";
            return proposal;
        }

        // OP:`UIActionIntent` in the shape the umbraflow exemplar's plugin
        // answers next_step with, and OP:`WaitIntent` beside it.
        constexpr auto k_uiActionIntent = std::string_view{
            "{\"action\":{\"action_id\":\"fixture.press\","
            "\"canonical_parameters\":{\"value\":1},"
            "\"surface_id\":\"fixture.surface\","
            "\"ui_target_id\":\"fixture.target\"},"
            "\"binding_variant_constraints\":[],"
            "\"delivery_class\":\"delivery_safe\","
            "\"expected_ui_postconditions\":[],"
            "\"required_ui_preconditions\":[],\"step_key\":\"fixture.step\","
            "\"timeout_policy\":{\"maximum_elapsed_ms\":5000,"
            "\"on_timeout\":\"reobserve\"}}"
        };

        constexpr auto k_waitIntent = std::string_view{
            "{\"condition\":{\"settled\":true},\"observation_budget\":4,"
            "\"step_key\":\"fixture.wait\","
            "\"timeout_policy\":{\"maximum_elapsed_ms\":5000,"
            "\"on_timeout\":\"reobserve\"}}"
        };

        // One substring of an otherwise accepted document, restated. Each
        // refusal below is about the single substitution it makes, so the
        // document it starts from has to be the accepted one and the
        // substitution has to be found.
        [[nodiscard]]
        auto substituted(
            std::string_view exact,
            std::string_view from,
            std::string_view to
        ) -> std::string
        {
            auto const at = exact.find(from);
            REQUIRE(at != std::string_view::npos);
            auto restated = std::string{exact};
            restated.replace(at, from.size(), to);
            return restated;
        }

        // A project state whose one member is a Fact, named by $ref into the
        // published fragment instead of by a copy of the Fact shape. Replacing
        // that copy is what the closed reference set is for, so every check the
        // copy performed has to be shown still firing through this document.
        constexpr auto k_factStateSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/state",
    "title": "a project state holding one Fact",
    "type": "object",
    "additionalProperties": false,
    "required": ["knowledge", "revision"],
    "properties": {
        "knowledge": {"$ref": "https://umbraflow.dev/schema/fact/v1"},
        "revision": {"type": "integer", "minimum": 0}
    }
})json"};

        constexpr auto k_collectionStateSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/state",
    "title": "a project state holding one Collection Fact",
    "type": "object",
    "additionalProperties": false,
    "required": ["knowledge", "revision"],
    "properties": {
        "knowledge": {
            "$ref": "https://umbraflow.dev/schema/collection-fact/v1"
        },
        "revision": {"type": "integer", "minimum": 0}
    }
})json"};

        // RFC 8785 form, because documentValidator judges exact bytes: every
        // member below is in sorted order and every number is canonical.
        [[nodiscard]]
        auto knowledgeState(std::string_view knowledge) -> std::string
        {
            auto document = std::string{"{\"knowledge\":"};
            document += knowledge;
            document += ",\"revision\":0}";
            return document;
        }

        constexpr auto k_provenanceEntry = std::string_view{
            "{\"kind\":\"observation\",\"reference\":\"observation-42\"}"
        };

        constexpr auto k_knownFact = std::string_view{
            "{\"confirmed_at_project_revision\":8,"
            "\"provenance\":[{\"kind\":\"observation\","
            "\"reference\":\"observation-42\"}],"
            "\"schema\":\"umbraflow-fact/v1\",\"status\":\"Known\","
            "\"value\":\"chaos\"}"
        };

        constexpr auto k_unknownFact = std::string_view{
            "{\"reason\":\"the panel was not on screen\","
            "\"schema\":\"umbraflow-fact/v1\",\"status\":\"Unknown\"}"
        };

        constexpr auto k_staleFact = std::string_view{
            "{\"confirmed_at_project_revision\":3,"
            "\"provenance\":[{\"kind\":\"observation\","
            "\"reference\":\"observation-7\"}],"
            "\"reason\":\"the screen moved on\","
            "\"schema\":\"umbraflow-fact/v1\",\"status\":\"Stale\","
            "\"value\":\"chaos\"}"
        };

        constexpr auto k_conflictFact = std::string_view{
            "{\"candidates\":[\"chaos\",\"order\"],"
            "\"reason\":\"two readers disagreed\","
            "\"schema\":\"umbraflow-fact/v1\",\"status\":\"Conflict\"}"
        };

        constexpr auto k_knownCollection = std::string_view{
            "{\"completeness\":\"Complete\",\"items\":[\"chaos\"],"
            "\"metadata\":{\"confirmed_at_project_revision\":2,"
            "\"provenance\":[{\"kind\":\"observation\","
            "\"reference\":\"observation-9\"}],"
            "\"schema\":\"umbraflow-fact/v1\",\"status\":\"Known\","
            "\"value\":{\"item_count\":1}},"
            "\"schema\":\"umbraflow-collection-fact/v1\"}"
        };

        constexpr auto k_unknownCollection = std::string_view{
            "{\"completeness\":\"Unknown\",\"items\":[],"
            "\"metadata\":{\"reason\":\"nothing was read\","
            "\"schema\":\"umbraflow-fact/v1\",\"status\":\"Unknown\"},"
            "\"schema\":\"umbraflow-collection-fact/v1\"}"
        };

        constexpr auto k_conflictCollection = std::string_view{
            "{\"completeness\":\"Partial\",\"items\":[],"
            "\"metadata\":{\"candidates\":[\"chaos\",\"order\"],"
            "\"reason\":\"two readers disagreed\","
            "\"schema\":\"umbraflow-fact/v1\",\"status\":\"Conflict\"},"
            "\"schema\":\"umbraflow-collection-fact/v1\"}"
        };

        // One clause of a published fragment, and one document that differs
        // from an accepted one only in what that clause judges. A row that
        // stops refusing names the check that was lost with the copy.
        struct FragmentCase final
        {
            std::string_view clause{};
            std::string      knowledge{};
            bool             accepted{};
        };

        [[nodiscard]]
        auto jsonString(std::string_view text) -> std::string
        {
            auto literal = std::string{"\""};
            literal += text;
            literal += '"';
            return literal;
        }

        // A deployment whose project state names one published fragment, and
        // nothing else changed: no manifest carries the project state schema's
        // digest, so substituting it leaves every other link intact.
        [[nodiscard]]
        auto knowledgeDeployment(
            umbraflow::DeploymentBundle const& bundle,
            std::string_view stateSchema
        ) -> ProjectDeployment
        {
            auto sources         = bundle.sources();
            sources.projectState = stateSchema;
            auto deployed        = ProjectDeployment::create(sources);
            auto const why       = deployed.has_value()
                      ? std::string{}
                      : std::string{deployed.error().message()};
            INFO(why);
            REQUIRE(deployed.has_value());
            return *std::move(deployed);
        }
    }

    // The canonical validator is one function for every project, so one case
    // answers for both exemplars. What differs is the allowlist each of them
    // used to carry, and both carried the plan-envelope recognizer below.
    TEST_CASE("the canonical validator refuses bytes the exemplars' allowlists accepted")
    {
        auto const validate = canonicalJsonValidator();

        // The premise: the deleted recognizer accepts these. Without this the
        // refusals below would prove nothing about what changed.
        REQUIRE(oldPlanEnvelopeRecognizer(k_unsortedPlanEnvelope));
        REQUIRE(oldPlanEnvelopeRecognizer(k_nonCanonicalNumberPlanEnvelope));

        CHECK_FALSE(validate(k_unsortedPlanEnvelope).has_value());
        CHECK_FALSE(validate(k_nonCanonicalNumberPlanEnvelope).has_value());

        // Whitespace, a duplicate member, an escape RFC 8785 does not emit, and
        // a trailing byte: none of them is a canonical document either.
        CHECK_FALSE(validate("{\"a\": 1}").has_value());
        CHECK_FALSE(validate("{\"a\":1,\"a\":2}").has_value());
        CHECK_FALSE(validate("{\"a\":\"\\u0041\"}").has_value());
        CHECK_FALSE(validate("{\"a\":1} ").has_value());

        // And it accepts what a project actually mints, so the refusals above
        // are about canonical form rather than about refusing everything.
        CHECK(validate("{\"revision\":0}").has_value());
        CHECK(validate(k_provenance).has_value());
        CHECK(validate(planEnvelope("{\"value\":1}")).has_value());
        CHECK(validate("{\"turn\":0}").has_value());
    }

    TEST_CASE("the document validator refuses documents the exemplars' switches accepted")
    {
        auto const umbraflowBundle = umbraflow::DeploymentBundle{"fixture.alpha"};
        auto const validate =
            umbraflowDeployment(umbraflowBundle).documentValidator();

        // The umbraflow exemplar read a reconcile document as any object whose
        // bytes opened with the member name and closed with a quoted string.
        constexpr auto forgedDisposition =
            std::string_view{"{\"disposition\":\"forged\"}"};
        REQUIRE(oldDispositionRecognizer(forgedDisposition));
        CHECK_FALSE(validate(
            ProjectPluginFunction::Reconcile,
            ProjectDocumentDirection::Output,
            forgedDisposition
        ).has_value());
        CHECK(validate(
            ProjectPluginFunction::Reconcile,
            ProjectDocumentDirection::Output,
            "{\"disposition\":\"confirmed\"}"
        ).has_value());

        // The plan envelope, likewise: the recognizer never asked what any
        // member's value was.
        REQUIRE(oldPlanEnvelopeRecognizer(k_unsortedPlanEnvelope));
        CHECK_FALSE(validate(
            ProjectPluginFunction::Plan,
            ProjectDocumentDirection::Input,
            k_unsortedPlanEnvelope
        ).has_value());
        CHECK(validate(
            ProjectPluginFunction::Plan,
            ProjectDocumentDirection::Input,
            planEnvelope("{\"value\":1}")
        ).has_value());

        // The envelope's own shape, isolated from everything nested in it: a
        // member the Operator never puts in a plan input, with a tool name and
        // arguments this project's catalog does accept. Only the envelope
        // schema can refuse this one -- so the observation it carries is the
        // same valid envelope planEnvelope embeds, and "extra" is the one
        // clause that can refuse.
        CHECK_FALSE(validate(
            ProjectPluginFunction::Plan,
            ProjectDocumentDirection::Input,
            "{\"canonical_args\":{\"value\":1},\"extra\":1,"
            "\"project_observation\":{\"canonical_opaque_payload\":{},"
            "\"observed_instances\":[],\"project_tool_preconditions\":[],"
            "\"schema\":\"umbraflow-project-observation/v1\"},"
            "\"project_state\":{\"revision\":0},\"tool_name\":\"command-1\","
            "\"tool_version\":\"1\"}"
        ).has_value());

        auto const arcanaBundle = arcana::DeploymentBundle{"arcana.expedition"};
        auto const judgeExpedition =
            arcanaDeployment(arcanaBundle).documentValidator();

        constexpr auto forgedVerdict = std::string_view{"{\"verdict\":\"forged\"}"};
        REQUIRE(oldVerdictRecognizer(forgedVerdict));
        CHECK_FALSE(judgeExpedition(
            ProjectPluginFunction::Reconcile,
            ProjectDocumentDirection::Output,
            forgedVerdict
        ).has_value());
        CHECK(judgeExpedition(
            ProjectPluginFunction::Reconcile,
            ProjectDocumentDirection::Output,
            "{\"verdict\":\"settled\"}"
        ).has_value());

        // The two projects do not accept each other's documents, which is what
        // makes the schema the project's rather than the framework's.
        CHECK_FALSE(judgeExpedition(
            ProjectPluginFunction::Reduce,
            ProjectDocumentDirection::Output,
            "{\"revision\":0}"
        ).has_value());
        CHECK_FALSE(validate(
            ProjectPluginFunction::Reduce,
            ProjectDocumentDirection::Output,
            "{\"turn\":0}"
        ).has_value());
    }

    // The clause list a project schema owes before it may replace its own copy
    // of the Fact shape with a $ref: every check the copy performed, named, and
    // observed refusing a document that differs from an accepted one only in
    // what that clause judges. A row that stops refusing is a check the copy
    // performed and the fragment does not.
    TEST_CASE("every check of the published Fact fragment fires through a $ref")
    {
        auto const bundle   = umbraflow::DeploymentBundle{"fixture.alpha"};
        auto const validate =
            knowledgeDeployment(bundle, k_factStateSchema).documentValidator();

        constexpr auto k_provenanceArray = std::string_view{
            "[{\"kind\":\"observation\",\"reference\":\"observation-42\"}]"
        };
        constexpr auto k_staleProvenanceArray = std::string_view{
            "[{\"kind\":\"observation\",\"reference\":\"observation-7\"}]"
        };

        auto cases = std::vector<FragmentCase>{};
        auto const admits =
            [&cases](std::string_view clause, std::string knowledge)
        {
            cases.emplace_back(FragmentCase{
                .clause    = clause,
                .knowledge = std::move(knowledge),
                .accepted  = true,
            });
        };
        auto const denies =
            [&cases](std::string_view clause, std::string knowledge)
        {
            cases.emplace_back(FragmentCase{
                .clause    = clause,
                .knowledge = std::move(knowledge),
                .accepted  = false,
            });
        };
        auto const knownWith = [](std::string_view entry) -> std::string
        {
            return substituted(k_knownFact, k_provenanceEntry, entry);
        };
        auto const filler = [](std::size_t size) -> std::string
        {
            return std::string(size, 'o');
        };

        // The four documents every refusal below is one substitution away from.
        admits("an accepted Known fact", std::string{k_knownFact});
        admits("an accepted Unknown fact", std::string{k_unknownFact});
        admits("an accepted Stale fact", std::string{k_staleFact});
        admits("an accepted Conflict fact", std::string{k_conflictFact});

        // The root's own shape.
        denies("type: object", jsonString("not an object"));
        denies(
            "additionalProperties: false",
            substituted(k_knownFact, "\"provenance\"", "\"omen\":true,\"provenance\"")
        );
        denies(
            "required: schema",
            substituted(k_knownFact, "\"schema\":\"umbraflow-fact/v1\",", "")
        );
        denies(
            "required: status",
            substituted(k_knownFact, "\"status\":\"Known\",", "")
        );
        denies(
            "schema: const umbraflow-fact/v1",
            substituted(k_knownFact, "umbraflow-fact/v1", "umbraflow-fact/v2")
        );
        denies("status: enum", substituted(k_knownFact, "\"Known\"", "\"Guessed\""));

        // value carries the boolean schema true, so nothing judges what a fact
        // asserts. Both rows are acceptances: the absence is deliberate, and a
        // constraint appearing there later has to be a decision.
        admits(
            "value: true admits an object",
            substituted(k_knownFact, "\"chaos\"", "{\"depth\":1}")
        );
        admits(
            "value: true admits null",
            substituted(k_knownFact, "\"chaos\"", "null")
        );

        // Every other member's own shape.
        denies(
            "candidates: type array",
            substituted(k_conflictFact, "[\"chaos\",\"order\"]", "\"chaos\"")
        );
        denies(
            "candidates: minItems 2",
            substituted(k_conflictFact, "[\"chaos\",\"order\"]", "[\"chaos\"]")
        );
        denies(
            "confirmed_at_project_revision: type integer",
            substituted(
                k_knownFact,
                "\"confirmed_at_project_revision\":8",
                "\"confirmed_at_project_revision\":1.5"
            )
        );
        denies(
            "confirmed_at_project_revision: minimum 0",
            substituted(
                k_knownFact,
                "\"confirmed_at_project_revision\":8",
                "\"confirmed_at_project_revision\":-1"
            )
        );
        admits(
            "confirmed_at_run_generation: 1 is the least generation",
            substituted(
                k_knownFact,
                "\"provenance\"",
                "\"confirmed_at_run_generation\":1,\"provenance\""
            )
        );
        denies(
            "confirmed_at_run_generation: minimum 1",
            substituted(
                k_knownFact,
                "\"provenance\"",
                "\"confirmed_at_run_generation\":0,\"provenance\""
            )
        );
        denies(
            "confirmed_at_run_generation: type integer",
            substituted(
                k_knownFact,
                "\"provenance\"",
                "\"confirmed_at_run_generation\":1.5,\"provenance\""
            )
        );
        admits(
            "source_observation_id: a bounded string",
            substituted(
                k_knownFact,
                "\"status\"",
                "\"source_observation_id\":\"observation-42\",\"status\""
            )
        );
        denies(
            "source_observation_id: type string",
            substituted(
                k_knownFact,
                "\"status\"",
                "\"source_observation_id\":12,\"status\""
            )
        );
        denies(
            "source_observation_id: minLength 1",
            substituted(
                k_knownFact,
                "\"status\"",
                "\"source_observation_id\":\"\",\"status\""
            )
        );
        denies(
            "source_observation_id: maxLength 128",
            substituted(
                k_knownFact,
                "\"status\"",
                "\"source_observation_id\":" + jsonString(filler(129U)) + ",\"status\""
            )
        );
        denies(
            "reason: type string",
            substituted(k_unknownFact, "\"the panel was not on screen\"", "5")
        );
        denies(
            "reason: minLength 1",
            substituted(k_unknownFact, "\"the panel was not on screen\"", "\"\"")
        );
        denies(
            "reason: maxLength 512",
            substituted(
                k_unknownFact,
                "\"the panel was not on screen\"",
                jsonString(filler(513U))
            )
        );
        denies(
            "provenance: type array",
            substituted(k_knownFact, k_provenanceArray, k_provenanceEntry)
        );

        // The second hop of the closure: provenance items are judged by the
        // separately published fact-provenance document, so these rows fire
        // only when a $ref inside a $ref-ed document resolved.
        denies(
            "fact-provenance: type object",
            knownWith(jsonString("observation-42"))
        );
        denies(
            "fact-provenance: additionalProperties false",
            knownWith(
                "{\"kind\":\"observation\",\"operator_trust\":\"high\","
                "\"reference\":\"observation-42\"}"
            )
        );
        denies(
            "fact-provenance: required kind",
            knownWith("{\"reference\":\"observation-42\"}")
        );
        denies(
            "fact-provenance: required reference",
            knownWith("{\"kind\":\"observation\"}")
        );
        denies(
            "fact-provenance: kind enum",
            knownWith("{\"kind\":\"rumour\",\"reference\":\"observation-42\"}")
        );
        denies(
            "fact-provenance: reference type string",
            knownWith("{\"kind\":\"observation\",\"reference\":7}")
        );
        denies(
            "fact-provenance: reference minLength 1",
            knownWith("{\"kind\":\"observation\",\"reference\":\"\"}")
        );
        denies(
            "fact-provenance: reference maxLength 512",
            knownWith(
                "{\"kind\":\"observation\",\"reference\":" + jsonString(filler(513U))
                + "}"
            )
        );
        admits(
            "fact-provenance: project_revision 0",
            knownWith(
                "{\"kind\":\"observation\",\"project_revision\":0,"
                "\"reference\":\"observation-42\"}"
            )
        );
        denies(
            "fact-provenance: project_revision minimum 0",
            knownWith(
                "{\"kind\":\"observation\",\"project_revision\":-1,"
                "\"reference\":\"observation-42\"}"
            )
        );
        denies(
            "fact-provenance: project_revision type integer",
            knownWith(
                "{\"kind\":\"observation\",\"project_revision\":1.5,"
                "\"reference\":\"observation-42\"}"
            )
        );
        admits(
            "fact-provenance: detail object",
            knownWith(
                "{\"detail\":{},\"kind\":\"observation\","
                "\"reference\":\"observation-42\"}"
            )
        );
        denies(
            "fact-provenance: detail type object",
            knownWith(
                "{\"detail\":\"why\",\"kind\":\"observation\","
                "\"reference\":\"observation-42\"}"
            )
        );

        // The four cross-member conditionals, which are the rules the plugin's
        // own code states nowhere.
        denies(
            "status Known requires value",
            substituted(k_knownFact, ",\"value\":\"chaos\"", "")
        );
        denies(
            "status Known requires confirmed_at_project_revision",
            substituted(k_knownFact, "\"confirmed_at_project_revision\":8,", "")
        );
        denies(
            "status Known requires provenance",
            substituted(
                k_knownFact,
                "\"provenance\":[{\"kind\":\"observation\","
                "\"reference\":\"observation-42\"}],",
                ""
            )
        );
        denies(
            "status Known requires provenance minItems 1",
            substituted(k_knownFact, k_provenanceArray, "[]")
        );
        denies(
            "status Known forbids candidates",
            substituted(
                k_knownFact,
                "{\"confirmed",
                "{\"candidates\":[\"chaos\",\"order\"],\"confirmed"
            )
        );
        denies(
            "status Known forbids reason",
            substituted(k_knownFact, "\"schema\"", "\"reason\":\"stale\",\"schema\"")
        );
        denies(
            "status Unknown requires reason",
            substituted(k_unknownFact, "\"reason\":\"the panel was not on screen\",", "")
        );
        denies(
            "status Unknown forbids value",
            substituted(
                k_unknownFact,
                "\"status\":\"Unknown\"",
                "\"status\":\"Unknown\",\"value\":\"chaos\""
            )
        );
        denies(
            "status Unknown forbids candidates",
            substituted(
                k_unknownFact,
                "{\"reason\"",
                "{\"candidates\":[\"chaos\",\"order\"],\"reason\""
            )
        );
        denies(
            "status Unknown forbids confirmed_at_project_revision",
            substituted(
                k_unknownFact,
                "{\"reason\"",
                "{\"confirmed_at_project_revision\":1,\"reason\""
            )
        );
        denies(
            "status Unknown forbids confirmed_at_run_generation",
            substituted(
                k_unknownFact,
                "{\"reason\"",
                "{\"confirmed_at_run_generation\":1,\"reason\""
            )
        );
        denies(
            "status Stale requires value",
            substituted(k_staleFact, ",\"value\":\"chaos\"", "")
        );
        denies(
            "status Stale requires confirmed_at_project_revision",
            substituted(k_staleFact, "\"confirmed_at_project_revision\":3,", "")
        );
        denies(
            "status Stale requires provenance",
            substituted(
                k_staleFact,
                "\"provenance\":[{\"kind\":\"observation\","
                "\"reference\":\"observation-7\"}],",
                ""
            )
        );
        denies(
            "status Stale requires provenance minItems 1",
            substituted(k_staleFact, k_staleProvenanceArray, "[]")
        );
        denies(
            "status Stale requires reason",
            substituted(k_staleFact, "\"reason\":\"the screen moved on\",", "")
        );
        denies(
            "status Stale forbids candidates",
            substituted(
                k_staleFact,
                "{\"confirmed",
                "{\"candidates\":[\"chaos\",\"order\"],\"confirmed"
            )
        );
        denies(
            "status Conflict requires candidates",
            substituted(k_conflictFact, "\"candidates\":[\"chaos\",\"order\"],", "")
        );
        denies(
            "status Conflict requires reason",
            substituted(k_conflictFact, "\"reason\":\"two readers disagreed\",", "")
        );
        denies(
            "status Conflict forbids value",
            substituted(
                k_conflictFact,
                "\"status\":\"Conflict\"",
                "\"status\":\"Conflict\",\"value\":\"chaos\""
            )
        );

        for (auto const& entry : cases)
        {
            CAPTURE(entry.clause);
            CAPTURE(entry.knowledge);
            auto const outcome = validate(
                ProjectPluginFunction::Reduce,
                ProjectDocumentDirection::Output,
                knowledgeState(entry.knowledge)
            );
            CHECK(outcome.has_value() == entry.accepted);
        }
    }

    // The same enumeration for the second published fragment. Its metadata is
    // a Fact, so these rows also fire only when a three-document closure held.
    TEST_CASE("every check of the published Collection Fact fragment fires through a $ref")
    {
        auto const bundle = umbraflow::DeploymentBundle{"fixture.alpha"};
        auto const validate =
            knowledgeDeployment(bundle, k_collectionStateSchema)
                .documentValidator();

        constexpr auto k_knownMetadata = std::string_view{
            "\"metadata\":{\"confirmed_at_project_revision\":2,"
            "\"provenance\":[{\"kind\":\"observation\","
            "\"reference\":\"observation-9\"}],"
            "\"schema\":\"umbraflow-fact/v1\",\"status\":\"Known\","
            "\"value\":{\"item_count\":1}},"
        };

        auto cases = std::vector<FragmentCase>{};
        auto const admits =
            [&cases](std::string_view clause, std::string knowledge)
        {
            cases.emplace_back(FragmentCase{
                .clause    = clause,
                .knowledge = std::move(knowledge),
                .accepted  = true,
            });
        };
        auto const denies =
            [&cases](std::string_view clause, std::string knowledge)
        {
            cases.emplace_back(FragmentCase{
                .clause    = clause,
                .knowledge = std::move(knowledge),
                .accepted  = false,
            });
        };

        admits("an accepted Known collection", std::string{k_knownCollection});
        admits("an accepted Unknown collection", std::string{k_unknownCollection});
        admits("an accepted Conflict collection", std::string{k_conflictCollection});

        denies("type: object", jsonString("not an object"));
        denies(
            "additionalProperties: false",
            substituted(k_knownCollection, "\"items\"", "\"extra\":1,\"items\"")
        );
        denies(
            "required: completeness",
            substituted(k_knownCollection, "\"completeness\":\"Complete\",", "")
        );
        denies(
            "required: items",
            substituted(k_knownCollection, "\"items\":[\"chaos\"],", "")
        );
        denies(
            "required: metadata",
            substituted(k_knownCollection, k_knownMetadata, "")
        );
        denies(
            "required: schema",
            substituted(
                k_knownCollection,
                ",\"schema\":\"umbraflow-collection-fact/v1\"",
                ""
            )
        );
        denies(
            "schema: const umbraflow-collection-fact/v1",
            substituted(
                k_knownCollection,
                "umbraflow-collection-fact/v1",
                "umbraflow-collection-fact/v2"
            )
        );
        denies(
            "items: type array",
            substituted(k_knownCollection, "\"items\":[\"chaos\"]", "\"items\":\"chaos\"")
        );
        denies(
            "completeness: enum",
            substituted(k_knownCollection, "\"Complete\"", "\"Total\"")
        );
        denies(
            "metadata: $ref umbraflow-fact/v1 status enum",
            substituted(k_knownCollection, "\"status\":\"Known\"", "\"status\":\"Guessed\"")
        );
        denies(
            "metadata: $ref umbraflow-fact/v1 additionalProperties false",
            substituted(k_knownCollection, "\"provenance\"", "\"omen\":true,\"provenance\"")
        );

        // The $ref's sibling properties apply beside it, which is what lets a
        // collection say what its own metadata value holds.
        denies(
            "metadata.value: required item_count",
            substituted(k_knownCollection, "{\"item_count\":1}", "{}")
        );
        denies(
            "metadata.value: additionalProperties false",
            substituted(
                k_knownCollection,
                "{\"item_count\":1}",
                "{\"item_count\":1,\"tally\":1}"
            )
        );
        denies(
            "metadata.value: item_count minimum 0",
            substituted(k_knownCollection, "{\"item_count\":1}", "{\"item_count\":-1}")
        );
        denies(
            "metadata.value: item_count type integer",
            substituted(k_knownCollection, "{\"item_count\":1}", "{\"item_count\":1.5}")
        );

        denies(
            "metadata Unknown holds no items",
            substituted(k_unknownCollection, "\"items\":[]", "\"items\":[\"chaos\"]")
        );
        denies(
            "metadata Unknown forces completeness Unknown",
            substituted(
                k_unknownCollection,
                "\"completeness\":\"Unknown\"",
                "\"completeness\":\"Complete\""
            )
        );
        denies(
            "metadata Conflict holds no items",
            substituted(k_conflictCollection, "\"items\":[]", "\"items\":[\"chaos\"]")
        );

        for (auto const& entry : cases)
        {
            CAPTURE(entry.clause);
            CAPTURE(entry.knowledge);
            auto const outcome = validate(
                ProjectPluginFunction::Reduce,
                ProjectDocumentDirection::Output,
                knowledgeState(entry.knowledge)
            );
            CHECK(outcome.has_value() == entry.accepted);
        }
    }

    // The three ways a reference fails to resolve, reported apart because each
    // has a different repair: remove the reference, widen the set, or fix the
    // pointer. A single message for all three would leave a schema author
    // guessing which of the three happened.
    TEST_CASE("a project schema's unresolvable references are refused apart")
    {
        struct RefusalCase final
        {
            std::string_view reference{};
            std::string_view diagnostic{};
        };

        constexpr auto k_cases = std::array{
            RefusalCase{
                .reference  = "https://elsewhere.test/schema.json",
                .diagnostic = "a remote document, and this evaluator fetches nothing",
            },
            RefusalCase{
                .reference  = "https://umbraflow.dev/schema/project/observation",
                .diagnostic = "outside the set this schema was compiled from",
            },
            RefusalCase{
                .reference  = "https://umbraflow.dev/schema/fact/v1#/$defs/Missing",
                .diagnostic = "no target in the document it names",
            },
        };

        auto const bundle = umbraflow::DeploymentBundle{"fixture.alpha"};
        for (auto const& entry : k_cases)
        {
            CAPTURE(entry.reference);
            auto schema = std::string{
                R"json({"$schema":"https://json-schema.org/draft/2020-12/schema",)json"
                R"json("$id":"https://umbraflow.dev/schema/project/state","$ref":")json"
            };
            schema += entry.reference;
            schema += R"json("})json";

            auto sources         = bundle.sources();
            sources.projectState = schema;
            auto const refused   = ProjectDeployment::create(sources);
            REQUIRE_FALSE(refused.has_value());
            CHECK(why(refused).contains(entry.diagnostic));
        }
    }

    // The four schema families a registration owns that no other document
    // embeds, probed with a pointer that misses inside the published Fact
    // document. The refusal naming the pointer rather than the document is what
    // says the document was in that compiler's closed set: the case above shows
    // that a document the set does not carry is refused with the other message
    // instead.
    //
    // The project state and observation documents cannot be probed this way and
    // are not probed here. Both are embedded in the operator envelope schemas,
    // which compile first, so a bad reference in either is always reported by
    // the envelope's compilation and the probe would pass whatever the
    // project's own compilation received. They are proved instead by the two
    // fragment cases above and the observation case below, each of which
    // applies the schema that compilation produced.
    TEST_CASE("every registration-owned schema compiler receives the Fact closure")
    {
        constexpr auto k_missingTarget = std::string_view{
            "https://umbraflow.dev/schema/fact/v1#/$defs/Missing"
        };
        constexpr auto k_inClosure =
            std::string_view{"no target in the document it names"};

        // Appending the view binds it to a reference, so it is odr-used and
        // must be captured however constant it is.
        auto const referencing =
            [k_missingTarget](std::string_view identity) -> std::string
        {
            auto text = std::string{
                R"json({"$schema":"https://json-schema.org/draft/2020-12/schema",)json"
                R"json("$id":")json"
            };
            text += identity;
            text += R"json(","$ref":")json";
            text += k_missingTarget;
            text += R"json("})json";
            return text;
        };
        auto const bundle = umbraflow::DeploymentBundle{"fixture.alpha"};

        auto preconditionSources             = bundle.sources();
        auto const preconditionSchema        = referencing(k_toolPreconditionSchemaId);
        preconditionSources.toolPrecondition = preconditionSchema;
        auto const precondition = ProjectDeployment::create(preconditionSources);
        REQUIRE_FALSE(precondition.has_value());
        CHECK(why(precondition).contains(k_inClosure));

        auto reconcileSources      = bundle.sources();
        auto const reconcileSchema = referencing(k_reconcileSchemaId);
        reconcileSources.reconcile = reconcileSchema;
        auto const reconcile       = ProjectDeployment::create(reconcileSources);
        REQUIRE_FALSE(reconcile.has_value());
        CHECK(why(reconcile).contains(k_inClosure));

        auto journalSources      = bundle.sources();
        auto const journalSchema = referencing(
            "https://umbraflow.dev/schema/project/journal/missing-target"
        );
        auto const journalSchemas = std::array{std::string_view{journalSchema}};
        journalSources.journalPayloadSchemas = journalSchemas;
        auto const journal = ProjectDeployment::create(journalSources);
        REQUIRE_FALSE(journal.has_value());
        CHECK(why(journal).contains(k_inClosure));

        auto effectSources      = bundle.sources();
        auto const effectSchema = referencing(
            "https://umbraflow.dev/schema/project/effect/missing-target"
        );
        auto const effectSchemas = std::array{std::string_view{effectSchema}};
        effectSources.effectPayloadSchemas = effectSchemas;
        auto const effect = ProjectDeployment::create(effectSources);
        REQUIRE_FALSE(effect.has_value());
        CHECK(why(effect).contains(k_inClosure));
    }

    // The observation compiler's own closure, proved by the schema it produced
    // rather than by a refusal message an earlier compilation could have
    // written. A derived observation is judged by projectObservation.validate,
    // so a Fact clause firing here fired inside that compilation's document set.
    TEST_CASE("the project observation compiler receives the Fact closure")
    {
        constexpr auto k_factObservationSchema = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/observation",
    "title": "a project observation holding one Fact",
    "type": "object",
    "additionalProperties": false,
    "required": ["reading"],
    "properties": {
        "reading": {"$ref": "https://umbraflow.dev/schema/fact/v1"}
    }
})json"};

        auto const bundle          = umbraflow::DeploymentBundle{"fixture.alpha"};
        auto sources               = bundle.sources();
        sources.projectObservation = k_factObservationSchema;
        auto const deployed        = ProjectDeployment::create(sources);
        INFO(why(deployed));
        REQUIRE(deployed.has_value());

        auto const validate = deployed->documentValidator();
        auto const observed = [&validate](std::string_view fact)
        {
            auto document = std::string{"{\"reading\":"};
            document += fact;
            document += "}";
            return validate(
                ProjectPluginFunction::Derive,
                ProjectDocumentDirection::Output,
                document
            ).has_value();
        };

        CHECK(observed(k_knownFact));
        CHECK_FALSE(observed(substituted(k_knownFact, ",\"value\":\"chaos\"", "")));
        CHECK_FALSE(observed(substituted(k_knownFact, "\"Known\"", "\"Guessed\"")));
    }

    TEST_CASE("the framework schema catalog publishes every runtime schema source")
    {
        auto const catalog = framework_schema::frameworkSchemaCatalog();
        CHECK_MESSAGE(
            catalog.size() == 12U,
            "framework schema catalog must contain exactly twelve declared sources"
        );

        auto const collectionFact = framework_schema::findFrameworkSchema(
            "schema/umbraflow-collection-fact-v1.schema.json"
        );
        REQUIRE_MESSAGE(
            collectionFact.has_value(),
            "framework schema catalog must include collection Fact"
        );
        CHECK(valueOf(collectionFact).identity
              == "https://umbraflow.dev/schema/collection-fact/v1");
        CHECK(valueOf(collectionFact).sha256
              == schemaDigest(valueOf(collectionFact).relativePath));

        auto const workflowTool = framework_schema::findFrameworkSchema(
            "schema/umbraflow-declarative-workflow-tool-v1.schema.json"
        );
        REQUIRE_MESSAGE(
            workflowTool.has_value(),
            "framework schema catalog must include declarative workflow tools"
        );
        CHECK(valueOf(workflowTool).identity
              == "https://umbraflow.dev/schema/declarative-workflow-tool/v1");

        // The product lifecycle facade reads the Operator protocol schema out of
        // this catalog, so a catalog without it fails at first observe rather
        // than at load.
        auto const operatorProtocol = framework_schema::findFrameworkSchema(
            "schema/umbraflow-operator-v1.schema.json"
        );
        REQUIRE_MESSAGE(
            operatorProtocol.has_value(),
            "framework schema catalog must include the Operator protocol schema"
        );

        auto const provenance = framework_schema::findFrameworkSchema(
            "schema/umbraflow-fact-provenance-v1.schema.json"
        );
        REQUIRE_MESSAGE(
            provenance.has_value(),
            "framework schema catalog must include Fact provenance"
        );
        CHECK(valueOf(provenance).identity
              == "https://umbraflow.dev/schema/fact-provenance/v1");
        CHECK(valueOf(provenance).sha256
              == schemaDigest(valueOf(provenance).relativePath));

        auto const fact = framework_schema::findFrameworkSchema(
            "schema/umbraflow-fact-v1.schema.json"
        );
        REQUIRE_MESSAGE(
            fact.has_value(),
            "framework schema catalog must include Fact"
        );
        CHECK(valueOf(fact).identity == "https://umbraflow.dev/schema/fact/v1");
        CHECK(valueOf(fact).sha256
              == schemaDigest(valueOf(fact).relativePath));

        auto const policy = framework_schema::findFrameworkSchema(
            "schema/umbraflow-policy-v1.schema.json"
        );
        REQUIRE_MESSAGE(
            policy.has_value(),
            "framework schema catalog must include Operator policy"
        );
        CHECK(valueOf(policy).identity == "https://umbraflow.local/schema/policy-v1");

        auto const registration = framework_schema::findFrameworkSchema(
            "schema/umbraflow-project-registration-v1.schema.json"
        );
        REQUIRE_MESSAGE(
            registration.has_value(),
            "framework schema catalog must include project registration"
        );
        CHECK(valueOf(registration).identity
              == "https://umbraflow.local/schema/project-registration-v1");

        // umbraflow-project.json's shape, published because two readers that
        // cannot link one another both compile it: the runtime loader and the
        // offline project kit. No digest is asserted for it -- it is this
        // repository's own document and is not a member of the consumer's
        // interface lock, so a digest here would be pinned against nothing.
        auto const directory = framework_schema::findFrameworkSchema(
            "schema/umbraflow-project-v1.schema.json"
        );
        REQUIRE_MESSAGE(
            directory.has_value(),
            "framework schema catalog must include the project directory shape"
        );
        CHECK(valueOf(directory).identity
              == "https://umbraflow.dev/schema/project/directory");
    }

    // The ruling on run.ended-v1, made executable. A journal event type reaches
    // the Operator's payload_schema_hash through the manifest entry that names
    // its schema, so "this event type has no payload schema" is not a thing the
    // manifest can say: an entry without a digest is refused, and an event type
    // with no entry cannot be emitted. An event whose payload is genuinely empty
    // still publishes the schema that says so.
    TEST_CASE("a journal event type cannot be carried without a payload schema")
    {
        auto const bundle = umbraflow::DeploymentBundle{"fixture.alpha"};

        auto const withoutDigest = substituted(
            bundle.journalEventManifest(),
            "\"sha256\":\"" + umbraflow::schemaHashHex(umbraflow::k_progressPayloadSchema)
                + "\"",
            "\"sha256\":\"\""
        );
        auto digestSources                 = bundle.sources();
        digestSources.journalEventManifest = withoutDigest;
        auto const refusedDigest = ProjectDeployment::create(digestSources);
        REQUIRE_FALSE(refusedDigest.has_value());
        INFO(why(refusedDigest));
        CHECK(why(refusedDigest).contains("sha256"));

        // And the other half: an event type the manifest never named has no
        // schema to be judged by, so the Operator refuses it rather than
        // carrying an unjudged payload.
        auto const validate =
            umbraflowDeployment(bundle).documentValidator();
        auto const unnamed = substituted(
            reduceEnvelope("{\"value\":1}"),
            "fixture.progress",
            "fixture.unnamed"
        );
        auto const refusedEvent = validate(
            ProjectPluginFunction::Reduce,
            ProjectDocumentDirection::Input,
            unnamed
        );
        REQUIRE_FALSE(refusedEvent.has_value());
        CHECK(std::string{refusedEvent.error().message()}.contains(
            "names no payload schema for fixture.unnamed"
        ));
    }

    // The ruling on which schema keywords this evaluator can never reach. A
    // tool precondition document and a reconcile document are judged only
    // through the definition their manifest names -- validateDefinition, never
    // validate -- so nothing at either root is ever evaluated. Both roots below
    // carry an assertion that would refuse every instance, and both documents
    // are still accepted, which is what licenses deleting a root keyword from
    // one of those two families and nothing else.
    TEST_CASE("the tool precondition and reconcile roots are never evaluated")
    {
        auto const bundle = umbraflow::DeploymentBundle{"fixture.alpha"};

        auto const precondition = substituted(
            umbraflow::k_toolPreconditionSchema,
            "    \"$defs\": {",
            "    \"type\": \"null\",\n    \"$defs\": {"
        );
        auto const reconcile = substituted(
            umbraflow::k_reconcileSchema,
            "    \"$defs\": {",
            "    \"type\": \"null\",\n    \"$defs\": {"
        );

        // Everything else repaired: both digests are named by a manifest, so a
        // refusal here would be about the digest rather than about the root.
        auto const catalog = substituted(
            bundle.toolCatalog(),
            umbraflow::schemaHashHex(umbraflow::k_toolPreconditionSchema),
            umbraflow::schemaHashHex(precondition)
        );
        auto const manifest = substituted(
            bundle.reconcileManifest(),
            umbraflow::schemaHashHex(umbraflow::k_reconcileSchema),
            umbraflow::schemaHashHex(reconcile)
        );

        auto sources              = bundle.sources();
        sources.toolPrecondition  = precondition;
        sources.reconcile         = reconcile;
        sources.toolCatalog       = catalog;
        sources.reconcileManifest = manifest;

        auto const deployed = ProjectDeployment::create(sources);
        INFO(why(deployed));
        REQUIRE(deployed.has_value());

        auto const validate = deployed->documentValidator();
        CHECK(validate(
            ProjectPluginFunction::Plan,
            ProjectDocumentDirection::Input,
            planEnvelope("{\"value\":1}")
        ).has_value());
        CHECK(validate(
            ProjectPluginFunction::Reconcile,
            ProjectDocumentDirection::Output,
            "{\"disposition\":\"confirmed\"}"
        ).has_value());

        // The named definitions still refuse what they refused, so the inert
        // root is the root's own property rather than the whole document's.
        CHECK_FALSE(validate(
            ProjectPluginFunction::Plan,
            ProjectDocumentDirection::Input,
            planEnvelope("{\"value\":9}")
        ).has_value());
        CHECK_FALSE(validate(
            ProjectPluginFunction::Reconcile,
            ProjectDocumentDirection::Output,
            "{\"disposition\":\"forged\"}"
        ).has_value());
    }

    // The requirement the header states: every project-owned payload nested
    // inside an envelope is judged, not carried through.
    TEST_CASE("every project-owned nested payload is judged")
    {
        auto const bundle   = umbraflow::DeploymentBundle{"fixture.alpha"};
        auto const validate = umbraflowDeployment(bundle).documentValidator();
        auto const effectHex =
            umbraflow::schemaHashHex(umbraflow::k_effectPayloadSchema);

        // A reduce envelope whose shape is exact and whose one event carries a
        // payload fixture.progress does not accept.
        CHECK(validate(
            ProjectPluginFunction::Reduce,
            ProjectDocumentDirection::Input,
            reduceEnvelope("{\"value\":1}")
        ).has_value());
        CHECK_FALSE(validate(
            ProjectPluginFunction::Reduce,
            ProjectDocumentDirection::Input,
            reduceEnvelope("{\"value\":2}")
        ).has_value());

        // A plan envelope whose arguments the invoked tool's own definition
        // refuses: 0 is below the minimum FixtureArguments states.
        CHECK_FALSE(validate(
            ProjectPluginFunction::Plan,
            ProjectDocumentDirection::Input,
            planEnvelope("{\"value\":0}")
        ).has_value());

        // An OP:PlanProposal whose effect payload its own payload_schema_hash
        // refuses, and one naming a payload schema this deployment does not
        // carry at all.
        CHECK(validate(
            ProjectPluginFunction::Plan,
            ProjectDocumentDirection::Output,
            planProposal("{\"value\":1}", effectHex)
        ).has_value());
        CHECK_FALSE(validate(
            ProjectPluginFunction::Plan,
            ProjectDocumentDirection::Output,
            planProposal("{\"value\":-1}", effectHex)
        ).has_value());
        CHECK_FALSE(validate(
            ProjectPluginFunction::Plan,
            ProjectDocumentDirection::Output,
            planProposal(
                "{\"value\":1}",
                "00000000000000000000000000000000000000000000000000000000000000a1"
            )
        ).has_value());

        // The derive envelope's two project members, and the framework's own
        // ui_snapshot, are each judged where they sit.
        constexpr auto deriveEnvelope = std::string_view{
            "{\"pending_operation_transition\":null,"
            "\"pinned_project_artifact_identities\":[],"
            "\"prior_project_observation\":null,\"project_state\":{\"revision\":0},"
            "\"ui_snapshot\":{\"kind\":\"resolved_state\","
            "\"ordered_surface_stack\":[\"fixture.surface\"]}}"
        };
        CHECK(validate(
            ProjectPluginFunction::Derive,
            ProjectDocumentDirection::Input,
            deriveEnvelope
        ).has_value());
        CHECK_FALSE(validate(
            ProjectPluginFunction::Derive,
            ProjectDocumentDirection::Input,
            "{\"pending_operation_transition\":null,"
            "\"pinned_project_artifact_identities\":[],"
            "\"prior_project_observation\":null,\"project_state\":{\"turn\":0},"
            "\"ui_snapshot\":{\"kind\":\"resolved_state\","
            "\"ordered_surface_stack\":[\"fixture.surface\"]}}"
        ).has_value());
        CHECK_FALSE(validate(
            ProjectPluginFunction::Derive,
            ProjectDocumentDirection::Input,
            "{\"pending_operation_transition\":null,"
            "\"pinned_project_artifact_identities\":[],"
            "\"prior_project_observation\":null,\"project_state\":{\"revision\":0},"
            "\"ui_snapshot\":{\"kind\":\"invented_state\"}}"
        ).has_value());

        // The framework's Unknown-reason vocabulary is spelled three times --
        // schema/umbraflow-runtime-v2.schema.json, modules/task/runtime/
        // evidence.luau, and this module's own StateResolution definition --
        // and nothing else holds the three together. budget_exhausted is what
        // TaskHost::observe reports when a cycle stops reading, so a deployment
        // that refused it here would refuse the documents the Host produces.
        // The second row is the control: a validator that never read `reason`
        // would accept both.
        CHECK(validate(
            ProjectPluginFunction::Derive,
            ProjectDocumentDirection::Input,
            deriveEnvelopeReading("budget_exhausted")
        ).has_value());
        CHECK_FALSE(validate(
            ProjectPluginFunction::Derive,
            ProjectDocumentDirection::Input,
            deriveEnvelopeReading("budget_spent")
        ).has_value());
    }

    // A deployment that cannot answer for its own sources refuses to exist,
    // rather than answering for them anyway once a document arrives.
    TEST_CASE("a deployment refuses sources whose links do not close")
    {
        auto const bundle = umbraflow::DeploymentBundle{"fixture.alpha"};

        auto foreignIdentity = bundle.sources();
        foreignIdentity.projectState =
            R"json({"$id":"https://umbraflow.dev/schema/project/elsewhere",)json"
            R"json("type":"object"})json";
        CHECK_FALSE(ProjectDeployment::create(foreignIdentity).has_value());

        // The same refusal where nothing else can reach it. The reconcile
        // schema is named by hash rather than by $ref, so a document declaring
        // another identity, with a manifest that names its hash and its two
        // definitions, closes every other link this deployment checks.
        constexpr auto misidentifiedReconcile = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/elsewhere",
    "$defs": {
        "ReconcileRequest": {"type": "object"},
        "ReconcileVerdict": {"type": "object"}
    }
})json"};
        auto const misidentifiedManifest = std::string{
            R"json({"dispositions":[{"disposition":"continue","value":"continue"}],)json"
            R"json("plugin_id":"fixture.alpha","reconcile_schema_sha256":")json"
        }
            + umbraflow::schemaHashHex(misidentifiedReconcile)
            + R"json(","request_definition":"ReconcileRequest",)json"
              R"json("schema":"umbraflow-reconcile-manifest/v1",)json"
              R"json("verdict_definition":"ReconcileVerdict",)json"
              R"json("verdict_member":"disposition"})json";
        auto misidentified              = bundle.sources();
        misidentified.reconcile         = misidentifiedReconcile;
        misidentified.reconcileManifest = misidentifiedManifest;
        CHECK_FALSE(ProjectDeployment::create(misidentified).has_value());

        auto otherPlugin     = bundle.sources();
        otherPlugin.pluginId = "fixture.other";
        CHECK_FALSE(ProjectDeployment::create(otherPlugin).has_value());

        // The journal manifest names four payload schemas by sha256; handing
        // over three of them leaves one entry naming bytes nobody supplied.
        auto missingPayload = bundle.sources();
        missingPayload.journalPayloadSchemas =
            std::span{umbraflow::k_journalPayloadSchemas}.first(3U);
        CHECK_FALSE(ProjectDeployment::create(missingPayload).has_value());

        // The same pair for the effect payload schemas, whose only route into
        // tool_catalog_hash -- and so into project_registration_hash -- is the
        // catalog's effect_payload_sha256s. Supplying none leaves that member
        // naming bytes nobody handed over.
        auto missingEffect                 = bundle.sources();
        missingEffect.effectPayloadSchemas = {};
        auto const missingEffectOutcome    = ProjectDeployment::create(missingEffect);
        REQUIRE_FALSE(missingEffectOutcome.has_value());
        CHECK(why(missingEffectOutcome).contains(
            umbraflow::schemaHashHex(umbraflow::k_effectPayloadSchema)
        ));
        CHECK(why(missingEffectOutcome).contains(
            "its effect_payload_schemas hash to nothing"
        ));

        // And the other direction, which is the one that decides whether the
        // bytes are inside any digest at all: a complete schema this evaluator
        // compiles, supplied as an effect payload and named by no digest the
        // catalog carries.
        constexpr auto surplusEffectSchemas = std::array{
            umbraflow::k_effectPayloadSchema,
            umbraflow::k_journalPayloadSchemas.front(),
        };
        auto surplusEffect                 = bundle.sources();
        surplusEffect.effectPayloadSchemas = surplusEffectSchemas;
        auto const surplusEffectOutcome    = ProjectDeployment::create(surplusEffect);
        REQUIRE_FALSE(surplusEffectOutcome.has_value());
        CHECK(why(surplusEffectOutcome).contains(
            umbraflow::schemaHashHex(umbraflow::k_journalPayloadSchemas.front())
        ));
        CHECK(why(surplusEffectOutcome).contains(
            "effect_payload_sha256s does not name"
        ));

        // Another project's precondition schema declares none of the argument
        // definitions this catalog names, so the catalog does not attach to it.
        auto foreignPrecondition             = bundle.sources();
        foreignPrecondition.toolPrecondition = arcana::k_toolPreconditionSchema;
        CHECK_FALSE(ProjectDeployment::create(foreignPrecondition).has_value());

        // The same definition under different bytes: every name the catalog
        // states still resolves, so only the sha256 the catalog carries can
        // tell this schema from the one the registration pinned.
        constexpr auto restatedPrecondition = std::string_view{R"json({
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "https://umbraflow.dev/schema/project/tool-precondition",
    "title": "umbraflow fixture tool arguments, restated",
    "$defs": {
        "FixtureArguments": {
            "type": "object",
            "additionalProperties": false,
            "required": ["value"],
            "properties": {
                "value": {"type": "integer", "minimum": 1, "maximum": 8}
            }
        }
    }
})json"};
        auto restated             = bundle.sources();
        restated.toolPrecondition = restatedPrecondition;
        CHECK_FALSE(ProjectDeployment::create(restated).has_value());

        // And the unmodified sources do build one, so the refusals above are
        // about the link that was broken.
        CHECK(ProjectDeployment::create(bundle.sources()).has_value());
    }

    // Every document the two readers used to refuse, refused by whichever of
    // the deployment's two validators refuses it now. Both run inside
    // ProjectSchemaOwner before a ValidatedDocument exists, so none of these
    // can reach a reader at all.
    TEST_CASE("the operator protocol documents are judged before a reader sees them")
    {
        auto const bundle    = umbraflow::DeploymentBundle{"fixture.alpha"};
        auto const validate  = umbraflowDeployment(bundle).documentValidator();
        auto const canonical = canonicalJsonValidator();
        auto const effectHex =
            umbraflow::schemaHashHex(umbraflow::k_effectPayloadSchema);
        auto const exact = planProposal("{\"value\":1}", effectHex);

        auto const judgeProposal = [&validate](std::string_view document)
        {
            return validate(
                ProjectPluginFunction::Plan,
                ProjectDocumentDirection::Output,
                document
            ).has_value();
        };
        auto const judgeIntent = [&validate](std::string_view document)
        {
            return validate(
                ProjectPluginFunction::NextStep,
                ProjectDocumentDirection::Output,
                document
            ).has_value();
        };

        // The premise: unmodified, all three documents pass both gates, so
        // every refusal below is about its own substitution and not about a
        // fixture nothing can succeed against.
        CHECK(canonical(exact).has_value());
        CHECK(judgeProposal(exact));
        CHECK(canonical(k_uiActionIntent).has_value());
        CHECK(judgeIntent(k_uiActionIntent));
        CHECK(canonical(k_waitIntent).has_value());
        CHECK(judgeIntent(k_waitIntent));

        // Not their own RFC 8785 form: one space, and one pair of members in
        // the order a project would write them rather than the order JCS sorts
        // them to. canonicalize refuses these, so they never become a
        // CanonicalJson and cannot be stamped.
        CHECK_FALSE(canonical(
            substituted(exact, "\"risk\":\"low\"", "\"risk\": \"low\"")
        ).has_value());
        CHECK_FALSE(canonical(substituted(
            exact,
            "\"scope_key\":\"alpha\",\"scope_kind\":\"instance\"",
            "\"scope_kind\":\"instance\",\"scope_key\":\"alpha\""
        )).has_value());
        CHECK_FALSE(canonical(
            substituted(k_uiActionIntent, "\"step_key\":", "\"step_key\": ")
        ).has_value());

        // Each of the four ways the definition itself refuses a document that
        // is canonical: a missing member, an extra one, a member of the wrong
        // type, and a value outside an enum. "extra" sorts between "effects"
        // and "tool_name", so the second of these stays canonical and only
        // additionalProperties can answer it.
        CHECK_FALSE(judgeProposal(substituted(exact, ",\"tool_version\":\"1\"", "")));
        CHECK_FALSE(judgeProposal(
            substituted(exact, "\"tool_name\":", "\"extra\":1,\"tool_name\":")
        ));
        CHECK_FALSE(judgeProposal(
            substituted(exact, "\"tool_name\":\"command-1\"", "\"tool_name\":1")
        ));
        CHECK_FALSE(judgeProposal(
            substituted(exact, "\"risk\":\"low\"", "\"risk\":\"unknown\"")
        ));

        // A payload schema identity that is not OP:`Hash`, refused by the
        // definition's own pattern.
        CHECK_FALSE(judgeProposal(
            substituted(exact, effectHex, std::string(64U, 'z'))
        ));

        // The same three for a step intent, plus the case oneOf exists for: a
        // document carrying both shapes' discriminating members satisfies
        // neither, because each definition closes itself.
        CHECK_FALSE(judgeIntent(substituted(
            k_waitIntent,
            "{\"condition\":",
            "{\"action\":{\"action_id\":\"fixture.press\","
            "\"canonical_parameters\":{},\"surface_id\":\"fixture.surface\","
            "\"ui_target_id\":\"fixture.target\"},\"condition\":"
        )));
        CHECK_FALSE(
            judgeIntent(substituted(k_waitIntent, ",\"observation_budget\":4", ""))
        );
        CHECK_FALSE(judgeIntent(substituted(
            k_uiActionIntent,
            "\"delivery_class\":\"delivery_safe\"",
            "\"delivery_class\":\"invented\""
        )));

        // The one refusal that did not move, stated as the positive result it
        // is: the definition bounds each workflow limit from below and none
        // from above, so this document is canonical and conforming and only
        // readPlanProposal's own uint32 range refuses it. Nothing asserts that
        // refusal any more -- reaching it needs a ValidatedDocument carrying
        // this number, and only a plugin can produce one.
        auto const overflowing = substituted(
            exact,
            "\"maximum_steps\":8",
            "\"maximum_steps\":4294967296"
        );
        CHECK(canonical(overflowing).has_value());
        CHECK(judgeProposal(overflowing));
    }
}
