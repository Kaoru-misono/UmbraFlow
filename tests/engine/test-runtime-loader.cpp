#include "../annotation/test-helpers.hpp"

#include <runtime-loader.hpp>

#include <annotation/authoring-compiler.hpp>
#include <annotation/authoring-document.hpp>
#include <annotation/content-hash.hpp>
#include <annotation/recognition.hpp>
#include <annotation/recognition-runtime.hpp>

#include <core/numeric/checked-cast.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <image/png.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uf::engine
{
    namespace
    {
        namespace anno = annotation;

        constexpr auto g_sourceId = "00000000-0000-0000-0000-000000000201";
        constexpr auto g_anchorId = "00000000-0000-0000-0000-000000000001";
        constexpr auto g_actionId = "00000000-0000-0000-0000-000000000002";
        constexpr auto g_pageId   = "00000000-0000-0000-0000-000000000101";

        [[nodiscard]]
        constexpr auto asByte(uint8 value) noexcept -> std::byte
        {
            return static_cast<std::byte>(value);
        }

        struct ProjectFixture final
        {
            anno::AuthoringDocument    m_document;
            anno::AuthoringSourceAsset m_sourceAsset;
        };

        // A three-by-two RGBA source whose top-left pixel is RGB(1, 2, 3). The
        // anchor crops that single pixel, so a frame painted with the same colour
        // reproduces the template exactly and the page resolves. Matches the
        // established compiler fixture in tests/annotation/test-authoring-compiler.
        [[nodiscard]]
        auto encodedSource() -> std::vector<std::byte>
        {
            auto const pixels = std::vector<std::byte>{
                asByte(1), asByte(2), asByte(3), asByte(255),
                asByte(4), asByte(5), asByte(6), asByte(255),
                asByte(7), asByte(8), asByte(9), asByte(255),
                asByte(10), asByte(11), asByte(12), asByte(255),
                asByte(13), asByte(14), asByte(15), asByte(255),
                asByte(16), asByte(17), asByte(18), asByte(255),
            };
            auto encoded = image::encodeRgbaPng("runtime-loader-source.png", 3, 2, pixels);
            REQUIRE(encoded.has_value());
            return *std::move(encoded);
        }

        [[nodiscard]]
        auto projectFixture() -> ProjectFixture
        {
            auto const fingerprint = anno::test::fingerprint(3, 2, 96, 96);
            auto const sourceId    = anno::test::sourceId(g_sourceId);
            auto pngBytes          = encodedSource();
            auto const sourceHash  = anno::sha256(pngBytes);
            REQUIRE(sourceHash.has_value());

            auto source = anno::AuthoringSource::create(
                anno::AuthoringSourceSpec{
                    .m_id          = sourceId,
                    .m_contentHash = *sourceHash,
                    .m_fingerprint = fingerprint,
                    .m_provenance  = anno::ImportedSourceProvenance{},
                }
            );
            REQUIRE(source.has_value());
            auto const anchorId = anno::test::recognizerId(g_anchorId);
            auto const actionId = anno::test::recognizerId(g_actionId);
            auto const pageId   = anno::test::pageId(g_pageId);
            auto const click    = anno::TemplateOffset::create(1, 1, 2, 2);
            REQUIRE(click.has_value());
            auto document = anno::AuthoringDocument::create(
                anno::test::projectId(),
                fingerprint,
                {*source},
                {
                    anno::AuthoringRecognizerSpec{
                        .m_definition = anno::test::recognizer(
                            fingerprint,
                            anchorId,
                            "home_marker",
                            anno::AnnotationType::PageAnchor,
                            anno::test::pixelRect(0, 0, 1, 1),
                            anno::test::pixelRect(0, 0, 3, 2)
                        ),
                        .m_sourceId = sourceId,
                    },
                    anno::AuthoringRecognizerSpec{
                        .m_definition = anno::test::recognizer(
                            fingerprint,
                            actionId,
                            "daily_button",
                            anno::AnnotationType::ActionTarget,
                            anno::test::pixelRect(1, 0, 2, 2),
                            anno::test::pixelRect(0, 0, 3, 2),
                            {pageId},
                            *click
                        ),
                        .m_sourceId = sourceId,
                    },
                },
                {anno::test::page(pageId, "home", {anchorId})},
                {}
            );
            REQUIRE(document.has_value());
            return ProjectFixture{
                .m_document    = *std::move(document),
                .m_sourceAsset = anno::AuthoringSourceAsset{
                    .m_id       = sourceId,
                    .m_pngBytes = std::move(pngBytes),
                },
            };
        }

        class TemporaryDir final
        {
            std::filesystem::path m_path{};

        public:
            explicit TemporaryDir(std::string_view role)
            {
                static auto s_sequence = std::atomic<uint64>{1U};
                auto const token = std::chrono::steady_clock::now()
                    .time_since_epoch()
                    .count();
                m_path = std::filesystem::temp_directory_path()
                    / std::format(
                        "umbraflow-engine-loader-{}-{}-{}",
                        role,
                        token,
                        s_sequence.fetch_add(1U, std::memory_order_relaxed)
                    );
                auto error         = std::error_code{};
                auto const created = std::filesystem::create_directory(m_path, error);
                REQUIRE(created);
                REQUIRE_FALSE(error);
            }

            TemporaryDir(TemporaryDir const&) = delete;
            TemporaryDir(TemporaryDir&&) = delete;
            auto operator=(TemporaryDir const&) -> TemporaryDir& = delete;
            auto operator=(TemporaryDir&&) -> TemporaryDir& = delete;

            ~TemporaryDir() noexcept
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

        auto writeFile(
            std::filesystem::path const& path,
            std::span<std::byte const> bytes
        ) -> void
        {
            auto error = std::error_code{};
            std::filesystem::create_directories(path.parent_path(), error);
            REQUIRE_FALSE(error);
            auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};
            REQUIRE(stream.is_open());
            // Write byte by byte through std::to_integer so no cast is needed to
            // reach the underlying char storage; template PNGs here are tiny.
            for (auto const byte : bytes)
            {
                stream.put(std::to_integer<char>(byte));
            }
            stream.flush();
            REQUIRE(stream.good());
        }

        auto writeText(std::filesystem::path const& path, std::string_view text) -> void
        {
            writeFile(path, std::as_bytes(std::span{text}));
        }

        // Writes a project's runtime manifest and every template asset so that
        // loadRuntimeProject can read them back. The source PNG is intentionally
        // omitted: the loader reads templates only.
        auto writeProject(
            std::filesystem::path const& root,
            anno::CompiledAuthoringProject const& compiled
        ) -> void
        {
            writeText(
                root / "generated" / "annotations.runtime.toml",
                compiled.m_runtimeManifestToml
            );
            for (auto const& asset : compiled.m_templateAssets)
            {
                writeFile(root / asset.m_relativePath, asset.m_pngBytes);
            }
        }

        // A frame painted uniformly with the source's top-left pixel in BGRA
        // order (B=3, G=2, R=1). Its grey value equals the anchor template's, so
        // the anchor matches with zero sum-of-absolute-differences.
        [[nodiscard]]
        auto matchingFrame(anno::ProjectFingerprint fingerprint) -> Frame
        {
            auto const transform = CoordinateTransform::create(
                Point<DesktopSpace>{0.0F, 0.0F},
                static_cast<float>(fingerprint.width()),
                static_cast<float>(fingerprint.height()),
                fingerprint.width(),
                fingerprint.height()
            );
            REQUIRE(transform.has_value());
            auto const width  = checkedCast<std::size_t>(fingerprint.width());
            auto const height = checkedCast<std::size_t>(fingerprint.height());
            REQUIRE(width.has_value());
            REQUIRE(height.has_value());
            auto const pixelCount = *width * *height;
            auto pixels           = std::vector<std::byte>{};
            pixels.reserve(pixelCount * 4U);
            for (auto index = std::size_t{0}; index < pixelCount; ++index)
            {
                pixels.emplace_back(asByte(3));
                pixels.emplace_back(asByte(2));
                pixels.emplace_back(asByte(1));
                pixels.emplace_back(asByte(255));
            }
            auto const stride = *width * bytesPerPixel(PixelFormat::Bgra8);
            auto const buffer = std::shared_ptr<FrameBuffer const>{
                std::make_shared<FrameBuffer>(std::move(pixels))
            };
            auto frame = Frame::create(
                FrameId{17},
                SessionId{7},
                TargetGeneration::fromValue(3),
                MonotonicInstant::fromTimePoint(MonotonicInstant::TimePoint{}),
                fingerprint.width(),
                fingerprint.height(),
                stride,
                PixelFormat::Bgra8,
                buffer,
                *transform
            );
            REQUIRE(frame.has_value());
            return *std::move(frame);
        }
    }

    TEST_CASE("engine loads a published project and recognizes a page")
    {
        auto const temp     = TemporaryDir{"round-trip"};
        auto const fixture  = projectFixture();
        auto const assets   = std::span{&fixture.m_sourceAsset, std::size_t{1}};
        auto const compiled = anno::compileAuthoringDocument(fixture.m_document, assets);
        REQUIRE(compiled.has_value());
        writeProject(temp.path(), *compiled);

        auto loaded = loadRuntimeProject(temp.path());
        REQUIRE(loaded.has_value());

        auto const fingerprint = anno::test::fingerprint(3, 2, 96, 96);
        auto const frame       = matchingFrame(fingerprint);
        auto const outcome     = loaded->m_runtime.recognizePage(
            frame,
            fingerprint,
            anno::RecognitionPolicy{.m_maximumPixelComparisons = 100}
        );
        REQUIRE(outcome.has_value());
        REQUIRE(std::holds_alternative<anno::ResolvedPage>(*outcome));
        CHECK(
            std::get<anno::ResolvedPage>(*outcome).pageId()
            == anno::test::pageId(g_pageId)
        );
    }

    TEST_CASE("engine rejects a corrupt runtime manifest and names its path")
    {
        auto const temp = TemporaryDir{"corrupt"};
        auto const manifestPath = temp.path()
            / "generated"
            / "annotations.runtime.toml";
        writeText(manifestPath, "this is not a valid runtime manifest\n");

        auto const loaded = loadRuntimeProject(temp.path());
        REQUIRE_FALSE(loaded.has_value());
        auto const kind = automationErrorKind(loaded.error());
        REQUIRE(kind.has_value());
        CHECK(*kind == AutomationErrorKind::InvalidResource);
    }

    TEST_CASE("engine reports a missing template file by path")
    {
        auto const temp     = TemporaryDir{"missing-template"};
        auto const fixture  = projectFixture();
        auto const assets   = std::span{&fixture.m_sourceAsset, std::size_t{1}};
        auto const compiled = anno::compileAuthoringDocument(fixture.m_document, assets);
        REQUIRE(compiled.has_value());
        // Write only the manifest, leaving every referenced template absent.
        writeText(
            temp.path() / "generated" / "annotations.runtime.toml",
            compiled->m_runtimeManifestToml
        );

        auto const loaded = loadRuntimeProject(temp.path());
        REQUIRE_FALSE(loaded.has_value());
        auto const kind = automationErrorKind(loaded.error());
        REQUIRE(kind.has_value());
        CHECK(*kind == AutomationErrorKind::IoFailure);
        CHECK(loaded.error().message().contains("assets/templates"));
    }

    TEST_CASE("engine rejects a template whose bytes do not match its hash")
    {
        auto const temp     = TemporaryDir{"tampered-template"};
        auto const fixture  = projectFixture();
        auto const assets   = std::span{&fixture.m_sourceAsset, std::size_t{1}};
        auto const compiled = anno::compileAuthoringDocument(fixture.m_document, assets);
        REQUIRE(compiled.has_value());
        writeProject(temp.path(), *compiled);

        REQUIRE_FALSE(compiled->m_templateAssets.empty());
        auto const& tampered = compiled->m_templateAssets.front();
        auto bytes           = tampered.m_pngBytes;
        REQUIRE_FALSE(bytes.empty());
        bytes.back() ^= std::byte{0x01};
        writeFile(temp.path() / tampered.m_relativePath, bytes);

        auto const loaded = loadRuntimeProject(temp.path());
        REQUIRE_FALSE(loaded.has_value());
        auto const kind = automationErrorKind(loaded.error());
        REQUIRE(kind.has_value());
        CHECK(*kind == AutomationErrorKind::InvalidResource);
    }

    TEST_CASE("engine rejects an oversized runtime manifest before reading it")
    {
        auto const temp         = TemporaryDir{"oversized"};
        auto const manifestPath = temp.path()
            / "generated"
            / "annotations.runtime.toml";
        auto error = std::error_code{};
        std::filesystem::create_directories(manifestPath.parent_path(), error);
        REQUIRE_FALSE(error);

        auto stream = std::ofstream{manifestPath, std::ios::binary | std::ios::trunc};
        REQUIRE(stream.is_open());
        // Grow the file past the cap with a single trailing byte rather than
        // materializing the whole payload, exercising the stat-first guard.
        auto const beyondCap = checkedCast<std::streamoff>(g_maximumRuntimeManifestBytes);
        REQUIRE(beyondCap.has_value());
        stream.seekp(*beyondCap);
        stream.put('\0');
        stream.flush();
        REQUIRE(stream.good());
        stream.close();

        auto const loaded = loadRuntimeProject(temp.path());
        REQUIRE_FALSE(loaded.has_value());
        auto const kind = automationErrorKind(loaded.error());
        REQUIRE(kind.has_value());
        CHECK(*kind == AutomationErrorKind::InvalidResource);
    }
}
