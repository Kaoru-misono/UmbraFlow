#pragma once

#include "text.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <optional>
#include <string_view>

namespace uf
{
    class BgraImage;
}

namespace uf::ocr
{
    // How much of the pipeline a read has to run.
    //
    // This is the one knob that changes what a read COSTS, which is why it is a
    // parameter rather than something the engine infers. Measured on a real
    // 1600x900 frame: recognising one already-located line costs single-digit
    // milliseconds, and locating lines is the expensive stage.
    enum class TextLayout : uint8
    {
        // The rect holds exactly one line, and the caller is asserting it.
        // Detection is skipped entirely: the caller has already said where the
        // text is, which is both cheaper and immune to a detector that splits
        // one label in two or merges a label with the one under it.
        //
        // A rect that in fact holds several lines reads as one run of nonsense
        // rather than failing, because nothing here can tell that apart from a
        // font it has not seen. Use Block when the count is not known.
        SingleLine = 1,

        // The rect may hold any number of lines anywhere in it. Runs detection
        // first, then recognises each box it found.
        //
        // Every line comes back with its own rectangle in the coordinate space
        // of the image, never of the rect: a caller that had to add the origin
        // back would eventually forget, and a click one origin away from the
        // text it was aimed at fails nothing on the way out.
        Block,
    };

    // What to read, and how hard to look.
    struct ReadSpec final
    {
        // The region of the image to read. Absent reads the whole image, which
        // is the already-cropped-UI-block case; the two are the same operation
        // and deliberately not two verbs.
        std::optional<PixelRect> rect{};

        TextLayout layout{TextLayout::Block};

        // The most lines a Block read may recognise, or absent for no ceiling
        // at all.
        //
        // IT IS A BUDGET THE CALLER OWNS, which is why it is an argument rather
        // than a constant here. Detection is one inference over the region and
        // recognition is one MORE inference per line it located, so the price of
        // a block read is set by what the screen happens to hold -- and the
        // layer that has to keep an observation inside its lease is the only one
        // that knows how many of those it can afford.
        //
        // Exceeding it FAILS, and never returns the first n lines. A caller that
        // was handed part of a region would conclude "the name I want is not
        // here" from a region nobody finished looking at, which is the fail-open
        // answer this project refuses everywhere else a search can stop early.
        // The refusal happens after detection and before any recognition, so an
        // over-full region costs one inference rather than one per line.
        std::optional<uint32> maximumLines{};
    };

    // Turns pixels into text.
    //
    // A port rather than a concrete class for the two reasons this project
    // already makes ports: a test must be able to stand in for it without
    // loading 30 MB of model weights, and the engine behind it is a decision
    // that was measured rather than settled forever. Nothing in this header
    // names an inference type, so an adapter can be swapped without touching a
    // caller.
    //
    // NOT thread-safe: an implementation owns model sessions and every call runs
    // on the owning thread.
    class IOcrEngine
    {
    public:
        IOcrEngine() = default;

        IOcrEngine(IOcrEngine const&) = delete;
        IOcrEngine(IOcrEngine&&) = delete;
        auto operator=(IOcrEngine const&) -> IOcrEngine& = delete;
        auto operator=(IOcrEngine&&) -> IOcrEngine& = delete;

        virtual ~IOcrEngine() = default;

        // What produced the text: the runtime and the model together, in one
        // stable string.
        //
        // It is on the port rather than left to the caller because only the
        // adapter knows which weights it loaded, and a trace line that does not
        // name them cannot be replayed once the model changes -- the same pixels
        // decode differently and nothing else in the stream would say why. The
        // view is valid for the engine's lifetime, which is the only borrow an
        // engine hands out.
        [[nodiscard]]
        virtual auto identity() const noexcept UF_LIFETIME_BOUND -> std::string_view = 0;

        // Reads `spec`'s region of `image`.
        //
        // Finding no text is a Readout with no lines, not a failure: an empty
        // region is an ordinary answer about the screen, and reporting it as an
        // error would make every caller unwrap a result to learn that a popup
        // was not showing. A failure here means the read could not be attempted
        // -- a rect outside the image, a model that would not load.
        //
        // `image` is borrowed for the call only. An implementation must not
        // retain it: BgraImage is a view over storage this engine does not own.
        [[nodiscard]]
        virtual auto read(
            BgraImage const& image,
            ReadSpec const& spec
        ) -> Result<Readout> = 0;
    };
}
