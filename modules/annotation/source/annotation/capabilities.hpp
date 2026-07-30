#pragma once

#include "resource.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <optional>

namespace uf::annotation
{
    // The three uses one patch of pixels can be put to. They are a set rather
    // than a choice, so an element that both names its page and can be clicked
    // is one element matched once per cycle instead of two matched twice.
    //
    // Each capability owns its own configuration instead of the element holding
    // a flat bag of fields. That buys a structural fact a bitmask cannot state:
    // an element without the read capability has nowhere to put OCR parameters.

    // Contributing evidence to a page signature. Whether a page requires this
    // element or forbids it is a property of that page's reference to it, not of
    // the element -- an element declares what it can do, a reference declares
    // how one page uses it -- so `Required | Forbidden` lands on the page side
    // and this payload stays empty until an element-side identify parameter
    // actually exists.
    struct Identify final
    {
        auto operator==(Identify const&) const -> bool = default;
    };

    // Receiving a delivered action. The click offset is the one datum this
    // capability already owns, which is what keeps "only an action target may
    // define a default click" a fact of the type rather than a cross-field rule.
    struct Interact final
    {
        std::optional<TemplateOffset> clickOffset{};

        auto operator==(Interact const&) const -> bool = default;
    };

    // Whether the rectangle holds one line of text or a block of several. Both
    // are already needed: a level readout is one line, a dialogue body is not.
    enum class ReadLayout : uint8
    {
        SingleLine,
        Block,
    };

    // The only restriction with a measured need: a field that is always digits,
    // as the level readout showing `Lv.65` is. Absent means unrestricted.
    //
    // One enumerator is deliberate. The enum exists so that a second measured
    // restriction arrives as an added enumerator rather than a reshaped field,
    // and nothing beyond the one measured instance is invented in advance.
    enum class CharsetRestriction : uint8
    {
        Digits,
    };

    // Reading text out of the element's rectangle. Both parameters are
    // authoring-time facts rather than per-call arguments: "this cell is digits"
    // is a fact about the UI, not a policy of one call, and a per-call charset
    // would let two scripts derive different values from identical pixels --
    // policy altering evidence, which is the wrong side of "C++ owns all
    // guarantees, Luau owns all policy". Thresholds and colour keys are already
    // authored facts for exactly that reason.
    struct Read final
    {
        ReadLayout                        layout{ReadLayout::SingleLine};
        std::optional<CharsetRestriction> charset{};

        auto operator==(Read const&) const -> bool = default;
    };

    class ElementCapabilities final
    {
        std::optional<Identify> m_identify;
        std::optional<Interact> m_interact;
        std::optional<Read>     m_read;

        ElementCapabilities(
            std::optional<Identify> identify,
            std::optional<Interact> interact,
            std::optional<Read> read
        ) noexcept;

    public:
        auto operator==(ElementCapabilities const&) const -> bool = default;

        // Rejects the all-empty set. An element no capability can reach is not a
        // temporarily unused element, it is an entry nothing can explain.
        [[nodiscard]]
        static auto create(
            std::optional<Identify> identify,
            std::optional<Interact> interact,
            std::optional<Read> read
        ) -> Result<ElementCapabilities>;

        [[nodiscard]]
        auto identify() const noexcept UF_LIFETIME_BOUND -> std::optional<Identify> const&;

        [[nodiscard]]
        auto interact() const noexcept UF_LIFETIME_BOUND -> std::optional<Interact> const&;

        [[nodiscard]]
        auto read() const noexcept UF_LIFETIME_BOUND -> std::optional<Read> const&;

        [[nodiscard]] auto hasIdentify() const noexcept -> bool;
        [[nodiscard]] auto hasInteract() const noexcept -> bool;
        [[nodiscard]] auto hasRead() const noexcept -> bool;
    };

    // The page side of the same three capabilities, and a separate type rather
    // than a second use of the element's one. The element declares what it CAN
    // do; a page's reference to it declares what THAT page does with it, and
    // must be a subset. Two types keep the payloads honest: what a page adds to
    // identify has no meaning on an element, and the OCR parameters have no
    // meaning on a reference. One shared type would have to hold the union of
    // both and let each side ignore half of it.

    // Which way a page's identify evidence points.
    enum class SignatureRole : uint8
    {
        Required,
        Forbidden,
    };

    // Exercising identify on a page means being evidence FOR that page or
    // AGAINST it. That is a property of the page's reference, never of the
    // element -- one mark is evidence for page A and evidence against page B,
    // so an element-side field could only ever hold one of the two answers.
    struct ExercisedIdentify final
    {
        // Stated rather than left to `{}`, since both enumerators are ordinary
        // authored choices and neither is the zero value by accident: a
        // reference that puts an element in a page's signature normally means
        // it as positive evidence, and forbidding is the rarer deliberate one.
        SignatureRole role{SignatureRole::Required};

        auto operator==(ExercisedIdentify const&) const -> bool = default;
    };

    // Authorisation IS the reference: a page that references an element and
    // exercises interact has authorised it, which is the whole of what the
    // separate allowed-page list used to say. Nothing page-local yet. A
    // per-page click offset would land here if one is ever measured; today the
    // element's Interact carries the only one there is.
    struct ExercisedInteract final
    {
        auto operator==(ExercisedInteract const&) const -> bool = default;
    };

    // Nothing page-local yet; the OCR parameters belong to the element, because
    // layout and charset are facts about the UI cell rather than about one
    // page's use of it. A per-page override would land here if a page ever
    // needed to read the same pixels differently.
    struct ExercisedRead final
    {
        auto operator==(ExercisedRead const&) const -> bool = default;
    };

    class ExercisedCapabilities final
    {
        std::optional<ExercisedIdentify> m_identify;
        std::optional<ExercisedInteract> m_interact;
        std::optional<ExercisedRead>     m_read;

        ExercisedCapabilities(
            std::optional<ExercisedIdentify> identify,
            std::optional<ExercisedInteract> interact,
            std::optional<ExercisedRead> read
        ) noexcept;

    public:
        auto operator==(ExercisedCapabilities const&) const -> bool = default;

        // Rejects the all-empty set, for the reason ElementCapabilities does: a
        // page that references an element and then exercises none of it is an
        // edge nothing can explain, and deleting it would change no behaviour.
        [[nodiscard]]
        static auto create(
            std::optional<ExercisedIdentify> identify,
            std::optional<ExercisedInteract> interact,
            std::optional<ExercisedRead> read
        ) -> Result<ExercisedCapabilities>;

        // The two-level split made checkable: a page may only exercise what the
        // element declares. It sits on the subset rather than the superset so
        // the direction is readable at the call site.
        [[nodiscard]]
        auto isSubsetOf(ElementCapabilities const& declared) const noexcept -> bool;

        [[nodiscard]]
        auto identify() const noexcept UF_LIFETIME_BOUND
            -> std::optional<ExercisedIdentify> const&;

        [[nodiscard]]
        auto interact() const noexcept UF_LIFETIME_BOUND
            -> std::optional<ExercisedInteract> const&;

        [[nodiscard]]
        auto read() const noexcept UF_LIFETIME_BOUND -> std::optional<ExercisedRead> const&;

        [[nodiscard]] auto hasIdentify() const noexcept -> bool;
        [[nodiscard]] auto hasInteract() const noexcept -> bool;
        [[nodiscard]] auto hasRead() const noexcept -> bool;
    };
}
