#pragma once

// The product CLI's own argument header. It is included for the one number both
// tools have to agree on -- the per-recognition pixel comparison ceiling -- so a
// project that runs under `umbra-flow run` is verified here under the same
// budget rather than under a second copy of it that can drift.
#include <args.hpp>

#include <annotation/authoring-document.hpp>
#include <annotation/capabilities.hpp>
#include <annotation/resource.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uf::authoring
{
    // The density a project is authored at unless --dpi says otherwise. It has
    // to be a flag: a screenshot of a 144-dpi window describes a 144-dpi
    // project, AuthoringDocument refuses a source whose fingerprint differs
    // from the project's, and the runtime refuses to deliver a click when the
    // live fingerprint differs from the catalog's. Pinning it at 96 would leave
    // every high-DPI target unauthorable through this tool.
    inline constexpr auto k_defaultSourceDpi = uint32{96};

    // The similarity an element is authored at unless --min-similarity-bp says
    // otherwise. The matcher reads 9000 basis points as a mean absolute grey
    // difference of 25.5 per template pixel.
    inline constexpr auto k_defaultSimilarityBasisPoints = uint32{9'000};

    // How far a pixel may sit from a colour key and still count fully, unless
    // --tolerance says otherwise. Measured on real menu text: 12 takes 93.9% of
    // a glyph and leaves its antialiased rim to the compiler's weight ramp.
    inline constexpr auto k_defaultColourTolerance = uint32{12};

    // How many dominant colours `frames census` reports unless --top says
    // otherwise. A UI element is drawn in a handful of colours over whatever
    // artwork it sits on, so eight is past the interesting ones without
    // printing a histogram of the background.
    inline constexpr auto k_defaultCensusEntries = uint32{8};

    // One rectangle an author measured, and everything that decides what the
    // matcher does with its pixels. Shared by every subcommand that draws one,
    // because a page's first anchor is drawn exactly like its later members.
    //
    // `source` is what --source named and is deliberately still text here: it is
    // either a content hash of a screen the project already holds or a path to a
    // PNG to ingest, and only the loaded document knows which hashes exist.
    struct ElementDraw final
    {
        std::string name{};
        std::string source{};

        PixelRect                            templateRect;
        std::optional<PixelRect>             searchRoi{};
        std::optional<annotation::ColourKey> colourKey{};

        annotation::SimilarityThreshold threshold;
    };

    // What one drawn rectangle may be used for, as --capability collected it.
    // Three fields rather than a list of tokens, so "identify twice" and "a
    // signature role without identify" are both unrepresentable here instead of
    // being rejected later.
    //
    // The role rides on identify because that is where the model keeps it: an
    // element declares that it CAN identify, and the page's reference declares
    // whether it is evidence FOR that page or AGAINST it. No element-side field
    // could hold the answer, since one mark is required by one page and
    // forbidden by another. Interact and read have no such page-side datum,
    // which is exactly why they are plain flags.
    struct DrawnCapabilities final
    {
        std::optional<annotation::SignatureRole> identify{};

        bool interact{};
        bool read{};
    };

    struct InitProject final
    {
        std::filesystem::path root{};
        std::string           projectId{};

        annotation::ProjectFingerprint fingerprint;
    };

    struct ShowProject final
    {
        std::filesystem::path root{};
    };

    struct SaveProject final
    {
        std::filesystem::path root{};
    };

    // A page and the first anchor that identifies it, which the annotation model
    // requires to be authored together: PageSignature::create refuses a page
    // whose signature names no recognizer, so an empty page has no
    // representation to create and then fill in.
    struct CreatePage final
    {
        std::filesystem::path root{};
        std::string           page{};

        ElementDraw anchor;
    };

    // One element drawn onto a page, with everything that page does with it
    // committed in the same edit. One capability set serves as both halves:
    // pixels are drawn where they are used, so at the moment of drawing what
    // the element declares and what the page exercises are the same set. The
    // two only ever differ once a SECOND page borrows the element, which is
    // ReferenceElement below.
    struct AddElement final
    {
        std::filesystem::path root{};
        std::string           page{};

        DrawnCapabilities capabilities{};
        ElementDraw       draw;
    };

    // Puts an element the project already holds onto a second page. This is the
    // only way Holding::Referenced is produced: everything drawn is Owned by the
    // page it was drawn on, so borrowing is what a second page does instead of
    // redrawing the same pixels under a second id and a second search per cycle.
    struct ReferenceElement final
    {
        std::filesystem::path root{};
        std::string           page{};
        std::string           element{};

        // Absent means this page searches the element's own region, which is
        // what an absent per-page refinement means in the document too.
        std::optional<PixelRect> searchRoi{};
    };

    struct MatchRecognizer final
    {
        std::filesystem::path root{};
        std::string           recognizer{};
        std::filesystem::path frame{};

        // Which page to locate a click target on. Locating one is page-scoped
        // now -- the refined search region and the pinned appearance both live
        // on the page's reference -- so it is answered from the references when
        // exactly one page exercises interact, and asked for when several do.
        std::optional<std::string> page{};

        uint64 budget{cli::k_defaultPixelComparisonBudget};
    };

    // The three questions `frames` answers about a set of screenshots, before
    // any of them is a project. Nothing below reads or writes a project root:
    // these measure the pixels an author would otherwise measure by hand, and
    // the numbers they report are what the flags above are then set from.
    //
    // Frames arrive as PNG paths rather than as project sources on purpose. The
    // interesting set is two captures of one screen over different artwork plus
    // a third taken seconds later, and only the first of those is ever a source
    // the project holds.

    // Which pixels of a rect held still across every frame. This is how a
    // rectangle worth annotating is found in the first place: the UI is what
    // does not move when the artwork under it does.
    struct AnalyseFrameStability final
    {
        std::vector<std::filesystem::path> frames{};

        // Absent means the whole of the first frame, which is the question an
        // author asks before they have a rectangle to narrow it to.
        std::optional<PixelRect> rect{};

        uint32 grayTolerance{};
        uint32 minimumGap{};
    };

    // How well one colour key isolates whatever holds still in a rect, which is
    // the measurement that decides whether the rect is a reliable anchor.
    struct ProbeFrameColour final
    {
        std::vector<std::filesystem::path> frames{};

        PixelRect rect;

        // The key travels as the domain value the annotation model stores, so
        // --key and --tolerance are validated here exactly as they are when the
        // same two flags author an element.
        annotation::ColourKey key;
    };

    // The colours one rect of one frame is drawn in, so a key is picked from
    // data instead of by sampling a pixel and hoping.
    struct CensusFrameColours final
    {
        std::filesystem::path frame{};

        PixelRect rect;

        uint32 maximumEntries{k_defaultCensusEntries};
    };

    using AuthoringCommand = std::variant<
        InitProject,
        ShowProject,
        SaveProject,
        CreatePage,
        AddElement,
        ReferenceElement,
        MatchRecognizer,
        AnalyseFrameStability,
        ProbeFrameColour,
        CensusFrameColours
    >;

    [[nodiscard]]
    auto parseAuthoringCommand(
        std::span<std::string const> raw
    ) -> Result<AuthoringCommand>;

    [[nodiscard]] auto authoringUsageText() noexcept -> std::string_view;
}
