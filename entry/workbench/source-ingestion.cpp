#include "source-ingestion.hpp"

#include <annotation/content-hash.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/space.hpp>

#include <image/pixels.hpp>
#include <image/png.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::workbench
{
    namespace
    {
        // A bare PNG import carries no display density, so it adopts the
        // conventional 96 DPI. A WGC capture supplies the real target density
        // instead (see ingestSourceFromFrame) so a project authored from a
        // high-DPI window matches that window's runtime fingerprint.
        constexpr auto k_defaultSourceDpi = uint32{96};

        constexpr auto k_sourceResourceName = std::string_view{
            "workbench-source.png"
        };

        [[nodiscard]]
        auto assembleSource(
            annotation::SourceId id,
            uint32 width,
            uint32 height,
            uint32 dpi,
            std::vector<std::byte> const& canonicalRgba,
            annotation::SourceProvenance provenance
        ) -> Result<IngestedSource>
        {
            UF_TRY_VALUE(
                fingerprint,
                annotation::ProjectFingerprint::create(
                    width,
                    height,
                    dpi,
                    dpi
                )
            );
            UF_TRY_VALUE(
                pngBytes,
                image::encodeRgbaPng(
                    k_sourceResourceName,
                    width,
                    height,
                    canonicalRgba
                )
            );
            UF_TRY_VALUE(contentHash, annotation::sha256(pngBytes));
            return IngestedSource{
                .spec = annotation::AuthoringSourceSpec{
                    .id          = id,
                    .contentHash = contentHash,
                    .fingerprint = fingerprint,
                    .provenance  = std::move(provenance),
                },
                .asset = annotation::AuthoringSourceAsset{
                    .id       = id,
                    .pngBytes = std::move(pngBytes),
                },
            };
        }
    }

    auto importSourcePng(
        annotation::SourceId id,
        std::filesystem::path const& path
    ) -> Result<IngestedSource>
    {
        UF_TRY_VALUE(decoded, image::loadPng(path));
        return assembleSource(
            id,
            decoded.width,
            decoded.height,
            k_defaultSourceDpi,
            decoded.pixels,
            annotation::ImportedSourceProvenance{}
        );
    }

    auto ingestSourceFromFrame(
        annotation::SourceId id,
        Frame const& frame,
        uint32 dpi,
        std::string capturedAt
    ) -> Result<IngestedSource>
    {
        if (frame.pixelFormat() != PixelFormat::Bgra8)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "workbench source capture requires a BGRA8 frame"
            );
        }
        UF_TRY_VALUE(
            rect,
            PixelRect::create(0, 0, frame.width(), frame.height())
        );
        UF_TRY_VALUE(
            packedBgra,
            image::cropBgra8(
                frame.pixels()->bytes(),
                frame.width(),
                frame.height(),
                frame.stride(),
                rect
            )
        );
        UF_TRY_VALUE(rgba, image::bgra8ToRgba8(std::move(packedBgra)));
        return assembleSource(
            id,
            frame.width(),
            frame.height(),
            dpi,
            rgba,
            annotation::WgcSourceProvenance{
                .targetGeneration = frame.targetGeneration(),
                .capturedAt       = std::move(capturedAt),
            }
        );
    }
}
