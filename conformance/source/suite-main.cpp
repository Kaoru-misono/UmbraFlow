// The suite owns its entry point so a consuming repository links one target and
// runs one executable, rather than adopting a test framework of ours.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
