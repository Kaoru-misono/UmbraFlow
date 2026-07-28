#include "../annotation/test-helpers.hpp"

#include <source-ingestion.hpp>

#include <annotation/authoring-document.hpp>
#include <annotation/content-hash.hpp>

#include <core/numeric/checked-cast.hpp>
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
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace uf::workbench
{
    namespace
    {
        constexpr auto k_sourceId = "00000000-0000-0000-0000-000000000301";

        [[nodiscard]]
        constexpr auto asByte(uint8 value) noexcept -> std::byte
        {
            return static_cast<std::byte>(value);
        }

        class TemporaryFile final
        {
            std::filesystem::path m_path{};

        public:
            explicit TemporaryFile(std::string_view suffix)
            {
                static auto s_sequence = std::atomic<uint64>{1U};
                auto const token = std::chrono::steady_clock::now()
                    .time_since_epoch()
                    .count();
                m_path = std::filesystem::temp_directory_path()
                    / std::format(
                        "umbraflow-ingest-{}-{}.{}",
                        token,
                        s_sequence.fetch_add(1U, std::memory_order_relaxed),
                        suffix
                    );
            }

            TemporaryFile(TemporaryFile const&) = delete;
            auto operator=(TemporaryFile const&) -> TemporaryFile& = delete;
            TemporaryFile(TemporaryFile&&) = delete;
            auto operator=(TemporaryFile&&) -> TemporaryFile& = delete;

            ~TemporaryFile() noexcept
            {
                try
                {
                    auto error = std::error_code{};
                    static_cast<void>(std::filesystem::remove(m_path, error));
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

        // A 2x2 RGBA image with four distinct opaque pixels.
        [[nodiscard]]
        auto sampleRgba() -> std::vector<std::byte>
        {
            return std::vector{
                asByte(10), asByte(20), asByte(30), asByte(255),
                asByte(40), asByte(50), asByte(60), asByte(255),
                asByte(70), asByte(80), asByte(90), asByte(255),
                asByte(100), asByte(110), asByte(120), asByte(255),
            };
        }

        auto writeBytes(
            std::filesystem::path const& path,
            std::span<std::byte const> bytes
        ) -> void
        {
            auto text = std::string{};
            text.reserve(bytes.size());
            for (auto const byte : bytes)
            {
                text.push_back(static_cast<char>(std::to_integer<uint8>(byte)));
            }
            auto stream = std::ofstream{path, std::ios::binary};
            REQUIRE(stream.is_open());
            auto const size = checkedCast<std::streamsize>(text.size());
            REQUIRE(size.has_value());
            if (!text.empty())
            {
                stream.write(text.data(), size.value_or(std::streamsize{0}));
            }
            REQUIRE(stream.good());
        }

        [[nodiscard]]
        auto bgraFrame(
            uint32 width,
            uint32 height,
            std::size_t stride,
            std::vector<std::byte> pixels,
            TargetGeneration generation
        ) -> Frame
        {
            auto const transform = CoordinateTransform::create(
                Point<DesktopSpace>{0.0F, 0.0F},
                static_cast<float>(width),
                static_cast<float>(height),
                width,
                height
            );
            REQUIRE(transform.has_value());
            auto const buffer = std::shared_ptr<FrameBuffer const>{
                std::make_shared<FrameBuffer>(std::move(pixels))
            };
            auto frame = Frame::create(
                FrameId{5},
                CaptureSessionId{9},
                generation,
                MonotonicInstant::fromTimePoint(MonotonicInstant::TimePoint{}),
                width,
                height,
                stride,
                PixelFormat::Bgra8,
                buffer,
                *transform
            );
            REQUIRE(frame.has_value());
            return *std::move(frame);
        }
    }

    TEST_CASE("importSourcePng canonically re-encodes and hashes a PNG source")
    {
        auto const id      = annotation::test::sourceId(k_sourceId);
        auto const pixels  = sampleRgba();
        auto encoded       = image::encodeRgbaPng("on-disk.png", 2, 2, pixels);
        REQUIRE(encoded.has_value());
        auto const file    = TemporaryFile{"png"};
        writeBytes(file.path(), *encoded);

        auto const imported = importSourcePng(id, file.path());
        REQUIRE(imported.has_value());

        CHECK(imported->spec.id == id);
        CHECK(imported->asset.id == id);
        CHECK(imported->spec.fingerprint.width() == 2);
        CHECK(imported->spec.fingerprint.height() == 2);
        // A bare PNG carries no density, so an import keeps the conventional 96.
        CHECK(imported->spec.fingerprint.dpiX() == 96);
        CHECK(imported->spec.fingerprint.dpiY() == 96);
        CHECK(
            std::holds_alternative<annotation::ImportedSourceProvenance>(
                imported->spec.provenance
            )
        );

        // The re-encode is canonical: identical input bytes reproduce exactly.
        CHECK(imported->asset.pngBytes == *encoded);
        auto const rehash = annotation::sha256(imported->asset.pngBytes);
        REQUIRE(rehash.has_value());
        CHECK(imported->spec.contentHash == *rehash);

        auto const decoded = image::decodePng(
            imported->asset.pngBytes,
            "reimported.png"
        );
        REQUIRE(decoded.has_value());
        CHECK(decoded->pixels == pixels);
    }

    TEST_CASE("importSourcePng rejects a file that is not a PNG")
    {
        auto const id   = annotation::test::sourceId(k_sourceId);
        auto const file = TemporaryFile{"png"};
        auto const junk = std::vector{
            asByte(0x4E),
            asByte(0x4F),
            asByte(0x50),
            asByte(0x45),
        };
        writeBytes(file.path(), junk);

        auto const imported = importSourcePng(id, file.path());
        REQUIRE_FALSE(imported.has_value());
    }

    TEST_CASE("ingestSourceFromFrame encodes BGRA into a WGC-provenance source")
    {
        auto const id         = annotation::test::sourceId(k_sourceId);
        auto const generation = TargetGeneration::fromValue(7);
        // BGRA channel order for the same pixels sampleRgba() carries in RGBA.
        auto const bgra = std::vector{
            asByte(30), asByte(20), asByte(10), asByte(255),
            asByte(60), asByte(50), asByte(40), asByte(255),
            asByte(90), asByte(80), asByte(70), asByte(255),
            asByte(120), asByte(110), asByte(100), asByte(255),
        };
        auto frame = bgraFrame(2, 2, std::size_t{8}, bgra, generation);

        auto const ingested = ingestSourceFromFrame(
            id,
            frame,
            120,
            "2026-07-23T00:00:00Z"
        );
        REQUIRE(ingested.has_value());

        CHECK(ingested->spec.fingerprint.width() == 2);
        CHECK(ingested->spec.fingerprint.height() == 2);
        // The captured window's density is stamped into the source fingerprint,
        // not defaulted to 96, so a high-DPI target authors against its real DPI.
        CHECK(ingested->spec.fingerprint.dpiX() == 120);
        CHECK(ingested->spec.fingerprint.dpiY() == 120);
        auto const* p_wgc = std::get_if<annotation::WgcSourceProvenance>(
            &ingested->spec.provenance
        );
        REQUIRE(p_wgc != nullptr);
        CHECK(p_wgc->targetGeneration == generation);
        CHECK(p_wgc->capturedAt == "2026-07-23T00:00:00Z");

        auto const rehash = annotation::sha256(ingested->asset.pngBytes);
        REQUIRE(rehash.has_value());
        CHECK(ingested->spec.contentHash == *rehash);

        // The encoded asset is the RGBA view of the captured BGRA pixels.
        auto const decoded = image::decodePng(
            ingested->asset.pngBytes,
            "captured.png"
        );
        REQUIRE(decoded.has_value());
        CHECK(decoded->pixels == sampleRgba());
    }

    TEST_CASE("ingestSourceFromFrame drops stride padding from captured rows")
    {
        auto const id         = annotation::test::sourceId(k_sourceId);
        auto const generation = TargetGeneration::fromValue(1);
        // Two rows of one BGRA pixel each, padded to a 6-byte stride.
        auto const padded = std::vector{
            asByte(30), asByte(20), asByte(10), asByte(255), asByte(0xEE), asByte(0xEE),
            asByte(90), asByte(80), asByte(70), asByte(255), asByte(0xEE), asByte(0xEE),
        };
        auto frame = bgraFrame(1, 2, std::size_t{6}, padded, generation);

        auto const ingested = ingestSourceFromFrame(id, frame, 96, "t");
        REQUIRE(ingested.has_value());
        auto const decoded = image::decodePng(
            ingested->asset.pngBytes,
            "padded.png"
        );
        REQUIRE(decoded.has_value());
        CHECK(decoded->width == 1);
        CHECK(decoded->height == 2);
        CHECK(
            decoded->pixels
            == std::vector{
                asByte(10), asByte(20), asByte(30), asByte(255),
                asByte(70), asByte(80), asByte(90), asByte(255),
            }
        );
    }

    TEST_CASE("ingestSourceFromFrame rejects a non-BGRA frame")
    {
        auto const id = annotation::test::sourceId(k_sourceId);
        auto const transform = CoordinateTransform::create(
            Point<DesktopSpace>{0.0F, 0.0F},
            1.0F,
            1.0F,
            1,
            1
        );
        REQUIRE(transform.has_value());
        auto const buffer = std::shared_ptr<FrameBuffer const>{
            std::make_shared<FrameBuffer>(std::vector{asByte(5)})
        };
        auto grayFrame = Frame::create(
            FrameId{1},
            CaptureSessionId{1},
            TargetGeneration::fromValue(1),
            MonotonicInstant::fromTimePoint(MonotonicInstant::TimePoint{}),
            1,
            1,
            std::size_t{1},
            PixelFormat::Gray8,
            buffer,
            *transform
        );
        REQUIRE(grayFrame.has_value());

        auto const ingested = ingestSourceFromFrame(id, *grayFrame, 96, "t");
        REQUIRE_FALSE(ingested.has_value());
        annotation::test::requireErrorKind(
            ingested.error(),
            AutomationErrorKind::InvalidResource
        );
    }
}
