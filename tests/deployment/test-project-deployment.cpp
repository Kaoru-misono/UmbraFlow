// What a ProjectDeployment refuses in the declaration it is built from, and
// what it still accepts.
//
// Every refusal below is paired with the acceptance it is the negation of. A
// reader that refused everything would satisfy the first half of each case and
// fail the second, which is the failure mode this file exists to exclude: the
// substituted constants it replaces were green precisely because nothing ever
// asked them to refuse.
//
// After the cut a deployment compiles exactly two kinds of project-supplied
// schema -- each Tool's inline argument_schema and each inline observed-instance
// identity schema -- and each of them compiles alone. There is no framework
// document set around either, so a project's schema reaches nothing but itself.

#include "arcana-expedition/project-schemas.hpp"
#include "umbraflow/project-schemas.hpp"

#include <core/safety/annotations.hpp>

#include <deployment/project-deployment.hpp>

#include <domain/content-hash.hpp>

#include <operator/project-plugin.hpp>
#include <operator/tool-invocation.hpp>

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

        // Why one set of sources was refused, for a case whose subject is the
        // message rather than the outcome. A refusal naming a different defect
        // satisfies CHECK_FALSE exactly as the intended one does, so a case
        // about which rule fired has to read what the refusal said.
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

        [[nodiscard]]
        auto deploymentOf(deployment::ProjectDeploymentSources const& sources)
            -> ProjectDeployment
        {
            auto deployed = ProjectDeployment::create(sources);
            INFO(why(deployed));
            REQUIRE(deployed.has_value());
            return *std::move(deployed);
        }
    }

    // The tool arguments a project accepts are the project's, judged by the
    // schema its own Tool declaration states inline. A call one exemplar admits
    // is a call the other refuses, which is what makes the guard the Project's
    // limit rather than one the framework invented.
    TEST_CASE("the argument validator answers for one project's own Tools")
    {
        auto const umbraflowBundle = umbraflow::DeploymentBundle{"fixture.alpha"};
        auto const judgeFixture =
            deploymentOf(umbraflowBundle.sources()).toolArgumentValidator();

        auto const arcanaBundle = arcana::DeploymentBundle{"arcana.expedition"};
        auto const judgeExpedition =
            deploymentOf(arcanaBundle.sources()).toolArgumentValidator();

        CHECK(judgeFixture(
            "fixture.alpha.command-1",
            R"({"value":1})"
        ).has_value());
        CHECK(judgeExpedition(
            "arcana.expedition.move",
            R"({"steps":1})"
        ).has_value());

        // Neither project's arguments are the other's, and neither validator
        // answers for a Tool its own declaration never carried.
        CHECK_FALSE(judgeFixture(
            "fixture.alpha.command-1",
            R"({"steps":1})"
        ).has_value());
        CHECK_FALSE(judgeExpedition(
            "arcana.expedition.move",
            R"({"value":1})"
        ).has_value());
        CHECK_FALSE(judgeFixture(
            "arcana.expedition.move",
            R"({"steps":1})"
        ).has_value());

        // And the bound each of them states is enforced, so the refusals above
        // are about the schema rather than about refusing everything.
        CHECK_FALSE(judgeFixture(
            "fixture.alpha.command-1",
            R"({"value":9})"
        ).has_value());
        CHECK_FALSE(judgeExpedition(
            "arcana.expedition.move",
            R"({"steps":9})"
        ).has_value());
    }

    // The three ways a reference fails to resolve, reported apart because each
    // has a different repair: remove the reference, widen the set, or fix the
    // pointer. A single message for all three would leave a schema author
    // guessing which of the three happened.
    //
    // An identity schema compiles on its own, so the only document in its set
    // is itself: reuse inside one schema is JSON Schema's own $defs, and there
    // is nothing else for a $ref to reach.
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
                .reference  = "https://umbraflow.dev/schema/fact/v1",
                .diagnostic = "outside the set this schema was compiled from",
            },
            RefusalCase{
                .reference  = "#/$defs/Missing",
                .diagnostic = "no target in the document it names",
            },
        };

        auto const bundle = umbraflow::DeploymentBundle{"fixture.alpha"};
        for (auto const& entry : k_cases)
        {
            CAPTURE(entry.reference);
            auto schema = std::string{
                R"json({"$schema":"https://json-schema.org/draft/2020-12/schema",)json"
                R"json("$id":"https://umbraflow.dev/schema/project/identity",)json"
                R"json("$ref":")json"
            };
            schema += entry.reference;
            schema += R"json("})json";

            auto const declared = std::array{
                ProjectIdentitySchemaSource{
                    .name   = umbraflow::k_observedIdentitySchemaName,
                    .schema = schema,
                },
            };
            auto sources                            = bundle.sources();
            sources.observedInstanceIdentitySchemas = declared;
            auto const refused = ProjectDeployment::create(sources);
            REQUIRE_FALSE(refused.has_value());
            CHECK(why(refused).contains(entry.diagnostic));
        }
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
            "schema/umbraflow-project-registration-v4.schema.json"
        );
        REQUIRE_MESSAGE(
            registration.has_value(),
            "framework schema catalog must include project registration"
        );
        CHECK(valueOf(registration).identity
              == "https://umbraflow.local/schema/project-registration-v4");

        // umbraflow-project.json's shape, published because two readers that
        // cannot link one another both compile it: the runtime loader and the
        // offline project kit. No digest is asserted for it -- it is this
        // repository's own document and is not a member of the consumer's
        // interface lock, so a digest here would be pinned against nothing.
        auto const directory = framework_schema::findFrameworkSchema(
            "schema/umbraflow-project-v3.schema.json"
        );
        REQUIRE_MESSAGE(
            directory.has_value(),
            "framework schema catalog must include the project directory shape"
        );
        CHECK(valueOf(directory).identity
              == "https://umbraflow.dev/schema/project/directory");
    }

    // A deployment that cannot answer for its own declaration refuses to exist,
    // rather than answering for it anyway once a call arrives.
    TEST_CASE("a deployment refuses a declaration it cannot own")
    {
        auto const bundle = umbraflow::DeploymentBundle{"fixture.alpha"};

        // Ownership is the one rule no JSON Schema can state, because it
        // compares a Tool's name with the namespace the deployment registered.
        // The same declaration under another registrant's namespace is a
        // declaration no registration owns.
        auto otherPlugin     = bundle.sources();
        otherPlugin.pluginId = "fixture.other";
        auto const foreign   = ProjectDeployment::create(otherPlugin);
        REQUIRE_FALSE(foreign.has_value());
        CHECK(why(foreign).contains("fixture.other"));

        // Two identity schemas under one name would leave the authority keyed
        // on a name that means two documents.
        constexpr auto repeated = std::array{
            ProjectIdentitySchemaSource{
                .name   = umbraflow::k_observedIdentitySchemaName,
                .schema = umbraflow::k_observedIdentitySchema,
            },
            ProjectIdentitySchemaSource{
                .name   = umbraflow::k_observedIdentitySchemaName,
                .schema = umbraflow::k_observedIdentitySchema,
            },
        };
        auto twice                            = bundle.sources();
        twice.observedInstanceIdentitySchemas = repeated;
        auto const declaredTwice = ProjectDeployment::create(twice);
        REQUIRE_FALSE(declaredTwice.has_value());
        CHECK(why(declaredTwice).contains("twice"));

        // And the unmodified sources do build one, so the refusals above are
        // about the rule that was broken.
        CHECK(ProjectDeployment::create(bundle.sources()).has_value());
    }

    // What tool_runtime_protocol_identity is taken over, and what it is not.
    //
    // The value it replaced was the Framework Tool catalog hash, which covers
    // Framework Tool descriptors and nothing else: the state vocabulary, the
    // identity preimages, the durable record and the canonical-form contract
    // could every one of them move without moving it, so the equality a resume
    // performs was named for a property it could not observe. The last check
    // here is the one that would go red if the two were ever made the same
    // value again.
    TEST_CASE("the Tool Runtime protocol identity is derived from protocol material")
    {
        auto const material = currentToolRuntimeProtocolMaterial();
        if (!material.has_value())
        {
            FAIL(material.error().message());
        }

        // Every member the identity claims to cover, named. A member dropped
        // from the assembly is a protocol change this identity would stop
        // seeing, and this is the only place that says so.
        for (auto const member : std::array{
                 std::string_view{"\"call_vocabulary\":"},
                 std::string_view{"\"canonical_form_contract\":"},
                 std::string_view{"\"durable_record\":"},
                 std::string_view{"\"identity_preimages\":"},
                 std::string_view{"\"tool_declaration_schema\":"},
                 std::string_view{"\"tool_declaration_wire_tag\":"},
             })
        {
            CAPTURE(member);
            CHECK(material->contains(member));
        }

        // The two halves the durable record and the vocabulary each contribute,
        // spot-checked by a value only that half can supply.
        CHECK(material->contains("terminally_unresolved"));
        CHECK(material->contains("CREATE TABLE tool_call_positions("));
        CHECK(material->contains("umbraflow-internal-tool-call-v1"));

        auto const identity = currentToolRuntimeProtocolIdentity();
        REQUIRE(identity.has_value());
        auto const recomputed = sha256(std::as_bytes(std::span{*material}));
        REQUIRE(recomputed.has_value());
        CHECK(*identity == *recomputed);

        // Deriving it twice from one release gives one value, which is what
        // makes the resume equality a join rather than a coin toss.
        auto const again = currentToolRuntimeProtocolIdentity();
        REQUIRE(again.has_value());
        CHECK(*identity == *again);

        auto const catalog = operator_runtime::FrameworkToolCatalogOwner::create();
        REQUIRE(catalog.has_value());
        CHECK_MESSAGE(
            *identity != catalog->toolCatalogHash(),
            "the protocol identity must not be the Framework Tool catalog hash: "
            "a framework call's provider identity IS that hash, and one value "
            "doing both jobs cannot mismatch"
        );
    }
}
