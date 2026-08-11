#pragma once

#include <span>
#include <string>

namespace uf::operator_runtime::conformance
{
    // The suite's whole surface: everything umbra-flow-conformance does, minus
    // the process argument vector that entry/conformance/main.cpp turns into
    // this one. `arguments` is the vector without argv[0]; --project names the
    // directory the run is judged against and every other argument is doctest's.
    //
    // The return value is a process exit code, not a Result: doctest's own run
    // reports failure that way and this is the boundary that hands it on.
    [[nodiscard]]
    auto runSuite(std::span<std::string const> arguments) -> int;
}
