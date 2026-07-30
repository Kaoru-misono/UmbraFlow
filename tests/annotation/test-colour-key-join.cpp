#include "test-helpers.hpp"

// The colour-key crops live with the workbench tests because the authoring end
// measured them first. They are real screen pixels rather than workbench
// vocabulary, and duplicating four kilobytes of PNG to give this test its own
// copy would let the two ends drift apart on the very images that are supposed
// to keep them honest.
#include "../workbench/colour-key-fixture.hpp"

#include <annotation/authoring-compiler.hpp>
#include <annotation/authoring-document.hpp>
#include <annotation/content-hash.hpp>
#include <annotation/recognition-runtime.hpp>
#include <annotation/resource.hpp>

#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/safety/checked-access.hpp>
#include <core/types/integer.hpp>

#include <domain/frame.hpp>
#include <domain/space.hpp>

#include <image/pixels.hpp>
#include <image/png.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace uf::annotation
{
    namespace
    {
        namespace fixture = workbench::colour_key_fixture;

        constexpr auto k_sourceId      = "00000000-0000-0000-0000-000000000701";
        constexpr auto k_keyedId       = "00000000-0000-0000-0000-000000000702";
        constexpr auto k_unkeyedId     = "00000000-0000-0000-0000-000000000703";
        constexpr auto k_keyedPageId   = "00000000-0000-0000-0000-000000000711";
        constexpr auto k_unkeyedPageId = "00000000-0000-0000-0000-000000000712";

        // The tolerance the measurement in colour-key-fixture.hpp was taken at.
        constexpr auto k_tolerance = uint32{12};

        // Both crops are the whole authored screen here, so the template is the
        // whole screen too and the search has exactly one candidate position.
        // That keeps the reported score a measurement of the two images rather
        // than of wherever the search happened to land.
        [[nodiscard]]
        auto wholeCrop() -> PixelRect
        {
            return test::pixelRect(0, 0, fixture::k_width, fixture::k_height);
        }

        [[nodiscard]]
        auto whiteTextKey() -> ColourKey
        {
            auto const key = ColourKey::create(
                fixture::k_textRed,
                fixture::k_textGreen,
                fixture::k_textBlue,
                k_tolerance
            );
            REQUIRE(key.has_value());
            return *key;
        }

        [[nodiscard]]
        auto anchorOver(
            ProjectFingerprint fingerprint,
            RecognizerId id,
            std::string name,
            SourceId sourceId,
            std::optional<ColourKey> colourKey
        ) -> Element
        {
            auto result = Element::create(
                fingerprint,
                Element::Spec{
                    .id           = id,
                    .name         = test::resourceName(std::move(name)),
                    .sourceId     = sourceId,
                    .templateRect = wholeCrop(),
                    .searchRoi    = wholeCrop(),
                    .threshold    = test::threshold(),
                    .colourKey    = colourKey,
                    .kind         = AnchorElement{},
                    .shared       = false,
                }
            );
            REQUIRE(result.has_value());
            return *std::move(result);
        }

        struct AuthoredProject final
        {
            AuthoringDocument    document;
            AuthoringSourceAsset sourceAsset;
            ProjectFingerprint   fingerprint;
        };

        // One authored screen carrying the same rectangle twice: once keyed to
        // the white menu text, once unkeyed. Each is the sole anchor of its own
        // page, so one page evaluation reports both.
        [[nodiscard]]
        auto authoredProject() -> AuthoredProject
        {
            auto const fingerprint = test::fingerprint(
                fixture::k_width,
                fixture::k_height,
                96,
                96
            );
            auto const sourceId = test::sourceId(k_sourceId);
            auto pngBytes       = fixture::pngBytes(fixture::k_menuOverBlueArtwork);
            auto const hash     = sha256(pngBytes);
            REQUIRE(hash.has_value());
            auto source = AuthoringSource::create(
                AuthoringSourceSpec{
                    .id          = sourceId,
                    .contentHash = *hash,
                    .fingerprint = fingerprint,
                    .provenance  = ImportedSourceProvenance{},
                }
            );
            REQUIRE(source.has_value());

            auto const keyedId   = test::recognizerId(k_keyedId);
            auto const unkeyedId = test::recognizerId(k_unkeyedId);
            auto elements        = std::vector<Element>{};
            elements.emplace_back(
                anchorOver(
                    fingerprint,
                    keyedId,
                    "keyed_menu",
                    sourceId,
                    whiteTextKey()
                )
            );
            elements.emplace_back(
                anchorOver(
                    fingerprint,
                    unkeyedId,
                    "unkeyed_menu",
                    sourceId,
                    std::nullopt
                )
            );

            auto document = AuthoringDocument::create(
                test::projectId("personal.colour_key_join"),
                fingerprint,
                {*source},
                std::move(elements),
                {
                    test::page(test::pageId(k_keyedPageId), "keyed", {keyedId}),
                    test::page(test::pageId(k_unkeyedPageId), "unkeyed", {unkeyedId}),
                },
                {},
                {}
            );
            REQUIRE(document.has_value());
            return AuthoredProject{
                .document    = *std::move(document),
                .sourceAsset = AuthoringSourceAsset{
                    .id       = sourceId,
                    .pngBytes = std::move(pngBytes),
                },
                .fingerprint = fingerprint,
            };
        }

        // The other capture of the same rectangle, as the frame the runtime is
        // handed. Same geometry, different artwork behind the same glyphs.
        [[nodiscard]]
        auto frameOverOtherArtwork(ProjectFingerprint fingerprint) -> Frame
        {
            auto decoded = image::decodePng(
                fixture::pngBytes(fixture::k_menuOverPurpleArtwork),
                "colour-key-join-frame.png"
            );
            REQUIRE(decoded.has_value());
            REQUIRE(decoded->width == fixture::k_width);
            REQUIRE(decoded->height == fixture::k_height);
            auto bgra = image::rgba8ToBgra8(std::move(decoded->pixels));
            REQUIRE(bgra.has_value());

            auto const transform = CoordinateTransform::create(
                Point<DesktopSpace>{0.0F, 0.0F},
                static_cast<float>(fingerprint.width()),
                static_cast<float>(fingerprint.height()),
                fingerprint.width(),
                fingerprint.height()
            );
            REQUIRE(transform.has_value());
            auto const width = checkedCast<std::size_t>(fingerprint.width());
            REQUIRE(width.has_value());
            auto const stride = checkedMultiply(
                width.value_or(std::size_t{0}),
                bytesPerPixel(PixelFormat::Bgra8)
            );
            REQUIRE(stride.has_value());
            auto const buffer = std::shared_ptr<FrameBuffer const>{
                std::make_shared<FrameBuffer>(*std::move(bgra))
            };
            auto frame = Frame::create(
                FrameId{29},
                CaptureSessionId{11},
                TargetGeneration::fromValue(1),
                MonotonicInstant::fromTimePoint(MonotonicInstant::TimePoint{}),
                fingerprint.width(),
                fingerprint.height(),
                stride.value_or(std::size_t{0}),
                PixelFormat::Bgra8,
                buffer,
                *transform
            );
            REQUIRE(frame.has_value());
            return *std::move(frame);
        }

        // How much of the emitted template the baked alpha actually keeps. A
        // mask that selected almost nothing would also score near zero, so the
        // match on its own does not prove the key picked out the glyphs.
        struct MaskCoverage final
        {
            std::size_t weighted{};
            std::size_t full{};
        };

        [[nodiscard]]
        auto maskCoverage(std::span<std::byte const> pngBytes) -> MaskCoverage
        {
            auto const decoded = image::decodePng(
                pngBytes,
                "colour-key-join-template.png"
            );
            REQUIRE(decoded.has_value());
            auto coverage = MaskCoverage{};
            for (
                auto index = std::size_t{3};
                index < decoded->pixels.size();
                index += 4U
            )
            {
                auto const alpha = std::to_integer<uint8>(
                    checkedAt(decoded->pixels, index)
                );
                coverage.weighted += (alpha > 0 ? 1U : 0U);
                coverage.full += (alpha == 255 ? 1U : 0U);
            }
            return coverage;
        }

        struct JoinOutcome final
        {
            std::vector<AnchorEvidence> evidence{};
            std::size_t                 templateAssetCount{};
            bool                        templateHashesDiffer{};
            MaskCoverage                keyedMask{};
        };

        // Authoring to matcher with nothing hand-built in between: compile the
        // document, hand the emitted PNGs straight to the runtime, and match the
        // other capture.
        [[nodiscard]]
        auto compileAndMatch() -> JoinOutcome
        {
            auto const project = authoredProject();
            auto const assets  = std::span{&project.sourceAsset, std::size_t{1}};
            auto compiled      = compileAuthoringDocument(project.document, assets);
            REQUIRE(compiled.has_value());

            auto const* p_keyed = compiled->runtimeManifest.findAsset(
                test::recognizerId(k_keyedId)
            );
            auto const* p_unkeyed = compiled->runtimeManifest.findAsset(
                test::recognizerId(k_unkeyedId)
            );
            REQUIRE(p_keyed != nullptr);
            REQUIRE(p_unkeyed != nullptr);
            auto const hashesDiffer = p_keyed->templateHash != p_unkeyed->templateHash;

            auto const keyedAsset = std::ranges::find(
                compiled->templateAssets,
                p_keyed->templateHash,
                &TemplateAsset::hash
            );
            REQUIRE(keyedAsset != compiled->templateAssets.end());
            auto const keyedMask = maskCoverage(keyedAsset->pngBytes);

            auto encodedTemplates = std::vector<EncodedRuntimeTemplate>{};
            encodedTemplates.reserve(compiled->templateAssets.size());
            for (auto& asset : compiled->templateAssets)
            {
                encodedTemplates.emplace_back(
                    EncodedRuntimeTemplate{
                        .hash     = asset.hash,
                        .pngBytes = std::move(asset.pngBytes),
                    }
                );
            }
            auto const assetCount = encodedTemplates.size();

            auto runtime = RecognitionRuntime::create(
                std::move(compiled->runtimeManifest),
                std::move(encodedTemplates)
            );
            REQUIRE(runtime.has_value());

            auto const frame   = frameOverOtherArtwork(project.fingerprint);
            auto const attempt = runtime->evaluatePage(
                frame,
                project.fingerprint,
                RecognitionPolicy{.maximumPixelComparisons = 1'000'000}
            );
            REQUIRE(attempt.has_value());
            return JoinOutcome{
                .evidence             = attempt->completedAnchorEvidence,
                .templateAssetCount   = assetCount,
                .templateHashesDiffer = hashesDiffer,
                .keyedMask            = keyedMask,
            };
        }

        [[nodiscard]]
        auto evidenceFor(
            std::span<AnchorEvidence const> evidence,
            RecognizerId id
        ) -> AnchorEvidence
        {
            auto const found = std::ranges::find(
                evidence,
                id,
                &AnchorEvidence::recognizerId
            );
            REQUIRE(found != evidence.end());
            return *found;
        }

        // The matcher's score is a sum over the template rectangle; per pixel it
        // is the mean absolute grey difference the two captures show through the
        // mask, on the same 0..255 scale the authored threshold speaks.
        [[nodiscard]]
        auto meanAbsoluteDifference(AnchorEvidence const& evidence) -> double
        {
            REQUIRE(evidence.sadScore().has_value());
            auto const pixels = static_cast<double>(fixture::k_width)
                * static_cast<double>(fixture::k_height);
            return static_cast<double>(evidence.sadScore().value_or(0)) / pixels;
        }
    }

    TEST_CASE("annotation colour key authored into a template masks the runtime match")
    {
        auto const outcome = compileAndMatch();

        // A keyed and an unkeyed template of the same rectangle are two assets,
        // and each recognizer points at its own.
        CHECK(outcome.templateAssetCount == 2);
        CHECK(outcome.templateHashesDiffer);

        auto const keyed = evidenceFor(
            outcome.evidence,
            test::recognizerId(k_keyedId)
        );
        auto const unkeyed = evidenceFor(
            outcome.evidence,
            test::recognizerId(k_unkeyedId)
        );

        // The whole point: the artwork under the glyphs changed completely, and
        // only the keyed template still matches.
        CHECK(keyed.hit());
        CHECK_FALSE(unkeyed.hit());
        CHECK(keyed.matchedRect() == wholeCrop());

        // The mask the compiler baked keeps 360 of the rectangle's 4000 pixels,
        // 328 of them within the tolerance and 32 on the ramp beyond it. That
        // is a glyph-shaped ninth of a rectangle the fixture measures as mostly
        // artwork, and it is what stops the score below being vacuous: a mask
        // that selected almost nothing would score near zero too.
        CHECK(outcome.keyedMask.weighted == 360);
        CHECK(outcome.keyedMask.full == 328);

        // Measured on these two captures. Per pixel of the template rectangle
        // the score is the mean absolute grey difference the mask lets through,
        // on the same 0..255 scale the authored 9000 basis point threshold
        // reads as 25.5. The keyed template compares glyph strokes that are
        // near identical across the two backgrounds; the unkeyed one compares
        // the artwork that changed.
        CHECK(meanAbsoluteDifference(keyed) == doctest::Approx(0.0455));
        CHECK(meanAbsoluteDifference(unkeyed) == doctest::Approx(56.979));
    }
}
