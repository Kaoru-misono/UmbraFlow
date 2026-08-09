#include <task/page-model-file.hpp>

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
        auto publish(
            std::filesystem::path const& root,
            std::string_view model,
            std::string_view asset
        ) -> ContentHash
        {
            constexpr auto assetPath = std::string_view{"assets/button.bin"};
            write(root / k_runtimeModelFileName, model);
            write(root / "assets" / "button.bin", asset);
            auto const manifest = std::format(
                "{{\"assets\":[{}],\"manifest_schema_hash\":\"{}\","
                "\"page_model\":{},\"runtime_model_schema_hash\":\"{}\"}}",
                file(assetPath, asset),
                k_runtimeArtifactSchemaHash,
                file(k_runtimeModelFileName, model),
                k_runtimeModelSchemaHash
            );
            write(root / k_runtimeArtifactManifestFileName, manifest);
            return hash(manifest);
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
}
