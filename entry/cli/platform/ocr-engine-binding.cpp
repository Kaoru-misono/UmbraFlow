#include "ocr-engine-binding.hpp"

#include <core/error/result.hpp>

#include <ocr/engine.hpp>
#include <ocr/onnx-engine.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace uf::cli::platform
{
    namespace
    {
        // The payload layout, and since the input agent retired the only place
        // it is spelled: a directory is not itself the model, it is the "models"
        // root the release lays the recognition model under, and the one this
        // binary's own build step stages into.
        constexpr auto k_recognitionDirectoryName = std::string_view{
            "ppocr-v6-small-rec"
        };
        constexpr auto k_recognitionModelName  = std::string_view{"inference.onnx"};
        constexpr auto k_recognitionConfigName = std::string_view{"inference.yml"};
    }

    auto bindOcrEngine(
        std::optional<std::filesystem::path> const& modelDirectory
    ) -> Result<std::unique_ptr<ocr::IOcrEngine>>
    {
        if (!modelDirectory.has_value())
        {
            return std::unique_ptr<ocr::IOcrEngine>{};
        }

        auto const recognition = *modelDirectory / k_recognitionDirectoryName;
        return ocr::createOnnxEngine(
            ocr::OnnxEngineConfig{
                .recognitionModel  = recognition / k_recognitionModelName,
                .recognitionConfig = recognition / k_recognitionConfigName,
            }
        );
    }
}
