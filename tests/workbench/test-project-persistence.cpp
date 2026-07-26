#include "../annotation/test-helpers.hpp"

#include <project-persistence.hpp>

#include <annotation/authoring-compiler.hpp>
#include <annotation/authoring-document.hpp>
#include <annotation/content-hash.hpp>

#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <image/png.hpp>

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::workbench
{
    namespace
    {
        constexpr auto k_sourceId       = "00000000-0000-0000-0000-000000000201";
        constexpr auto k_secondSourceId = "00000000-0000-0000-0000-000000000202";
        constexpr auto k_anchorId       = "00000000-0000-0000-0000-000000000001";
        constexpr auto k_secondAnchorId = "00000000-0000-0000-0000-000000000002";
        constexpr auto k_pageId         = "00000000-0000-0000-0000-000000000101";

        struct ProjectFixture final
        {
            annotation::AuthoringDocument    document;
            annotation::AuthoringSourceAsset sourceAsset;
        };

        struct MultiSourceProjectFixture final
        {
            annotation::AuthoringDocument                  document;
            std::array<annotation::AuthoringSourceAsset, 2> sourceAssets;
        };

        class TemporaryProject final
        {
            std::filesystem::path m_path{};

        public:
            explicit TemporaryProject(std::string_view role)
            {
                static auto s_sequence = std::atomic<uint64>{1U};
                auto const token = std::chrono::steady_clock::now()
                    .time_since_epoch()
                    .count();
                m_path = std::filesystem::temp_directory_path()
                    / std::format(
                        "umbraflow-workbench-{}-{}-{}",
                        role,
                        token,
                        s_sequence.fetch_add(1U, std::memory_order_relaxed)
                    );
                auto error = std::error_code{};
                auto const created = std::filesystem::create_directory(
                    m_path,
                    error
                );
                REQUIRE(created);
                REQUIRE_FALSE(error);
            }

            TemporaryProject(TemporaryProject const&) = delete;
            auto operator=(TemporaryProject const&) -> TemporaryProject& = delete;
            TemporaryProject(TemporaryProject&&) = delete;
            auto operator=(TemporaryProject&&) -> TemporaryProject& = delete;

            ~TemporaryProject() noexcept
            {
                try
                {
                    auto error = std::error_code{};
                    static_cast<void>(std::filesystem::remove_all(m_path, error));
                }
                catch (...)
                {
                }
            }

            [[nodiscard]]
            auto path() const -> std::filesystem::path
            {
                return m_path;
            }
        };

        [[nodiscard]]
        constexpr auto asByte(uint8 value) noexcept -> std::byte
        {
            return static_cast<std::byte>(value);
        }

        [[nodiscard]]
        auto encodedSource(uint8 redOffset) -> std::vector<std::byte>
        {
            auto const pixels = std::vector{
                asByte(static_cast<uint8>(1U ^ redOffset)),
                asByte(2),
                asByte(3),
                asByte(255),
                asByte(4),
                asByte(5),
                asByte(6),
                asByte(255),
                asByte(7),
                asByte(8),
                asByte(9),
                asByte(255),
                asByte(10),
                asByte(11),
                asByte(12),
                asByte(255),
            };
            auto encoded = image::encodeRgbaPng(
                "workbench-source.png",
                2,
                2,
                pixels
            );
            REQUIRE(encoded.has_value());
            return *std::move(encoded);
        }

        [[nodiscard]]
        auto projectFixture(uint8 redOffset) -> ProjectFixture
        {
            auto const fingerprint = annotation::test::fingerprint(2, 2, 96, 96);
            auto const sourceId    = annotation::test::sourceId(k_sourceId);
            auto const anchorId    = annotation::test::recognizerId(k_anchorId);
            auto const pageId      = annotation::test::pageId(k_pageId);
            auto pngBytes          = encodedSource(redOffset);
            auto const sourceHash  = annotation::sha256(pngBytes);
            REQUIRE(sourceHash.has_value());

            auto source = annotation::AuthoringSource::create(
                annotation::AuthoringSourceSpec{
                    .id          = sourceId,
                    .contentHash = *sourceHash,
                    .fingerprint = fingerprint,
                    .provenance  = annotation::ImportedSourceProvenance{},
                }
            );
            REQUIRE(source.has_value());
            auto document = annotation::AuthoringDocument::create(
                annotation::test::projectId(),
                fingerprint,
                {*source},
                {
                    annotation::test::anchorElement(
                        fingerprint,
                        anchorId,
                        "home_marker",
                        sourceId,
                        annotation::test::pixelRect(0, 0, 1, 1),
                        annotation::test::pixelRect(0, 0, 2, 2)
                    ),
                },
                {annotation::test::page(pageId, "home", {anchorId})},
                {},
                {}
            );
            REQUIRE(document.has_value());
            return ProjectFixture{
                .document    = *std::move(document),
                .sourceAsset = annotation::AuthoringSourceAsset{
                    .id       = sourceId,
                    .pngBytes = std::move(pngBytes),
                },
            };
        }

        [[nodiscard]]
        auto multiSourceProjectFixture() -> MultiSourceProjectFixture
        {
            auto const fingerprint    = annotation::test::fingerprint(2, 2, 96, 96);
            auto const firstSourceId  = annotation::test::sourceId(k_sourceId);
            auto const secondSourceId = annotation::test::sourceId(k_secondSourceId);
            auto const firstAnchorId  = annotation::test::recognizerId(k_anchorId);
            auto const secondAnchorId = annotation::test::recognizerId(k_secondAnchorId);
            auto const pageId         = annotation::test::pageId(k_pageId);

            auto firstPng  = encodedSource(0);
            auto secondPng = encodedSource(0x40);
            auto const firstHash  = annotation::sha256(firstPng);
            auto const secondHash = annotation::sha256(secondPng);
            REQUIRE(firstHash.has_value());
            REQUIRE(secondHash.has_value());

            auto firstSource  = annotation::AuthoringSource::create(
                annotation::AuthoringSourceSpec{
                    .id          = firstSourceId,
                    .contentHash = *firstHash,
                    .fingerprint = fingerprint,
                    .provenance  = annotation::ImportedSourceProvenance{},
                }
            );
            auto secondSource = annotation::AuthoringSource::create(
                annotation::AuthoringSourceSpec{
                    .id          = secondSourceId,
                    .contentHash = *secondHash,
                    .fingerprint = fingerprint,
                    .provenance  = annotation::ImportedSourceProvenance{},
                }
            );
            REQUIRE(firstSource.has_value());
            REQUIRE(secondSource.has_value());

            auto document = annotation::AuthoringDocument::create(
                annotation::test::projectId(),
                fingerprint,
                {*firstSource, *secondSource},
                {
                    annotation::test::anchorElement(
                        fingerprint,
                        firstAnchorId,
                        "first_marker",
                        firstSourceId,
                        annotation::test::pixelRect(0, 0, 1, 1),
                        annotation::test::pixelRect(0, 0, 2, 2)
                    ),
                    annotation::test::anchorElement(
                        fingerprint,
                        secondAnchorId,
                        "second_marker",
                        secondSourceId,
                        annotation::test::pixelRect(0, 0, 1, 1),
                        annotation::test::pixelRect(0, 0, 2, 2)
                    ),
                },
                {
                    annotation::test::page(
                        pageId,
                        "home",
                        {firstAnchorId, secondAnchorId}
                    ),
                },
                {},
                {}
            );
            REQUIRE(document.has_value());

            return MultiSourceProjectFixture{
                .document     = *std::move(document),
                .sourceAssets = {
                    annotation::AuthoringSourceAsset{
                        .id       = firstSourceId,
                        .pngBytes = std::move(firstPng),
                    },
                    annotation::AuthoringSourceAsset{
                        .id       = secondSourceId,
                        .pngBytes = std::move(secondPng),
                    },
                },
            };
        }

        [[nodiscard]]
        auto readText(std::filesystem::path const& path) -> std::string
        {
            auto error           = std::error_code{};
            auto const fileBytes = std::filesystem::file_size(path, error);
            REQUIRE_FALSE(error);
            auto const size       = checkedCast<std::size_t>(fileBytes);
            auto const streamSize = checkedCast<std::streamsize>(fileBytes);
            REQUIRE(size.has_value());
            REQUIRE(streamSize.has_value());

            auto stream = std::ifstream{path, std::ios::binary};
            REQUIRE(stream.is_open());
            auto text = std::string(size.value_or(std::size_t{0}), '\0');
            if (!text.empty())
            {
                stream.read(
                    text.data(),
                    streamSize.value_or(std::streamsize{0})
                );
                REQUIRE(stream.good());
            }
            return text;
        }

        [[nodiscard]]
        auto readBytes(
            std::filesystem::path const& path
        ) -> std::vector<std::byte>
        {
            auto const text = readText(path);
            auto const view = std::as_bytes(std::span{text});
            return std::vector<std::byte>{view.begin(), view.end()};
        }

        auto overwriteFile(
            std::filesystem::path const& path,
            std::span<std::byte const> bytes
        ) -> void
        {
            auto contents = std::string(bytes.size(), '\0');
            for (auto index = std::size_t{0}; index < bytes.size(); ++index)
            {
                contents[index] = std::to_integer<char>(bytes[index]);
            }
            auto stream = std::ofstream{
                path,
                std::ios::binary | std::ios::trunc
            };
            REQUIRE(stream.is_open());
            stream.write(
                contents.data(),
                static_cast<std::streamsize>(contents.size())
            );
            REQUIRE(stream.good());
        }

        [[nodiscard]]
        auto containsTemporaryFile(
            std::filesystem::path const& root
        ) -> bool
        {
            auto error = std::error_code{};
            auto iterator = std::filesystem::recursive_directory_iterator{
                root,
                error
            };
            REQUIRE_FALSE(error);
            auto const end = std::filesystem::recursive_directory_iterator{};
            while (iterator != end)
            {
                if (
                    iterator->path().filename().string().contains(
                        ".umbraflow-tmp-"
                    )
                )
                {
                    return true;
                }
                iterator.increment(error);
                REQUIRE_FALSE(error);
            }
            return false;
        }
    }

    TEST_CASE("workbench saves and atomically publishes a complete authoring project")
    {
        auto const project = TemporaryProject{"complete"};
        auto const fixture = projectFixture(0);
        auto const assets  = std::span{&fixture.sourceAsset, std::size_t{1}};
        auto const compiled = annotation::compileAuthoringDocument(
            fixture.document,
            assets
        );
        REQUIRE(compiled.has_value());

        auto const saved = saveAndGenerateAuthoringProject(
            project.path(),
            fixture.document,
            assets
        );
        auto const savedInfo = saved
            ? std::string{"saved"}
            : toString(saved.error());
        INFO(savedInfo);
        REQUIRE(saved.has_value());
        REQUIRE(
            saveAndGenerateAuthoringProject(
                project.path(),
                fixture.document,
                assets
            ).has_value()
        );

        auto const authoringToml = readText(project.path() / "annotations.toml");
        CHECK(
            authoringToml
            == annotation::serializeAuthoringDocument(fixture.document)
        );
        auto const reopened = annotation::parseAuthoringDocument(authoringToml);
        REQUIRE(reopened.has_value());

        auto const runtimeToml = readText(
            project.path() / "generated" / "annotations.runtime.toml"
        );
        CHECK(runtimeToml == compiled->runtimeManifestToml);
        auto const parsedRuntime = annotation::parseRuntimeManifest(runtimeToml);
        REQUIRE(parsedRuntime.has_value());

        CHECK(
            readBytes(project.path() / fixture.document.sources().front().relativePath())
            == fixture.sourceAsset.pngBytes
        );
        for (auto const& asset : compiled->templateAssets)
        {
            CHECK(
                readBytes(project.path() / asset.relativePath)
                == asset.pngBytes
            );
        }
        CHECK_FALSE(containsTemporaryFile(project.path()));
    }

    TEST_CASE("workbench atomically replaces an existing published project")
    {
        auto const project = TemporaryProject{"replace"};
        auto const first   = projectFixture(0);
        auto const firstAssets = std::span{
            &first.sourceAsset,
            std::size_t{1}
        };
        REQUIRE(
            saveAndGenerateAuthoringProject(
                project.path(),
                first.document,
                firstAssets
            ).has_value()
        );
        auto const firstCompiled = annotation::compileAuthoringDocument(
            first.document,
            firstAssets
        );
        REQUIRE(firstCompiled.has_value());
        REQUIRE(firstCompiled->templateAssets.size() == 1U);
        auto const firstSourcePath = project.path()
            / first.document.sources().front().relativePath();
        auto const firstTemplatePath = project.path()
            / firstCompiled->templateAssets.front().relativePath;

        auto const second = projectFixture(0x40);
        auto const secondAssets = std::span{
            &second.sourceAsset,
            std::size_t{1}
        };
        auto const secondCompiled = annotation::compileAuthoringDocument(
            second.document,
            secondAssets
        );
        REQUIRE(secondCompiled.has_value());
        REQUIRE(secondCompiled->templateAssets.size() == 1U);
        REQUIRE(
            saveAndGenerateAuthoringProject(
                project.path(),
                second.document,
                secondAssets
            ).has_value()
        );

        CHECK(
            readText(project.path() / "annotations.toml")
            == annotation::serializeAuthoringDocument(second.document)
        );
        CHECK(
            readText(project.path() / "generated" / "annotations.runtime.toml")
            == secondCompiled->runtimeManifestToml
        );
        CHECK(std::filesystem::is_regular_file(firstSourcePath));
        CHECK(std::filesystem::is_regular_file(firstTemplatePath));
        CHECK(
            readBytes(
                project.path()
                / second.document.sources().front().relativePath()
            ) == second.sourceAsset.pngBytes
        );
        CHECK(
            readBytes(
                project.path()
                / secondCompiled->templateAssets.front().relativePath
            ) == secondCompiled->templateAssets.front().pngBytes
        );
        CHECK_FALSE(containsTemporaryFile(project.path()));
    }

    TEST_CASE("workbench validates before creating a project root")
    {
        auto const project = TemporaryProject{"invalid"};
        auto const fixture = projectFixture(0);
        auto tampered      = fixture.sourceAsset;
        REQUIRE_FALSE(tampered.pngBytes.empty());
        tampered.pngBytes.back() ^= std::byte{1};

        auto error         = std::error_code{};
        auto const removed = std::filesystem::remove(project.path(), error);
        REQUIRE(removed);
        REQUIRE_FALSE(error);

        auto const assets = std::span{&tampered, std::size_t{1}};
        auto const saved  = saveAndGenerateAuthoringProject(
            project.path(),
            fixture.document,
            assets
        );
        REQUIRE_FALSE(saved.has_value());
        annotation::test::requireErrorKind(
            saved.error(),
            AutomationErrorKind::InvalidResource
        );
        CHECK_FALSE(std::filesystem::exists(project.path(), error));
        CHECK_FALSE(error);
    }

    TEST_CASE("workbench maps reversed source assets to their own paths")
    {
        auto const project = TemporaryProject{"source-order"};
        auto const fixture = multiSourceProjectFixture();
        auto const reversedAssets = std::array{
            fixture.sourceAssets[1],
            fixture.sourceAssets[0],
        };
        auto const saved = saveAndGenerateAuthoringProject(
            project.path(),
            fixture.document,
            reversedAssets
        );
        REQUIRE(saved.has_value());

        auto const sources = fixture.document.sources();
        REQUIRE(sources.size() == fixture.sourceAssets.size());
        CHECK(
            readBytes(project.path() / sources[0].relativePath())
            == fixture.sourceAssets[0].pngBytes
        );
        CHECK(
            readBytes(project.path() / sources[1].relativePath())
            == fixture.sourceAssets[1].pngBytes
        );
        CHECK_FALSE(containsTemporaryFile(project.path()));
    }

    TEST_CASE("workbench rejects corrupted content-addressed assets")
    {
        auto const project = TemporaryProject{"rollback"};
        auto const first   = projectFixture(0);
        auto const firstAssets = std::span{
            &first.sourceAsset,
            std::size_t{1}
        };
        auto const firstSaved = saveAndGenerateAuthoringProject(
            project.path(),
            first.document,
            firstAssets
        );
        auto const firstSavedInfo = firstSaved
            ? std::string{"saved"}
            : toString(firstSaved.error());
        INFO(firstSavedInfo);
        REQUIRE(firstSaved.has_value());
        auto const originalAuthoring = readText(
            project.path() / "annotations.toml"
        );
        auto const originalRuntime = readText(
            project.path() / "generated" / "annotations.runtime.toml"
        );

        auto const second = projectFixture(0x40);
        auto const secondAssets = std::span{
            &second.sourceAsset,
            std::size_t{1}
        };
        auto const secondCompiled = annotation::compileAuthoringDocument(
            second.document,
            secondAssets
        );
        REQUIRE(secondCompiled.has_value());
        REQUIRE(secondCompiled->templateAssets.size() == 1U);
        auto const blockedTemplate = project.path()
            / secondCompiled->templateAssets.front().relativePath;
        REQUIRE_FALSE(std::filesystem::exists(blockedTemplate));
        auto error = std::error_code{};
        auto const blocked = std::filesystem::copy_file(
            project.path() / first.document.sources().front().relativePath(),
            blockedTemplate,
            error
        );
        REQUIRE(blocked);
        REQUIRE_FALSE(error);

        auto const failed = saveAndGenerateAuthoringProject(
            project.path(),
            second.document,
            secondAssets
        );
        REQUIRE_FALSE(failed.has_value());
        annotation::test::requireErrorKind(
            failed.error(),
            AutomationErrorKind::InvalidResource
        );
        CHECK(readText(project.path() / "annotations.toml") == originalAuthoring);
        CHECK(
            readText(project.path() / "generated" / "annotations.runtime.toml")
            == originalRuntime
        );
        CHECK_FALSE(containsTemporaryFile(project.path()));
    }

    TEST_CASE("workbench loads a saved project and round-trips its document and sources")
    {
        auto const project = TemporaryProject{"load"};
        auto const fixture = projectFixture(0);
        auto const assets  = std::span{&fixture.sourceAsset, std::size_t{1}};
        REQUIRE(
            saveAndGenerateAuthoringProject(
                project.path(),
                fixture.document,
                assets
            ).has_value()
        );

        auto const loaded     = loadAuthoringProject(project.path());
        auto const loadedInfo = loaded
            ? std::string{"loaded"}
            : toString(loaded.error());
        INFO(loadedInfo);
        REQUIRE(loaded.has_value());

        CHECK(
            annotation::serializeAuthoringDocument(loaded->document)
            == annotation::serializeAuthoringDocument(fixture.document)
        );
        REQUIRE(loaded->sources.size() == 1U);
        CHECK(loaded->sources.front().id == fixture.sourceAsset.id);
        CHECK(
            loaded->sources.front().pngBytes
            == fixture.sourceAsset.pngBytes
        );
    }

    TEST_CASE("workbench rejects a source whose bytes no longer match the document hash")
    {
        auto const project = TemporaryProject{"load-hash"};
        auto const fixture = projectFixture(0);
        auto const assets  = std::span{&fixture.sourceAsset, std::size_t{1}};
        REQUIRE(
            saveAndGenerateAuthoringProject(
                project.path(),
                fixture.document,
                assets
            ).has_value()
        );

        auto const sourcePath = project.path()
            / fixture.document.sources().front().relativePath();
        auto tampered = fixture.sourceAsset.pngBytes;
        REQUIRE_FALSE(tampered.empty());
        tampered.back() ^= std::byte{1};
        overwriteFile(sourcePath, tampered);

        auto const loaded = loadAuthoringProject(project.path());
        REQUIRE_FALSE(loaded.has_value());
        annotation::test::requireErrorKind(
            loaded.error(),
            AutomationErrorKind::InvalidResource
        );
    }

    TEST_CASE("workbench rejects a project whose referenced source is missing")
    {
        auto const project = TemporaryProject{"load-missing"};
        auto const fixture = projectFixture(0);
        auto const assets  = std::span{&fixture.sourceAsset, std::size_t{1}};
        REQUIRE(
            saveAndGenerateAuthoringProject(
                project.path(),
                fixture.document,
                assets
            ).has_value()
        );

        auto const sourcePath = project.path()
            / fixture.document.sources().front().relativePath();
        auto error         = std::error_code{};
        auto const removed = std::filesystem::remove(sourcePath, error);
        REQUIRE(removed);
        REQUIRE_FALSE(error);

        auto const loaded = loadAuthoringProject(project.path());
        REQUIRE_FALSE(loaded.has_value());
        annotation::test::requireErrorKind(
            loaded.error(),
            AutomationErrorKind::InvalidResource
        );
    }
}
