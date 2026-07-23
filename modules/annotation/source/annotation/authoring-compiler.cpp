#include "authoring-compiler.hpp"

#include <core/numeric/checked-arithmetic.hpp>
#include <core/safety/checked-access.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <image/pixels.hpp>
#include <image/png.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace uf::annotation
{
    namespace
    {
        constexpr auto g_rgbaBytesPerPixel            = std::size_t{4};
        constexpr auto g_maximumCompiledTemplateBytes = std::size_t{512} * 1024U * 1024U;

        struct RecognizerWork final
        {
            SourceId    m_sourceId;
            std::size_t m_recognizerIndex{};
        };

        [[nodiscard]]
        auto invalidCompilation(std::string message) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::move(message)
            );
        }

    }

    auto compileAuthoringDocument(
        AuthoringDocument const& document,
        std::span<AuthoringSourceAsset const> sourceAssets
    ) -> Result<CompiledAuthoringProject>
    {
        auto const sources = document.sources();
        if (sourceAssets.size() != sources.size())
        {
            return invalidCompilation(
                "authoring compilation requires exactly one asset per source record"
            );
        }

        auto assetOrder = std::vector<std::size_t>{};
        assetOrder.reserve(sourceAssets.size());
        for (auto index = std::size_t{0}; index < sourceAssets.size(); ++index)
        {
            assetOrder.emplace_back(index);
        }
        std::ranges::sort(
            assetOrder,
            {},
            [&sourceAssets](std::size_t index) -> ResourceId
            {
                return checkedAt(sourceAssets, index).m_id.value();
            }
        );
        for (auto index = std::size_t{0}; index < sources.size(); ++index)
        {
            auto const& source    = checkedAt(sources, index);
            auto const assetIndex = checkedAt(assetOrder, index);
            auto const& asset     = checkedAt(sourceAssets, assetIndex);
            if (asset.m_id != source.id())
            {
                return invalidCompilation(
                    "authoring compilation requires one distinct matching asset per source"
                );
            }
        }

        auto const recognizers       = document.catalog().recognizers();
        auto const recognizerSources = document.recognizerSources();
        UF_CHECK_MSG(
            recognizers.size() == recognizerSources.size(),
            "authoring recognizer source closure is inconsistent"
        );
        auto recognizerWork = std::vector<RecognizerWork>{};
        recognizerWork.reserve(recognizers.size());
        for (auto index = std::size_t{0}; index < recognizers.size(); ++index)
        {
            auto const& recognizer   = checkedAt(recognizers, index);
            auto const& relationship = checkedAt(recognizerSources, index);
            UF_CHECK_MSG(
                relationship.m_recognizerId == recognizer.id(),
                "authoring recognizer source order is inconsistent"
            );
            recognizerWork.emplace_back(
                RecognizerWork{
                    .m_sourceId        = relationship.m_sourceId,
                    .m_recognizerIndex = index,
                }
            );
        }
        std::ranges::sort(
            recognizerWork,
            [](RecognizerWork const& left, RecognizerWork const& right) noexcept
            {
                if (left.m_sourceId != right.m_sourceId)
                {
                    return left.m_sourceId.value() < right.m_sourceId.value();
                }
                return left.m_recognizerIndex < right.m_recognizerIndex;
            }
        );

        auto runtimeRecognizers = std::vector<RuntimeRecognizerSpec>{};
        auto templateAssets     = std::vector<TemplateAsset>{};
        runtimeRecognizers.reserve(document.catalog().recognizers().size());
        templateAssets.reserve(document.catalog().recognizers().size());
        auto compiledTemplateBytes = std::size_t{0};
        auto workIndex             = std::size_t{0};
        for (auto sourceIndex = std::size_t{0}; sourceIndex < sources.size(); ++sourceIndex)
        {
            auto const& source      = checkedAt(sources, sourceIndex);
            auto const assetIndex   = checkedAt(assetOrder, sourceIndex);
            auto const& sourceAsset = checkedAt(sourceAssets, assetIndex);
            if (sourceAsset.m_pngBytes.size() > image::g_maximumPngFileBytes)
            {
                return invalidCompilation(
                    std::format(
                        "authoring source {} exceeds the PNG byte quota",
                        source.relativePath()
                    )
                );
            }
            UF_TRY_VALUE_CONTEXT(
                actualHash,
                sha256(sourceAsset.m_pngBytes),
                std::format("hashing authoring source {}", source.relativePath())
            );
            if (actualHash != source.contentHash())
            {
                return invalidCompilation(
                    std::format(
                        "authoring source {} content hash does not match its document record",
                        source.relativePath()
                    )
                );
            }
            UF_TRY_VALUE_CONTEXT(
                decoded,
                image::decodePng(
                    sourceAsset.m_pngBytes,
                    source.relativePath()
                ),
                std::format("decoding authoring source {}", source.relativePath())
            );
            auto const fingerprint = source.fingerprint();
            if (
                decoded.m_width != fingerprint.width()
                || decoded.m_height != fingerprint.height()
            )
            {
                return invalidCompilation(
                    std::format(
                        "authoring source {} decoded as {}x{}, expected {}x{}",
                        source.relativePath(),
                        decoded.m_width,
                        decoded.m_height,
                        fingerprint.width(),
                        fingerprint.height()
                    )
                );
            }
            auto const stride = checkedMultiply(
                static_cast<std::size_t>(decoded.m_width),
                g_rgbaBytesPerPixel
            );
            if (!stride)
            {
                return invalidCompilation(
                    "authoring source stride overflowed addressable memory"
                );
            }
            UF_TRY_VALUE_CONTEXT(
                bgraPixels,
                image::rgba8ToBgra8(std::move(decoded.m_pixels)),
                std::format("converting authoring source {}", source.relativePath())
            );
            while (workIndex < recognizerWork.size())
            {
                auto const& work = checkedAt(recognizerWork, workIndex);
                if (work.m_sourceId != source.id())
                {
                    break;
                }
                auto const& recognizer = checkedAt(
                    recognizers,
                    work.m_recognizerIndex
                );
                UF_TRY_VALUE_CONTEXT(
                    generated,
                    generateTemplateAsset(
                        bgraPixels,
                        decoded.m_width,
                        decoded.m_height,
                        *stride,
                        recognizer.templateRect()
                    ),
                    std::format(
                        "generating template for recognizer {}",
                        recognizer.name().value()
                    )
                );
                if (generated.m_pngBytes.size() > image::g_maximumPngFileBytes)
                {
                    return invalidCompilation(
                        std::format(
                            "generated template for recognizer {} exceeds the PNG byte quota",
                            recognizer.name().value()
                        )
                    );
                }
                runtimeRecognizers.emplace_back(
                    RuntimeRecognizerSpec{
                        .m_definition   = recognizer,
                        .m_templateHash = generated.m_hash,
                        .m_sourceHash   = source.contentHash(),
                    }
                );

                auto const existing = std::ranges::find(
                    templateAssets,
                    generated.m_hash,
                    &TemplateAsset::m_hash
                );
                if (existing == templateAssets.end())
                {
                    auto const nextTotal = checkedAdd(
                        compiledTemplateBytes,
                        generated.m_pngBytes.size()
                    );
                    if (
                        !nextTotal
                        || *nextTotal > g_maximumCompiledTemplateBytes
                    )
                    {
                        return invalidCompilation(
                            "compiled templates exceed the 512 MiB project quota"
                        );
                    }
                    compiledTemplateBytes = *nextTotal;
                    templateAssets.emplace_back(std::move(generated));
                }
                else if (existing->m_pngBytes != generated.m_pngBytes)
                {
                    return invalidCompilation(
                        "distinct template bytes produced the same content hash"
                    );
                }
                ++workIndex;
            }
        }
        UF_CHECK_MSG(
            workIndex == recognizerWork.size(),
            "authoring recognizer source closure references an unknown source"
        );
        std::ranges::sort(
            templateAssets,
            {},
            &TemplateAsset::m_relativePath
        );

        auto pages = std::vector<PageSignature>{
            document.catalog().pages().begin(),
            document.catalog().pages().end()
        };
        UF_TRY_VALUE_CONTEXT(
            runtimeManifest,
            RuntimeManifest::create(
                document.catalog().projectId(),
                document.catalog().fingerprint(),
                std::move(runtimeRecognizers),
                std::move(pages)
            ),
            "compiling the annotation runtime manifest"
        );
        auto runtimeManifestToml = serializeRuntimeManifest(runtimeManifest);
        return CompiledAuthoringProject{
            .m_runtimeManifest     = std::move(runtimeManifest),
            .m_runtimeManifestToml = std::move(runtimeManifestToml),
            .m_templateAssets      = std::move(templateAssets),
        };
    }
}
