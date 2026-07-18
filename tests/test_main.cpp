// doctest entry point. Kept in a dedicated TU so the framework's guts are
// compiled exactly once; test files include doctest.h without the impl macro.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
