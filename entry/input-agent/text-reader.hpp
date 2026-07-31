#pragma once

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <domain/frame.hpp>
#include <domain/space.hpp>
#include <ocr/text.hpp>
#include <vision/bgra-image.hpp>

#include <vector>

namespace uf::input_agent
{
    // A BGRA8 view over one captured frame's pixels, which is the form every
    // reader below wants and the form a Frame does not already have.
    //
    // The returned view borrows the frame's pixel buffer and never copies it, so
    // `frame` must outlive it: a read is a call-scoped operation over an
    // observation the caller is holding, and copying a 1600x900 plane to satisfy
    // a weaker contract would cost more than the read.
    [[nodiscard]]
    auto frameAsBgraImage(
        Frame const& frame UF_LIFETIME_BOUND
    ) -> Result<BgraImage>;

    // What one read attempt did.
    //
    // There is deliberately no `ok` member, on DriveOutcome's reasoning: whether
    // the read ran is the same fact as whether `lines` holds an error, and the
    // results line reads its own `ok` off this rather than off a flag someone
    // has to remember to set.
    struct TextReadOutcome final
    {
        // The lines read, or why they were not.
        //
        // A read that ran and found nothing holds an EMPTY VECTOR rather than an
        // error. That is the distinction this member exists for: a region with
        // no text is an ordinary answer about the screen, and folding it into
        // the failure side would make "the popup is not showing" and "the model
        // would not load" the same answer to an operator.
        //
        // No in-class initializer for the same reason: an empty success is one
        // of the two states this tells apart, so it must not also be the value a
        // construction site gets for saying nothing.
        Result<std::vector<ocr::TextLine>> lines;

        // Whether the failure was the reader itself rather than this one read.
        //
        // It is not a detail of the error, for the reason DriveOutcome's
        // targetReplaced is not: a refused rect is this one command's business
        // and the next read may well succeed, while a reader that cannot come up
        // answers every read in the run the same way. The operator responds to
        // the two differently -- by fixing the rectangle, or by fixing the
        // payload beside the binary -- so the line has to say which it got.
        //
        // It does NOT end the run. Every other verb is unaffected by a reader
        // that will not start, and stopping the agent would take capture and the
        // input verbs down with it.
        bool readerUnavailable{};
    };

    // Turning a rectangle of one observation into text.
    //
    // It sits behind a port for the reason IInputAgentDrive does: the live half
    // stands on 20 MB of model weights and an inference runtime beside the
    // binary, while every decision the layer above it makes around a read -- the
    // rect fence, the three answers an operator has to tell apart, the shape of
    // the results line -- can be exercised without either.
    //
    // NOT thread-safe: an implementation owns model sessions, and every call
    // runs on the thread that owns the run.
    class IInputAgentTextReader
    {
    public:
        IInputAgentTextReader() = default;

        IInputAgentTextReader(IInputAgentTextReader const&) = delete;
        IInputAgentTextReader(IInputAgentTextReader&&) = delete;
        auto operator=(IInputAgentTextReader const&)
            -> IInputAgentTextReader& = delete;
        auto operator=(IInputAgentTextReader&&)
            -> IInputAgentTextReader& = delete;

        virtual ~IInputAgentTextReader() = default;

        // Reads the one line of text `rect` holds on `observation`.
        //
        // One line, because that is what the recognition model scores and what
        // an author measuring a label is asking about. A rect that in fact holds
        // several lines reads as one run of nonsense rather than failing, which
        // is the engine's own documented behaviour and not something this layer
        // can detect.
        //
        // `rect` is in the frame's own pixel space -- what an author measures on
        // a capture PNG -- and NOT the client space the pointer verbs take. The
        // two differ by exactly what `capture` reports as its `delta`.
        //
        // `observation` is borrowed for the call only; an implementation must
        // not retain it.
        [[nodiscard]]
        virtual auto read(
            Frame const& observation,
            PixelRect rect
        ) -> TextReadOutcome = 0;
    };
}
