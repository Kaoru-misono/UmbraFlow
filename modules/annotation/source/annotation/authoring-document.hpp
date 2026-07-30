#pragma once

#include "catalog.hpp"
#include "content-hash.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>
#include <core/types/strong-value.hpp>

#include <domain/ids.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uf::annotation
{
    // The schema the GUI reads and writes. Bumped from v1 when page membership
    // moved off the recognizer (allowed_page_ids) and onto page-side placements.
    // The v1 read path has been retired: the sole real project migrated to v2 on
    // disk, so an old schema string now fails with the ordinary unsupported-schema
    // error rather than upgrading.
    inline constexpr auto k_authoringDocumentSchema = std::string_view{
        "umbraflow-authoring/v2"
    };

    namespace detail
    {
        struct SourceIdTag;
        struct RegressionIdTag;
    }

    using SourceId = StrongValue<detail::SourceIdTag, ResourceId>;
    using RegressionId = StrongValue<detail::RegressionIdTag, ResourceId>;

    struct WgcSourceProvenance final
    {
        TargetGeneration targetGeneration{};
        std::string      capturedAt{};

        auto operator==(WgcSourceProvenance const&) const -> bool = default;
    };

    struct ImportedSourceProvenance final
    {
        auto operator==(ImportedSourceProvenance const&) const -> bool = default;
    };

    using SourceProvenance = std::variant<
        WgcSourceProvenance,
        ImportedSourceProvenance
    >;

    struct AuthoringSourceSpec final
    {
        SourceId           id;
        ContentHash        contentHash;
        ProjectFingerprint fingerprint;
        SourceProvenance   provenance{};
    };

    class AuthoringSource final
    {
        SourceId           m_id;
        ContentHash        m_contentHash;
        std::string        m_relativePath;
        ProjectFingerprint m_fingerprint;
        SourceProvenance   m_provenance;

        explicit AuthoringSource(AuthoringSourceSpec const& spec);

    public:
        [[nodiscard]]
        static auto create(
            AuthoringSourceSpec const& spec
        ) -> Result<AuthoringSource>;

        [[nodiscard]] auto id() const -> SourceId;
        [[nodiscard]] auto contentHash() const -> ContentHash;
        [[nodiscard]] auto fingerprint() const noexcept -> ProjectFingerprint;

        [[nodiscard]]
        auto relativePath() const noexcept UF_LIFETIME_BOUND -> std::string const&;

        [[nodiscard]]
        auto provenance() const noexcept UF_LIFETIME_BOUND -> SourceProvenance const&;
    };

    // The colour an author picked out of the source image, and how far another
    // pixel may sit from it and still belong to the element's mask. Distance is
    // the sum of the three absolute channel differences, so it runs 0..765 and
    // one unit is one level on one channel.
    //
    // What is stored is the key, never the mask it produces. An author reopening
    // a project has to be able to move the tolerance and watch the selection
    // change; a baked mask cannot be moved back.
    class ColourKey final
    {
    public:
        // The full range of a channel, and of the distance across all three. A
        // key at the maximum tolerance admits every colour, which is legal and
        // useless -- the same mask as carrying no key at all.
        static constexpr auto k_maximumChannel   = uint32{255};
        static constexpr auto k_maximumTolerance = uint32{765};

    private:
        uint8  m_red;
        uint8  m_green;
        uint8  m_blue;
        uint32 m_tolerance;

        constexpr ColourKey(
            uint8 red,
            uint8 green,
            uint8 blue,
            uint32 tolerance
        ) noexcept
            : m_red{red}
            , m_green{green}
            , m_blue{blue}
            , m_tolerance{tolerance}
        {
        }

    public:
        auto operator==(ColourKey const&) const -> bool = default;

        // Channels are taken widened because both callers -- the canonical TOML
        // reader and the picker UI -- hold values that are only supposed to be
        // channel-sized, and this is where that is established.
        [[nodiscard]]
        static auto create(
            uint32 red,
            uint32 green,
            uint32 blue,
            uint32 tolerance
        ) -> Result<ColourKey>;

        [[nodiscard]] constexpr auto red() const noexcept -> uint8 { return m_red; }
        [[nodiscard]] constexpr auto green() const noexcept -> uint8 { return m_green; }
        [[nodiscard]] constexpr auto blue() const noexcept -> uint8 { return m_blue; }

        [[nodiscard]]
        constexpr auto tolerance() const noexcept -> uint32 { return m_tolerance; }

        // The mask weight one source pixel earns, which is exactly the alpha
        // byte the compiled template carries for it: 255 counts fully, 0 is
        // excluded, and between them the matcher weights the pixel partially.
        //
        // Full weight out to the tolerance, then a linear ramp to nothing at
        // twice it. The ramp is not decoration. A hard cut makes an author's
        // tolerance control jump in steps, and it cuts through the antialiased
        // skirt of a glyph, where the pixels just past the cut are still mostly
        // glyph -- on the measured menu entry a tolerance of 12 around the white
        // text takes 93.9% of the glyph and leaves a rim of edge pixels at
        // distance 13..24. Those are the pixels the ramp readmits, at the weight
        // they deserve. A tolerance of 0 has no ramp to speak of and stays an
        // exact-colour mask.
        [[nodiscard]]
        auto alphaFor(uint8 red, uint8 green, uint8 blue) const noexcept -> uint8;
    };

    // The three kinds an element can be. They are the payloads of the element
    // sum type below, matched with core/utility/variant-match.hpp. Only the
    // interactive kind carries a click, which is what turns "only an action
    // target may define a default click" from a cross-field rule into a fact of
    // the type: an anchor or an info region has no field to misuse.
    struct AnchorElement final
    {
        auto operator==(AnchorElement const&) const -> bool = default;
    };

    struct InteractiveElement final
    {
        std::optional<TemplateOffset> clickOffset{};

        auto operator==(InteractiveElement const&) const -> bool = default;
    };

    struct InfoElement final
    {
        auto operator==(InfoElement const&) const -> bool = default;
    };

    using ElementKind = std::variant<
        AnchorElement,
        InteractiveElement,
        InfoElement
    >;

    // The annotation type that a kind compiles down to, so the runtime manifest
    // and the derived catalog keep their existing three-way vocabulary.
    [[nodiscard]]
    auto annotationTypeOfKind(ElementKind const& kind) noexcept -> AnnotationType;

    // An authored element: the template pixels drawn once, plus the two facts
    // only authoring keeps. The screen it was drawn on is what makes its
    // rectangles meaningful; the shared flag records that the author intends
    // these pixels to be reused on other pages, a statement of intent no other
    // field can carry. Where an element is placed no longer lives here -- that
    // is a page-side fact (see AuthoringPlacement). An anchor keeps its search
    // region on the element, since its page membership is a signature and no
    // requirement yet needs a per-signature region; an interactive or info
    // element carries its own search region here too, and each placement may
    // refine it per page in a later phase.
    class Element final
    {
    public:
        struct Spec final
        {
            RecognizerId             id;
            ResourceName             name;
            SourceId                 sourceId;
            PixelRect                templateRect;
            PixelRect                searchRoi;
            SimilarityThreshold      threshold;
            std::optional<ColourKey> colourKey{};
            ElementKind              kind;
            bool                     shared{};
        };

    private:
        RecognizerId             m_id;
        ResourceName             m_name;
        SourceId                 m_sourceId;
        PixelRect                m_templateRect;
        PixelRect                m_searchRoi;
        SimilarityThreshold      m_threshold;
        std::optional<ColourKey> m_colourKey;
        ElementKind              m_kind;
        bool                     m_shared;

        explicit Element(Spec spec) noexcept;

    public:
        [[nodiscard]]
        static auto create(
            ProjectFingerprint fingerprint,
            Spec const& spec
        ) -> Result<Element>;

        [[nodiscard]] auto id() const -> RecognizerId;
        [[nodiscard]] auto name() const -> ResourceName;
        [[nodiscard]] auto sourceId() const -> SourceId;
        [[nodiscard]] auto templateRect() const noexcept -> PixelRect;
        [[nodiscard]] auto searchRoi() const noexcept -> PixelRect;
        [[nodiscard]] auto threshold() const noexcept -> SimilarityThreshold;
        [[nodiscard]] auto annotationType() const noexcept -> AnnotationType;
        [[nodiscard]] auto shared() const noexcept -> bool;

        // Which of the template's pixels count. Absent means all of them, which
        // is what every element authored before the key existed says and keeps
        // saying: its compiled template is fully opaque, byte for byte as
        // before.
        [[nodiscard]]
        auto colourKey() const noexcept -> std::optional<ColourKey>;

        [[nodiscard]]
        auto kind() const noexcept UF_LIFETIME_BOUND -> ElementKind const&;
    };

    // One interactive or info element placed on one page, with where the runtime
    // should look for it there. This is the page-membership edge the recognizer
    // used to carry inverted; storing page_id here lets the whole set serialize
    // as a flat table, which the canonical TOML reader can parse.
    struct AuthoringPlacement final
    {
        PageId       pageId;
        RecognizerId elementId;
        PixelRect    searchRoi;

        auto operator==(AuthoringPlacement const&) const -> bool = default;
    };

    struct AuthoringRecognizerSource final
    {
        RecognizerId recognizerId;
        SourceId     sourceId;
        bool         shared{};
    };

    enum class RegressionClassification : uint8
    {
        Positive,
        Negative,
        Confusable,
    };

    struct ResolvedRegression final
    {
        PageId pageId;

        auto operator==(ResolvedRegression const&) const -> bool = default;
    };

    struct UnknownRegression final
    {
        auto operator==(UnknownRegression const&) const -> bool = default;
    };

    struct AmbiguousRegression final
    {
        auto operator==(AmbiguousRegression const&) const -> bool = default;
    };

    using RegressionExpectation = std::variant<
        ResolvedRegression,
        UnknownRegression,
        AmbiguousRegression
    >;

    struct RegressionSpec final
    {
        RegressionId             id;
        SourceId                 sourceId;
        RegressionClassification classification{};
        RegressionExpectation    expectation;
    };

    class RegressionCase final
    {
        RegressionId             m_id;
        SourceId                 m_sourceId;
        RegressionClassification m_classification;
        RegressionExpectation    m_expectation;

    public:
        explicit RegressionCase(RegressionSpec const& spec);

        [[nodiscard]] auto id() const -> RegressionId;
        [[nodiscard]] auto sourceId() const -> SourceId;

        [[nodiscard]]
        auto classification() const noexcept -> RegressionClassification;

        [[nodiscard]]
        auto expectation() const noexcept UF_LIFETIME_BOUND -> RegressionExpectation const&;
    };

    class AuthoringDocument final
    {
        // m_elements and m_placements are the v2 source of truth. m_catalog is a
        // read model derived from them at construction (its recognizers carry
        // the allowed_page_ids inverted from the placements) so that the
        // compiler and every UI panel that already reads catalog() keep working
        // unchanged; m_recognizerSources is derived likewise. Phase 3 removes
        // the derived read models once those callers speak placements natively.
        RecognitionCatalog                     m_catalog;
        std::vector<AuthoringSource>           m_sources;
        std::vector<Element>                   m_elements;
        std::vector<AuthoringPlacement>        m_placements;
        std::vector<AuthoringRecognizerSource> m_recognizerSources;
        std::vector<RegressionCase>            m_regressions;

        // LLVM 23's performance-unnecessary-value-param check can recurse
        // through StrongValue construction when these owned sinks are passed
        // by value.
        AuthoringDocument(
            RecognitionCatalog&& catalog,
            std::vector<AuthoringSource>&& sources,
            std::vector<Element>&& elements,
            std::vector<AuthoringPlacement>&& placements,
            std::vector<AuthoringRecognizerSource>&& recognizerSources,
            std::vector<RegressionCase>&& regressions
        ) noexcept;

    public:
        // The v2 model: elements plus the page-side placements that authorize
        // interactive and info elements onto pages.
        [[nodiscard]]
        static auto create(
            ProjectId projectId,
            ProjectFingerprint fingerprint,
            std::vector<AuthoringSource> sources,
            std::vector<Element> elements,
            std::vector<PageSignature> pages,
            std::vector<AuthoringPlacement> placements,
            std::vector<RegressionCase> regressions
        ) -> Result<AuthoringDocument>;

        [[nodiscard]]
        auto catalog() const noexcept UF_LIFETIME_BOUND -> RecognitionCatalog const&;

        [[nodiscard]]
        auto sources() const noexcept UF_LIFETIME_BOUND -> std::span<AuthoringSource const>;

        [[nodiscard]]
        auto elements() const noexcept UF_LIFETIME_BOUND -> std::span<Element const>;

        [[nodiscard]]
        auto placements() const noexcept UF_LIFETIME_BOUND
            -> std::span<AuthoringPlacement const>;

        [[nodiscard]]
        auto recognizerSources() const noexcept UF_LIFETIME_BOUND
            -> std::span<AuthoringRecognizerSource const>;

        [[nodiscard]]
        auto regressions() const noexcept UF_LIFETIME_BOUND -> std::span<RegressionCase const>;

        [[nodiscard]]
        auto findSource(
            SourceId id
        ) const noexcept UF_LIFETIME_BOUND -> AuthoringSource const*;

        [[nodiscard]]
        auto findElement(
            RecognizerId id
        ) const noexcept UF_LIFETIME_BOUND -> Element const*;
    };

    [[nodiscard]]
    auto serializeAuthoringDocument(
        AuthoringDocument const& document
    ) -> std::string;

    [[nodiscard]]
    auto parseAuthoringDocument(
        std::string_view canonicalToml
    ) -> Result<AuthoringDocument>;
}
