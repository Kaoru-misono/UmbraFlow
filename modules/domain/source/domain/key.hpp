#pragma once

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <array>
#include <compare>
#include <string_view>

namespace uf
{
    // The longest key name the accepted set holds ("F12"), and therefore the
    // whole storage one name needs.
    inline constexpr auto k_maxKeyNameBytes = uint8{3};

    // One key named exactly as the target's own UI prints it: "E", "3", "F1".
    //
    // It exists so a keystroke can cross the engine's action port without that
    // port naming a virtual key, which is a Windows fact and belongs to
    // modules/controller. A name is platform-neutral -- the target prints "E"
    // whatever the host is -- so the value that travels is the name and the
    // adapter at the delivery edge is what resolves it.
    //
    // This is the SINGLE definition of which key names exist. The accepted set is
    // "A".."Z", "0".."9" and "F1".."F12" in uppercase, and
    // controller::KeyInput::fromName routes through create() rather than
    // repeating the test, so the two cannot come to disagree about which names a
    // project may write. The set is injective on purpose: no two accepted names
    // share a virtual key, so a mistyped name fails loudly instead of resolving
    // to a neighbouring key.
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
