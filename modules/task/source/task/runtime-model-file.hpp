#pragma once

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

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
    inline constexpr auto k_runtimeModelFileName = std::string_view{"runtime-model.toml"};
    inline constexpr auto k_runtimeAssetDirectoryName = std::string_view{"assets"};
    // The generation of the RuntimeArtifact manifest contract this Host reads,
    // and the generation of the RuntimeModel contract its trusted parser reads.
    // Both are compatibility statements: an artifact declares the two numbers
    // and this binary decides whether it understands what they describe.
    //
    // They are generations rather than digests of the two schema files, because
    // a digest made every cosmetic edit to either file refuse every artifact
    // already published -- a cost paid on every edit for a property no reader
    // needed, and one the consumer repository was already paying with a stale
    // transcription that refused its own artifacts. modules/task/runtime/model.luau
    // states k_runtimeModelFormat again as model.format, and finalizeRuntimeModel
    // refuses an artifact whose parser answers with a different number, so a
    // drift between the two cannot activate.
    inline constexpr auto k_runtimeArtifactFormat = uint64{1U};
    inline constexpr auto k_runtimeModelFormat    = uint64{3U};

    // Each ceiling multiplies in std::size_t rather than widening a 32-bit
    // product: an unsigned product wraps silently, so a larger factor here would
    // otherwise pass as a much smaller limit.
    inline constexpr auto k_maximumRuntimeManifestBytes = std::size_t{1024} * 1024;
    inline constexpr auto k_maximumRuntimeModelBytes = std::size_t{4} * 1024 * 1024;
    inline constexpr auto k_maximumRuntimeAssetCount = std::size_t{4096};
    inline constexpr auto k_maximumRuntimeAssetBytes =
        std::size_t{256} * 1024 * 1024;
    inline constexpr auto k_maximumRuntimeArtifactBytes =
        std::size_t{256} * 1024 * 1024;

    class TaskHost;

    // A confined, byte-frozen deployment artifact. Construction is available
    // only through loadRuntimeArtifact(), which verifies the trusted root hash,
    // exact canonical manifest, complete file closure and every declared size and
    // digest. The handle interprets no runtime-model.toml semantics.
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
        std::filesystem::path  m_root;
        ContentHash            m_rootHash;
        uint64                 m_runtimeArtifactFormat;
        uint64                 m_runtimeModelFormat;
        std::vector<std::byte> m_manifestBytes;
        std::vector<File>      m_files;

        RuntimeArtifactHandle(
            std::filesystem::path root,
            ContentHash rootHash,
            uint64 runtimeArtifactFormat,
            uint64 runtimeModelFormat,
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
        auto runtimeArtifactFormat() const noexcept -> uint64;

        [[nodiscard]]
        auto runtimeModelFormat() const noexcept -> uint64;

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

    // The identifiers one RuntimeModel defines, published by the trusted parser
    // that already cross-references them. Three flat vocabularies of opaque
    // strings and nothing else: a reader may ask whether a name is in one of
    // them, which is identity, and cannot ask what the name means, which would
    // be interpreting RuntimeModel semantics in C++.
    //
    // The parser publishes each list sorted and duplicate-free. No C++ asserts
    // that, and nothing here depends on it: membership is a scan, and a rule
    // only the parser could break is a rule nothing can test.
    struct DeclaredRuntimeUi final
    {
        std::vector<std::string> surfaces{};
        std::vector<std::string> uiTargets{};
        std::vector<std::string> actions{};
    };

    // The generation-owned result of the trusted Runtime parser. It can be
    // observed but not constructed by callers: only TaskHost's private finalize
    // path can bind parser output to one verified artifact and generation.
    class RuntimeModelBinding final
    {
        GenerationId                                 m_generation;
        std::shared_ptr<RuntimeArtifactHandle const> m_artifact;
        ContentHash                                  m_semanticHash;
        DeclaredRuntimeUi                            m_declaredUi;
        ProjectFingerprint                           m_fingerprint;

        RuntimeModelBinding(
            GenerationId generation,
            std::shared_ptr<RuntimeArtifactHandle const> artifact,
            ContentHash semanticHash,
            DeclaredRuntimeUi declaredUi,
            ProjectFingerprint fingerprint
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
        auto semanticHash() const noexcept UF_LIFETIME_BOUND -> ContentHash const&;

        // What the model this generation parsed declares. It travels with the
        // artifact root hash above rather than on its own, so a reader that
        // trusts one of the two is trusting the same parse.
        [[nodiscard]]
        auto declaredUi() const noexcept UF_LIFETIME_BOUND
            -> DeclaredRuntimeUi const&;

        // The geometry this generation's model declares. It sits beside
        // declaredUi() rather than inside it: those are names, answerable only
        // by membership, while this is a measurement compared against a live
        // capture's extent. Every model states both base_resolution and
        // base_dpi and the trusted parser refuses one that does not, so there
        // is no absent case here and no default resolution below it.
        [[nodiscard]] auto fingerprint() const noexcept -> ProjectFingerprint;
    };

    [[nodiscard]]
    auto loadRuntimeArtifact(
        std::filesystem::path const& artifactRoot,
        ContentHash const& expectedRootHash
    ) -> Result<RuntimeArtifactHandle>;

    // H_genesis: the RuntimeModel that declares nothing, and the artifact that
    // carries it.
    //
    // The published schema admits the empty ui_targets, bindings and surfaces
    // arrays, so this document is a legal RuntimeModel; the trusted parser
    // compiles it to a model with no target, no binding and no surface. Every
    // project in the universe therefore starts from the SAME artifact root
    // hash, which is what gives the parentage chain a root rather than a
    // per-project seed.
    //
    // base_resolution and base_dpi carry the only values a model declaring
    // nothing can honestly carry: the smallest legal extent and the reference
    // DPI. They are geometry about nothing, and no binding exists to be placed
    // inside them.
    //
    // Sealed by construction. A closing record exists to say which session
    // produced a hash from which parent, and this hash has no parent and no
    // producing session -- it is a constant of the framework, not the output of
    // a run, so there is nothing for a record to attest and nothing mutable for
    // it to be wrong about.
    inline constexpr auto k_genesisRuntimeModelToml = std::string_view{
        "schema_version = 3\n"
        "base_resolution = [1, 1]\n"
        "base_dpi = [96, 96]\n"
    };

    // The exact canonical manifest bytes of the genesis artifact, derived from
    // the document above rather than transcribed beside it. Both formats come
    // from k_runtimeArtifactFormat and k_runtimeModelFormat, so a format cut
    // moves H_genesis with it instead of leaving a stale constant behind.
    [[nodiscard]]
    auto genesisRuntimeArtifactManifestJcs() -> Result<std::string>;

    // H_genesis itself: the content address of those manifest bytes, on the
    // same terms every other artifact root hash is derived.
    [[nodiscard]]
    auto genesisArtifactRootHash() -> Result<ContentHash>;
}
