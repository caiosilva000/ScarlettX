#include <cstdint>

extern "C" {
__declspec(dllimport) int32_t XGameRuntimeInitialize();
__declspec(dllimport) void XGameRuntimeUninitialize();
__declspec(dllimport) uint32_t XSystemGetDeviceType();

// Imported by ordinal #7 from xordinal_hle.dll (resolved via HleSymbolRegistry).
__declspec(dllimport) uint64_t XOrdinalCompute(uint64_t a, uint64_t b);

// Imported by name from xgameoverlay_missing.dll (auto-stubbed via x86-64 thunk).
__declspec(dllimport) uint64_t XOverlayShowNotice(uint64_t a, uint64_t b, uint64_t c, uint64_t d);

// Imported by ordinal #42 from xtelemetry_missing.dll (auto-stubbed via x86-64 thunk).
__declspec(dllimport) uint64_t XTelemetryRecord(uint64_t a, uint64_t b, uint64_t c, uint64_t d);

// Imported by name from pkg_local.dll, which exists only in the package folder.
__declspec(dllimport) uint64_t PkgLocalCombine(uint64_t a, uint64_t b);
}

namespace {

// Static pointer to static data forces a DIR64 base relocation in .reloc.
static const uint64_t kRelocMagicValue = 0x5CA81E7700001234ull;
static const uint64_t* const kRelocMagicPtr = &kRelocMagicValue;

}  // namespace

extern "C" {

__declspec(dllexport) uint64_t ProbeRelocatedGlobal() {
    return *kRelocMagicPtr;
}

// Unwinds a C++ exception through a guest frame; needs the image's unwind
// tables registered with the host. The helper has C++ linkage because MSVC
// assumes extern "C" functions never throw and drops the handler.
extern "C++" {
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void ProbeThrowHelper(int value) {
    throw value;
}
}

__declspec(dllexport) int ProbeThrowCatch(int value) {
    try {
        ProbeThrowHelper(value);
    } catch (int caught) {
        return caught + 1;
    }
    return -1;
}

// Per-thread storage of the guest image (implicit TLS on MSVC guests).
__declspec(dllexport) int ProbeThreadLocalCounter() {
    static thread_local int counter = 41;
    return ++counter;
}

__declspec(dllexport) uint64_t ProbeOrdinalHle(uint64_t a, uint64_t b) {
    return XOrdinalCompute(a, b);
}

__declspec(dllexport) uint64_t ProbeMissingNamedStub() {
    return XOverlayShowNotice(0x1111ull, 0x2222ull, 0x3333ull, 0x4444ull);
}

__declspec(dllexport) uint64_t ProbeMissingOrdinalStub() {
    return XTelemetryRecord(0xAAAAull, 0xBBBBull, 0xCCCCull, 0xDDDDull);
}

__declspec(dllexport) uint64_t ProbePackageDll(uint64_t a, uint64_t b) {
    return PkgLocalCombine(a, b);
}

__declspec(dllexport) int ScarlettGuestMain() {
    if (XGameRuntimeInitialize() != 0) {
        return 10;
    }
    const uint32_t dev_type = XSystemGetDeviceType();
    if (dev_type == 0) {
        XGameRuntimeUninitialize();
        return 11;
    }
    if (ProbeRelocatedGlobal() != kRelocMagicValue) {
        XGameRuntimeUninitialize();
        return 12;
    }
    const uint64_t stub1 = ProbeMissingNamedStub();
    const uint64_t stub2 = ProbeMissingOrdinalStub();
    XGameRuntimeUninitialize();
    return (stub1 == 0 && stub2 == 0) ? 0 : 13;
}

}  // extern "C"
