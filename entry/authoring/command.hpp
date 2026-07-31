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

    // What --capability collected, on whichever verb collected it: what a drawn
    // rectangle may be used for, or what one page exercises on an element it
    // borrows. Three fields rather than a list of tokens, so "identify twice"
    // and "a signature role without identify" are both unrepresentable here
    // instead of being rejected later.
    //
    // The role rides on identify because that is where the model keeps it: an
    // element declares that it CAN identify, and the page's reference declares
    // whether it is evidence FOR that page or AGAINST it. No element-side field
    // could hold the answer, since one mark is required by one page and
    // forbidden by another. Interact and read have no such page-side datum,
    // which is exactly why they are plain flags.
    struct StatedCapabilities final
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
    // requires to be authored together: RecognitionCatalog::create refuses a page
    // no reference exercises identify on, so an empty page has no representation
    // to create and then fill in.
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
    //
    // This is what `page add` parses to when the capability set includes
    // identify. Identify is the only capability that needs pixels of its own --
    // it IS the claim that these pixels say which page is on screen -- so it is
    // the only one that mints an appearance. AddRegion below is the other half.
    struct AddElement final
    {
        std::filesystem::path root{};
        std::string           page{};

        StatedCapabilities capabilities{};
        ElementDraw        draw;
    };

    // One rectangle added to a page with no appearance of its own: what
    // `page add` parses to when the capability set does not include identify.
    //
    // Interact and read say WHERE, not WHAT. A hand of cards is a place a click
    // may land and its pixels are different every turn; a level counter is read
    // precisely because its content is not known in advance. Cutting a template
    // for either states a stability neither has, and the model already has the
    // representation for saying so: an element declaring no variant is located
    // by the page being recognised, and its rectangle is where it was annotated.
    //
    // Separate from AddElement rather than an AddElement with its pixel fields
    // emptied, because there is no source, no threshold and no colour key here
    // -- and a shape that can hold them would let a caller supply one that is
    // then silently dropped.
    struct AddRegion final
    {
        std::filesystem::path root{};
        std::string           page{};
        std::string           name{};

        StatedCapabilities capabilities{};

        // The element's own search region, which for an element with no pixels
        // is the whole of its geometry: what evaluateActionTarget answers with
        // and what a click is derived from.
        PixelRect region;
    };

    // A second appearance of an element the project already holds.
    //
    // The back arrow is white on one screen and dark on another, at the same
    // rectangle: one element, two appearances, and the page that uses it says
    // which applies. Drawing it twice instead would make two elements, two ids
    // and two searches a cycle for one control, and would leave the script
    // choosing between them -- the judgment that belongs to the host.
    //
    // It draws with the same <draw> options page add uses, minus the two that
    // are not an appearance's to state: --capability belongs to the element, and
    // --search-roi is the element's one region that every appearance of it is
    // searched in.
    struct AddAppearance final
    {
        std::filesystem::path root{};
        std::string           element{};

        // `draw.name` is the appearance's name, not the element's. Names are why
        // Variant carries one at all: a script reads which appearance matched to
        // learn which state the target is in.
        ElementDraw draw;
    };

    // Puts an element the project already holds onto a second page. This is the
    // only way Holding::Referenced is produced: everything drawn is Owned by the
    // page it was drawn on, so borrowing is what a second page does instead of
    // redrawing the same pixels under a second id and a second search per cycle.
    //
    // The two optionals below are this page's refinements of a shared element,
    // never edits to the element itself. That is the asymmetry the whole verb
    // rests on: one element, and each page saying what it does with it.
    struct ReferenceElement final
    {
        std::filesystem::path root{};
        std::string           page{};
        std::string           element{};

        // What THIS page exercises. Absent means every use a placement carries
        // on its own -- interact and read, whichever the element declares --
        // which is what borrowing a control asks for when the author says
        // nothing. Identify is never among those: whether a mark is evidence
        // FOR this page or AGAINST it is a question the element has no answer
        // to, so a page joins a signature only by asking in so many words.
        std::optional<StatedCapabilities> exercised{};

        // Absent means this page searches the element's own region, which is
        // what an absent per-page refinement means in the document too. It is
        // left absent rather than seeded from the element, so a later
        // correction to the element's region reaches this page as well.
        std::optional<PixelRect> searchRoi{};

        // Which appearance this page expects, when the page is what decides.
        // Absent means every appearance is searched and the best margin wins,
        // which is the answer for a form the runtime state decides rather than
        // the page -- a speed button reading 1x, 2x or 3x.
        //
        // It is text here because only the loaded document knows which names the
        // element declares.
        std::optional<std::string> variant{};
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

    // The falsification matrix over a whole project: every declared appearance
    // measured against every screen the project holds, and every element
    // measured as the runtime folds it.
    //
    // This is the check an author cannot perform by eye and the one the
    // multi-appearance model is only sound with. A template always matches the
    // image it was cut from, so the only evidence that it identifies one screen
    // rather than another is what it does on the others -- and once several
    // appearances are folded into one answer, an appearance that matches
    // everywhere is invisible behind one that matches correctly.
    //
    // It takes no frame. A capture from the running target contributes no
    // column: a frame taken to measure against is not a screen the model is
    // authored on, and the matrix is a statement about the authored ones.
    struct CheckModel final
    {
        std::filesystem::path root{};

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
        AddRegion,
        AddAppearance,
        ReferenceElement,
        MatchRecognizer,
        CheckModel,
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
