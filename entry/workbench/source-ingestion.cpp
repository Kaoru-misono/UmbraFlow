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
        // Bare PNG imports and Windows Graphics Capture frames do not carry a
        // display density, so an authoring-time source adopts the conventional
        // 96 DPI. The project fingerprint governs recognition; the source density
        // is reconciled when the source joins a project.
        constexpr auto g_defaultSourceDpi = uint32{96};

        constexpr auto g_sourceResourceName = std::string_view{
            "workbench-source.png"
        };

        [[nodiscard]]
        auto assembleSource(
            annotation::SourceId id,
            uint32 width,
            uint32 height,
            std::vector<std::byte> const& canonicalRgba,
            annotation::SourceProvenance provenance
        ) -> Result<IngestedSource>
        {
            UF_TRY_VALUE(
                fingerprint,
                annotation::ProjectFingerprint::create(
                    width,
                    height,
                    g_defaultSourceDpi,
                    g_defaultSourceDpi
                )
            );
            UF_TRY_VALUE(
                pngBytes,
                image::encodeRgbaPng(
                    g_sourceResourceName,
                    width,
                    height,
                    canonicalRgba
                )
            );
            UF_TRY_VALUE(contentHash, annotation::sha256(pngBytes));
            return IngestedSource{
                .m_spec = annotation::AuthoringSourceSpec{
                    .m_id          = id,
                    .m_contentHash = contentHash,
                    .m_fingerprint = fingerprint,
                    .m_provenance  = std::move(provenance),
                },
                .m_asset = annotation::AuthoringSourceAsset{
                    .m_id       = id,
                    .m_pngBytes = std::move(pngBytes),
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
            decoded.m_width,
            decoded.m_height,
            decoded.m_pixels,
            annotation::ImportedSourceProvenance{}
        );
    }

    auto ingestSourceFromFrame(
        annotation::SourceId id,
        Frame const& frame,
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
            rgba,
            annotation::WgcSourceProvenance{
                .m_targetGeneration = frame.targetGeneration(),
                .m_capturedAt       = std::move(capturedAt),
            }
        );
    }
}
