// What the product binary does that nothing shipped had ever done: turn a
// project directory into plugins the Operator's registrar holds.
//
// tests/deployment already covers the load, one case per rule the directory
// format states. What is only covered here is the half beyond it -- the loader
// derives a registration without ever compiling the plugin, so a directory it
// accepts can still fail to register, and the case below that removes an entry
// point is the one that makes "registered" a claim rather than a word.

#include <cli/args.hpp>
#include <cli/open-project.hpp>

// Six other test translation units carry a copy of repositoryRoot; the header
// under tests/json is the one spelling for the files that can reach it, and a
// relative include is what lets this file reach it without a seventh copy.
#include "../json/repository-path.hpp"

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
        constexpr auto k_marker =
            std::string_view{"examples/umbraflow/umbraflow-project.json"};

        constexpr auto k_exemplar = std::string_view{"examples/umbraflow"};

        [[nodiscard]]
        auto why(Result<OpenedProject> const& outcome) -> std::string
        {
            return outcome.has_value()
                ? std::string{"<the directory was accepted>"}
                : std::string{outcome.error().message()};
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
                auto const root = json::repositoryRoot(k_marker);
                REQUIRE_FALSE(root.empty());

                auto const unique = std::filesystem::path{
                    "uf-open-" + std::to_string(std::random_device{}()),
                };
                m_root = std::filesystem::temp_directory_path() / unique;
                std::filesystem::remove_all(m_root);
                std::filesystem::copy(
                    root / k_exemplar,
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
        auto const root = json::repositoryRoot(k_marker);
        REQUIRE_FALSE(root.empty());

        auto const opened = openProjectProduct(
            OpenArgs{.project = root / k_exemplar}
        );
        INFO(why(opened));
        REQUIRE(opened.has_value());
        CHECK(everyPluginRegistered(*opened));

        CHECK(opened->primaryDeployment == "alpha");
        CHECK(opened->probeFrameBytes > std::size_t{0});
        REQUIRE(opened->deployments.size() == 2U);

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

        CHECK(opened->underTest.deployment == "alpha");
        CHECK(opened->underTest.mutatingTool == "command-1");
        CHECK(opened->underTest.uiTarget == "fixture.target");
        CHECK(opened->foreign.deployment == "foreign");
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

    // A directory holding neither root document. The message is asserted and
    // not only the failure: reading an absent document as empty bytes also
    // fails, on "is not JSON", so a case that asked whether the open failed is
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
        CHECK(refusal.contains("umbraflow-conformance.json"));

        auto discarded = std::error_code{};
        std::filesystem::remove_all(root, discarded);
    }

    // The report is the whole of what this verb delivers to an operator, so
    // every field a caller cannot recover elsewhere has to survive into it.
    TEST_CASE("the report carries the identity, the vocabulary and the refusal")
    {
        auto const accepted = std::string(64U, 'a');
        auto const refused  = std::string(64U, 'b');
        auto const opened   = OpenedProject{
              .directory           = "D:/projects/demo",
              .runtimeArtifactRoot = "D:/projects/demo/runtime/artifact",
              .probeFrameBytes     = 78U,
              .primaryDeployment   = "alpha",
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
              .underTest =
                OpenedRole{
                    .deployment   = "alpha",
                    .mutatingTool = "demo.advance",
                    .absentTool   = "demo.uncarried",
                    .uiTarget     = "demo.button",
                },
              .foreign = OpenedRole{.deployment = "beta"},
        };

        CHECK_FALSE(everyPluginRegistered(opened));

        auto const text = formatOpenedProject(opened);
        CHECK(text.contains("D:/projects/demo"));
        CHECK(text.contains("78 bytes"));
        CHECK(text.contains("demo.alpha"));
        CHECK(text.contains(accepted));
        CHECK(text.contains(refused));
        CHECK(text.contains("demo.advance"));
        CHECK(text.contains("demo.uncarried"));
        CHECK(text.contains("demo.button"));

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
