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
        // it is spelled: a directory is not itself a model, it is the "models"
        // root the release lays both models under, and the one this binary's own
        // build step stages into.
        constexpr auto k_recognitionDirectoryName = std::string_view{
            "ppocr-v6-small-rec"
        };
        constexpr auto k_detectionDirectoryName = std::string_view{
            "ppocr-v6-small-det"
        };

        // Both models in the release are files called inference.onnx, which is
        // why the directory above is what tells them apart and why these two
        // names are not per-model.
        constexpr auto k_modelFileName  = std::string_view{"inference.onnx"};
        constexpr auto k_configFileName = std::string_view{"inference.yml"};
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
        auto const detection   = *modelDirectory / k_detectionDirectoryName;
        return ocr::createOnnxEngine(
            ocr::OnnxEngineConfig{
                .recognitionModel  = recognition / k_modelFileName,
                .recognitionConfig = recognition / k_configFileName,
                // Named unconditionally rather than only when the file happens
                // to be there. An operator who passed --ocr-models asked for
                // text reads, and a models directory missing half the payload
                // must say so at startup on the same terms a missing
                // recognition model does -- not on the first block read, three
                // minutes into a run. createOnnxEngine refuses a path that is
                // not a file; an engine only ever silently lacks detection when
                // the caller names no path at all.
                .detectionModel = detection / k_modelFileName,
            }
        );
    }
}
