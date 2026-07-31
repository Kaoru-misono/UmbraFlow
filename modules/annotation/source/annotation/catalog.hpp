#pragma once

#include "capabilities.hpp"
#include "resource.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace uf::annotation
{
    // One appearance of one element as the runtime sees it: which rectangle to
    // cut and how close a match has to be. The authoring-side Variant carries
    // two more facts -- the source screen it was cut from and the colour key
    // that produced its mask -- and neither reaches the runtime, because the
    // mask is baked into the compiled template's alpha channel and the source
    // is authoring truth.
    struct RecognizerVariant final
    {
        ResourceName        name;
        PixelRect           templateRect;
        SimilarityThreshold threshold;

        auto operator==(RecognizerVariant const&) const -> bool = default;
    };

    struct RecognizerSpec final
    {
        ElementId           id;
        ResourceName        name;
        ElementCapabilities capabilities;
        PixelRect           searchRoi;

        // Ordered, and empty is a legal state: it says this rectangle is
        // located by the page being recognised rather than by pixels of its
        // own. Declaration order decides nothing but ties.
        std::vector<RecognizerVariant> variants{};
    };

    // The shape rules an element obeys whichever side states it: the search
    // region fits the project, every variant template fits the project and that
    // region, every threshold has a computable SAD ceiling, variant names are
    // distinct, a rectangle with no pixels of its own cannot be identity
    // evidence, and a click offset lands inside every appearance it could be
    // measured from. The authoring Element and the runtime RecognizerDefinition
    // both call it, so the two cannot drift apart.
    [[nodiscard]]
    auto validateElementShape(
        ProjectFingerprint fingerprint,
        PixelRect searchRoi,
        std::span<RecognizerVariant const> variants,
        ElementCapabilities const& capabilities
    ) -> Status;

    class RecognizerDefinition final
    {
        ElementId           m_id;
        ResourceName        m_name;
        ElementCapabilities m_capabilities;
        PixelRect           m_searchRoi;

        std::vector<RecognizerVariant> m_variants;

        explicit RecognizerDefinition(RecognizerSpec spec) noexcept;

    public:
        [[nodiscard]]
        static auto create(
            ProjectFingerprint fingerprint,
            RecognizerSpec const& spec
        ) -> Result<RecognizerDefinition>;

        [[nodiscard]] auto id() const -> ElementId;
        [[nodiscard]] auto name() const -> ResourceName;

        [[nodiscard]]
        auto capabilities() const noexcept UF_LIFETIME_BOUND -> ElementCapabilities const&;

        [[nodiscard]] auto searchRoi() const noexcept -> PixelRect;

        [[nodiscard]]
        auto variants() const noexcept UF_LIFETIME_BOUND -> std::span<RecognizerVariant const>;

        // Returned observations remain valid only while this definition lives.
        [[nodiscard]]
        auto findVariant(
            ResourceName const& name
        ) const noexcept UF_LIFETIME_BOUND -> RecognizerVariant const*;
    };

    // Which page owns an element's definition -- its rectangle and its
    // appearances. Owned is the element's home page, the one `page add` drew it
    // on; Referenced is another page borrowing it. Exactly one Owned row per
    // element is enforced here; every other page that uses it is Referenced.
    // It replaces the old `bool shared`, which could only record an intent and
    // could contradict the placements without anything noticing.
    //
    // Ownership is not exclusivity, and the two were conflated here until
    // 2026-07-31. "These pixels are this page's alone, so refuse to reference
    // them elsewhere" cannot be what Owned means: `runAddElement` marks every
    // drawn element Owned on its home page, so that reading makes
    // `page reference` fail for every element there is. If an author ever needs
    // to declare an element private to one page, that is a separate statement
    // on the element, not a second meaning for this enum.
    enum class Holding : uint8
    {
        Owned,
        Referenced,
    };

    // One page's use of one element: what that page does with it, plus the two
    // refinements a page may make. This is the edge the model is built on --
    // authorisation IS the reference, and a page's signature is derived from
    // the references that exercise identify.
    //
    // `holding` is an authoring-side editing guard rail that the runtime never
    // reads. It rides on this one type rather than forcing a near-identical
    // second one, which would duplicate five fields to hide one.
    struct PageReference final
    {
        PageId                pageId;
        ElementId             elementId;
        Holding               holding{Holding::Owned};
        ExercisedCapabilities exercised;

        // Absent means "use the element's own search region". A reference that
        // exercises identify may not set it: the anchor pass reads the
        // element-level region, and refining it would search the same pixels
        // twice per cycle, which is the cost the capability merge exists to
        // remove.
        std::optional<PixelRect> searchRoi{};

        // Which appearance applies here, when the page decides it. Absent means
        // every variant is searched and the best normalized margin wins.
        //
        // It binds the page-scoped paths only. The anchor pass runs before any
        // page is known -- that is what makes one search serve every page -- so
        // identify always folds across every variant, whatever a reference
        // pins.
        std::optional<ResourceName> variant{};

        auto operator==(PageReference const&) const -> bool = default;
    };

    struct PageSpec final
    {
        PageId       id;
        ResourceName name;
    };

    class PageSignature final
    {
        // A signature is derived, never authored. RecognitionCatalog builds it
        // from the page references whose exercised identify carries Required or
        // Forbidden; a public factory taking the two vectors would be a second
        // way to state one fact, and the two ways could disagree.
        friend class RecognitionCatalog;

        PageId                 m_id;
        ResourceName           m_name;
        std::vector<ElementId> m_required;
        std::vector<ElementId> m_forbidden;

        PageSignature(
            PageSpec spec,
            std::vector<ElementId> required,
            std::vector<ElementId> forbidden
        ) noexcept;

    public:
        [[nodiscard]] auto id() const -> PageId;
        [[nodiscard]] auto name() const -> ResourceName;

        [[nodiscard]]
        auto required() const noexcept UF_LIFETIME_BOUND -> std::span<ElementId const>;

        [[nodiscard]]
        auto forbidden() const noexcept UF_LIFETIME_BOUND -> std::span<ElementId const>;
    };

    class RecognitionCatalog final
    {
        ProjectId                         m_projectId;
        ProjectFingerprint                m_fingerprint;
        std::vector<RecognizerDefinition> m_recognizers;
        std::vector<PageSignature>        m_pages;
        std::vector<PageReference>        m_references;
        std::vector<ElementId>            m_pageAnchorOrder;

        RecognitionCatalog(
            ProjectId projectId,
            ProjectFingerprint fingerprint,
            std::vector<RecognizerDefinition> recognizers,
            std::vector<PageSignature> pages,
            std::vector<PageReference> references,
            std::vector<ElementId> pageAnchorOrder
        ) noexcept;

    public:
        // Both sides of the project converge here: the authoring document
        // derives its read model through this factory and the runtime manifest
        // is parsed into one, so every model invariant below holds on disk and
        // in memory without either side repeating the check.
        [[nodiscard]]
        static auto create(
            ProjectId projectId,
            ProjectFingerprint fingerprint,
            std::vector<RecognizerDefinition> recognizers,
            std::vector<PageSpec> pages,
            std::vector<PageReference> references
        ) -> Result<RecognitionCatalog>;

        [[nodiscard]]
        auto projectId() const noexcept UF_LIFETIME_BOUND -> ProjectId const&;

        [[nodiscard]] auto fingerprint() const noexcept -> ProjectFingerprint;

        [[nodiscard]]
        auto recognizers() const noexcept UF_LIFETIME_BOUND -> std::span<RecognizerDefinition const>;

        [[nodiscard]]
        auto pages() const noexcept UF_LIFETIME_BOUND -> std::span<PageSignature const>;

        [[nodiscard]]
        auto references() const noexcept UF_LIFETIME_BOUND -> std::span<PageReference const>;

        // Returned observations remain valid only while this catalog is alive.
        [[nodiscard]]
        auto findRecognizer(
            ElementId id
        ) const noexcept UF_LIFETIME_BOUND -> RecognizerDefinition const*;

        [[nodiscard]]
        auto findPage(PageId id) const noexcept UF_LIFETIME_BOUND -> PageSignature const*;

        [[nodiscard]]
        auto findReference(
            PageId pageId,
            ElementId elementId
        ) const noexcept UF_LIFETIME_BOUND -> PageReference const*;

        [[nodiscard]]
        auto pageAnchorOrder() const noexcept UF_LIFETIME_BOUND -> std::span<ElementId const>;
    };
}
