#include <core/project.hpp>

#include <cstdlib>
#include <iostream>

auto main() -> int
{
    try
    {
        std::cout << uf::g_projectName << '\n';
        return EXIT_SUCCESS;
    }
    catch (...)
    {
        return EXIT_FAILURE;
    }
}
