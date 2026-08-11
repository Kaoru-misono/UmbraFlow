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

#include <deployment/project-deployment.hpp>

#include <operator/project-plugin.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace uf::deployment
{
    namespace
    {
        namespace umbraflow = operator_runtime::test_support;
        namespace arcana    = operator_runtime::conformance::expedition;

        using operator_runtime::ProjectDocumentDirection;
        using operator_runtime::ProjectPluginFunction;

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

        [[nodiscard]]
        auto planEnvelope(std::string_view canonicalArgs) -> std::string
        {
            auto envelope = std::string{"{\"canonical_args\":"};
            envelope += canonicalArgs;
            envelope += ",\"project_observation\":{},\"project_state\":{\"revision\":0},"
                        "\"tool_name\":\"command-1\",\"tool_version\":\"1\"}";
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
        // schema can refuse this one.
        CHECK_FALSE(validate(
            ProjectPluginFunction::Plan,
            ProjectDocumentDirection::Input,
            "{\"canonical_args\":{\"value\":1},\"extra\":1,"
            "\"project_observation\":{},\"project_state\":{\"revision\":0},"
            "\"tool_name\":\"command-1\",\"tool_version\":\"1\"}"
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
