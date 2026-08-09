#pragma once

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/ids.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uf::operator_runtime
{
    class OperatorCoordinator;
}

namespace uf::task
{
    inline constexpr auto k_runtimeArtifactManifestFileName =
        std::string_view{"runtime-artifact.manifest.json"};
    inline constexpr auto k_runtimeModelFileName = std::string_view{"page-model.toml"};
    inline constexpr auto k_runtimeAssetDirectoryName = std::string_view{"assets"};
    inline constexpr auto k_runtimeArtifactSchemaHash = std::string_view{
        "57432151740401a245eac9c5f3e813438c97014a3739b18853ebf0bc19f46fe9"
    };
    inline constexpr auto k_runtimeModelSchemaHash = std::string_view{
        "44be8ecf11eccc707a1a30cd04e69b612eae249f0aae837d95c40bfbcc316b3a"
    };

    inline constexpr auto k_maximumRuntimeManifestBytes = std::size_t{1024U * 1024U};
    inline constexpr auto k_maximumRuntimeModelBytes = std::size_t{4U * 1024U * 1024U};
    inline constexpr auto k_maximumRuntimeAssetCount = std::size_t{4096U};
    inline constexpr auto k_maximumRuntimeAssetBytes =
        std::size_t{256U * 1024U * 1024U};
    inline constexpr auto k_maximumRuntimeArtifactBytes =
        std::size_t{256U * 1024U * 1024U};

    class TaskHost;

    // A confined, byte-frozen deployment artifact. Construction is available
    // only through loadRuntimeArtifact(), which verifies the trusted root hash,
    // exact canonical manifest, complete file closure and every declared size and
    // digest. The handle interprets no page-model.toml semantics.
    class RuntimeArtifactHandle final
    {
    public:
        // A frozen file record is data, not authority. Public visibility lets the
        // verifier assemble the value without making the handle constructible.
        struct File final
        {
            std::string            path{};
            ContentHash            hash;
            std::vector<std::byte> bytes{};
        };

    private:
        std::filesystem::path m_root;
        ContentHash           m_rootHash;
        ContentHash           m_manifestSchemaHash;
        ContentHash           m_runtimeModelSchemaHash;
        std::vector<std::byte> m_manifestBytes;
        std::vector<File>      m_files;

        RuntimeArtifactHandle(
            std::filesystem::path root,
            ContentHash rootHash,
            ContentHash manifestSchemaHash,
            ContentHash runtimeModelSchemaHash,
            std::vector<std::byte> manifestBytes,
            std::vector<File> files
        ) noexcept;

        friend auto loadRuntimeArtifact(
            std::filesystem::path const& artifactRoot,
            ContentHash const& expectedRootHash
        ) -> Result<RuntimeArtifactHandle>;

    public:
        RuntimeArtifactHandle(RuntimeArtifactHandle const&) = delete;
        RuntimeArtifactHandle(RuntimeArtifactHandle&&) noexcept = default;
        auto operator=(RuntimeArtifactHandle const&) -> RuntimeArtifactHandle& = delete;
        auto operator=(RuntimeArtifactHandle&&) noexcept
            -> RuntimeArtifactHandle& = default;

        ~RuntimeArtifactHandle() = default;

        [[nodiscard]]
        auto root() const noexcept UF_LIFETIME_BOUND -> std::filesystem::path const&;

        [[nodiscard]]
        auto rootHash() const noexcept UF_LIFETIME_BOUND -> ContentHash const&;

        [[nodiscard]]
        auto manifestSchemaHash() const noexcept UF_LIFETIME_BOUND
            -> ContentHash const&;

        [[nodiscard]]
        auto runtimeModelSchemaHash() const noexcept UF_LIFETIME_BOUND
            -> ContentHash const&;

        [[nodiscard]]
        auto modelHash() const noexcept UF_LIFETIME_BOUND -> ContentHash const&;

        [[nodiscard]]
        auto modelBytes() const noexcept UF_LIFETIME_BOUND -> std::span<std::byte const>;

        [[nodiscard]]
        auto manifestBytes() const noexcept UF_LIFETIME_BOUND -> std::span<std::byte const>;

        [[nodiscard]] auto assetPaths() const -> std::vector<std::string>;

        [[nodiscard]]
        auto fileBytes(std::string_view relativePath) const
            -> Result<std::vector<std::byte>>;
    };

    // Production activation authority. Verification produces only a
    // RuntimeArtifactHandle; the production-owned installed-generation CAS is
    // the sole constructor of this move-only value. TaskHost accepts this value
    // and never an arbitrary filesystem path.
    class InstalledRuntimeArtifact final
    {
        std::shared_ptr<RuntimeArtifactHandle const> m_artifact;
        uint64                                       m_installedGeneration;

        InstalledRuntimeArtifact(
            std::shared_ptr<RuntimeArtifactHandle const> artifact,
            uint64 installedGeneration
        ) noexcept;

        friend class TaskHost;
        friend class ::uf::operator_runtime::OperatorCoordinator;
        friend struct TaskHostTestAccess;

    public:
        InstalledRuntimeArtifact(InstalledRuntimeArtifact const&) = delete;
        InstalledRuntimeArtifact(InstalledRuntimeArtifact&&) noexcept = default;
        auto operator=(InstalledRuntimeArtifact const&)
            -> InstalledRuntimeArtifact& = delete;
        auto operator=(InstalledRuntimeArtifact&&) noexcept
            -> InstalledRuntimeArtifact& = default;
        ~InstalledRuntimeArtifact() = default;

        [[nodiscard]] auto installedGeneration() const noexcept -> uint64;

        [[nodiscard]]
        auto rootHash() const noexcept UF_LIFETIME_BOUND -> ContentHash const&;
    };

    // The generation-owned result of the trusted Runtime parser. It can be
    // observed but not constructed by callers: only TaskHost's private finalize
    // path can bind parser output to one verified artifact and generation.
    class RuntimeModelBinding final
    {
        GenerationId                           m_generation;
        std::shared_ptr<RuntimeArtifactHandle const> m_artifact;
        ContentHash                            m_semanticHash;

        RuntimeModelBinding(
            GenerationId generation,
            std::shared_ptr<RuntimeArtifactHandle const> artifact,
            ContentHash semanticHash
        ) noexcept;

        friend class TaskHost;

    public:
        RuntimeModelBinding(RuntimeModelBinding const&) = default;
        RuntimeModelBinding(RuntimeModelBinding&&) noexcept = default;
        auto operator=(RuntimeModelBinding const&) -> RuntimeModelBinding& = default;
        auto operator=(RuntimeModelBinding&&) noexcept -> RuntimeModelBinding& = default;

        ~RuntimeModelBinding() = default;

        [[nodiscard]] auto generation() const noexcept -> GenerationId;

        [[nodiscard]]
        auto artifactRootHash() const noexcept UF_LIFETIME_BOUND
            -> ContentHash const&;

        [[nodiscard]]
        auto runtimeModelSchemaHash() const noexcept UF_LIFETIME_BOUND
            -> ContentHash const&;

        [[nodiscard]]
        auto semanticHash() const noexcept UF_LIFETIME_BOUND -> ContentHash const&;
    };

    [[nodiscard]]
    auto loadRuntimeArtifact(
        std::filesystem::path const& artifactRoot,
        ContentHash const& expectedRootHash
    ) -> Result<RuntimeArtifactHandle>;
}
