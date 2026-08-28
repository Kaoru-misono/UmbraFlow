#pragma once

#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <string>
#include <vector>

namespace uf::ocr
{
    // One emitted CTC dictionary token (or the appended space class), with the
    // selected timestep's probability rounded to basis points. Unicode tokens
    // are retained whole; this is not a UTF-8 byte and carries no invented box.
    struct TextCharacter final
    {
        std::string text{};
        uint32      confidenceBp{};

        auto operator==(TextCharacter const&) const -> bool = default;
    };

    // One run of text a read produced, and where it sat. The model consumes a
    // strip and reports character scores but no individual character boxes.
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
        // raw emitted character probabilities, rounded only after averaging.
        // It is not an average of the already rounded character confidences.
        uint32 confidenceBp{};

        // In emission order after blank removal and repeat collapse. Joining
        // these tokens reproduces text exactly, including emitted spaces.
        std::vector<TextCharacter> characters{};

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
