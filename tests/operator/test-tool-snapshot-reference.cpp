#include <operator/snapshot-reference.hpp>
#include <operator/tool-admission-request.hpp>
#include <operator/tool-invocation.hpp>

#include <json/value.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::operator_runtime
{
    namespace
    {
        [[nodiscard]]
        auto testHash(std::string_view text) -> ContentHash
        {
            auto hash = sha256(std::as_bytes(std::span{text}));
            REQUIRE(hash.has_value());
            return *hash;
        }

        [[nodiscard]]
        auto observationSpec() -> SnapshotObservationSpec
        {
            return SnapshotObservationSpec{
                .controlledTargetId      = "target-1",
                .runtimeArtifactRootHash = testHash("runtime-artifact-1"),
                .projectRegistrationHash = testHash("registration-1"),
                .frameIdentityHash       = testHash("frame-1"),
                .hostGeneration          = 7U,
                .rootIdentity            = testHash("root-1"),
                .issuingParentIdentity   = testHash("parent-1"),
                .expiresAtUnixMillis     = 1'000U,

                .localSemanticTargets = {"menu-button", "start-button"},
                .authorizedUiActions  = {"click", "hover"},
            };
        }

        [[nodiscard]]
        auto consumptionOf(SnapshotObservationReference const& reference)
            -> SnapshotObservationConsumption
        {
            auto const& spec = reference.spec();
            return SnapshotObservationConsumption{
                .exactReferenceJcs       = reference.wire().bytes(),
                .controlledTargetId      = spec.controlledTargetId,
                .runtimeArtifactRootHash = spec.runtimeArtifactRootHash,
                .projectRegistrationHash = spec.projectRegistrationHash,
                .hostGeneration          = spec.hostGeneration,
                .rootIdentity            = spec.rootIdentity,
                .issuingParentIdentity   = spec.issuingParentIdentity,
                .localSemanticTarget     = "start-button",
                .uiAction                = "click",
                .presentedAtUnixMillis   = 500U,
            };
        }

        using PresentConsumption =
            SnapshotObservationConsumption (*)(SnapshotObservationConsumption);

        struct ObservationAttack final
        {
            std::string_view   name{};
            PresentConsumption present{};
            ObservationRefusal expected{ObservationRefusal::Stale};
        };

        constexpr auto k_refusals = std::array{
            ObservationRefusal::Unminted,
            ObservationRefusal::AlreadyConsumed,
            ObservationRefusal::Stale,
            ObservationRefusal::ForeignTarget,
            ObservationRefusal::ForeignRegistration,
            ObservationRefusal::ForeignRuntimeArtifact,
            ObservationRefusal::ChangedGeneration,
            ObservationRefusal::MissingParent,
            ObservationRefusal::DuplicateLocal,
            ObservationRefusal::UnknownLocalTarget,
            ObservationRefusal::ActionRefused,
        };
    }

    TEST_CASE("An observation reference is an opaque canonical-JSON handle")
    {
        auto authority = SnapshotObservationAuthority{};
        auto reference = authority.mint(observationSpec());
        REQUIRE(reference.has_value());

        // Exact RFC 8785 JCS, so replay equality is byte equality of recorded
        // JSON and the identity is the digest of those exact bytes.
        auto const& wire = reference->wire();
        auto reparsed    = CanonicalJson::parseExact(wire.bytes());
        REQUIRE(reparsed.has_value());
        CHECK(reparsed->bytes() == wire.bytes());
        CHECK(reference->identity() == wire.contentHash());
        CHECK(reference->identity() == reparsed->contentHash());

        // All six bindings section 6 names are inside the hashed bytes.
        auto const& bytes = wire.bytes();
        CHECK(bytes.find(R"("controlled_target_id":"target-1")") != std::string::npos);
        CHECK(bytes.find(R"("host_generation":"7")") != std::string::npos);
        CHECK(bytes.find(testHash("runtime-artifact-1").hex()) != std::string::npos);
        CHECK(bytes.find(testHash("registration-1").hex()) != std::string::npos);
        CHECK(bytes.find(testHash("frame-1").hex()) != std::string::npos);
        CHECK(bytes.find(R"("expires_at_unix_ms":"1000")") != std::string::npos);
        CHECK(bytes.find(R"("root_identity")") != std::string::npos);
        CHECK(bytes.find(testHash("parent-1").hex()) != std::string::npos);

        // A handle and not a payload: nothing about the frame's content is in
        // it, which is what makes the result a reference under R2.
        CHECK(bytes.find("pixel") == std::string::npos);
        CHECK(bytes.find("image") == std::string::npos);

        // A root-context observation renders its absent parent rather than
        // dropping the member, so the two coordinates cannot collide.
        auto rootSpec                  = observationSpec();
        rootSpec.issuingParentIdentity = std::nullopt;
        auto rootReference             = authority.mint(std::move(rootSpec));
        REQUIRE(rootReference.has_value());
        CHECK(
            rootReference->wire().bytes().find(
                R"("issuing_parent_identity":null)"
            )
            != std::string::npos
        );
        CHECK(rootReference->identity() != reference->identity());
    }

    TEST_CASE("Minting refuses an observation whose scope cannot be established")
    {
        auto authority = SnapshotObservationAuthority{};

        auto unnamedTarget               = observationSpec();
        unnamedTarget.controlledTargetId = "";
        CHECK_FALSE(authority.mint(std::move(unnamedTarget)).has_value());

        auto unexpiring                = observationSpec();
        unexpiring.expiresAtUnixMillis = 0U;
        CHECK_FALSE(authority.mint(std::move(unexpiring)).has_value());

        auto unnamedLocal                 = observationSpec();
        unnamedLocal.localSemanticTargets = {"start-button", ""};
        CHECK_FALSE(authority.mint(std::move(unnamedLocal)).has_value());

        auto unnamedAction                = observationSpec();
        unnamedAction.authorizedUiActions = {""};
        CHECK_FALSE(authority.mint(std::move(unnamedAction)).has_value());

        // Two observations with every binding equal are one observation, and a
        // second record of it would carry its own spent state.
        REQUIRE(authority.mint(observationSpec()).has_value());
        auto const repeated = authority.mint(observationSpec());
        REQUIRE_FALSE(repeated.has_value());
        CHECK(repeated.error().message().contains("already minted"));
    }

    TEST_CASE("Every observation refusal is a separate channel with its own diagnostic")
    {
        for (auto const outer : k_refusals)
        {
            auto const wire       = observationRefusalWireName(outer);
            auto const diagnostic = observationRefusalDiagnostic(outer);
            CAPTURE(wire);
            CHECK_FALSE(wire.empty());

            // The diagnostic names the channel that closed, so a refused call
            // reports which of the eleven it earned rather than "invalid".
            CHECK(diagnostic.starts_with(wire));

            for (auto const inner : k_refusals)
            {
                if (inner == outer)
                {
                    continue;
                }
                CHECK(observationRefusalWireName(inner) != wire);
                CHECK(observationRefusalDiagnostic(inner) != diagnostic);
            }
        }
    }

    TEST_CASE("The refusal matrix answers each E5 attack separately")
    {
        auto authority = SnapshotObservationAuthority{};
        auto reference = authority.mint(observationSpec());
        REQUIRE(reference.has_value());
        auto const admitted = consumptionOf(*reference);
        CHECK_FALSE(authority.refuse(admitted).has_value());

        constexpr auto k_attacks = std::array{
            ObservationAttack{
                "stale",
                [](SnapshotObservationConsumption claim)
                {
                    claim.presentedAtUnixMillis = 1'000U;
                    return claim;
                },
                ObservationRefusal::Stale,
            },
            ObservationAttack{
                "foreign-target",
                [](SnapshotObservationConsumption claim)
                {
                    claim.controlledTargetId = "target-2";
                    return claim;
                },
                ObservationRefusal::ForeignTarget,
            },
            ObservationAttack{
                "foreign-registration",
                [](SnapshotObservationConsumption claim)
                {
                    claim.projectRegistrationHash = testHash("registration-2");
                    return claim;
                },
                ObservationRefusal::ForeignRegistration,
            },
            ObservationAttack{
                "foreign-runtime-artifact",
                [](SnapshotObservationConsumption claim)
                {
                    claim.runtimeArtifactRootHash = testHash(
                        "runtime-artifact-2"
                    );
                    return claim;
                },
                ObservationRefusal::ForeignRuntimeArtifact,
            },
            ObservationAttack{
                "changed-generation",
                [](SnapshotObservationConsumption claim)
                {
                    claim.hostGeneration = 8U;
                    return claim;
                },
                ObservationRefusal::ChangedGeneration,
            },
            ObservationAttack{
                "missing-parent",
                [](SnapshotObservationConsumption claim)
                {
                    claim.issuingParentIdentity = std::nullopt;
                    return claim;
                },
                ObservationRefusal::MissingParent,
            },
            ObservationAttack{
                "another-parent",
                [](SnapshotObservationConsumption claim)
                {
                    claim.issuingParentIdentity = testHash("parent-2");
                    return claim;
                },
                ObservationRefusal::MissingParent,
            },
            ObservationAttack{
                "another-root",
                [](SnapshotObservationConsumption claim)
                {
                    claim.rootIdentity = testHash("root-2");
                    return claim;
                },
                ObservationRefusal::MissingParent,
            },
            ObservationAttack{
                "unknown-local-target",
                [](SnapshotObservationConsumption claim)
                {
                    claim.localSemanticTarget = "absent-button";
                    return claim;
                },
                ObservationRefusal::UnknownLocalTarget,
            },
            ObservationAttack{
                "action-refused",
                [](SnapshotObservationConsumption claim)
                {
                    claim.uiAction = "drag";
                    return claim;
                },
                ObservationRefusal::ActionRefused,
            },
        };

        for (auto const& attack : k_attacks)
        {
            CAPTURE(attack.name);
            auto const presented = attack.present(admitted);
            auto const refusal   = authority.refuse(presented);
            REQUIRE(refusal.has_value());
            CHECK(*refusal == attack.expected);

            auto const resolved = authority.resolve(presented);
            REQUIRE_FALSE(resolved.has_value());
            CHECK(resolved.error().message().contains(
                observationRefusalWireName(attack.expected)
            ));
        }

        // None of the ten attacks spent the authority, so the call that is
        // entitled to it still is: no rejected action published anything.
        CHECK(authority.resolve(admitted).has_value());
    }

    TEST_CASE("A snapshot-local semantic target declared twice is refused")
    {
        auto authority = SnapshotObservationAuthority{};
        auto ambiguous = observationSpec();
        ambiguous.localSemanticTargets = {
            "menu-button",
            "start-button",
            "start-button",
        };
        auto reference = authority.mint(std::move(ambiguous));
        REQUIRE(reference.has_value());

        auto const duplicated = consumptionOf(*reference);
        auto const refusal    = authority.refuse(duplicated);
        REQUIRE(refusal.has_value());
        CHECK(*refusal == ObservationRefusal::DuplicateLocal);

        // The unambiguous sibling target in the same observation is admitted,
        // so the refusal is about the named target and not about the frame.
        auto unambiguous                = duplicated;
        unambiguous.localSemanticTarget = "menu-button";
        CHECK_FALSE(authority.refuse(unambiguous).has_value());
    }

    TEST_CASE("At most one native input consumes one observation authority")
    {
        auto authority = SnapshotObservationAuthority{};
        auto reference = authority.mint(observationSpec());
        REQUIRE(reference.has_value());
        auto const admitted = consumptionOf(*reference);

        auto resolved = authority.resolve(admitted);
        REQUIRE(resolved.has_value());
        CHECK(resolved->referenceIdentity() == reference->identity());
        CHECK(resolved->controlledTargetId() == "target-1");
        CHECK(resolved->localSemanticTarget() == "start-button");
        CHECK(resolved->uiAction() == "click");
        CHECK(resolved->hostGeneration() == 7U);

        // The frame identity comes out of the minted reference and is never
        // restated by the consuming call, so a consumer cannot name a frame it
        // did not observe.
        CHECK(resolved->frameIdentityHash() == testHash("frame-1"));

        auto const spent = authority.refuse(admitted);
        REQUIRE(spent.has_value());
        CHECK(*spent == ObservationRefusal::AlreadyConsumed);

        auto const replayed = authority.resolve(admitted);
        REQUIRE_FALSE(replayed.has_value());
        CHECK(replayed.error().message().contains("already_consumed"));

        // A second local target of the same observation is refused too: the
        // budget is one input per authority, not one input per target.
        auto sibling                = admitted;
        sibling.localSemanticTarget = "menu-button";
        auto const siblingRefusal   = authority.refuse(sibling);
        REQUIRE(siblingRefusal.has_value());
        CHECK(*siblingRefusal == ObservationRefusal::AlreadyConsumed);
    }

    TEST_CASE("Caller-copied observation JSON is not an observation authority")
    {
        auto authority = SnapshotObservationAuthority{};
        auto reference = authority.mint(observationSpec());
        REQUIRE(reference.has_value());
        auto const admitted = consumptionOf(*reference);

        // A caller that edits the document to say what it wants to be true
        // moves the bytes, and the bytes are the whole of recognition.
        auto forgedValue = json::parse(reference->wire().bytes());
        REQUIRE(forgedValue.has_value());
        auto forgedMembers = std::vector<json::Member>{};
        for (auto const& member : forgedValue->members())
        {
            forgedMembers.emplace_back(
                member.first,
                member.first == "host_generation"
                    ? json::Value::ofString("8")
                    : member.second
            );
        }
        auto forged           = admitted;
        forged.hostGeneration = 8U;
        forged.exactReferenceJcs = json::canonicalBytes(
            json::Value::ofObject(std::move(forgedMembers))
        );
        CHECK(forged.exactReferenceJcs != admitted.exactReferenceJcs);
        auto const forgedRefusal = authority.refuse(forged);
        REQUIRE(forgedRefusal.has_value());
        CHECK(*forgedRefusal == ObservationRefusal::Unminted);

        // Nor does another run's observation resolve here. The run shows up in
        // the reference as its root identity, so a reference minted under
        // another root is other bytes and is not recognised at all -- it never
        // reaches the coordinate comparison.
        auto foreign             = SnapshotObservationAuthority{};
        auto foreignSpec         = observationSpec();
        foreignSpec.rootIdentity = testHash("root-2");
        auto foreignReference    = foreign.mint(std::move(foreignSpec));
        REQUIRE(foreignReference.has_value());
        auto const crossed = authority.refuse(consumptionOf(*foreignReference));
        REQUIRE(crossed.has_value());
        CHECK(*crossed == ObservationRefusal::Unminted);
    }

    TEST_CASE("The Framework Tool Catalog declares eleven built-in Tools")
    {
        auto catalog = FrameworkToolCatalogOwner::create();
        REQUIRE(catalog.has_value());

        struct CatalogExpectation final
        {
            std::string_view name{};
            ToolMutability   mutability{ToolMutability::Mutating};
            ToolSurface      surface{ToolSurface::Privileged};
            ToolIdempotency  idempotency{ToolIdempotency::NonIdempotent};
        };
        constexpr auto k_declared = std::array{
            CatalogExpectation{
                "framework.audit.record",
                ToolMutability::ReadOnly,
                ToolSurface::Semantic,
                ToolIdempotency::ReadSafe,
            },
            CatalogExpectation{
                "framework.input.deliver",
                ToolMutability::Mutating,
                ToolSurface::Privileged,
                ToolIdempotency::NonIdempotent,
            },
            CatalogExpectation{
                "framework.input.semantic_target",
                ToolMutability::Mutating,
                ToolSurface::Semantic,
                ToolIdempotency::NonIdempotent,
            },
            // Reading the project's own authoring store is the project's own
            // vocabulary and changes nothing, so deny-all admits it. WRITING it
            // is the authoring-write effect line, and it is Privileged because
            // its capture arm names a rectangle of the screen -- a Tool is
            // judged by the more restricted of the vocabularies it speaks
            // (docs/decisions/2026-08-24-policy-is-the-axis-and-observation-holds-a-frame.md
            // V4).
            CatalogExpectation{
                "framework.project.read",
                ToolMutability::ReadOnly,
                ToolSurface::Semantic,
                ToolIdempotency::ReadSafe,
            },
            CatalogExpectation{
                "framework.project.write",
                ToolMutability::Mutating,
                ToolSurface::Privileged,
                ToolIdempotency::DeliverySafe,
            },
            // The three measuring Tools an observation's body issues. Each is
            // Privileged because a rectangle of pixels is the machine's
            // vocabulary; a child's surface is bounded by its parent's
            // declaration rather than by the Operator's top-of-run grant, which
            // is what keeps a body measuring under deny-all.
            CatalogExpectation{
                "framework.screen.census_grid",
                ToolMutability::ReadOnly,
                ToolSurface::Privileged,
                ToolIdempotency::ReadSafe,
            },
            CatalogExpectation{
                "framework.screen.observe",
                ToolMutability::ReadOnly,
                ToolSurface::Semantic,
                ToolIdempotency::ReadSafe,
            },
            CatalogExpectation{
                "framework.screen.probe",
                ToolMutability::ReadOnly,
                ToolSurface::Privileged,
                ToolIdempotency::ReadSafe,
            },
            CatalogExpectation{
                "framework.screen.read_lines",
                ToolMutability::ReadOnly,
                ToolSurface::Privileged,
                ToolIdempotency::ReadSafe,
            },
            CatalogExpectation{
                "framework.workflow.status",
                ToolMutability::ReadOnly,
                ToolSurface::Semantic,
                ToolIdempotency::ReadSafe,
            },
            CatalogExpectation{
                "framework.workflow.wait",
                ToolMutability::ReadOnly,
                ToolSurface::Semantic,
                ToolIdempotency::ReadSafe,
            },
        };

        for (auto const& expectation : k_declared)
        {
            CAPTURE(expectation.name);
            auto const descriptor = catalog->describe(expectation.name);
            REQUIRE(descriptor.has_value());
            CHECK(descriptor->mutability == expectation.mutability);
            CHECK(descriptor->surface == expectation.surface);
            CHECK(descriptor->idempotency == expectation.idempotency);
            CHECK(
                catalog->canonicalJcs().find(std::string{expectation.name})
                != std::string::npos
            );
        }

        // A controller that is not restricted to semantic tools sees all eleven,
        // in the byte order the catalog declares them.
        auto noCapabilities = std::array<std::string, 0U>{};
        auto const offered  = catalog->offeredTools(
            controllerProfile(ControllerKind::Human),
            noCapabilities
        );
        REQUIRE(offered.size() == k_declared.size());
        for (auto index = std::size_t{0U}; index < offered.size(); ++index)
        {
            CAPTURE(index);
            CHECK(offered[index].name == k_declared[index].name);
        }

        // Per R5 audit is a read-only Tool that proposes no effect, so it needs
        // no OperatorPolicyAuthority material at all.
        auto const audit = catalog->describe("framework.audit.record");
        REQUIRE(audit.has_value());
        CHECK(audit->effectBounds.empty());
        CHECK(audit->uiActionBounds.empty());
        CHECK(audit->requiredCapabilities.empty());
        CHECK(audit->limits.maximumDispatches == 0U);

        // The two input Tools declare ONE effect line between them: same type,
        // same scope, same payload schema and the same highest risk band. An
        // input that landed is external and irreversible however it was aimed,
        // and what separates the two Tools is the vocabulary they are stated in
        // rather than a number in a bound
        // (docs/decisions/2026-08-24-policy-is-the-axis-and-observation-holds-a-frame.md).
        auto const semantic = catalog->describe(
            "framework.input.semantic_target"
        );
        auto const machine = catalog->describe("framework.input.deliver");
        REQUIRE(semantic.has_value());
        REQUIRE(machine.has_value());
        REQUIRE(semantic->effectBounds.size() == 1U);
        REQUIRE(machine->effectBounds.size() == 1U);
        CHECK(
            semantic->effectBounds.front().namespacedType
            == "framework.input.deliver"
        );
        CHECK(semantic->effectBounds.front().maximumRisk == Risk::Critical);
        CHECK(machine->effectBounds.front().maximumRisk == Risk::Critical);
        CHECK(
            semantic->effectBounds.front().scopeKind
            == machine->effectBounds.front().scopeKind
        );
        CHECK(
            semantic->effectBounds.front().payloadSchemaHash
            == machine->effectBounds.front().payloadSchemaHash
        );
        // The one machine-aimed Tool is Privileged and the declared-target one
        // is not, which is the whole of the difference the catalog states.
        CHECK(machine->surface == ToolSurface::Privileged);
        CHECK(semantic->surface == ToolSurface::Semantic);
        CHECK(semantic->timeout.onTimeout == TimeoutAction::Reobserve);

        // Observation is the one Tool that spends an observation, and it spends
        // exactly one without dispatching anything.
        auto const observe = catalog->describe("framework.screen.observe");
        REQUIRE(observe.has_value());
        CHECK(observe->limits.maximumObservations == 1U);
        CHECK(observe->limits.maximumDispatches == 0U);

        // SCREEN OBSERVATION IS CALLABLE UNDER DENY-ALL, and this is the whole
        // of what makes it so
        // (docs/decisions/2026-08-24-policy-is-the-axis-and-observation-holds-a-frame.md).
        // A read-only descriptor with no effect bound proposes no mutation, so
        // proposedToolMutation answers nothing and no policy is consulted at
        // all -- there is nothing for an artifact whose default is deny to deny.
        // It is the framework's own verification eating; its risk is zero.
        CHECK(observe->mutability == ToolMutability::ReadOnly);
        CHECK(observe->effectBounds.empty());
        auto arguments = CanonicalJson::parseExact("{}");
        REQUIRE(arguments.has_value());
        auto const observeCall = catalog->validate(
            "framework.screen.observe",
            std::move(*arguments)
        );
        REQUIRE(observeCall.has_value());
        CHECK_FALSE(
            proposedToolMutation(*observeCall, "any-target").has_value()
        );
    }

    TEST_CASE("The Framework Tool Catalog identity is pinned to its material")
    {
        auto catalog = FrameworkToolCatalogOwner::create();
        REQUIRE(catalog.has_value());

        // Moving one rendered byte of one descriptor moves this value. It is
        // written out rather than recomputed from the same material, because a
        // hash compared against itself pins nothing.
        CHECK(
            catalog->toolCatalogHash().hex()
            == "de2c27db4b699e85dc7aeeb42f244645bc7ed4d71fc42283c6030c4f43b0f14a"
        );

        auto material = CanonicalJson::parseExact(catalog->canonicalJcs());
        REQUIRE(material.has_value());
        CHECK(material->contentHash() == catalog->toolCatalogHash());
    }

    TEST_CASE("Framework Tool arguments are exact and bounded")
    {
        auto catalog = FrameworkToolCatalogOwner::create();
        REQUIRE(catalog.has_value());

        struct ArgumentCase final
        {
            std::string_view tool{};
            std::string_view arguments{};
            bool             admitted{};
        };
        constexpr auto k_cases = std::array{
            ArgumentCase{"framework.screen.observe", "{}", true},
            ArgumentCase{"framework.screen.observe", R"({"scale":1})", false},
            ArgumentCase{"framework.workflow.status", "{}", true},
            ArgumentCase{"framework.workflow.status", R"({"verbose":true})", false},
            ArgumentCase{
                "framework.audit.record",
                R"({"record":{"note":"looked"}})",
                true,
            },
            ArgumentCase{"framework.audit.record", R"({"record":"looked"})", false},
            ArgumentCase{"framework.audit.record", "{}", false},
            ArgumentCase{
                "framework.audit.record",
                R"({"position":1,"record":{}})",
                false,
            },
            ArgumentCase{
                "framework.input.semantic_target",
                R"({"observation_reference":{"schema":"x"},"semantic_target":"start-button","ui_action":"click"})",
                true,
            },
            ArgumentCase{
                "framework.input.semantic_target",
                R"({"observation_reference":"{}","semantic_target":"start-button","ui_action":"click"})",
                false,
            },
            ArgumentCase{
                "framework.input.semantic_target",
                R"({"observation_reference":{},"semantic_target":"","ui_action":"click"})",
                false,
            },
            // One arm per verb, and each arm's members are decided by its own
            // tag. The six admitted cases below are the whole enumeration; the
            // refusals under them are the three ways a tagged union can be
            // wrong -- a tag outside the set, an arm short a member, and an arm
            // carrying a member that belongs to a different one.
            ArgumentCase{
                "framework.input.deliver",
                R"({"action":"click","x":10,"y":20})",
                true,
            },
            ArgumentCase{
                "framework.input.deliver",
                R"({"action":"hold","x":10,"y":20})",
                true,
            },
            ArgumentCase{
                "framework.input.deliver",
                R"({"action":"move","x":10,"y":20})",
                true,
            },
            ArgumentCase{
                "framework.input.deliver",
                R"({"action":"drag","to_x":30,"to_y":40,"travel_ms":600,"x":10,"y":20})",
                true,
            },
            ArgumentCase{
                "framework.input.deliver",
                R"({"action":"key","key":"Escape"})",
                true,
            },
            ArgumentCase{
                "framework.input.deliver",
                R"({"action":"scroll","notches":-3})",
                true,
            },
            ArgumentCase{
                "framework.input.deliver",
                R"({"action":"click","x":10.5,"y":20})",
                false,
            },
            ArgumentCase{"framework.input.deliver", R"({"x":10,"y":20})", false},
            // The free string is gone: a verb the catalog cannot validate is an
            // unbound call it cannot refuse by name.
            ArgumentCase{
                "framework.input.deliver",
                R"({"action":"tap","x":10,"y":20})",
                false,
            },
            // Each arm's fields are REQUIRED. A drag without its endpoint and a
            // hold without a coordinate are not calls with absent members; they
            // are not that arm at all.
            ArgumentCase{
                "framework.input.deliver",
                R"({"action":"drag","travel_ms":600,"x":10,"y":20})",
                false,
            },
            ArgumentCase{
                "framework.input.deliver",
                R"({"action":"hold"})",
                false,
            },
            // x and y descend into the aiming arms and live nowhere else. A key
            // names no position, and forcing it to carry one would be an
            // accident dressed as a method.
            ArgumentCase{
                "framework.input.deliver",
                R"({"action":"key","key":"Escape","x":10,"y":20})",
                false,
            },
            ArgumentCase{
                "framework.input.deliver",
                R"({"action":"scroll","notches":-3,"x":10,"y":20})",
                false,
            },
            // A pixel is an index into a surface, so a negative one is not a
            // point the contract can carry at all.
            ArgumentCase{
                "framework.input.deliver",
                R"({"action":"click","x":-1,"y":20})",
                false,
            },
        };

        for (auto const& argumentCase : k_cases)
        {
            CAPTURE(argumentCase.tool);
            CAPTURE(argumentCase.arguments);
            auto arguments = CanonicalJson::parseExact(
                std::string{argumentCase.arguments}
            );
            REQUIRE(arguments.has_value());
            auto const invocation = catalog->validate(
                std::string{argumentCase.tool},
                std::move(*arguments)
            );
            CHECK(invocation.has_value() == argumentCase.admitted);
        }
    }
}
