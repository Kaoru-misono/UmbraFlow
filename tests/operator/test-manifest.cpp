#include <operator/manifest.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace uf::operator_runtime
{
    namespace
    {
        [[nodiscard]]
        auto hashOf(std::string_view value) -> ContentHash
        {
            auto const result = sha256(std::as_bytes(std::span{value}));
            REQUIRE(result.has_value());
            return *result;
        }

        [[nodiscard]]
        auto claimsFor(
            ContentHash schemaHash,
            ContentHash pluginHash
        ) -> ProjectRegistrationClaims
        {
            return ProjectRegistrationClaims{
                .manifestSchemaHash                 = schemaHash,
                .pluginId                           = "fixture.alpha",
                .pluginHash                         = pluginHash,
                .toolCatalogHash                    = hashOf("catalogue"),
                .projectStateSchemaHash             = hashOf("state"),
                .projectObservationSchemaHash       = hashOf("observation"),
                .projectToolPreconditionSchemaHash  = hashOf("precondition"),
                .reconcilePayloadSchemaManifestHash = hashOf("reconcile"),
                .journalEventSchemaManifestHash     = hashOf("journal"),
                .baselineEventType                  = "fixture.baseline",
            };
        }

        [[nodiscard]]
        auto registrationJcs(
            ProjectRegistrationClaims const& claims
        ) -> std::string
        {
            auto result = std::string{
                "{\"baseline_event_type\":\"" + claims.baselineEventType
                + "\",\"journal_event_schema_manifest_hash\":\""
                + claims.journalEventSchemaManifestHash.hex()
                + "\",\"manifest_schema_hash\":\""
                + claims.manifestSchemaHash.hex()
                + "\",\"plugin_hash\":\"" + claims.pluginHash.hex()
                + "\",\"plugin_id\":\"" + claims.pluginId
                + "\",\"project_artifact_roots\":["
            };
            for (auto index = std::size_t{0}; index < claims.projectArtifactRoots.size(); ++index)
            {
                if (index != 0U) result.push_back(',');
                auto const& root = claims.projectArtifactRoots[index];
                result += "{\"name\":\"" + root.name + "\",\"root_hash\":\""
                    + root.rootHash.hex() + "\"}";
            }
            result += "],\"project_observation_schema_hash\":\""
                + claims.projectObservationSchemaHash.hex()
                + "\",\"project_state_schema_hash\":\""
                + claims.projectStateSchemaHash.hex()
                + "\",\"project_tool_precondition_schema_hash\":\""
                + claims.projectToolPreconditionSchemaHash.hex()
                + "\",\"reconcile_payload_schema_manifest_hash\":\""
                + claims.reconcilePayloadSchemaManifestHash.hex()
                + "\",\"tool_catalog_hash\":\""
                + claims.toolCatalogHash.hex() + "\"}";
            return result;
        }

        [[nodiscard]]
        auto exactOwner(
            ContentHash schemaHash,
            std::string expectedJcs,
            ProjectRegistrationClaims claims
        ) -> ProjectRegistrationSchemaOwner
        {
            auto result = ProjectRegistrationSchemaOwner::create(
                schemaHash,
                [expectedJcs = std::move(expectedJcs), claims = std::move(claims)](
                    std::string_view candidate
                ) -> Result<ProjectRegistrationClaims>
                {
                    if (candidate != expectedJcs)
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            "fixture schema owner rejected non-exact JCS"
                        );
                    }
                    return claims;
                }
            );
            REQUIRE(result.has_value());
            return *result;
        }
    }

    static_assert(!std::is_default_constructible_v<VerifiedProjectRegistration>);
    static_assert(
        !std::is_constructible_v<
            VerifiedProjectRegistration,
            ProjectRegistrationClaims,
            std::string,
            ContentHash
        >
    );

    TEST_CASE("VerifiedProjectRegistration requires exact JCS schema and root")
    {
        auto const schemaHash = hashOf("registration-schema");
        auto const pluginHash = hashOf("plugin-bytes");
        auto const claims = claimsFor(schemaHash, pluginHash);
        auto const exactJcs = registrationJcs(claims);
        auto owner = exactOwner(
            schemaHash,
            exactJcs,
            claims
        );
        auto const rootHash = hashOf(exactJcs);

        auto const verified = ProjectRegistration::verifyExact(
            exactJcs,
            rootHash,
            owner
        );
        REQUIRE(verified.has_value());
        CHECK(verified->canonicalJcs() == exactJcs);
        CHECK(verified->hash() == rootHash);
        CHECK(verified->pluginId() == "fixture.alpha");
        CHECK(verified->pluginHash() == pluginHash);

        CHECK_FALSE(
            ProjectRegistration::verifyExact(
                " " + exactJcs,
                rootHash,
                owner
            ).has_value()
        );
        auto const wrongRoot = hashOf("wrong-root");
        auto const rootMismatch =
            ProjectRegistration::verifyExact(exactJcs, wrongRoot, owner);
        REQUIRE_FALSE(rootMismatch.has_value());
        CHECK(
            rootMismatch.error().message().contains(
                "do not match the expected root"
            )
        );
        CHECK(rootMismatch.error().message().contains(wrongRoot.hex()));
        CHECK(rootMismatch.error().message().contains(rootHash.hex()));
    }

    TEST_CASE("VerifiedProjectRegistration rejects claims from another schema owner")
    {
        auto const ownerSchema = hashOf("owner-schema");
        auto const claimedSchema = hashOf("claimed-schema");
        auto const claims = claimsFor(claimedSchema, hashOf("plugin"));
        auto const exactJcs = registrationJcs(claims);
        auto owner = exactOwner(
            ownerSchema,
            exactJcs,
            claims
        );
        CHECK_FALSE(
            ProjectRegistration::verifyExact(
                exactJcs,
                hashOf(exactJcs),
                owner
            ).has_value()
        );
    }

    TEST_CASE("VerifiedProjectRegistration rejects unordered artifact roots")
    {
        auto const schemaHash = hashOf("registration-schema");
        auto claims = claimsFor(schemaHash, hashOf("plugin"));
        claims.projectArtifactRoots = {
            NamedArtifactRoot{.name = "zeta", .rootHash = hashOf("z")},
            NamedArtifactRoot{.name = "alpha", .rootHash = hashOf("a")},
        };
        auto const exactJcs = registrationJcs(claims);
        auto owner = exactOwner(schemaHash, exactJcs, std::move(claims));
        CHECK_FALSE(
            ProjectRegistration::verifyExact(
                exactJcs,
                hashOf(exactJcs),
                owner
            ).has_value()
        );
    }

    TEST_CASE("VerifiedProjectRegistration enforces core routing names")
    {
        auto const schemaHash = hashOf("registration-schema");

        SUBCASE("plugin id is namespaced")
        {
            auto claims     = claimsFor(schemaHash, hashOf("plugin"));
            claims.pluginId = "fixture";
            auto const exactJcs = registrationJcs(claims);
            auto owner = exactOwner(schemaHash, exactJcs, std::move(claims));
            CHECK_FALSE(
                ProjectRegistration::verifyExact(exactJcs, hashOf(exactJcs), owner)
                    .has_value()
            );
        }

        SUBCASE("baseline event type is namespaced")
        {
            auto claims              = claimsFor(schemaHash, hashOf("plugin"));
            claims.baselineEventType = "Baseline";
            auto const exactJcs = registrationJcs(claims);
            auto owner = exactOwner(schemaHash, exactJcs, std::move(claims));
            CHECK_FALSE(
                ProjectRegistration::verifyExact(exactJcs, hashOf(exactJcs), owner)
                    .has_value()
            );
        }

        SUBCASE("artifact roots are names, not paths")
        {
            auto claims = claimsFor(schemaHash, hashOf("plugin"));
            claims.projectArtifactRoots = {
                NamedArtifactRoot{.name = "../content", .rootHash = hashOf("content")},
            };
            auto const exactJcs = registrationJcs(claims);
            auto owner = exactOwner(schemaHash, exactJcs, std::move(claims));
            CHECK_FALSE(
                ProjectRegistration::verifyExact(exactJcs, hashOf(exactJcs), owner)
                    .has_value()
            );
        }
    }
}
