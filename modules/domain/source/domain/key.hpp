#pragma once

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <array>
#include <compare>
#include <optional>
#include <string_view>

namespace uf
{
    // The keys a target prints as a word rather than as the character they type.
    //
    // Every entry is an affordance a target was observed publishing, never one
    // added because a keyboard has it. The four here are the battle screen's own
    // printed contract -- ENTER plays the selected card, ESC cancels the
    // selection, CAPS shows the card's information -- plus the SHIFT lock printed
    // in the same view. TAB is deliberately absent: nothing observed offers it,
    // and an unobserved name is a guess this set does not make.
    //
    // Without ENTER a run could select a card and never play one, and no mouse
    // path substitutes: clicking the target of a selected card cancels the
    // selection instead of committing it. That is why these are a family rather
    // than an afterthought.
    //
    // controller::KeyInput pairs each of these with a virtual key and fails to
    // compile when one is unpaired, so a name admitted here cannot resolve to
    // nothing.
    inline constexpr auto k_namedKeys = std::array<std::string_view, 4>{
        "ENTER",
        "ESC",
        "CAPS",
        "SHIFT",
    };

    // The longest key name the accepted set holds ("ENTER" and "SHIFT"), and
    // therefore the whole storage one name needs. key.cpp asserts that every
    // named key still fits.
    inline constexpr auto k_maxKeyNameBytes = uint8{5};

    // The function-key number `name` prints, or nullopt when it is not a
    // function key at all. A leading zero and a number outside 1..12 are both
    // refused rather than clamped, so "F0" and "F13" name nothing and "F" stays
    // the letter key.
    //
    // Exported because KeyName::create and controller::KeyInput::fromKeyName
    // must classify this family identically. fromKeyName is total, so a name
    // create() admits and the adapter fails to classify trips a release-active
    // check on a keystroke the author was entitled to write; one definition
    // leaves the two nothing to disagree about.
    [[nodiscard]]
    auto functionKeyNumber(std::string_view name) noexcept -> std::optional<uint32>;

    // One key named exactly as the target's own UI prints it: "E", "3", "F1",
    // "ENTER".
    //
    // It exists so a keystroke can cross the engine's action port without that
    // port naming a virtual key, which is a Windows fact and belongs to
    // modules/controller. A name is platform-neutral -- the target prints "E"
    // whatever the host is -- so the value that travels is the name and the
    // adapter at the delivery edge is what resolves it.
    //
    // This is the SINGLE definition of which key names exist. The accepted set is
    // "A".."Z", "0".."9", "F1".."F12" and every name in k_namedKeys, and
    // controller::KeyInput::fromName routes through create() rather than
    // repeating the test, so the two cannot come to disagree about which names a
    // project may write. The set is injective on purpose: no two accepted names
    // share a virtual key, so a mistyped name fails loudly instead of resolving
    // to a neighbouring key.
    //
    // The set is CLOSED and stays closed. It grows by naming an observed
    // affordance, never by admitting a virtual-key code or a free-form escape.
    // That closure is what lets fromKeyName be noexcept and lets a refusal print
    // the whole vocabulary it is refusing against.
    //
    // Names are case-sensitive: every one is spelled in uppercase and "enter" is
    // refused. Not an oversight. The bytes are the value that travels, so folding
    // case would either put a second spelling of one key into traces and into
    // this type's own byte comparison, or hand an author back a name they did not
    // write. One spelling per key is what keeps the set injective in both
    // directions, and the refusal says so rather than leaving the reader to
    // infer it from a rule about letters.
    //
    // Stored as bytes rather than as a code, because the name is what reaches a
    // trace line and what an author reads back. Trivially copyable and
    // comparable, so it travels by value everywhere.
    class KeyName final
    {
        // Both come from construction: create() is the only way to make one, and it
        // supplies both, so neither carries an in-class initializer that the
        // constructor would immediately overwrite.
        std::array<char, k_maxKeyNameBytes> m_text;
        uint8                               m_length;

        constexpr KeyName(
            std::array<char, k_maxKeyNameBytes> text,
            uint8 length
        ) noexcept
            : m_text{text}
            , m_length{length}
        {
        }

    public:
        auto operator<=>(KeyName const&) const = default;

        // Fails ActionRejected for a name outside the accepted set, which is the
        // kind the delivery layer already reports for input it refuses to guess
        // at: a name nobody can resolve is a rejected action, not a missing
        // resource.
        [[nodiscard]] static auto create(std::string_view name) -> Result<KeyName>;

        // The name's bytes. The view borrows this object's own storage, so it
        // lives exactly as long as the KeyName it came from.
        [[nodiscard]]
        auto value() const noexcept UF_LIFETIME_BOUND -> std::string_view;
    };
}
