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
#include <vector>

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

        // The smallest complete generation: one tool closure whose stated
        // entry set is explicitly empty because this registration binds no
        // Tool. The slot is always present; there is no shorter document.
        [[nodiscard]]
        auto claimsFor(ContentHash toolManifestHash) -> ProjectGenerationClaims
        {
            return ProjectGenerationClaims{
                .projectRegistrationFormat = k_projectGenerationFormat,
                .pluginId                  = "fixture.alpha",
                .toolClosure               = ProjectClosureClaims{
                    .moduleManifestHash  = toolManifestHash,
                    .exportedEntryPoints = {},
                },
                .pluginEnvironmentHash = hashOf("environment"),
                .toolCatalogHash       = hashOf("catalogue"),
            };
        }

        [[nodiscard]]
        auto closureJcs(ProjectClosureClaims const& closure) -> std::string
        {
            auto result = std::string{R"({"exported_entry_points":[)"};
            for (
                auto index = std::size_t{0};
                index < closure.exportedEntryPoints.size();
                ++index
            )
            {
                if (index != 0U) result.push_back(',');
                result += "\"" + closure.exportedEntryPoints[index] + "\"";
            }
            result += "],\"module_manifest_hash\":\""
                + closure.moduleManifestHash.hex() + "\"}";
            return result;
        }

        [[nodiscard]]
        auto generationJcs(
            ProjectGenerationClaims const& claims
        ) -> std::string
        {
            auto result = std::string{
                "{\"observed_instance_identity_schema_hashes\":["
            };
            for (
                auto index = std::size_t{0};
                index < claims.observedInstanceIdentitySchemaHashes.size();
                ++index
            )
            {
                if (index != 0U) result.push_back(',');
                result += "\"" + claims.observedInstanceIdentitySchemaHashes[index].hex() + "\"";
            }
            result += "],\"plugin_environment_hash\":\""
                + claims.pluginEnvironmentHash.hex()
                + "\",\"plugin_id\":\"" + claims.pluginId
                + "\",\"project_registration_format\":"
                + std::to_string(claims.projectRegistrationFormat)
                + ",\"project_resources\":[";
            for (auto index = std::size_t{0}; index < claims.projectResources.size(); ++index)
            {
                if (index != 0U) result.push_back(',');
                auto const& resource = claims.projectResources[index];
                auto kind = std::string_view{"json"};
                if (resource.kind == ProjectResourceKind::Utf8) kind = "utf8";
                if (resource.kind == ProjectResourceKind::Bytes) kind = "bytes";
                result += "{\"kind\":\"" + std::string{kind} + "\",\"name\":\""
                    + resource.name + "\",\"sha256\":\"" + resource.hash.hex()
                    + "\",\"size\":" + std::to_string(resource.size) + "}";
            }
            result += "],\"project_tool_bindings\":[";
            for (
                auto index = std::size_t{0};
                index < claims.projectToolBindings.size();
                ++index
            )
            {
                if (index != 0U) result.push_back(',');
                auto const& binding = claims.projectToolBindings[index];
                result += "{\"entry_point\":\"" + binding.entryPoint
                    + "\",\"tool_name\":\"" + binding.toolName + "\"}";
            }
            result += "],\"tool_catalog_hash\":\""
                + claims.toolCatalogHash.hex()
                + "\",\"tool_closure\":" + closureJcs(claims.toolClosure) + "}";
            return result;
        }

        // The reader every case here hands to verifyExact: it accepts exactly
        // the bytes this fixture rendered and answers with the claims those
        // bytes state. A real reader parses and validates a schema; what these
        // cases are about is what the framework checks AFTER a reader accepted.
        [[nodiscard]]
        auto exactReader(
            std::string expectedJcs,
            ProjectGenerationClaims claims
        ) -> ProjectGenerationExactValidator
        {
            return [expectedJcs = std::move(expectedJcs), claims = std::move(claims)](
                       std::string_view candidate
                   ) -> Result<ProjectGenerationClaims>
            {
                if (candidate != expectedJcs)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "fixture reader rejected non-exact JCS"
                    );
                }
                return claims;
            };
        }
    }

    static_assert(!std::is_default_constructible_v<VerifiedProjectGeneration>);
    static_assert(
        !std::is_constructible_v<
            VerifiedProjectGeneration,
            ProjectGenerationClaims,
            std::string,
            ContentHash
        >
    );

    TEST_CASE("VerifiedProjectGeneration requires exact JCS schema and root")
    {
        auto const toolManifestHash = hashOf("tool-manifest");
        auto const claims   = claimsFor(toolManifestHash);
        auto const exactJcs = generationJcs(claims);
        auto const reader   = exactReader(exactJcs, claims);
        auto const rootHash = hashOf(exactJcs);

        auto const verified = ProjectGeneration::verifyExact(
            exactJcs,
            rootHash,
            reader
        );
        REQUIRE(verified.has_value());
        CHECK(verified->canonicalJcs() == exactJcs);
        CHECK(verified->hash() == rootHash);
        CHECK(verified->pluginId() == "fixture.alpha");
        CHECK(
            verified->toolClosure().moduleManifestHash == toolManifestHash
        );

        CHECK_FALSE(
            ProjectGeneration::verifyExact(
                " " + exactJcs,
                rootHash,
                reader
            ).has_value()
        );
        auto const wrongRoot = hashOf("wrong-root");
        auto const rootMismatch =
            ProjectGeneration::verifyExact(exactJcs, wrongRoot, reader);
        REQUIRE_FALSE(rootMismatch.has_value());
        CHECK(
            rootMismatch.error().message().contains(
                "do not match the expected root"
            )
        );
        CHECK(rootMismatch.error().message().contains(wrongRoot.hex()));
        CHECK(rootMismatch.error().message().contains(rootHash.hex()));
    }

    // The refusal that replaced the registration's manifest_schema_hash. That
    // digest could only be reddened by decoupling the deriving loader from the
    // schema owner it handed the same local to; this one is reddened by the
    // document itself, which is what a compatibility statement is for.
    TEST_CASE("VerifiedProjectGeneration rejects a registration generation it does not read")
    {
        auto claims                      = claimsFor(hashOf("plugin"));
        claims.projectRegistrationFormat = k_projectGenerationFormat + 1U;
        auto const exactJcs = generationJcs(claims);
        auto const reader   = exactReader(exactJcs, claims);
        auto const refused =
            ProjectGeneration::verifyExact(exactJcs, hashOf(exactJcs), reader);
        REQUIRE_FALSE(refused.has_value());
        // Both generations, so a reader is never left hunting the second one.
        CHECK(refused.error().message().contains(
            std::to_string(claims.projectRegistrationFormat)
        ));
        CHECK(refused.error().message().contains(
            std::to_string(k_projectGenerationFormat)
        ));
    }

    // The forward case above proves nothing about the migration itself: a
    // consumer that accepted any format <= 7 would keep it green. This case
    // names the generation this framework just stopped reading -- format 6,
    // the two-closure document that still pinned a project state schema, a
    // tool precondition schema and a journal event schema manifest -- so the
    // 6 -> 7 break is red before any future format-8 document is.
    TEST_CASE("VerifiedProjectGeneration refuses the previous generation's format")
    {
        auto claims                      = claimsFor(hashOf("plugin"));
        claims.projectRegistrationFormat = 6U;
        auto const exactJcs = generationJcs(claims);
        auto const reader   = exactReader(exactJcs, claims);
        auto const refused =
            ProjectGeneration::verifyExact(exactJcs, hashOf(exactJcs), reader);
        REQUIRE_FALSE(refused.has_value());
        // The message names the stated format and the format this framework
        // reads, so a refusal of the wrong generation cannot be green.
        CHECK(refused.error().message().contains("6"));
        CHECK(refused.error().message().contains(
            std::to_string(k_projectGenerationFormat)
        ));
    }

    TEST_CASE("VerifiedProjectGeneration rejects unordered resources")
    {
        auto claims = claimsFor(hashOf("plugin"));
        claims.projectResources = {
            ProjectResource{
                .kind = ProjectResourceKind::Json,
                .name = "zeta",
                .hash = hashOf("z"),
                .size = 1U,
            },
            ProjectResource{
                .kind = ProjectResourceKind::Json,
                .name = "alpha",
                .hash = hashOf("a"),
                .size = 1U,
            },
        };
        auto const exactJcs = generationJcs(claims);
        auto const reader   = exactReader(exactJcs, std::move(claims));
        CHECK_FALSE(
            ProjectGeneration::verifyExact(
                exactJcs,
                hashOf(exactJcs),
                reader
            ).has_value()
        );
    }

    // The loader writes the identity hashes sorted, so a claim set in any other
    // order is a document the loader never derived. The check is the framework's
    // own reading of the derived document, on the same terms as the artifact
    // roots above: the registration schema cannot state sortedness, so this
    // side refuses it.
    TEST_CASE("VerifiedProjectGeneration rejects unordered or duplicate identity schema hashes")
    {
        auto first  = hashOf("identity-alpha");
        auto second = hashOf("identity-beta");
        if (second < first)
        {
            std::swap(first, second);
        }
        REQUIRE(first < second);

        SUBCASE("unordered")
        {
            auto claims                                 = claimsFor(hashOf("plugin"));
            claims.observedInstanceIdentitySchemaHashes = {second, first};
            auto const exactJcs = generationJcs(claims);
            auto const reader   = exactReader(exactJcs, std::move(claims));
            auto const refused =
                ProjectGeneration::verifyExact(exactJcs, hashOf(exactJcs), reader);
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "observed instance identity schema hashes must be unique and sorted"
            ));
        }

        SUBCASE("duplicate")
        {
            auto claims                                 = claimsFor(hashOf("plugin"));
            claims.observedInstanceIdentitySchemaHashes = {first, first};
            auto const exactJcs = generationJcs(claims);
            auto const reader   = exactReader(exactJcs, std::move(claims));
            auto const refused =
                ProjectGeneration::verifyExact(exactJcs, hashOf(exactJcs), reader);
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "observed instance identity schema hashes must be unique and sorted"
            ));
        }
    }

    TEST_CASE("VerifiedProjectGeneration enforces core routing names")
    {
        SUBCASE("plugin id is namespaced")
        {
            auto claims     = claimsFor(hashOf("plugin"));
            claims.pluginId = "fixture";
            auto const exactJcs = generationJcs(claims);
            auto const reader   = exactReader(exactJcs, std::move(claims));
            CHECK_FALSE(
                ProjectGeneration::verifyExact(exactJcs, hashOf(exactJcs), reader)
                    .has_value()
            );
        }

        // A registrant's plugin_id is the namespace it owns Tool names in, so
        // the Framework's ownership of `framework.*` is enforced on the claim
        // rather than once per Tool name. Without this the positive ownership
        // rule at catalog admission would happily let a project calling itself
        // `framework.anything` own names inside the Framework's namespace.
        SUBCASE("plugin id cannot claim the Framework namespace")
        {
            auto claims     = claimsFor(hashOf("plugin"));
            claims.pluginId = "framework.impostor";
            auto const exactJcs = generationJcs(claims);
            auto const reader   = exactReader(exactJcs, std::move(claims));
            auto const refused =
                ProjectGeneration::verifyExact(exactJcs, hashOf(exactJcs), reader);
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "claims the reserved framework namespace"
            ));
        }

        // The prefix is a namespace boundary and not a spelling: a plugin id
        // that merely begins with the same letters owns its own namespace and
        // is admitted.
        SUBCASE("plugin id beside the Framework namespace is admitted")
        {
            auto claims     = claimsFor(hashOf("plugin"));
            claims.pluginId = "frameworks.impostor";
            auto const exactJcs = generationJcs(claims);
            auto const reader   = exactReader(exactJcs, std::move(claims));
            CHECK(
                ProjectGeneration::verifyExact(exactJcs, hashOf(exactJcs), reader)
                    .has_value()
            );
        }

        SUBCASE("resources are names, not paths")
        {
            auto claims = claimsFor(hashOf("plugin"));
            claims.projectResources = {
                ProjectResource{
                    .kind = ProjectResourceKind::Json,
                    .name = "../content",
                    .hash = hashOf("content"),
                    .size = 1U,
                },
            };
            auto const exactJcs = generationJcs(claims);
            auto const reader   = exactReader(exactJcs, std::move(claims));
            CHECK_FALSE(
                ProjectGeneration::verifyExact(exactJcs, hashOf(exactJcs), reader)
                    .has_value()
            );
        }

        SUBCASE("resource name segments are runtime-bounded")
        {
            auto claims = claimsFor(hashOf("plugin"));
            claims.projectResources = {
                ProjectResource{
                    .kind = ProjectResourceKind::Json,
                    .name = std::string(65U, 'a'),
                    .hash = hashOf("content"),
                    .size = 1U,
                },
            };
            auto const exactJcs = generationJcs(claims);
            auto const reader   = exactReader(exactJcs, std::move(claims));
            CHECK_FALSE(
                ProjectGeneration::verifyExact(exactJcs, hashOf(exactJcs), reader)
                    .has_value()
            );
        }

        SUBCASE("resource name segment count is runtime-bounded")
        {
            auto name = std::string{"a"};
            for (auto index = std::size_t{1U}; index < 17U; ++index)
            {
                name += ".a";
            }
            auto claims = claimsFor(hashOf("plugin"));
            claims.projectResources = {
                ProjectResource{
                    .kind = ProjectResourceKind::Json,
                    .name = std::move(name),
                    .hash = hashOf("content"),
                    .size = 1U,
                },
            };
            auto const exactJcs = generationJcs(claims);
            auto const reader   = exactReader(exactJcs, std::move(claims));
            CHECK_FALSE(
                ProjectGeneration::verifyExact(exactJcs, hashOf(exactJcs), reader)
                    .has_value()
            );
        }
    }

    TEST_CASE("registration identity covers resource name kind hash and size")
    {
        auto base = claimsFor(hashOf("plugin"));
        base.projectResources = {
            ProjectResource{
                .kind = ProjectResourceKind::Json,
                .name = "runtime.corpus",
                .hash = hashOf("resource-bytes"),
                .size = 14U,
            },
        };
        auto renamed = base;
        renamed.projectResources[0].name = "runtime.other";
        auto retyped = base;
        retyped.projectResources[0].kind = ProjectResourceKind::Bytes;
        auto resized = base;
        resized.projectResources[0].size = 15U;
        auto rehashed = base;
        rehashed.projectResources[0].hash = hashOf("other-resource-bytes");

        CHECK(hashOf(generationJcs(base)) != hashOf(generationJcs(renamed)));
        CHECK(hashOf(generationJcs(base)) != hashOf(generationJcs(retyped)));
        CHECK(hashOf(generationJcs(base)) != hashOf(generationJcs(resized)));
        CHECK(hashOf(generationJcs(base)) != hashOf(generationJcs(rehashed)));
    }
}
