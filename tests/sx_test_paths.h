#pragma once

// Fixture roots for the Windows test executables.
//
// CMake force-includes this header into every scarlettx_add_test target and passes the
// configure-time folders as SCARLETTX_BINARY_DIR_DEFAULT / SCARLETTX_SOURCE_DIR_DEFAULT. The
// tests keep using SCARLETTX_BINARY_DIR and SCARLETTX_SOURCE_DIR, which now resolve at run time:
//
//   SCARLETTX_TEST_BINARY_DIR  folder holding the built shims, packages/ and dummy_import_*/
//   SCARLETTX_TEST_SOURCE_DIR  checkout root (tests/, include/ ...)
//
// Unset or empty, each falls back to the configure-time path, so a build that runs where it was
// configured (Windows CI) behaves exactly as before. Set them to run a build tree that was moved
// to another machine, a CrossOver bottle or an unpacked artifact.

#include <cstdlib>

#ifndef SCARLETTX_BINARY_DIR_DEFAULT
#error "SCARLETTX_BINARY_DIR_DEFAULT must be defined by CMake"
#endif
#ifndef SCARLETTX_SOURCE_DIR_DEFAULT
#error "SCARLETTX_SOURCE_DIR_DEFAULT must be defined by CMake"
#endif

namespace sx_test {

inline const char* EnvOr(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return (value && value[0] != '\0') ? value : fallback;
}

inline const char* BinaryDir() {
    return EnvOr("SCARLETTX_TEST_BINARY_DIR", SCARLETTX_BINARY_DIR_DEFAULT);
}

inline const char* SourceDir() {
    return EnvOr("SCARLETTX_TEST_SOURCE_DIR", SCARLETTX_SOURCE_DIR_DEFAULT);
}

}  // namespace sx_test

#define SCARLETTX_BINARY_DIR (::sx_test::BinaryDir())
#define SCARLETTX_SOURCE_DIR (::sx_test::SourceDir())
