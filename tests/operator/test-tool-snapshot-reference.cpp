#include <operator/snapshot-reference.hpp>
#include <operator/tool-admission-request.hpp>
#include <operator/tool-invocation.hpp>

#include <json/schema.hpp>
#include <json/value.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <optional>
#include <ranges>
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
                .screenshotSha256        = testHash("screenshot-1"),
                .hostGeneration          = 7U,
                .expiresAtUnixMillis     = 1'000U,
                .uiActions = {
                    ObservedUiAction{
                        .uiTarget = "menu-button",
                        .binding  = "menu.present",
                        .action   = "hover",
                        .kind     = "move",
                    },
                    ObservedUiAction{
                        .uiTarget = "start-button",
                        .binding  = "start.present",
                        .action   = "activate",
                        .kind     = "click",
                    },
                },
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
                .uiTarget                = "start-button",
                .binding                 = "start.present",
                .action                  = "activate",
                .expectedActionKind      = "click",
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
            ObservationRefusal::DuplicateIdentifier,
            ObservationRefusal::UnknownUiTarget,
            ObservationRefusal::UnknownBinding,
            ObservationRefusal::UnknownAction,
            ObservationRefusal::ActionKindMismatch,
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

        // Every world binding and exact resolved action tuple is hashed.
        auto const& bytes = wire.bytes();
        CHECK(bytes.find(R"("controlled_target_id":"target-1")") != std::string::npos);
        CHECK(bytes.find(R"("host_generation":"7")") != std::string::npos);
        CHECK(bytes.find(testHash("runtime-artifact-1").hex()) != std::string::npos);
        CHECK(bytes.find(testHash("registration-1").hex()) != std::string::npos);
        CHECK(bytes.find(testHash("frame-1").hex()) != std::string::npos);
        CHECK(bytes.find(testHash("screenshot-1").hex()) != std::string::npos);
        CHECK(bytes.find(R"("expires_at_unix_ms":"1000")") != std::string::npos);
        CHECK(bytes.find(R"("binding":"start.present")") != std::string::npos);
        CHECK(bytes.find(R"("action":"activate")") != std::string::npos);
        CHECK(bytes.find(R"("kind":"click")") != std::string::npos);

        // A handle and not a payload: nothing about the frame's content is in
        // it, which is what makes the result a reference under R2.
        CHECK(bytes.find("pixel") == std::string::npos);
        CHECK(bytes.find("image") == std::string::npos);

        CHECK(bytes.find("root_identity") == std::string::npos);
        CHECK(bytes.find("issuing_parent_identity") == std::string::npos);
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

        for (auto const member : {0U, 1U, 2U, 3U})
        {
            auto unnamed = observationSpec();
            auto& action = unnamed.uiActions.front();
            if (member == 0U) action.uiTarget.clear();
            if (member == 1U) action.binding.clear();
            if (member == 2U) action.action.clear();
            if (member == 3U) action.kind.clear();
            CHECK_FALSE(authority.mint(std::move(unnamed)).has_value());
        }

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
                "unknown-ui-target",
                [](SnapshotObservationConsumption claim)
                {
                    claim.uiTarget = "absent-button";
                    return claim;
                },
                ObservationRefusal::UnknownUiTarget,
            },
            ObservationAttack{
                "unknown-binding",
                [](SnapshotObservationConsumption claim)
                {
                    claim.binding = "start.absent";
                    return claim;
                },
                ObservationRefusal::UnknownBinding,
            },
            ObservationAttack{
                "unknown-action",
                [](SnapshotObservationConsumption claim)
                {
                    claim.action = "dismiss";
                    return claim;
                },
                ObservationRefusal::UnknownAction,
            },
            ObservationAttack{
                "action-kind-mismatch",
                [](SnapshotObservationConsumption claim)
                {
                    claim.expectedActionKind = "move";
                    return claim;
                },
                ObservationRefusal::ActionKindMismatch,
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

        // None of the attacks spent the authority, so the call that is
        // entitled to it still is: no rejected action published anything.
        CHECK(authority.resolve(admitted).has_value());
    }

    TEST_CASE("An observed UI action identity declared twice is refused")
    {
        auto authority = SnapshotObservationAuthority{};
        auto ambiguous = observationSpec();
        ambiguous.uiActions.emplace_back(ambiguous.uiActions.back());
        auto reference = authority.mint(std::move(ambiguous));
        REQUIRE(reference.has_value());

        auto const duplicated = consumptionOf(*reference);
        auto const refusal    = authority.refuse(duplicated);
        REQUIRE(refusal.has_value());
        CHECK(*refusal == ObservationRefusal::DuplicateIdentifier);

        // The unambiguous sibling tuple in the same observation is admitted.
        auto unambiguous               = duplicated;
        unambiguous.uiTarget           = "menu-button";
        unambiguous.binding            = "menu.present";
        unambiguous.action             = "hover";
        unambiguous.expectedActionKind = "move";
        CHECK_FALSE(authority.refuse(unambiguous).has_value());
    }

    TEST_CASE("framework.ui.click refuses a binding absent from the named observation")
    {
        auto authority = SnapshotObservationAuthority{};
        auto reference = authority.mint(observationSpec());
        REQUIRE(reference.has_value());
        auto consumption    = consumptionOf(*reference);
        consumption.binding = "start.missing";

        auto const refused = authority.resolve(consumption);
        auto const refusedAsUnknownBinding = !refused.has_value()
            && std::string{refused.error().message()}
                == "unknown_binding: the named observation contains no such binding "
                   "identifier for that ui_target: start.missing";
        CHECK(refusedAsUnknownBinding);
    }

    TEST_CASE("framework.ui.click refuses an observed action whose kind is not click")
    {
        auto authority = SnapshotObservationAuthority{};
        auto reference = authority.mint(observationSpec());
        REQUIRE(reference.has_value());
        auto consumption               = consumptionOf(*reference);
        consumption.expectedActionKind = "move";

        auto const refused = authority.resolve(consumption);
        auto const refusedAsActionKindMismatch = !refused.has_value()
            && std::string{refused.error().message()}
                == "action_kind_mismatch: the named observation declares a different "
                   "action kind than this Tool: activate";
        CHECK(refusedAsActionKindMismatch);
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
        CHECK(resolved->screenshotSha256() == testHash("screenshot-1"));
        CHECK(resolved->uiTarget() == "start-button");
        CHECK(resolved->binding() == "start.present");
        CHECK(resolved->action() == "activate");
        CHECK(resolved->actionKind() == "click");
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
        auto sibling               = admitted;
        sibling.uiTarget           = "menu-button";
        sibling.binding            = "menu.present";
        sibling.action             = "hover";
        sibling.expectedActionKind = "move";
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

        // Nor does another authority's observation resolve here, even when its
        // wire bindings are otherwise valid.
        auto foreign                  = SnapshotObservationAuthority{};
        auto foreignSpec              = observationSpec();
        foreignSpec.frameIdentityHash = testHash("frame-2");
        auto foreignReference         = foreign.mint(std::move(foreignSpec));
        REQUIRE(foreignReference.has_value());
        auto const crossed = authority.refuse(consumptionOf(*foreignReference));
        REQUIRE(crossed.has_value());
        CHECK(*crossed == ObservationRefusal::Unminted);
    }

    TEST_CASE("The Framework Tool Catalog declares twenty-nine built-in Tools")
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
                "framework.input.click",
                ToolMutability::Mutating,
                ToolSurface::Privileged,
                ToolIdempotency::NonIdempotent,
            },
            CatalogExpectation{
                "framework.input.drag",
                ToolMutability::Mutating,
                ToolSurface::Privileged,
                ToolIdempotency::NonIdempotent,
            },
            CatalogExpectation{
                "framework.input.hold",
                ToolMutability::Mutating,
                ToolSurface::Privileged,
                ToolIdempotency::NonIdempotent,
            },
            CatalogExpectation{
                "framework.input.key",
                ToolMutability::Mutating,
                ToolSurface::Privileged,
                ToolIdempotency::NonIdempotent,
            },
            CatalogExpectation{
                "framework.input.move",
                ToolMutability::Mutating,
                ToolSurface::Privileged,
                ToolIdempotency::NonIdempotent,
            },
            CatalogExpectation{
                "framework.input.scroll",
                ToolMutability::Mutating,
                ToolSurface::Privileged,
                ToolIdempotency::NonIdempotent,
            },
            // Text paths and contents are Project vocabulary. Copying a
            // retained evidence artifact is Privileged because its digest
            // names machine evidence rather than Project semantics.
            CatalogExpectation{
                "framework.project.read_text",
                ToolMutability::ReadOnly,
                ToolSurface::Semantic,
                ToolIdempotency::ReadSafe,
            },
            CatalogExpectation{
                "framework.project.write_file",
                ToolMutability::Mutating,
                ToolSurface::Privileged,
                ToolIdempotency::DeliverySafe,
            },
            CatalogExpectation{
                "framework.project.write_text",
                ToolMutability::Mutating,
                ToolSurface::Semantic,
                ToolIdempotency::DeliverySafe,
            },
            CatalogExpectation{
                "framework.screen.capture",
                ToolMutability::ReadOnly,
                ToolSurface::Semantic,
                ToolIdempotency::ReadSafe,
            },
            CatalogExpectation{
                "framework.screen.census_grid",
                ToolMutability::ReadOnly,
                ToolSurface::Privileged,
                ToolIdempotency::ReadSafe,
            },
            CatalogExpectation{
                "framework.screen.crop",
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
            // The layout the caller ASSERTS, and the only thing that
            // separates it from read_lines above. Same descriptor in every
            // other respect: measuring a rectangle changes nothing whichever
            // pass runs over it.
            CatalogExpectation{
                "framework.screen.read_single_line",
                ToolMutability::ReadOnly,
                ToolSurface::Privileged,
                ToolIdempotency::ReadSafe,
            },
            // A session's own state is the caller's vocabulary, so Semantic,
            // and it reaches no world outside the Operator process, so
            // ReadOnly with no effect bound -- which is what makes remembering
            // something a session under deny-all can still do. Only the write
            // is DeliverySafe rather than ReadSafe: redelivering it costs the
            // store it already made.
            CatalogExpectation{
                "framework.session.get",
                ToolMutability::ReadOnly,
                ToolSurface::Semantic,
                ToolIdempotency::ReadSafe,
            },
            CatalogExpectation{
                "framework.session.list",
                ToolMutability::ReadOnly,
                ToolSurface::Semantic,
                ToolIdempotency::ReadSafe,
            },
            CatalogExpectation{
                "framework.session.set",
                ToolMutability::ReadOnly,
                ToolSurface::Semantic,
                ToolIdempotency::DeliverySafe,
            },
            CatalogExpectation{
                "framework.ui.click",
                ToolMutability::Mutating,
                ToolSurface::Semantic,
                ToolIdempotency::NonIdempotent,
            },
            CatalogExpectation{
                "framework.ui.drag",
                ToolMutability::Mutating,
                ToolSurface::Semantic,
                ToolIdempotency::NonIdempotent,
            },
            CatalogExpectation{
                "framework.ui.hold",
                ToolMutability::Mutating,
                ToolSurface::Semantic,
                ToolIdempotency::NonIdempotent,
            },
            CatalogExpectation{
                "framework.ui.key",
                ToolMutability::Mutating,
                ToolSurface::Semantic,
                ToolIdempotency::NonIdempotent,
            },
            CatalogExpectation{
                "framework.ui.move",
                ToolMutability::Mutating,
                ToolSurface::Semantic,
                ToolIdempotency::NonIdempotent,
            },
            CatalogExpectation{
                "framework.ui.scroll",
                ToolMutability::Mutating,
                ToolSurface::Semantic,
                ToolIdempotency::NonIdempotent,
            },
            // Reading the clock is Semantic and ReadOnly beside status and
            // wait: it observes no frame, changes nothing outside the
            // Operator, and asks for nothing in the machine's vocabulary. It
            // is ReadSafe despite answering a different instant each time --
            // idempotency here is what redelivering costs, not whether the
            // answer repeats, which is why framework.screen.capture carries
            // the same label.
            CatalogExpectation{
                "framework.workflow.now",
                ToolMutability::ReadOnly,
                ToolSurface::Semantic,
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

        // A controller that is not restricted to semantic tools sees all tools,
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

        // Each verb owns its own effect type, while raw and semantic variants
        // retain the policy-axis surface distinction.
        auto const semantic = catalog->describe("framework.ui.click");
        auto const machine = catalog->describe("framework.input.click");
        REQUIRE(semantic.has_value());
        REQUIRE(machine.has_value());
        REQUIRE(semantic->effectBounds.size() == 1U);
        REQUIRE(machine->effectBounds.size() == 1U);
        CHECK(
            semantic->effectBounds.front().namespacedType
            == "framework.ui.click"
        );
        CHECK(semantic->effectBounds.front().maximumRisk == Risk::Critical);
        CHECK(machine->effectBounds.front().maximumRisk == Risk::Critical);
        CHECK(machine->effectBounds.front().namespacedType == "framework.input.click");
        CHECK(semantic->effectBounds.front().scopeKind == machine->effectBounds.front().scopeKind);
        // Machine aiming is privileged; observation-bound aiming is semantic.
        CHECK(machine->surface == ToolSurface::Privileged);
        CHECK(semantic->surface == ToolSurface::Semantic);
        CHECK(semantic->timeout.onTimeout == TimeoutAction::Reobserve);

        // Capture is the one screen Tool that samples the controlled target.
        auto const capture = catalog->describe("framework.screen.capture");
        auto const observe = catalog->describe("framework.screen.observe");
        REQUIRE(capture.has_value());
        REQUIRE(observe.has_value());

        // SCREEN OBSERVATION IS CALLABLE UNDER DENY-ALL, and this is the whole
        // of what makes it so
        // (docs/decisions/2026-08-24-policy-is-the-axis-and-observation-holds-a-frame.md).
        // A read-only descriptor with no effect bound proposes no mutation, so
        // proposedToolMutation answers nothing and no policy is consulted at
        // all -- there is nothing for an artifact whose default is deny to deny.
        // It is the framework's own verification eating; its risk is zero.
        CHECK(observe->mutability == ToolMutability::ReadOnly);
        CHECK(observe->effectBounds.empty());
        auto arguments = CanonicalJson::parseExact(
            R"({"screenshot_sha256":"0000000000000000000000000000000000000000000000000000000000000000"})"
        );
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
            == "abdaf0c2009a45ac5837d1a2d745d0c3df3df590358400e4f54cd62d50635650"
        );

        auto material = CanonicalJson::parseExact(catalog->canonicalJcs());
        REQUIRE(material.has_value());
        CHECK(material->contentHash() == catalog->toolCatalogHash());
    }

    // What a model that has never seen this repository is handed when it is
    // offered these Tools. Every clause below is something such a caller has to
    // be able to read for itself -- a description that says more than the name,
    // a schema an ordinary Draft 2020-12 reader can apply, a result shape it
    // can write its next call against -- and each one was false of this catalog
    // until the dialect and the placeholders went.
    TEST_CASE("Every Framework Tool publishes a contract a stranger can read")
    {
        auto catalog = FrameworkToolCatalogOwner::create();
        REQUIRE(catalog.has_value());
        auto const entries = catalog->entries();
        REQUIRE_FALSE(entries.empty());

        for (auto const& entry : entries)
        {
            CAPTURE(entry.name);

            // A description generated from the name carries exactly what the
            // name carried. This is the shape the placeholder had.
            CHECK_MESSAGE(
                entry.description
                    != "Call the " + entry.name + " Tool.",
                "a Tool description must be written, not derived from the name"
            );
            CHECK(entry.description.size() > entry.name.size());

            auto const inputBytes = json::canonicalBytes(entry.inputSchema);
            auto const standardKeywordsOnly =
                !inputBytes.contains("additional_properties")
                && !inputBytes.contains("min_length")
                && !inputBytes.contains("maximum_duration_ms");
            CHECK_MESSAGE(
                standardKeywordsOnly,
                "an argument schema must use Draft 2020-12 keywords and no "
                "private spelling of one"
            );

            // The evaluator a Project's own argument_schema is compiled
            // through. A Framework schema it cannot apply is a contract only
            // this repository could check.
            auto const input = json::Schema::compile(json::Schema::Document{
                .label      = entry.name,
                .exactBytes = inputBytes,
            });
            CHECK_MESSAGE(
                input.has_value(),
                "a Framework argument schema must compile as JSON Schema"
            );

            // Every member the contract requires is declared, with its own
            // description, where a caller filling the call in will look.
            auto const* const p_properties = entry.inputSchema.find(
                "properties"
            );
            if (auto const* const p_required = entry.inputSchema.find("required"))
            {
                REQUIRE(p_properties != nullptr);
                for (auto const& member : p_required->items())
                {
                    CAPTURE(member.string());
                    auto const* const p_declared = p_properties->find(
                        member.string()
                    );
                    REQUIRE_MESSAGE(
                        p_declared != nullptr,
                        "a required member must be declared in properties"
                    );
                    auto const* const p_description = p_declared->find(
                        "description"
                    );
                    auto const described = p_description != nullptr
                        && !p_description->string().empty();
                    CHECK_MESSAGE(
                        described,
                        "every declared member must carry its own description"
                    );
                    CHECK(p_declared->find("type") != nullptr);
                }
            }

            // A result nobody wrote down cannot be chained.
            REQUIRE_MESSAGE(
                entry.outputSchema.kind() == json::ValueKind::Object,
                "every Framework Tool must declare its result shape"
            );
            auto const output = json::Schema::compile(json::Schema::Document{
                .label      = entry.name,
                .exactBytes = json::canonicalBytes(entry.outputSchema),
            });
            CHECK_MESSAGE(
                output.has_value(),
                "a Framework result schema must compile as JSON Schema"
            );
        }

        auto const named = [&entries](std::string_view name)
            -> ToolCatalogEntry const&
        {
            auto const found = std::ranges::find(
                entries,
                name,
                &ToolCatalogEntry::name
            );
            REQUIRE(found != entries.end());
            return *found;
        };

        // THE WAIT CEILING IS IN ONE PLACE, AND IT IS THE STANDARD ONE. It
        // used to sit under an invented top-level keyword that no schema
        // evaluator and no model reading the catalog could find.
        auto const& wait = named("framework.workflow.wait");
        auto const* const p_waitProperties = wait.inputSchema.find("properties");
        REQUIRE(p_waitProperties != nullptr);
        auto const* const p_duration = p_waitProperties->find("duration_ms");
        REQUIRE(p_duration != nullptr);
        auto const* const p_ceiling = p_duration->find("maximum");
        REQUIRE_MESSAGE(
            p_ceiling != nullptr,
            "the wait ceiling must live in properties.duration_ms.maximum"
        );
        CHECK(p_ceiling->number() == 60'000.0);

        // The chain the whole surface exists for, read out of the published
        // bytes rather than out of prose: capture answers with the digest
        // observe requires.
        auto const& capture = named("framework.screen.capture");
        auto const* const p_captureResult = capture.outputSchema.find(
            "properties"
        );
        REQUIRE(p_captureResult != nullptr);
        CHECK_MESSAGE(
            p_captureResult->find(k_screenshotSha256Member) != nullptr,
            "capture must publish the digest every screen Tool takes"
        );
        auto const& observe = named("framework.screen.observe");
        auto const* const p_observeRequired = observe.inputSchema.find(
            "required"
        );
        REQUIRE(p_observeRequired != nullptr);
        CHECK(
            std::ranges::any_of(
                p_observeRequired->items(),
                [](json::Value const& member)
                {
                    return member.string() == k_screenshotSha256Member;
                }
            )
        );

        // And the envelope around every result is stated once, by the catalog,
        // rather than twenty-five times inside the result shapes.
        auto const material = json::parse(catalog->canonicalJcs());
        REQUIRE(material.has_value());
        CHECK(material->find("answer_envelope") != nullptr);
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
            ArgumentCase{"framework.screen.capture", "{}", true},
            ArgumentCase{"framework.screen.capture", R"({"scale":1})", false},
            ArgumentCase{
                "framework.screen.observe",
                R"({"screenshot_sha256":"0000000000000000000000000000000000000000000000000000000000000000"})",
                true,
            },
            ArgumentCase{"framework.screen.observe", "{}", false},
            ArgumentCase{
                "framework.screen.observe",
                R"({"screenshot_sha256":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"})",
                false,
            },
            ArgumentCase{
                "framework.screen.crop",
                R"({"height":20,"screenshot_sha256":"0000000000000000000000000000000000000000000000000000000000000000","width":10,"x":1,"y":2})",
                true,
            },
            ArgumentCase{
                "framework.screen.crop",
                R"({"height":20,"width":10,"x":1,"y":2})",
                false,
            },
            ArgumentCase{
                "framework.screen.read_lines",
                R"({"height":20,"screenshot_sha256":"0000000000000000000000000000000000000000000000000000000000000000","width":10,"x":1,"y":2})",
                true,
            },
            // The two reading Tools take the SAME closed rectangle and
            // differ only in which pass runs over it. A layout member is
            // refused on both, from one argument contract: which of the two a
            // caller wanted is the Tool it named, and a member offering the
            // choice again would be a second answer to a settled question.
            ArgumentCase{
                "framework.screen.read_lines",
                R"({"height":20,"layout":"single_line","screenshot_sha256":"0000000000000000000000000000000000000000000000000000000000000000","width":10,"x":1,"y":2})",
                false,
            },
            ArgumentCase{
                "framework.screen.read_single_line",
                R"({"height":20,"screenshot_sha256":"0000000000000000000000000000000000000000000000000000000000000000","width":10,"x":1,"y":2})",
                true,
            },
            ArgumentCase{
                "framework.screen.read_single_line",
                R"({"height":20,"layout":"block","screenshot_sha256":"0000000000000000000000000000000000000000000000000000000000000000","width":10,"x":1,"y":2})",
                false,
            },
            ArgumentCase{"framework.workflow.status", "{}", true},
            ArgumentCase{"framework.session.list", "{}", true},
            ArgumentCase{
                "framework.session.set",
                R"({"name":"plan","value":{"turn":3}})",
                true,
            },
            // Any JSON is a value, so the contract admits an array and a
            // scalar exactly as it admits an object.
            ArgumentCase{
                "framework.session.set",
                R"({"name":"plan","value":[1,2]})",
                true,
            },
            ArgumentCase{"framework.session.set", R"({"name":"plan"})", false},
            ArgumentCase{"framework.session.get", R"({"name":""})", false},
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
                "framework.ui.click",
                R"({"action":"activate","binding":"start.present","observation_reference":{"schema":"x"},"ui_target":"start-button"})",
                true,
            },
            ArgumentCase{
                "framework.ui.drag",
                R"({"action":"activate","binding":"start.present","observation_reference":{"schema":"x"},"ui_target":"start-button"})",
                true,
            },
            ArgumentCase{
                "framework.ui.hold",
                R"({"action":"activate","binding":"start.present","observation_reference":{"schema":"x"},"return_screen":"observe","ui_target":"start-button"})",
                true,
            },
            ArgumentCase{
                "framework.ui.key",
                R"({"action":"activate","binding":"start.present","observation_reference":{"schema":"x"},"ui_target":"start-button"})",
                true,
            },
            ArgumentCase{
                "framework.ui.move",
                R"({"action":"activate","binding":"start.present","observation_reference":{"schema":"x"},"ui_target":"start-button"})",
                true,
            },
            ArgumentCase{
                "framework.ui.scroll",
                R"({"action":"activate","binding":"start.present","observation_reference":{"schema":"x"},"ui_target":"start-button"})",
                true,
            },
            ArgumentCase{
                "framework.ui.click",
                R"({"action":"activate","binding":"start.present","observation_reference":"{}","ui_target":"start-button"})",
                false,
            },
            ArgumentCase{
                "framework.ui.hold",
                R"({"action":"activate","binding":"start.present","observation_reference":{},"return_screen":"again","ui_target":"start-button"})",
                false,
            },
            ArgumentCase{
                "framework.input.click",
                R"({"screenshot_sha256":"0000000000000000000000000000000000000000000000000000000000000000","x":10,"y":20})",
                true,
            },
            ArgumentCase{
                "framework.input.hold",
                R"({"duration_ms":250,"return_screen":"capture","screenshot_sha256":"0000000000000000000000000000000000000000000000000000000000000000","x":10,"y":20})",
                true,
            },
            ArgumentCase{
                "framework.input.move",
                R"({"screenshot_sha256":"0000000000000000000000000000000000000000000000000000000000000000","x":10,"y":20})",
                true,
            },
            ArgumentCase{
                "framework.input.drag",
                R"({"screenshot_sha256":"0000000000000000000000000000000000000000000000000000000000000000","to_x":30,"to_y":40,"travel_ms":600,"x":10,"y":20})",
                true,
            },
            ArgumentCase{
                "framework.input.key",
                R"({"key":"Escape","screenshot_sha256":"0000000000000000000000000000000000000000000000000000000000000000"})",
                true,
            },
            ArgumentCase{
                "framework.input.scroll",
                R"({"notches":-3,"screenshot_sha256":"0000000000000000000000000000000000000000000000000000000000000000"})",
                true,
            },
            ArgumentCase{
                "framework.input.click",
                R"({"screenshot_sha256":"0000000000000000000000000000000000000000000000000000000000000000","x":10.5,"y":20})",
                false,
            },
            ArgumentCase{"framework.input.click", R"({"x":10,"y":20})", false},
            ArgumentCase{
                "framework.input.deliver",
                R"({"action":"click","x":10,"y":20})",
                false,
            },
            ArgumentCase{
                "framework.input.drag",
                R"({"screenshot_sha256":"0000000000000000000000000000000000000000000000000000000000000000","travel_ms":600,"x":10,"y":20})",
                false,
            },
            ArgumentCase{
                "framework.input.hold",
                R"({"screenshot_sha256":"0000000000000000000000000000000000000000000000000000000000000000","x":10,"y":20})",
                false,
            },
            ArgumentCase{
                "framework.input.key",
                R"({"key":"Escape","screenshot_sha256":"0000000000000000000000000000000000000000000000000000000000000000","x":10,"y":20})",
                false,
            },
            ArgumentCase{
                "framework.input.click",
                R"({"screenshot_sha256":"0000000000000000000000000000000000000000000000000000000000000000","x":-1,"y":20})",
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
