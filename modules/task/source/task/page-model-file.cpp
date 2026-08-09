#include "page-model-file.hpp"

#include "platform/confined-read.hpp"

#include <core/error/result.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::task
{
    namespace
    {
        struct ManifestFile final
        {
            std::string path{};
            ContentHash hash;
            uint64      size{};
        };

        struct ParsedManifest final
        {
            std::vector<ManifestFile> assets{};
            ContentHash               manifestSchemaHash;
            ManifestFile              pageModel;
            ContentHash               runtimeModelSchemaHash;
        };

        [[nodiscard]]
        auto refuse(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        [[nodiscard]]
        auto schemaHash(std::string_view hex) -> Result<ContentHash>
        {
            auto encoded = std::string{"sha256:"};
            encoded += hex;
            return ContentHash::parse(encoded);
        }

        [[nodiscard]]
        auto ioFailure(
            std::string_view operation,
            std::filesystem::path const& path,
            std::error_code error
        ) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::IoFailure,
                std::format(
                    "cannot {} runtime artifact path {}: {}",
                    operation,
                    path.string(),
                    error.message()
                )
            );
        }

        [[nodiscard]]
        auto isWithin(
            std::filesystem::path const& path,
            std::filesystem::path const& root
        ) -> bool
        {
            auto component = path.begin();
            for (auto const& expected : root)
            {
                if (component == path.end() || *component != expected)
                {
                    return false;
                }
                ++component;
            }
            return true;
        }

        [[nodiscard]]
        auto pathFromUtf8(std::string_view text) -> std::filesystem::path
        {
            auto encoded = std::u8string{};
            encoded.reserve(text.size());
            for (auto const value : text)
            {
                encoded.push_back(static_cast<char8_t>(static_cast<unsigned char>(value)));
            }
            return std::filesystem::path{encoded};
        }

        [[nodiscard]]
        auto artifactPath(std::string_view text) -> Result<std::filesystem::path>
        {
            if (text.empty() || text.starts_with('/') || text.contains('\\'))
            {
                return refuse(
                    std::format("runtime artifact path '{}' is not canonical", text)
                );
            }

            auto result = std::filesystem::path{};
            auto first  = std::size_t{0};
            while (first < text.size())
            {
                auto const slash = text.find('/', first);
                auto const last = slash == std::string_view::npos ? text.size() : slash;
                auto const component = text.substr(first, last - first);
                if (
                    component.empty()
                    || component == "."
                    || component == ".."
                    || component.ends_with('.')
                    || component.ends_with(' ')
                )
                {
                    return refuse(
                        std::format("runtime artifact path '{}' is not canonical", text)
                    );
                }
                for (auto const value : component)
                {
                    auto const byte = static_cast<unsigned char>(value);
                    if (
                        byte < 0x20U
                        || value == ':'
                        || value == '<'
                        || value == '>'
                        || value == '|'
                        || value == '?'
                        || value == '*'
                        || value == '"'
                    )
                    {
                        return refuse(
                            std::format(
                                "runtime artifact path '{}' contains a forbidden byte",
                                text
                            )
                        );
                    }
                }
                result /= pathFromUtf8(component);
                if (slash == std::string_view::npos)
                {
                    break;
                }
                first = slash + 1U;
            }
            return result;
        }

        [[nodiscard]]
        auto isAssetComponent(std::string_view component) noexcept -> bool
        {
            auto const validFirst = [](char value) noexcept
            {
                return (value >= 'A' && value <= 'Z')
                    || (value >= 'a' && value <= 'z')
                    || (value >= '0' && value <= '9')
                    || value == '_'
                    || value == '-';
            };
            auto const validRest = [&validFirst](char value) noexcept
            {
                return validFirst(value) || value == '.';
            };
            return !component.empty()
                && validFirst(component.front())
                && std::ranges::all_of(component.substr(1U), validRest);
        }

        [[nodiscard]]
        auto validateAssetPath(std::string_view text) -> Status
        {
            constexpr auto prefix = std::string_view{"assets/"};
            if (!text.starts_with(prefix))
            {
                return refuse(
                    std::format("runtime asset '{}' is outside assets/", text)
                );
            }

            auto remaining = text.substr(prefix.size());
            while (!remaining.empty())
            {
                auto const slash = remaining.find('/');
                auto const component = remaining.substr(0U, slash);
                if (!isAssetComponent(component))
                {
                    return refuse(
                        std::format("runtime asset path '{}' is not canonical", text)
                    );
                }
                if (slash == std::string_view::npos)
                {
                    return ok();
                }
                remaining.remove_prefix(slash + 1U);
            }
            return refuse(std::format("runtime asset path '{}' is not canonical", text));
        }

        class ManifestReader final
        {
            std::string_view m_source;
            std::size_t      m_offset{};

            [[nodiscard]]
            auto failure(std::string_view expected) const -> std::unexpected<Error>
            {
                return refuse(
                    std::format(
                        "runtime artifact manifest is not canonical at byte {}: expected {}",
                        m_offset,
                        expected
                    )
                );
            }

        public:
            explicit ManifestReader(std::string_view source) noexcept
                : m_source{source}
            {
            }

            [[nodiscard]] auto atEnd() const noexcept -> bool
            {
                return m_offset == m_source.size();
            }

            [[nodiscard]] auto consume(std::string_view literal) -> Status
            {
                if (!m_source.substr(m_offset).starts_with(literal))
                {
                    return failure(literal);
                }
                m_offset += literal.size();
                return ok();
            }

            [[nodiscard]] auto peek(char value) const noexcept -> bool
            {
                return m_offset < m_source.size() && m_source[m_offset] == value;
            }

            [[nodiscard]] auto string() -> Result<std::string>
            {
                UF_TRY(consume("\""));
                auto value = std::string{};
                while (m_offset < m_source.size() && m_source[m_offset] != '"')
                {
                    auto const next = m_source[m_offset];
                    auto const byte = static_cast<unsigned char>(next);
                    if (next == '\\' || byte < 0x20U)
                    {
                        return failure("an unescaped canonical path byte");
                    }
                    value.push_back(next);
                    ++m_offset;
                }
                UF_TRY(consume("\""));
                return value;
            }

            [[nodiscard]] auto hash() -> Result<ContentHash>
            {
                UF_TRY_VALUE(text, string());
                auto encoded = std::string{"sha256:"};
                encoded += text;
                return ContentHash::parse(encoded);
            }

            [[nodiscard]] auto size() -> Result<uint64>
            {
                if (m_offset >= m_source.size())
                {
                    return failure("an unsigned byte size");
                }
                auto const first = m_offset;
                while (
                    m_offset < m_source.size()
                    && m_source[m_offset] >= '0'
                    && m_source[m_offset] <= '9'
                )
                {
                    ++m_offset;
                }
                auto const digits = m_source.substr(first, m_offset - first);
                if (digits.empty() || (digits.size() > 1U && digits.front() == '0'))
                {
                    return failure("a canonical unsigned byte size");
                }
                auto value = uint64{};
                auto const parsed = std::from_chars(
                    digits.data(),
                    digits.data() + digits.size(),
                    value
                );
                if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size())
                {
                    return failure("an unsigned byte size within range");
                }
                return value;
            }
        };

        [[nodiscard]]
        auto parseFile(ManifestReader& reader) -> Result<ManifestFile>
        {
            UF_TRY(reader.consume("{\"path\":"));
            UF_TRY_VALUE(path, reader.string());
            UF_TRY(reader.consume(",\"sha256\":"));
            UF_TRY_VALUE(hash, reader.hash());
            UF_TRY(reader.consume(",\"size\":"));
            UF_TRY_VALUE(size, reader.size());
            UF_TRY(reader.consume("}"));
            return ManifestFile{
                .path = std::move(path),
                .hash = hash,
                .size = size,
            };
        }

        [[nodiscard]]
        auto parseManifest(std::string_view source) -> Result<ParsedManifest>
        {
            auto reader = ManifestReader{source};
            UF_TRY(reader.consume("{\"assets\":["));

            auto assets = std::vector<ManifestFile>{};
            while (!reader.peek(']'))
            {
                if (!assets.empty())
                {
                    UF_TRY(reader.consume(","));
                }
                UF_TRY_VALUE(asset, parseFile(reader));
                assets.emplace_back(std::move(asset));
            }

            UF_TRY(reader.consume("],\"manifest_schema_hash\":"));
            UF_TRY_VALUE(manifestSchemaHash, reader.hash());
            UF_TRY(reader.consume(",\"page_model\":"));
            UF_TRY_VALUE(pageModel, parseFile(reader));
            UF_TRY(reader.consume(",\"runtime_model_schema_hash\":"));
            UF_TRY_VALUE(runtimeModelSchemaHash, reader.hash());
            UF_TRY(reader.consume("}"));
            if (!reader.atEnd())
            {
                return refuse(
                    "runtime artifact manifest must have no BOM, whitespace, or "
                    "trailing bytes outside its canonical JSON value"
                );
            }
            return ParsedManifest{
                .assets                 = std::move(assets),
                .manifestSchemaHash     = manifestSchemaHash,
                .pageModel              = std::move(pageModel),
                .runtimeModelSchemaHash = runtimeModelSchemaHash,
            };
        }

        [[nodiscard]]
        auto readBytes(
            std::filesystem::path const& path,
            std::size_t maximumBytes
        ) -> Result<std::vector<std::byte>>
        {
            auto error        = std::error_code{};
            auto const status = std::filesystem::symlink_status(path, error);
            if (error)
            {
                return ioFailure("inspect", path, error);
            }
            if (status.type() != std::filesystem::file_type::regular)
            {
                return refuse(
                    std::format("runtime artifact path {} is not a regular file", path.string())
                );
            }

            auto const rawSize = std::filesystem::file_size(path, error);
            if (error)
            {
                return ioFailure("measure", path, error);
            }
            auto const size = checkedCast<std::size_t>(rawSize);
            if (!size || *size > maximumBytes)
            {
                return refuse(
                    std::format(
                        "runtime artifact file {} exceeds its {} byte ceiling",
                        path.string(),
                        maximumBytes
                    )
                );
            }

            auto stream = std::ifstream{path, std::ios::binary};
            if (!stream)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format("cannot open runtime artifact file {}", path.string())
                );
            }
            auto text = std::string(*size, '\0');
            if (*size != 0U)
            {
                stream.read(text.data(), static_cast<std::streamsize>(*size));
            }
            if (!stream || stream.gcount() != static_cast<std::streamsize>(*size))
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format("runtime artifact file {} changed while reading", path.string())
                );
            }

            auto bytes = std::vector<std::byte>{};
            bytes.reserve(text.size());
            for (auto const value : text)
            {
                bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
            }
            return bytes;
        }

        [[nodiscard]]
        auto canonicalArtifactRoot(std::filesystem::path const& root)
            -> Result<std::filesystem::path>
        {
            auto error        = std::error_code{};
            auto const status = std::filesystem::symlink_status(root, error);
            if (error)
            {
                return ioFailure("inspect", root, error);
            }
            if (!std::filesystem::is_directory(status) || std::filesystem::is_symlink(status))
            {
                return refuse("runtime artifact root must be a real directory, not a link");
            }
            auto const canonical = std::filesystem::canonical(root, error);
            if (error)
            {
                return ioFailure("canonicalize", root, error);
            }
            return canonical;
        }

        [[nodiscard]]
        auto resolveFile(
            std::filesystem::path const& canonicalRoot,
            std::string_view relativeText
        ) -> Result<std::filesystem::path>
        {
            UF_TRY_VALUE(relative, artifactPath(relativeText));
            auto current = canonicalRoot;
            for (auto const& component : relative)
            {
                current /= component;
                auto error        = std::error_code{};
                auto const status = std::filesystem::symlink_status(current, error);
                if (error)
                {
                    return ioFailure("inspect", current, error);
                }
                if (std::filesystem::is_symlink(status))
                {
                    return refuse(
                        std::format(
                            "runtime artifact path '{}' crosses a symbolic link",
                            relativeText
                        )
                    );
                }
            }

            auto error            = std::error_code{};
            auto const canonical = std::filesystem::canonical(current, error);
            if (error)
            {
                return ioFailure("canonicalize", current, error);
            }
            if (!isWithin(canonical, canonicalRoot))
            {
                return refuse(
                    std::format("runtime artifact path '{}' escapes its root", relativeText)
                );
            }
            return canonical;
        }

        [[nodiscard]]
        auto freezeFile(
            task_platform::ConfinedRoot const& confinedRoot,
            std::filesystem::path const& canonicalRoot,
            ManifestFile const& declared,
            std::size_t maximumBytes
        ) -> Result<RuntimeArtifactHandle::File>
        {
            // resolveFile still runs, because it is what refuses a manifest
            // spelling that escapes the root; the bytes then come from the
            // confined root, so what was checked and what is read are one
            // resolution rather than two.
            UF_TRY_VALUE(checked, resolveFile(canonicalRoot, declared.path));
            static_cast<void>(checked);
            UF_TRY_VALUE(bytes, confinedRoot.readFile(declared.path, maximumBytes));
            auto const declaredSize = checkedCast<std::size_t>(declared.size);
            if (!declaredSize || *declaredSize != bytes.size())
            {
                return refuse(
                    std::format(
                        "runtime artifact file '{}' has size {}, manifest declares {}",
                        declared.path,
                        bytes.size(),
                        declared.size
                    )
                );
            }
            UF_TRY_VALUE(hash, sha256(bytes));
            if (hash != declared.hash)
            {
                return refuse(
                    std::format("runtime artifact file '{}' failed SHA-256 verification", declared.path)
                );
            }
            return RuntimeArtifactHandle::File{
                .path  = declared.path,
                .hash  = hash,
                .bytes = std::move(bytes),
            };
        }

        [[nodiscard]]
        auto expectedDirectories(std::set<std::string> const& files)
            -> std::set<std::string>
        {
            auto directories = std::set<std::string>{};
            for (auto const& file : files)
            {
                auto current = std::filesystem::path{file}.parent_path();
                while (!current.empty())
                {
                    directories.emplace(current.generic_string());
                    current = current.parent_path();
                }
            }
            return directories;
        }

        [[nodiscard]]
        auto verifyClosure(
            std::filesystem::path const& canonicalRoot,
            std::set<std::string> const& expectedFiles
        ) -> Status
        {
            auto const expectedDirs = expectedDirectories(expectedFiles);
            auto error              = std::error_code{};
            auto iterator = std::filesystem::recursive_directory_iterator{
                canonicalRoot,
                std::filesystem::directory_options::none,
                error,
            };
            if (error)
            {
                return ioFailure("enumerate", canonicalRoot, error);
            }
            // The guard belongs in the condition, not the body: a failed
            // increment leaves the iterator at end, so a body-only check never
            // runs for the last step and an interrupted walk would be accepted
            // as a closed set.
            auto const end = std::filesystem::recursive_directory_iterator{};
            for (; !error && iterator != end; iterator.increment(error))
            {
                auto const status = iterator->symlink_status(error);
                if (error)
                {
                    return ioFailure("inspect", iterator->path(), error);
                }
                auto const relative = std::filesystem::relative(
                    iterator->path(),
                    canonicalRoot,
                    error
                );
                if (error)
                {
                    return ioFailure("relativize", iterator->path(), error);
                }
                auto const spelling = relative.generic_string();
                if (std::filesystem::is_symlink(status))
                {
                    return refuse(
                        std::format("runtime artifact contains link '{}'", spelling)
                    );
                }
                if (std::filesystem::is_directory(status))
                {
                    if (!expectedDirs.contains(spelling))
                    {
                        return refuse(
                            std::format("runtime artifact contains undeclared directory '{}'", spelling)
                        );
                    }
                    continue;
                }
                if (
                    status.type() != std::filesystem::file_type::regular
                    || !expectedFiles.contains(spelling)
                )
                {
                    return refuse(
                        std::format("runtime artifact contains undeclared path '{}'", spelling)
                    );
                }
            }
            if (error)
            {
                return ioFailure("enumerate", canonicalRoot, error);
            }
            return ok();
        }

        [[nodiscard]]
        auto asString(std::span<std::byte const> bytes) -> std::string
        {
            auto text = std::string{};
            text.reserve(bytes.size());
            for (auto const value : bytes)
            {
                text.push_back(static_cast<char>(std::to_integer<unsigned char>(value)));
            }
            return text;
        }
    }

    RuntimeArtifactHandle::RuntimeArtifactHandle(
        std::filesystem::path root,
        ContentHash rootHash,
        ContentHash manifestSchemaHash,
        ContentHash runtimeModelSchemaHash,
        std::vector<std::byte> manifestBytes,
        std::vector<File> files
    ) noexcept
        : m_root{std::move(root)}
        , m_rootHash{rootHash}
        , m_manifestSchemaHash{manifestSchemaHash}
        , m_runtimeModelSchemaHash{runtimeModelSchemaHash}
        , m_manifestBytes{std::move(manifestBytes)}
        , m_files{std::move(files)}
    {
    }

    auto RuntimeArtifactHandle::root() const noexcept -> std::filesystem::path const&
    {
        return m_root;
    }

    auto RuntimeArtifactHandle::rootHash() const noexcept -> ContentHash const&
    {
        return m_rootHash;
    }

    auto RuntimeArtifactHandle::manifestSchemaHash() const noexcept
        -> ContentHash const&
    {
        return m_manifestSchemaHash;
    }

    auto RuntimeArtifactHandle::runtimeModelSchemaHash() const noexcept
        -> ContentHash const&
    {
        return m_runtimeModelSchemaHash;
    }

    auto RuntimeArtifactHandle::modelHash() const noexcept -> ContentHash const&
    {
        auto const found = std::ranges::find(
            m_files,
            k_runtimeModelFileName,
            &File::path
        );
        return found->hash;
    }

    auto RuntimeArtifactHandle::modelBytes() const noexcept
        -> std::span<std::byte const>
    {
        auto const found = std::ranges::find(
            m_files,
            k_runtimeModelFileName,
            &File::path
        );
        return found->bytes;
    }

    auto RuntimeArtifactHandle::manifestBytes() const noexcept
        -> std::span<std::byte const>
    {
        return m_manifestBytes;
    }

    auto RuntimeArtifactHandle::assetPaths() const -> std::vector<std::string>
    {
        auto result = std::vector<std::string>{};
        result.reserve(m_files.size() - 1U);
        for (auto const& file : m_files)
        {
            if (file.path != k_runtimeModelFileName)
            {
                result.emplace_back(file.path);
            }
        }
        return result;
    }

    auto RuntimeArtifactHandle::fileBytes(
        std::string_view relativePath
    ) const -> Result<std::vector<std::byte>>
    {
        auto const found = std::ranges::find(m_files, relativePath, &File::path);
        if (found == m_files.end())
        {
            return refuse(
                std::format("'{}' is not in this runtime artifact", relativePath)
            );
        }
        return found->bytes;
    }

    InstalledRuntimeArtifact::InstalledRuntimeArtifact(
        std::shared_ptr<RuntimeArtifactHandle const> artifact,
        uint64 installedGeneration
    ) noexcept
        : m_artifact{std::move(artifact)}
        , m_installedGeneration{installedGeneration}
    {
    }

    auto InstalledRuntimeArtifact::installedGeneration() const noexcept -> uint64
    {
        return m_installedGeneration;
    }

    auto InstalledRuntimeArtifact::rootHash() const noexcept -> ContentHash const&
    {
        return m_artifact->rootHash();
    }

    RuntimeModelBinding::RuntimeModelBinding(
        GenerationId generation,
        std::shared_ptr<RuntimeArtifactHandle const> artifact,
        ContentHash semanticHash
    ) noexcept
        : m_generation{generation}
        , m_artifact{std::move(artifact)}
        , m_semanticHash{semanticHash}
    {
    }

    auto RuntimeModelBinding::generation() const noexcept -> GenerationId
    {
        return m_generation;
    }

    auto RuntimeModelBinding::artifactRootHash() const noexcept -> ContentHash const&
    {
        return m_artifact->rootHash();
    }

    auto RuntimeModelBinding::runtimeModelSchemaHash() const noexcept
        -> ContentHash const&
    {
        return m_artifact->runtimeModelSchemaHash();
    }

    auto RuntimeModelBinding::semanticHash() const noexcept -> ContentHash const&
    {
        return m_semanticHash;
    }

    auto loadRuntimeArtifact(
        std::filesystem::path const& artifactRoot,
        ContentHash const& expectedRootHash
    ) -> Result<RuntimeArtifactHandle>
    {
        UF_TRY_VALUE(root, canonicalArtifactRoot(artifactRoot));
        UF_TRY_VALUE(confinedRoot, task_platform::ConfinedRoot::open(root));
        UF_TRY_VALUE(
            manifestBytes,
            confinedRoot.readFile(
                k_runtimeArtifactManifestFileName,
                k_maximumRuntimeManifestBytes
            )
        );
        UF_TRY_VALUE(rootHash, sha256(manifestBytes));
        if (rootHash != expectedRootHash)
        {
            return refuse("runtime artifact manifest does not match the deployment root hash");
        }

        UF_TRY_VALUE(manifest, parseManifest(asString(manifestBytes)));
        UF_TRY_VALUE(expectedManifestSchema, schemaHash(k_runtimeArtifactSchemaHash));
        UF_TRY_VALUE(expectedRuntimeSchema, schemaHash(k_runtimeModelSchemaHash));
        if (manifest.manifestSchemaHash != expectedManifestSchema)
        {
            return refuse("runtime artifact manifest schema is not supported by this Host");
        }
        if (manifest.runtimeModelSchemaHash != expectedRuntimeSchema)
        {
            return refuse("runtime model schema is not supported by this trusted parser");
        }
        if (manifest.pageModel.path != k_runtimeModelFileName)
        {
            return refuse("runtime artifact page_model must be root page-model.toml");
        }
        if (manifest.pageModel.size == 0U)
        {
            return refuse("runtime artifact page_model must not be empty");
        }
        if (manifest.assets.size() > k_maximumRuntimeAssetCount)
        {
            return refuse(
                std::format(
                    "runtime artifact declares more than {} assets",
                    k_maximumRuntimeAssetCount
                )
            );
        }

        auto previous = std::string_view{};
        auto expected = std::set<std::string>{
            std::string{k_runtimeArtifactManifestFileName},
            std::string{k_runtimeModelFileName},
        };
        for (auto const& asset : manifest.assets)
        {
            UF_TRY(validateAssetPath(asset.path));
            if (asset.size == 0U)
            {
                return refuse(
                    std::format("runtime asset '{}' must not be empty", asset.path)
                );
            }
            if (!previous.empty() && previous >= asset.path)
            {
                return refuse(
                    "runtime artifact assets must be unique and sorted by UTF-8 path bytes"
                );
            }
            previous = asset.path;
            expected.emplace(asset.path);
        }
        UF_TRY(verifyClosure(root, expected));

        auto files = std::vector<RuntimeArtifactHandle::File>{};
        files.reserve(manifest.assets.size() + 1U);
        UF_TRY_VALUE(
            model,
            freezeFile(confinedRoot, root, manifest.pageModel, k_maximumRuntimeModelBytes)
        );
        auto totalBytes = model.bytes.size();
        files.emplace_back(std::move(model));
        for (auto const& asset : manifest.assets)
        {
            UF_TRY_VALUE(
                frozen,
                freezeFile(confinedRoot, root, asset, k_maximumRuntimeAssetBytes)
            );
            auto const next = checkedAdd(totalBytes, frozen.bytes.size());
            if (!next || *next > k_maximumRuntimeArtifactBytes)
            {
                return refuse(
                    std::format(
                        "runtime artifact exceeds its {} byte closure ceiling",
                        k_maximumRuntimeArtifactBytes
                    )
                );
            }
            totalBytes = *next;
            files.emplace_back(std::move(frozen));
        }

        return RuntimeArtifactHandle{
            std::move(root),
            rootHash,
            manifest.manifestSchemaHash,
            manifest.runtimeModelSchemaHash,
            std::move(manifestBytes),
            std::move(files),
        };
    }
}
