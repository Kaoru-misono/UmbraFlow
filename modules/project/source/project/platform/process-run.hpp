#pragma once

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <span>
#include <string>

namespace uf::project
{
    // Runs one program to completion and answers the status it exited with.
    //
    // commandLine.front() is the program: looked up on PATH when it carries no
    // path separator, and taken as written when it does. Every element is
    // UTF-8, and putting each one into the child as exactly one argument is
    // this function's job rather than the caller's -- the Windows spawn family
    // joins argv into a single command line without preserving boundaries, so a
    // project directory with a space in it is otherwise two arguments.
    //
    // A NON-ZERO EXIT STATUS IS A VALUE, NOT A FAILURE. The two callers read
    // the same number differently: curl's 63 says the byte ceiling stopped the
    // transfer, and the newly installed `project` binary's 1 says it printed a
    // work list. Folding those into one refusal here would take the distinction
    // away from the only code that can make it. The Result is engaged only when
    // the child could not be started or could not be waited for.
    //
    // The child inherits this process's standard streams, so whatever it writes
    // is what the operator sees, in the order it was written.
    [[nodiscard]]
    auto runProcess(std::span<std::string const> commandLine) -> Result<int32>;
}
