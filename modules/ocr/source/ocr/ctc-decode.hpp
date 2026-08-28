#pragma once

#include "text.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace uf::ocr::detail
{
    struct DecodedLine final
    {
        std::string                text{};
        uint32                     confidenceBp{};
        std::vector<TextCharacter> characters{};
    };

    // Internal model postprocessing, separated from ONNX so its emission and
    // score semantics can be tested without inference. scores is the owned
    // tensor's call-scoped row-major [timesteps, classes] view; the adapter
    // validates tensor rank and element type before constructing that view.
    // This function checks the view's dimensions and emitted probabilities.
    // Class 0 is blank, dictionary entry i is class i+1, and any class past the
    // dictionary emits the space the recognition model appends. Empty steps or
    // blank-only paths return empty text/characters and zero line confidence.
    [[nodiscard]]
    auto decodeCtc(
        std::span<float const> scores,
        std::size_t timesteps,
        std::size_t classes,
        std::span<std::string const> dictionary
    ) -> Result<DecodedLine>;
}
