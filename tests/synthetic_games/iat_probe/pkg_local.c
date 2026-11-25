#include <stdint.h>

// A DLL that ships inside the package, next to the guest image, and nowhere on
// the host's DLL search path. The loader has to bind it from the package folder.
__declspec(dllexport) uint64_t PkgLocalCombine(uint64_t a, uint64_t b) {
    return a * 1000ull + b + 7ull;
}
