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

    // Orders two JSON member names the way RFC 8785 section 3.2.3 requires: by
    // the UTF-16 code-unit sequence of the name. That is neither UTF-8 byte
    // order nor code-point order -- a supplementary code point becomes a
    // surrogate pair starting in D800..DBFF, so it sorts BEFORE every name
    // beginning in E000..FFFF, which both other orderings put first.
    //
    // A strict weak ordering, usable as a sort predicate. JCS member names are
    // valid UTF-8 by construction and the other two implementations of this
    // scheme refuse anything else outright, which a bool cannot report; so that
    // this stays a strict weak ordering rather than undefined behaviour, a name
    // that is not valid UTF-8 sorts after every name that is, and two such
    // names order by their bytes. Callers canonicalizing a document must still
    // reject invalid UTF-8 themselves -- ordering it is not encoding it.
    //
    // This is the second RFC 8785 rule, and until it was written here it was
    // spelled only in comments: every C++ emitter hand-writes its members in
    // this order and the rule was enforced by review. Adding the predicate does
    // not by itself make those emitters compute their order.
    [[nodiscard]]
    auto jsonMemberNameLess(std::string_view left, std::string_view right)
        -> bool;
}
