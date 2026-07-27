#include "../annotation/test-helpers.hpp"

#include <project-persistence.hpp>

#include <annotation/authoring-document.hpp>
#include <annotation/content-hash.hpp>

#include <core/types/integer.hpp>

#include <image/png.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <span>
#include <system_error>
#include <vector>

// The AddressSanitizer smoke test (registered by tests/CMakeLists.txt) launches
// the real umbra-workbench GUI with `--smoke N`, which loads a project from disk
// and draws every panel for N frames under ASan. That needs a *valid* project
// tree on disk: annotations.toml plus content-hash-matched source PNGs. Those
// files cannot be hand-authored (the loader verifies each SHA-256 and the
// decoded PNG dimensions), and committing them as binaries would silently rot if
// the authoring TOML format changed. So the fixture is generated here, from the
// same library the workbench uses, into the build tree just before the smoke
// test runs. UF_ASAN_SMOKE_FIXTURE_ROOT names that directory.
#ifndef UF_ASAN_SMOKE_FIXTURE_ROOT
    #define UF_ASAN_SMOKE_FIXTURE_ROOT ""
#endif

namespace uf::workbench
{
    namespace
    {
        [[nodiscard]]
        constexpr auto asByte(uint8 value) noexcept -> std::byte
        {
            return static_cast<std::byte>(value);
        }

        [[nodiscard]]
        auto encodedSource() -> std::vector<std::byte>
        {
            auto const pixels = std::vector{
                asByte(1),  asByte(2),  asByte(3),  asByte(255),
                asByte(4),  asByte(5),  asByte(6),  asByte(255),
                asByte(7),  asByte(8),  asByte(9),  asByte(255),
                asByte(10), asByte(11), asByte(12), asByte(255),
            };
            auto encoded = image::encodeRgbaPng(
                "asan-smoke-source.png",
                2,
                2,
                pixels
            );
            REQUIRE(encoded.has_value());
            return *std::move(encoded);
        }
    }

    // Skipped in ordinary runs; the ASan smoke fixture step selects it by name
    // with --no-skip to (re)generate the project tree.
    TEST_CASE("workbench emits the AddressSanitizer smoke fixture" * doctest::skip())
    {
        auto const root = std::filesystem::path{UF_ASAN_SMOKE_FIXTURE_ROOT};
        REQUIRE_FALSE(root.empty());

        auto removeError = std::error_code{};
        std::filesystem::remove_all(root, removeError);
        REQUIRE_FALSE(removeError);

        auto const fingerprint = annotation::test::fingerprint(2, 2, 96, 96);
        auto const sourceId    = annotation::test::sourceId(
            "00000000-0000-0000-0000-000000000201"
        );
        auto const anchorId = annotation::test::recognizerId(
            "00000000-0000-0000-0000-000000000001"
        );
        auto const pageId = annotation::test::pageId(
            "00000000-0000-0000-0000-000000000101"
        );

        auto pngBytes         = encodedSource();
        auto const sourceHash = annotation::sha256(pngBytes);
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

        auto const asset = annotation::AuthoringSourceAsset{
            .id       = sourceId,
            .pngBytes = std::move(pngBytes),
        };
        auto const assets = std::span{&asset, std::size_t{1}};

        auto const saved = saveAndGenerateAuthoringProject(
            root,
            *document,
            assets
        );
        auto const savedInfo = saved
            ? std::string{"saved"}
            : toString(saved.error());
        INFO(savedInfo);
        REQUIRE(saved.has_value());
    }
}
