// What the product binary does that nothing shipped had ever done: turn a
// project directory into plugins the Operator's registrar holds.
//
// tests/deployment already covers the load, one case per rule the directory
// format states. What is only covered here is the half beyond it -- the loader
// derives a registration without ever compiling the plugin, so a directory it
// accepts can still fail to register, and the case below that removes an entry
// point is the one that makes "registered" a claim rather than a word.

#include <cli/args.hpp>
#include <cli/cli-result.hpp>
#include <cli/open-project.hpp>

#include <task/runtime-model-file.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace uf::cli
{
    namespace
    {
        // The refusal exactly as the binary prints it, context and all. A
        // refusal that names the artifact root does so through Error::context,
        // which error().message() alone drops.
        [[nodiscard]]
        auto why(Result<OpenedProject> const& outcome) -> std::string
        {
            return outcome.has_value()
                ? std::string{"<the directory was accepted>"}
                : formatError(outcome.error());
        }

        [[nodiscard]]
        auto findOpened(
            OpenedProject const& opened,
            std::string_view name
        ) -> OpenedDeployment const*
        {
            auto const found = std::ranges::find(
                opened.deployments,
                name,
                &OpenedDeployment::name
            );
            if (found == opened.deployments.end()) return nullptr;
            return std::to_address(found);
        }

        // A writable copy of this repository's own exemplar. The exemplar is
        // checked in and read by other suites, so a case that broke one file in
        // place would break theirs; every mutation below lands here.
        class ExemplarCopy final
        {
            std::filesystem::path m_root{};

        public:
            ExemplarCopy()
            {
                auto const unique = std::filesystem::path{
                    "uf-open-" + std::to_string(std::random_device{}()),
                };
                m_root = std::filesystem::temp_directory_path() / unique;
                std::filesystem::remove_all(m_root);
                std::filesystem::copy(
                    std::filesystem::path{UF_STAGED_UMBRAFLOW_PROJECT},
                    m_root,
                    std::filesystem::copy_options::recursive
                );
            }

            ExemplarCopy(ExemplarCopy const&)                    = delete;
            ExemplarCopy(ExemplarCopy&&)                         = delete;
            auto operator=(ExemplarCopy const&) -> ExemplarCopy& = delete;
            auto operator=(ExemplarCopy&&) -> ExemplarCopy&      = delete;

            ~ExemplarCopy()
            {
                auto discarded = std::error_code{};
                std::filesystem::remove_all(m_root, discarded);
            }

            [[nodiscard]] auto args() const -> OpenArgs
            {
                return OpenArgs{.project = m_root};
            }

            [[nodiscard]] auto read(std::string_view relative) const -> std::string
            {
                auto stream = std::ifstream{m_root / relative, std::ios::binary};
                return std::string{
                    std::istreambuf_iterator<char>{stream},
                    std::istreambuf_iterator<char>{},
                };
            }

            auto remove(std::string_view relative) const -> void
            {
                REQUIRE(std::filesystem::remove(m_root / relative));
            }

            auto rewrite(std::string_view relative, std::string_view bytes) const
                -> void
            {
                auto stream = std::ofstream{
                    m_root / relative,
                    std::ios::binary | std::ios::trunc,
                };
                stream.write(
                    bytes.data(),
                    static_cast<std::streamsize>(bytes.size())
                );
                REQUIRE(stream.good());
            }
        };
    }

    // The acceptance every refusal below is the negation of, and the only place
    // this suite states what a successful open produces.
    TEST_CASE("this repository's exemplar becomes plugins the registrar holds")
    {
        auto const opened = openProjectProduct(
            OpenArgs{.project = std::filesystem::path{UF_STAGED_UMBRAFLOW_PROJECT}}
        );
        INFO(why(opened));
        REQUIRE(opened.has_value());
        CHECK(everyPluginRegistered(*opened));

        CHECK(opened->primaryDeployment == "alpha");
        REQUIRE(opened->deployments.size() == 2U);

        // The artifact half. The two generations are equal to the numbers this
        // binary reads by construction once the verification succeeded, so what
        // these catch is a report that carries the wrong field -- the two are
        // adjacent, and swapping them is invisible without this.
        CHECK(opened->artifact.rootHash.size() == 64U);
        CHECK(opened->artifact.runtimeArtifactFormat == task::k_runtimeArtifactFormat);
        CHECK(opened->artifact.runtimeModelFormat == task::k_runtimeModelFormat);
        CHECK(opened->artifact.modelBytes > std::size_t{0});
        CHECK(opened->artifact.assets == 2U);

        auto const* const p_alpha = findOpened(*opened, "alpha");
        REQUIRE(p_alpha != nullptr);
        CHECK(p_alpha->pluginId == "fixture.alpha");
        CHECK(p_alpha->registrationHash.size() == 64U);

        // Two deployments are two registrations, and the registrar keys on the
        // pair. Without this the case would equally describe a registry holding
        // one plugin twice.
        auto const* const p_foreign = findOpened(*opened, "foreign");
        REQUIRE(p_foreign != nullptr);
        CHECK(p_foreign->pluginId == "fixture.foreign");
        CHECK(p_foreign->registrationHash != p_alpha->registrationHash);
    }

    // The verb takes the production path, so a directory holding no conformance
    // fixture at all opens. The exemplar ships one, so it is removed here:
    // without this case nothing on the product path says which of the two root
    // documents `open` actually needs.
    TEST_CASE("a project shipping no conformance fixture opens")
    {
        auto const copy = ExemplarCopy{};

        auto const whole = openProjectProduct(copy.args());
        INFO(why(whole));
        REQUIRE(whole.has_value());

        copy.remove("umbraflow-conformance.json");

        auto const opened = openProjectProduct(copy.args());
        INFO(why(opened));
        REQUIRE(opened.has_value());
        CHECK(everyPluginRegistered(*opened));
        CHECK(opened->deployments.size() == whole->deployments.size());
    }

    // The case that makes "registered" a claim. The plugin's bytes reach the
    // registration through plugin_hash and through nothing else, and that digest
    // is the loader's own arithmetic on both sides -- so a plugin the script
    // substrate cannot compile loads with a different registration hash and no
    // complaint at all. Only the registrar refuses it.
    TEST_CASE("a plugin missing an entry point loads and does not register")
    {
        auto const copy = ExemplarCopy{};

        // The positive control: without it every assertion below is satisfied
        // by a registrar that refused this directory whatever its plugin said.
        auto const whole = openProjectProduct(copy.args());
        INFO(why(whole));
        REQUIRE(whole.has_value());
        REQUIRE(everyPluginRegistered(*whole));

        auto const plugin = copy.read("plugin/alpha.luau");
        auto const opens  = plugin.find("    reduce = function(input)");
        REQUIRE(opens != std::string::npos);
        constexpr auto k_closes = std::string_view{"    end,\n"};
        auto const     closes   = plugin.find(k_closes, opens);
        REQUIRE(closes != std::string::npos);
        copy.rewrite(
            "plugin/alpha.luau",
            plugin.substr(0U, opens) + plugin.substr(closes + k_closes.size())
        );

        auto const opened = openProjectProduct(copy.args());
        INFO(why(opened));
        REQUIRE(opened.has_value());
        CHECK_FALSE(everyPluginRegistered(*opened));

        auto const* const p_alpha = findOpened(*opened, "alpha");
        REQUIRE(p_alpha != nullptr);
        REQUIRE(p_alpha->refusal.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): REQUIRE above proved engagement.
        CHECK(p_alpha->refusal->contains("entry point"));

        // The registration moved rather than being refused, which is what says
        // the load never looked at what the plugin contains.
        auto const* const p_whole = findOpened(*whole, "alpha");
        REQUIRE(p_whole != nullptr);
        CHECK(p_alpha->registrationHash != p_whole->registrationHash);

        // The other deployment still registers, so this is a fact about one
        // plugin rather than about a registrar that started refusing.
        auto const* const p_foreign = findOpened(*opened, "foreign");
        REQUIRE(p_foreign != nullptr);
        CHECK_FALSE(p_foreign->refusal.has_value());
    }

    // The defect this verb carried until 2026-08-12, measured on a project
    // whose runtime-artifact.manifest.json stated a runtime_artifact_format
    // this binary does not read: `open` printed a completely clean load, every
    // deployment registered, while the conformance suite failed twelve of its
    // sixteen cases against the same directory. The production load reads that
    // artifact's model file for emptiness and nothing else, so nothing on the
    // open path had ever opened the manifest beside it.
    //
    // Both mutations move one file each and leave everything else in the
    // directory alone, and the reopen between them is what says the mutation is
    // what refused rather than the copy having gone stale.
    TEST_CASE("a RuntimeArtifact this binary cannot start is refused by name")
    {
        auto const copy = ExemplarCopy{};

        // The positive control: without it every refusal below is equally
        // consistent with a verb that refuses this directory whatever its
        // artifact says.
        auto const whole = openProjectProduct(copy.args());
        INFO(why(whole));
        REQUIRE(whole.has_value());

        constexpr auto k_manifest =
            std::string_view{"runtime/artifact/runtime-artifact.manifest.json"};
        auto const declaredFormat = std::format(
            "\"runtime_artifact_format\":{}",
            task::k_runtimeArtifactFormat
        );

        auto const manifest = copy.read(k_manifest);
        auto const declared = manifest.find(declaredFormat);
        REQUIRE(declared != std::string::npos);

        // Another canonical positive integer keeps the manifest exact canonical
        // JSON, so the only thing wrong with the directory is the generation it
        // declares.
        constexpr auto k_substitute = uint64{99U};
        auto stale                  = manifest;
        stale.replace(
            declared,
            declaredFormat.size(),
            std::format("\"runtime_artifact_format\":{}", k_substitute)
        );
        REQUIRE(stale != manifest);
        copy.rewrite(k_manifest, stale);

        auto const staleOpen    = openProjectProduct(copy.args());
        auto const staleRefusal = why(staleOpen);
        INFO(staleRefusal);
        REQUIRE_FALSE(staleOpen.has_value());
        CHECK(staleRefusal.contains("manifest format is not supported by this Host"));
        CHECK(staleRefusal.contains(std::format("the manifest states {}", k_substitute)));
        CHECK(staleRefusal.contains(
            std::format("this Host reads {}", task::k_runtimeArtifactFormat)
        ));
        CHECK(staleRefusal.contains("is not one this binary can start"));

        copy.rewrite(k_manifest, manifest);
        auto const restored = openProjectProduct(copy.args());
        INFO(why(restored));
        REQUIRE(restored.has_value());

        // The other half of what the loader never looked at: a file the
        // manifest pins by size and digest, moved without changing its size.
        constexpr auto k_model =
            std::string_view{"runtime/artifact/runtime-model.toml"};
        auto model = copy.read(k_model);
        REQUIRE_FALSE(model.empty());
        model.back() = model.back() == 'x' ? 'y' : 'x';
        copy.rewrite(k_model, model);

        auto const movedOpen    = openProjectProduct(copy.args());
        auto const movedRefusal = why(movedOpen);
        INFO(movedRefusal);
        REQUIRE_FALSE(movedOpen.has_value());
        CHECK(movedRefusal.contains("runtime-model.toml"));
        CHECK(movedRefusal.contains("failed SHA-256 verification"));
    }

    // A directory holding no project document. The message is asserted and not
    // only the failure: reading an absent document as empty bytes also fails,
    // on "is not JSON", so a case that asked whether the open failed is
    // satisfied by the reading this refusal exists to forbid.
    TEST_CASE("a directory that is not a project is refused by name")
    {
        auto const unique = std::filesystem::path{
            "uf-open-empty-" + std::to_string(std::random_device{}()),
        };
        auto const root = std::filesystem::temp_directory_path() / unique;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);

        auto const opened = openProjectProduct(OpenArgs{.project = root});
        REQUIRE_FALSE(opened.has_value());
        auto const refusal = why(opened);
        CHECK(refusal.contains(root.string()));
        CHECK(refusal.contains("umbraflow-project.json"));

        // The document this verb never opens is not named in what it refuses:
        // a reader sent after a conformance fixture would go looking for a file
        // the product does not want.
        CHECK_FALSE(refusal.contains("umbraflow-conformance.json"));

        auto discarded = std::error_code{};
        std::filesystem::remove_all(root, discarded);
    }

    // The report is the whole of what this verb delivers to an operator, so
    // every field a caller cannot recover elsewhere has to survive into it.
    TEST_CASE("the report carries the identity, the artifact and the refusal")
    {
        auto const accepted = std::string(64U, 'a');
        auto const refused  = std::string(64U, 'b');
        auto const rootHash = std::string(64U, 'c');
        auto const opened   = OpenedProject{
              .directory           = "D:/projects/demo",
              .runtimeArtifactRoot = "D:/projects/demo/runtime/artifact",
              .artifact =
                OpenedArtifact{
                    .rootHash              = rootHash,
                    .runtimeArtifactFormat = 7U,
                    .runtimeModelFormat    = 8U,
                    .modelBytes            = 1268U,
                    .assets                = 2U,
                },

              .primaryDeployment = "alpha",

              .deployments =
                {
                    OpenedDeployment{
                        .name             = "alpha",
                        .pluginId         = "demo.alpha",
                        .registrationHash = accepted,
                    },
                    OpenedDeployment{
                        .name             = "beta",
                        .pluginId         = "demo.beta",
                        .registrationHash = refused,
                        .artifactBlobs    = 2U,
                        .refusal          = "the module is missing an entry point",
                    },
                },
        };

        CHECK_FALSE(everyPluginRegistered(opened));

        auto const text = formatOpenedProject(opened);
        CHECK(text.contains("D:/projects/demo"));

        // The artifact block. A reader has to be able to tell a verified
        // artifact from an unread one, which is the whole reason this verb
        // prints what it accepted rather than only its verdict. The two
        // generations are printed on their own lines, so each is asserted with
        // its label: the numbers alone would be found anywhere in the block.
        CHECK(text.contains("D:/projects/demo/runtime/artifact"));
        CHECK(text.contains(rootHash));
        CHECK(text.contains("manifest format 7"));
        CHECK(text.contains("model format    8"));
        CHECK(text.contains("accepted by this binary"));
        CHECK(text.contains("1268 bytes"));

        CHECK(text.contains("demo.alpha"));
        CHECK(text.contains(accepted));
        CHECK(text.contains(refused));

        // A deployment that did not register has to read differently from one
        // that did, at a glance and in a log somebody greps.
        CHECK(text.contains("NOT REGISTERED"));
        CHECK(text.contains("the module is missing an entry point"));

        auto const registered = OpenedProject{
            .deployments = {OpenedDeployment{.name = "alpha"}},
        };
        CHECK(everyPluginRegistered(registered));
        CHECK_FALSE(formatOpenedProject(registered).contains("NOT REGISTERED"));
    }
}
