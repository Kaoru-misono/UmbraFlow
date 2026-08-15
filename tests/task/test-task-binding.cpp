#include <task/runtime-model-file.hpp>

#include <domain/content-hash.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uf::task
{
    namespace
    {
        class TemporaryDir final
        {
            std::filesystem::path m_path;

        public:
            TemporaryDir()
            {
                static auto sequence = std::atomic<uint64>{1};
                m_path = std::filesystem::temp_directory_path()
                    / std::format(
                        "umbraflow-runtime-artifact-{}-{}",
                        std::chrono::steady_clock::now().time_since_epoch().count(),
                        sequence.fetch_add(1, std::memory_order_relaxed)
                    );
                REQUIRE(std::filesystem::create_directory(m_path));
            }

            // The directory is removed by whoever holds it, so holding it is
            // exclusive and untransferable: a second holder would remove a
            // directory the first still names.
            TemporaryDir(TemporaryDir const&)                    = delete;
            TemporaryDir(TemporaryDir&&)                         = delete;
            auto operator=(TemporaryDir const&) -> TemporaryDir& = delete;
            auto operator=(TemporaryDir&&) -> TemporaryDir&      = delete;

            ~TemporaryDir() noexcept
            {
                auto error = std::error_code{};
                static_cast<void>(std::filesystem::remove_all(m_path, error));
            }

            [[nodiscard]] auto path() const -> std::filesystem::path const& { return m_path; }
        };

        auto write(std::filesystem::path const& path, std::string_view text) -> void
        {
            std::filesystem::create_directories(path.parent_path());
            auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
            REQUIRE(stream.good());
        }

        [[nodiscard]] auto hash(std::string_view text) -> ContentHash
        {
            auto result = sha256(std::as_bytes(std::span{text}));
            REQUIRE(result.has_value());
            return *result;
        }

        [[nodiscard]] auto file(std::string_view path, std::string_view bytes) -> std::string
        {
            return std::format(
                "{{\"path\":\"{}\",\"sha256\":\"{}\",\"size\":{}}}",
                path,
                hash(bytes).hex(),
                bytes.size()
            );
        }

        [[nodiscard]]
        auto publishWithFormats(
            std::filesystem::path const& root,
            std::string_view model,
            std::string_view asset,
            uint64 runtimeArtifactFormat,
            uint64 runtimeModelFormat
        ) -> ContentHash
        {
            constexpr auto assetPath = std::string_view{"assets/button.bin"};
            write(root / k_runtimeModelFileName, model);
            write(root / "assets" / "button.bin", asset);
            auto const manifest = std::format(
                "{{\"assets\":[{}],\"page_model\":{},"
                "\"runtime_artifact_format\":{},\"runtime_model_format\":{}}}",
                file(assetPath, asset),
                file(k_runtimeModelFileName, model),
                runtimeArtifactFormat,
                runtimeModelFormat
            );
            write(root / k_runtimeArtifactManifestFileName, manifest);
            return hash(manifest);
        }

        [[nodiscard]]
        auto publish(
            std::filesystem::path const& root,
            std::string_view model,
            std::string_view asset
        ) -> ContentHash
        {
            return publishWithFormats(
                root,
                model,
                asset,
                k_runtimeArtifactFormat,
                k_runtimeModelFormat
            );
        }
    }

    TEST_CASE("RuntimeArtifact freezes the exact manifest closure without TOML semantics")
    {
        auto const directory = TemporaryDir{};
        auto const rootHash = publish(directory.path(), "not TOML\r\n", "asset-v1");

        auto artifact = loadRuntimeArtifact(directory.path(), rootHash);
        REQUIRE(artifact.has_value());
        CHECK(artifact->rootHash() == rootHash);
        CHECK(artifact->modelHash() == hash("not TOML\r\n"));
        CHECK(artifact->assetPaths() == std::vector<std::string>{"assets/button.bin"});

        write(directory.path() / k_runtimeModelFileName, "mutated");
        auto const frozen = artifact->modelBytes();
        auto text = std::string{};
        for (auto const value : frozen)
        {
            text.push_back(static_cast<char>(std::to_integer<unsigned char>(value)));
        }
        CHECK(text == "not TOML\r\n");
    }

    // What the two format members buy and what they must not cost.
    //
    // They buy a refusal that names both generations, because neither is
    // visible from the other side: the artifact states one and this binary was
    // built with the other, and a publisher told only "unsupported" cannot tell
    // which generation to move to. What they must not cost is the file
    // closure -- the digests that were always the point of the manifest are
    // untouched by the stage that replaced the two schema pins, and the last
    // case is what says so.
    TEST_CASE("RuntimeArtifact refuses a generation this binary does not read")
    {
        auto const directory = TemporaryDir{};
        constexpr auto k_supplied = uint64{99U};

        SUBCASE("an unsupported runtime_model_format names both generations")
        {
            auto const rootHash = publishWithFormats(
                directory.path(),
                "not TOML\r\n",
                "asset-v1",
                k_runtimeArtifactFormat,
                k_supplied
            );
            auto const refused = loadRuntimeArtifact(directory.path(), rootHash);
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                std::format("the manifest states {}", k_supplied)
            ));
            CHECK(refused.error().message().contains(
                std::format("this parser reads {}", k_runtimeModelFormat)
            ));
        }

        SUBCASE("an unsupported runtime_artifact_format names both generations")
        {
            auto const rootHash = publishWithFormats(
                directory.path(),
                "not TOML\r\n",
                "asset-v1",
                k_supplied,
                k_runtimeModelFormat
            );
            auto const refused = loadRuntimeArtifact(directory.path(), rootHash);
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                std::format("the manifest states {}", k_supplied)
            ));
            CHECK(refused.error().message().contains(
                std::format("this Host reads {}", k_runtimeArtifactFormat)
            ));
        }

        SUBCASE("one mutated asset byte is still the closure refusal")
        {
            auto const rootHash = publish(directory.path(), "not TOML\r\n", "asset-v1");
            REQUIRE(loadRuntimeArtifact(directory.path(), rootHash).has_value());

            // Same length, so the declared size still holds and the only thing
            // left to notice the change is the declared digest.
            write(directory.path() / "assets" / "button.bin", "asset-v2");
            auto const refused = loadRuntimeArtifact(directory.path(), rootHash);
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains("assets/button.bin"));
            CHECK(refused.error().message().contains("failed SHA-256 verification"));
        }
    }
}
