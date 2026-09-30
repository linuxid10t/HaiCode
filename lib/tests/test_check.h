#pragma once
#include <cstdlib>
#include <iostream>

// Always-on assertion for test scaffolding — must fire in Release builds,
// unlike assert() which -DNDEBUG removes.
#define TEST_REQUIRE(cond, msg) \
    do { if (!(cond)) { \
        std::cerr << "[FATAL] " << (msg) << "  (" #cond ") at " \
                  << __FILE__ << ":" << __LINE__ << "\n"; \
        std::exit(1); } } while (0)
