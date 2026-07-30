#pragma once

#include "text.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <optional>

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
