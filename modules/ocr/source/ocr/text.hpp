#pragma once

#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <string>
#include <vector>

namespace uf::ocr
{
    // One run of text a read produced, and where it sat.
    //
    // A line rather than a word or a character, because that is the unit the
    // recognition model actually scores: it consumes a strip of pixels and emits
    // a string, and a per-character box would be this layer inventing detail the
    // model does not report.
    struct TextLine final
    {
        std::string text{};

        // In the coordinate space of the image handed to read(), never relative
        // to the spec's rect. A caller that passed a rect gets boxes it can draw
        // on the frame it already has, without adding an origin back.
        //
        // No in-class initializer: PixelRect has no default state, so every
        // construction site states the bounds it found.
        PixelRect bounds;

        // The model's own confidence, in basis points, matching how this project
        // already spells a similarity threshold. It is the mean over the
        // characters the line decoded to, so a long line with one uncertain
        // glyph does not read as uncertain overall -- a caller that needs the
        // weakest glyph needs a different number, and this layer does not
        // pretend to be it.
        uint32 confidenceBp{};

        auto operator==(TextLine const&) const -> bool = default;
    };

    // Everything one read found.
    //
    // Ordering is top to bottom, then left to right, and it is a contract rather
    // than a convenience: this project forbids behaviour that depends on an
    // unordered traversal, and a caller joining lines into one string must get
    // the same string on every run over the same pixels.
    struct Readout final
    {
        std::vector<TextLine> lines{};

        auto operator==(Readout const&) const -> bool = default;
    };
}
