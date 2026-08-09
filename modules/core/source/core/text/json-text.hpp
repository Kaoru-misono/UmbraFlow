#pragma once

#include <string>
#include <string_view>

namespace uf
{
    // Appends value as a complete JSON string token, surrounding quotes
    // included, using the escaping RFC 8785 (JCS) mandates: the two required
    // escapes, the five short control escapes, and \u00xx in lowercase hex for
    // every other byte below 0x20. Every other byte is copied unchanged, so a
    // caller that needs the result to be valid JSON must have already
    // established that value is valid UTF-8 -- see isValidUtf8.
    //
    // It lives in core because three components write JSON that a fourth reader
    // compares byte for byte: the audit trace, the Operator registration
    // canonicalizer whose output must equal what tools/annotate/jcs.py produces,
    // and the CLI exploration protocol. A second spelling of this transform
    // cannot fail a test -- it produces bytes that merely disagree.
    auto appendJsonString(std::string& output, std::string_view value) -> void;
}
