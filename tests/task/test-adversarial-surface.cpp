#include <task/runtime-model-file.hpp>

#include <domain/content-hash.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

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
                        "umbraflow-artifact-attack-{}-{}",
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

        [[nodiscard]] auto digest(std::string_view text) -> ContentHash
        {
            auto result = sha256(std::as_bytes(std::span{text}));
            REQUIRE(result.has_value());
            return *result;
        }

        [[nodiscard]] auto manifest(std::string_view model) -> std::string
        {
            return std::format(
                "{{\"assets\":[],\"manifest_schema_hash\":\"{}\","
                "\"page_model\":{{\"path\":\"runtime-model.toml\",\"sha256\":\"{}\","
                "\"size\":{}}},\"runtime_model_schema_hash\":\"{}\"}}",
                k_runtimeArtifactSchemaHash,
                digest(model).hex(),
                model.size(),
                k_runtimeModelSchemaHash
            );
        }
    }

    static_assert(!std::is_default_constructible_v<RuntimeArtifactHandle>);
    static_assert(!std::is_copy_constructible_v<RuntimeArtifactHandle>);
    static_assert(!std::is_default_constructible_v<RuntimeModelBinding>);
    static_assert(!std::is_aggregate_v<RuntimeModelBinding>);

    TEST_CASE("RuntimeArtifact rejects authority and closure injection")
    {
        auto const directory = TemporaryDir{};
        write(directory.path() / k_runtimeModelFileName, "model");
        auto const bytes = manifest("model");
        write(directory.path() / k_runtimeArtifactManifestFileName, bytes);

        SUBCASE("deployment root must match exact manifest bytes")
        {
            CHECK_FALSE(loadRuntimeArtifact(directory.path(), digest("foreign")).has_value());
        }

        SUBCASE("unlisted production files are rejected")
        {
            write(directory.path() / "annotation-screenshot.png", "pixels");
            CHECK_FALSE(loadRuntimeArtifact(directory.path(), digest(bytes)).has_value());
        }

        SUBCASE("non-canonical JSON is rejected")
        {
            write(directory.path() / k_runtimeArtifactManifestFileName, bytes + "\n");
            CHECK_FALSE(loadRuntimeArtifact(directory.path(), digest(bytes + "\n")).has_value());
        }
    }
}
