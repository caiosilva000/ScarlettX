// ScarlettX — xgameruntime QueryApiImpl tables.
//
// Slot layouts were read from the console runtime: each table is IUnknown
// followed by the listed methods, and every method takes the table as its
// first argument.
#include "xgr_api_tables.h"

#include "scarlettx/gdk/xgameruntime_abi.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <utility>

namespace scarlettx::xgr {
namespace {

struct Guid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
};

constexpr Guid kIidIUnknown = {0x00000000u, 0x0000u, 0x0000u, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
// XGameGetXboxTitleId.
constexpr Guid kApiGame = {0x973A344Eu, 0x24BFu, 0x4D0Fu, {0x84, 0x57, 0x56, 0xC5, 0x34, 0x89, 0x2B, 0x29}};
// XUser.
constexpr Guid kApiUser = {0x01ACD177u, 0x91F9u, 0x4763u, {0xA3, 0x8E, 0xCC, 0xBB, 0x55, 0xCE, 0x32, 0xE0}};
// Second interface of the XUser API: XUserGetGamertag with a component.
constexpr Guid kIidUserGamertag = {0xCEF4FAC0u, 0x7676u, 0x4A94u, {0xA1, 0x19, 0x4C, 0x43, 0xF9, 0xEB, 0x5B, 0x74}};
// XGameSave.
constexpr Guid kApiGameSave = {0x704C3F58u, 0xE629u, 0x4CC2u, {0xB1, 0x97, 0x30, 0x51, 0x1B, 0x99, 0x6F, 0xE2}};
// XSystemGetConsoleId, XSystemGetXboxLiveSandboxId.
constexpr Guid kApiSystem = {0xE349BD1Au, 0xFC20u, 0x4E40u, {0xB9, 0x9C, 0x41, 0x78, 0xCC, 0x6B, 0x40, 0x9F}};
// Interface ids a title built against a later GDK asks for instead of the
// API's own id. Its calls (XGame slot 3; XUser slots 3, 4, 7, 8, 9, 11, 23,
// 24, 25, 34) pass the same arguments as the main tables take.
constexpr Guid kIidGameLater = {0x2549F142u, 0x6419u, 0x4A06u, {0x97, 0xB5, 0x93, 0x1A, 0xAB, 0x7C, 0x2F, 0x34}};
constexpr Guid kIidUserLater = {0x079415E3u, 0x6727u, 0x437Fu, {0x8E, 0x9D, 0x8F, 0x8F, 0x9B, 0x24, 0x39, 0xF7}};
// For these the console object answers both ids with one table.
constexpr Guid kIidGameSaveLater = {0x704C3F58u, 0xE629u, 0x4CC2u, {0xB1, 0x97, 0x30, 0x51, 0x1B, 0x99, 0x6E, 0xE2}};
// Asked for by a title built against the October 2026 GDK (2610). Its first
// calls land on the same slots with the same arguments as the main table
// (4 InitializeProviderAsync, 5 InitializeProviderResult, 15
// EnumerateContainerInfoByName), so it is answered with that table. Slots the
// sample used to check this are the only ones verified.
constexpr Guid kIidGameSave2610 = {0x1BFFF3AFu, 0xF14Au, 0x40A3u, {0x8E, 0x35, 0x9A, 0xDA, 0x90, 0x65, 0x93, 0xF9}};
constexpr Guid kIidSystemLater = {0x67CE4BFCu, 0xB1D1u, 0x4AC7u, {0xBC, 0x3A, 0xCB, 0x92, 0x19, 0xA9, 0x7A, 0x85}};
// XGameRuntimeIsFeatureAvailable.
constexpr Guid kApiFeature = {0x8836FE87u, 0xEDB9u, 0x4FE3u, {0x8D, 0xAD, 0x05, 0xF0, 0xD2, 0xCD, 0x5B, 0x40}};
// XGameEventWrite.
constexpr Guid kApiGameEvent = {0xBBFBDCC7u, 0xBFE7u, 0x409Bu, {0xA5, 0xCA, 0xED, 0xF0, 0x54, 0x96, 0x0B, 0x4D}};
// XPackage, and the latest of the interface ids its console object answers.
constexpr Guid kApiPackage = {0xAF406016u, 0xE850u, 0x4AA8u, {0xA8, 0x8D, 0x2F, 0x3D, 0xCB, 0x9D, 0xAC, 0x7E}};
constexpr Guid kIidPackageLater = {0xF92D8712u, 0x2B27u, 0x4D8Au, {0xBF, 0x01, 0x11, 0xA6, 0xF8, 0xE3, 0xEB, 0x42}};

constexpr XGameResult kNoInterface = static_cast<XGameResult>(0x80004002u);
constexpr XGameResult kNotImplemented = static_cast<XGameResult>(0x80004001u);
constexpr XGameResult kInsufficientBuffer = static_cast<XGameResult>(0x8007007Au);
constexpr size_t kConsoleIdBytes = 39;      // XSystemConsoleIdBytes
constexpr size_t kSandboxIdMaxBytes = 16;   // XSystemXboxLiveSandboxIdMaxBytes

bool Equal(const void* a, const Guid& b) { return a && std::memcmp(a, &b, sizeof(Guid)) == 0; }

bool Trace() {
    const char* v = std::getenv("SCARLETTX_TRACE");
    return v && v[0] != '\0' && v[0] != '0';
}

using Slot = void (*)();

struct Table {
    const Slot* vtbl;
    std::atomic<uint32_t> refs{1};
    Guid api;
    Guid iid;
};

// The vtable for interface `riid` of API `api_id`, or nullptr.
const Slot* FindVtbl(const void* api_id, const void* riid, Guid* iid);
void* NewTable(const Slot* vtbl, const void* api_id, const Guid& iid);

XGameResult SCARLETTX_GDK_CALLBACK TableQueryInterface(void* self, const void* riid, void** out) {
    if (!out) return XGR_E_POINTER;
    *out = nullptr;
    if (!riid) return XGR_E_POINTER;
    auto* table = static_cast<Table*>(self);
    if (Equal(riid, table->iid) || Equal(riid, kIidIUnknown)) {
        table->refs.fetch_add(1, std::memory_order_relaxed);
        *out = self;
        return XGR_S_OK;
    }
    // Another interface of the same API is a separate table object.
    Guid iid{};
    const Slot* vtbl = FindVtbl(&table->api, riid, &iid);
    if (!vtbl) return kNoInterface;
    *out = NewTable(vtbl, &table->api, iid);
    return *out ? XGR_S_OK : XGR_E_OUTOFMEMORY;
}

uint32_t SCARLETTX_GDK_CALLBACK TableAddRef(void* self) {
    return static_cast<Table*>(self)->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

uint32_t SCARLETTX_GDK_CALLBACK TableRelease(void* self) {
    auto* table = static_cast<Table*>(self);
    const uint32_t left = table->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) delete table;
    return left;
}

// {973A344E} slot 3.
XGameResult SCARLETTX_GDK_CALLBACK GameGetXboxTitleId(void* /*self*/, uint32_t* title_id) {
    const XGameResult hr = XGameGetXboxTitleId(title_id);
    if (Trace()) {
        std::fprintf(stderr, "[ScarlettX][xgameruntime] XGameGetXboxTitleId -> 0x%08X (0x%08X)\n",
                     static_cast<unsigned>(hr), title_id ? static_cast<unsigned>(*title_id) : 0u);
    }
    return hr;
}

// {E349BD1A} slot 3. The console always answers with exactly 39 bytes.
XGameResult SCARLETTX_GDK_CALLBACK SystemGetConsoleId(void* /*self*/, size_t size, char* buffer, size_t* used) {
    if (!buffer) return XGR_E_POINTER;
    if (size < kConsoleIdBytes) return kInsufficientBuffer;
    char id[kConsoleIdBytes] = {};
    size_t id_used = 0;
    const XGameResult hr = XSystemGetConsoleId(sizeof(id), id, &id_used);
    if (hr != XGR_S_OK) return hr;
    for (size_t i = std::strlen(id); i + 1 < kConsoleIdBytes; ++i) id[i] = '0';
    id[kConsoleIdBytes - 1] = '\0';
    std::memcpy(buffer, id, kConsoleIdBytes);
    if (used) *used = kConsoleIdBytes;
    return XGR_S_OK;
}

// {E349BD1A} slot 4.
XGameResult SCARLETTX_GDK_CALLBACK SystemGetXboxLiveSandboxId(void* /*self*/, size_t size, char* buffer,
                                                              size_t* used) {
    if (!buffer) return XGR_E_POINTER;
    if (size < kSandboxIdMaxBytes) return kInsufficientBuffer;
    return XSystemGetXboxLiveSandboxId(size, buffer, used);
}

const Slot kGameVtbl[] = {
    reinterpret_cast<Slot>(&TableQueryInterface),
    reinterpret_cast<Slot>(&TableAddRef),
    reinterpret_cast<Slot>(&TableRelease),
    reinterpret_cast<Slot>(&GameGetXboxTitleId),
};

const Slot kSystemVtbl[] = {
    reinterpret_cast<Slot>(&TableQueryInterface),
    reinterpret_cast<Slot>(&TableAddRef),
    reinterpret_cast<Slot>(&TableRelease),
    reinterpret_cast<Slot>(&SystemGetConsoleId),
    reinterpret_cast<Slot>(&SystemGetXboxLiveSandboxId),
};

// {01ACD177}: XUser, in the declaration order of the public XUser.h. The
// console table was matched against that order at slots 5, 6, 9, 13, 22, 30,
// 32, 33 and 34; slots without an implementation log their name and fail.
constexpr int kUserSlots = 37;
constexpr const char* kUserSlotNames[kUserSlots] = {
    "QueryInterface", "AddRef", "Release",
    "XUserDuplicateHandle", "XUserCloseHandle", "XUserCompare", "XUserGetMaxUsers",
    "XUserAddAsync", "XUserAddResult", "XUserGetLocalId", "XUserFindUserByLocalId",
    "XUserGetId", "XUserFindUserById", "XUserGetIsGuest", "XUserGetState",
    "XUserGetGamertag", "XUserGetGamerPictureAsync", "XUserGetGamerPictureResultSize",
    "XUserGetGamerPictureResult", "XUserGetAgeGroup", "XUserCheckPrivilege",
    "XUserResolvePrivilegeWithUiAsync", "XUserResolvePrivilegeWithUiResult",
    "XUserGetTokenAndSignatureAsync", "XUserGetTokenAndSignatureResultSize",
    "XUserGetTokenAndSignatureResult", "XUserGetTokenAndSignatureUtf16Async",
    "XUserGetTokenAndSignatureUtf16ResultSize", "XUserGetTokenAndSignatureUtf16Result",
    "XUserResolveIssueWithUiAsync", "XUserResolveIssueWithUiResult",
    "XUserResolveIssueWithUiUtf16Async", "XUserResolveIssueWithUiUtf16Result",
    "XUserRegisterForChangeEvent", "XUserUnregisterForChangeEvent",
    "XUserGetSignOutDeferral", "XUserCloseSignOutDeferralHandle",
};

XGameResult LogUnimplemented(const char* name, const char* api, int slot, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    std::fprintf(stderr, "[ScarlettX][xgameruntime] %s (ApiImpl{%s} slot %d)(0x%llx, 0x%llx, 0x%llx) -> E_NOTIMPL\n",
                 name, api, slot, static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                 static_cast<unsigned long long>(a3));
    std::fflush(stderr);
    return kNotImplemented;
}

template <int Index>
XGameResult SCARLETTX_GDK_CALLBACK UserUnimplemented(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    return LogUnimplemented(kUserSlotNames[Index], "01ACD177", Index, a1, a2, a3);
}

XGameResult SCARLETTX_GDK_CALLBACK UserDuplicateHandle(void*, XUserHandle user, XUserHandle* duplicate) {
    return XUserDuplicateHandle(user, duplicate);
}

void SCARLETTX_GDK_CALLBACK UserCloseHandle(void*, XUserHandle user) { XUserCloseHandle(user); }

// Zero when both handles refer to the same user.
int32_t SCARLETTX_GDK_CALLBACK UserCompare(void*, XUserHandle a, XUserHandle b) {
    if (a == b) return 0;
    uint64_t id_a = 0;
    uint64_t id_b = 0;
    const bool has_a = a && XUserGetId(a, &id_a) == XGR_S_OK;
    const bool has_b = b && XUserGetId(b, &id_b) == XGR_S_OK;
    if (has_a != has_b) return has_a ? 1 : -1;
    return id_a == id_b ? 0 : (id_a < id_b ? -1 : 1);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetMaxUsers(void*, uint32_t* max_users) { return XUserGetMaxUsers(max_users); }

XGameResult SCARLETTX_GDK_CALLBACK UserAddAsync(void*, XUserAddOptions options, XAsyncBlock* async) {
    const XGameResult hr = XUserAddAsync(options, async);
    if (Trace()) {
        std::fprintf(stderr, "[ScarlettX][xgameruntime] XUserAddAsync(options=%u, async=%p) -> 0x%08X\n",
                     static_cast<unsigned>(options), static_cast<void*>(async), static_cast<unsigned>(hr));
    }
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK UserAddResult(void*, XAsyncBlock* async, XUserHandle* user) {
    const XGameResult hr = XUserAddResult(async, user);
    if (Trace()) {
        std::fprintf(stderr, "[ScarlettX][xgameruntime] XUserAddResult(async=%p) -> 0x%08X\n",
                     static_cast<void*>(async), static_cast<unsigned>(hr));
    }
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetLocalId(void*, XUserHandle user, XUserLocalId* local_id) {
    return XUserGetLocalId(user, local_id);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetId(void*, XUserHandle user, uint64_t* id) { return XUserGetId(user, id); }

XGameResult SCARLETTX_GDK_CALLBACK UserGetIsGuest(void*, XUserHandle user, bool* is_guest) {
    return XUserGetIsGuest(user, is_guest);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetState(void*, XUserHandle user, XUserState* state) {
    return XUserGetState(user, state);
}

// {CEF4FAC0} slot 3. The console checks the buffer against the component's
// maximum size, whatever the actual gamertag length.
XGameResult SCARLETTX_GDK_CALLBACK UserGetGamertagComponent(void*, XUserHandle user, uint32_t component, size_t size,
                                                            char* gamertag, size_t* used) {
    constexpr size_t kMaxBytes[] = {16, 97, 15, 101};  // classic, modern, modern suffix, unique modern
    if (used) *used = 0;
    if (component > 3) return XGR_E_INVALIDARG;
    if (size < kMaxBytes[component]) return kInsufficientBuffer;
    return XUserGetGamertag(user, static_cast<XUserGamertagComponent>(component), size, gamertag, used);
}

// {01ACD177} slot 15: the original signature without a component; the
// console forwards it to the method above with the classic component.
XGameResult SCARLETTX_GDK_CALLBACK UserGetGamertag(void* self, XUserHandle user, size_t size, char* gamertag,
                                                   size_t* used) {
    return UserGetGamertagComponent(self, user, 0, size, gamertag, used);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetGamerPictureAsync(void*, XUserHandle user, uint32_t size, XAsyncBlock* async) {
    const XGameResult hr = XUserGetGamerPictureAsync(user, static_cast<XUserGamerPictureSize>(size), async);
    if (Trace()) {
        std::fprintf(stderr, "[ScarlettX][xgameruntime] XUserGetGamerPictureAsync(size=%u, async=%p) -> 0x%08X\n",
                     static_cast<unsigned>(size), static_cast<void*>(async), static_cast<unsigned>(hr));
    }
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetGamerPictureResultSize(void*, XAsyncBlock* async, size_t* size) {
    return XUserGetGamerPictureResultSize(async, size);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetGamerPictureResult(void*, XAsyncBlock* async, size_t size, void* buffer,
                                                             size_t* used) {
    return XUserGetGamerPictureResult(async, size, buffer, used);
}

XGameResult SCARLETTX_GDK_CALLBACK UserGetAgeGroup(void*, XUserHandle user, XUserAgeGroup* age_group) {
    return XUserGetAgeGroup(user, age_group);
}

XGameResult SCARLETTX_GDK_CALLBACK UserCheckPrivilege(void*, XUserHandle user, XUserPrivilegeOptions options,
                                                      XUserPrivilege privilege, bool* has_privilege,
                                                      XUserPrivilegeDenyReason* reason) {
    return XUserCheckPrivilege(user, options, privilege, has_privilege, reason);
}

// The signed-in user never changes in the HLE, so registrations only hand
// out a token; the callback is never invoked.
std::atomic<uint64_t> g_user_change_tokens{0};

XGameResult SCARLETTX_GDK_CALLBACK UserRegisterForChangeEvent(void*, XTaskQueueHandle /*queue*/, void* /*context*/,
                                                              void* callback, XTaskQueueRegistrationToken* token) {
    if (!callback || !token) return XGR_E_INVALIDARG;
    std::memset(token, 0, sizeof(*token));
    const uint64_t id = g_user_change_tokens.fetch_add(1, std::memory_order_relaxed) + 1;
    std::memcpy(token, &id, sizeof(id) < sizeof(*token) ? sizeof(id) : sizeof(*token));
    return XGR_S_OK;
}

bool SCARLETTX_GDK_CALLBACK UserUnregisterForChangeEvent(void*, XTaskQueueRegistrationToken token, bool /*wait*/) {
    uint64_t id = 0;
    std::memcpy(&id, &token, sizeof(id) < sizeof(token) ? sizeof(id) : sizeof(token));
    return id != 0 && id <= g_user_change_tokens.load(std::memory_order_relaxed);
}

template <size_t... I>
std::array<Slot, kUserSlots> MakeUserVtbl(std::index_sequence<I...>) {
    std::array<Slot, kUserSlots> vtbl = {reinterpret_cast<Slot>(&UserUnimplemented<static_cast<int>(I)>)...};
    vtbl[0] = reinterpret_cast<Slot>(&TableQueryInterface);
    vtbl[1] = reinterpret_cast<Slot>(&TableAddRef);
    vtbl[2] = reinterpret_cast<Slot>(&TableRelease);
    vtbl[3] = reinterpret_cast<Slot>(&UserDuplicateHandle);
    vtbl[4] = reinterpret_cast<Slot>(&UserCloseHandle);
    vtbl[5] = reinterpret_cast<Slot>(&UserCompare);
    vtbl[6] = reinterpret_cast<Slot>(&UserGetMaxUsers);
    vtbl[7] = reinterpret_cast<Slot>(&UserAddAsync);
    vtbl[8] = reinterpret_cast<Slot>(&UserAddResult);
    vtbl[9] = reinterpret_cast<Slot>(&UserGetLocalId);
    vtbl[11] = reinterpret_cast<Slot>(&UserGetId);
    vtbl[13] = reinterpret_cast<Slot>(&UserGetIsGuest);
    vtbl[14] = reinterpret_cast<Slot>(&UserGetState);
    vtbl[15] = reinterpret_cast<Slot>(&UserGetGamertag);
    vtbl[16] = reinterpret_cast<Slot>(&UserGetGamerPictureAsync);
    vtbl[17] = reinterpret_cast<Slot>(&UserGetGamerPictureResultSize);
    vtbl[18] = reinterpret_cast<Slot>(&UserGetGamerPictureResult);
    vtbl[19] = reinterpret_cast<Slot>(&UserGetAgeGroup);
    vtbl[20] = reinterpret_cast<Slot>(&UserCheckPrivilege);
    vtbl[33] = reinterpret_cast<Slot>(&UserRegisterForChangeEvent);
    vtbl[34] = reinterpret_cast<Slot>(&UserUnregisterForChangeEvent);
    return vtbl;
}
const std::array<Slot, kUserSlots> kUserVtbl = MakeUserVtbl(std::make_index_sequence<kUserSlots>{});

const Slot kUserGamertagVtbl[] = {
    reinterpret_cast<Slot>(&TableQueryInterface),
    reinterpret_cast<Slot>(&TableAddRef),
    reinterpret_cast<Slot>(&TableRelease),
    reinterpret_cast<Slot>(&UserGetGamertagComponent),
};

// {704C3F58}: XGameSave, in the declaration order of the public XGameSave.h.
// The console code names slots 3, 7, 13, 14, 15, 18, 19 and 20 in its error
// reports, and the order of the others was matched by argument shape.
// Slots 30-32 are XGameSaveFiles.h in the same way: the console names slot
// 32, and a title's calls to 30 and 31 pass the Async/Result arguments.
constexpr int kGameSaveSlots = 33;
constexpr const char* kGameSaveSlotNames[kGameSaveSlots] = {
    "QueryInterface", "AddRef", "Release",
    "XGameSaveInitializeProvider", "XGameSaveInitializeProviderAsync", "XGameSaveInitializeProviderResult",
    "XGameSaveCloseProvider", "XGameSaveGetRemainingQuota", "XGameSaveGetRemainingQuotaAsync",
    "XGameSaveGetRemainingQuotaResult", "XGameSaveDeleteContainer", "XGameSaveDeleteContainerAsync",
    "XGameSaveDeleteContainerResult", "XGameSaveGetContainerInfo", "XGameSaveEnumerateContainerInfo",
    "XGameSaveEnumerateContainerInfoByName", "XGameSaveCreateContainer", "XGameSaveCloseContainer",
    "XGameSaveEnumerateBlobInfo", "XGameSaveEnumerateBlobInfoByName", "XGameSaveReadBlobData",
    "XGameSaveReadBlobDataAsync", "XGameSaveReadBlobDataResult", "XGameSaveCreateUpdate",
    "XGameSaveCloseUpdate", "XGameSaveSubmitBlobWrite", "XGameSaveSubmitBlobDelete",
    "XGameSaveSubmitUpdate", "XGameSaveSubmitUpdateAsync", "XGameSaveSubmitUpdateResult",
    "XGameSaveFilesGetFolderWithUiAsync", "XGameSaveFilesGetFolderWithUiResult",
    "XGameSaveFilesGetRemainingQuota",
};

template <int Index>
XGameResult SCARLETTX_GDK_CALLBACK GameSaveUnimplemented(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    return LogUnimplemented(kGameSaveSlotNames[Index], "704C3F58", Index, a1, a2, a3);
}

void TraceGameSave(const char* name, XGameResult hr) {
    if (!Trace()) return;
    std::fprintf(stderr, "[ScarlettX][xgameruntime] %s -> 0x%08X\n", name, static_cast<unsigned>(hr));
}

XGameResult SCARLETTX_GDK_CALLBACK SaveInitializeProvider(void*, XUserHandle user, const char* configuration_id,
                                                          bool sync_on_demand, XGameSaveProviderHandle* provider) {
    const XGameResult hr = XGameSaveInitializeProvider(user, configuration_id, sync_on_demand, provider);
    TraceGameSave("XGameSaveInitializeProvider", hr);
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK SaveInitializeProviderAsync(void*, XUserHandle user, const char* configuration_id,
                                                               bool sync_on_demand, XAsyncBlock* async) {
    const XGameResult hr = XGameSaveInitializeProviderAsync(user, configuration_id, sync_on_demand, async);
    TraceGameSave("XGameSaveInitializeProviderAsync", hr);
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK SaveInitializeProviderResult(void*, XAsyncBlock* async,
                                                                XGameSaveProviderHandle* provider) {
    return XGameSaveInitializeProviderResult(async, provider);
}

void SCARLETTX_GDK_CALLBACK SaveCloseProvider(void*, XGameSaveProviderHandle provider) {
    XGameSaveCloseProvider(provider);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveGetRemainingQuota(void*, XGameSaveProviderHandle provider, int64_t* quota) {
    return XGameSaveGetRemainingQuota(provider, quota);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveDeleteContainer(void*, XGameSaveProviderHandle provider, const char* name) {
    const XGameResult hr = XGameSaveDeleteContainer(provider, name);
    TraceGameSave("XGameSaveDeleteContainer", hr);
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK SaveEnumerateContainerInfo(void*, XGameSaveProviderHandle provider, void* context,
                                                              XGameSaveContainerInfoCallback* callback) {
    const XGameResult hr = XGameSaveEnumerateContainerInfo(provider, context, callback);
    TraceGameSave("XGameSaveEnumerateContainerInfo", hr);
    return hr;
}

// Slots 13, 15 and 19 filter the full enumeration by name or name prefix. The
// enumeration reports stored names, so the filter compares in that form.
template <typename Info, typename Callback>
struct SaveNameFilter {
    std::string name;
    bool exact;
    void* context;
    Callback* callback;

    static bool SCARLETTX_GDK_CALLBACK Thunk(const Info* info, void* filter_ptr) {
        auto* filter = static_cast<SaveNameFilter*>(filter_ptr);
        const char* name = info && info->name ? info->name : "";
        const bool match = filter->exact ? filter->name == name
                                         : std::strncmp(name, filter->name.c_str(), filter->name.size()) == 0;
        return match ? filter->callback(info, filter->context) : true;
    }
};
using SaveContainerFilter = SaveNameFilter<XGameSaveContainerInfo, XGameSaveContainerInfoCallback>;
using SaveBlobFilter = SaveNameFilter<XGameSaveBlobInfo, XGameSaveBlobInfoCallback>;

// An empty prefix matches every name.
std::string StoredSavePrefix(const char* prefix) {
    return prefix && *prefix ? StoredSaveName(prefix) : std::string();
}

XGameResult SCARLETTX_GDK_CALLBACK SaveGetContainerInfo(void*, XGameSaveProviderHandle provider, const char* name,
                                                        void* context, XGameSaveContainerInfoCallback* callback) {
    if (!name || !callback) return XGR_E_INVALIDARG;
    SaveContainerFilter filter{StoredSaveName(name), true, context, callback};
    return XGameSaveEnumerateContainerInfo(provider, &filter, &SaveContainerFilter::Thunk);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveEnumerateContainerInfoByName(void*, XGameSaveProviderHandle provider,
                                                                    const char* prefix, void* context,
                                                                    XGameSaveContainerInfoCallback* callback) {
    if (!callback) return XGR_E_INVALIDARG;
    SaveContainerFilter filter{StoredSavePrefix(prefix), false, context, callback};
    return XGameSaveEnumerateContainerInfo(provider, &filter, &SaveContainerFilter::Thunk);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveCreateContainer(void*, XGameSaveProviderHandle provider, const char* name,
                                                       XGameSaveContainerHandle* container) {
    const XGameResult hr = XGameSaveCreateContainer(provider, name, container);
    TraceGameSave("XGameSaveCreateContainer", hr);
    return hr;
}

void SCARLETTX_GDK_CALLBACK SaveCloseContainer(void*, XGameSaveContainerHandle container) {
    XGameSaveCloseContainer(container);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveEnumerateBlobInfo(void*, XGameSaveContainerHandle container, void* context,
                                                         XGameSaveBlobInfoCallback* callback) {
    return XGameSaveEnumerateBlobInfo(container, context, callback);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveEnumerateBlobInfoByName(void*, XGameSaveContainerHandle container,
                                                               const char* prefix, void* context,
                                                               XGameSaveBlobInfoCallback* callback) {
    if (!callback) return XGR_E_INVALIDARG;
    SaveBlobFilter filter{StoredSavePrefix(prefix), false, context, callback};
    return XGameSaveEnumerateBlobInfo(container, &filter, &SaveBlobFilter::Thunk);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveReadBlobData(void*, XGameSaveContainerHandle container, const char** names,
                                                    uint32_t* count, size_t blobs_size, XGameSaveBlob* blobs) {
    const XGameResult hr = XGameSaveReadBlobData(container, names, count, blobs_size, blobs);
    TraceGameSave("XGameSaveReadBlobData", hr);
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK SaveReadBlobDataAsync(void*, XGameSaveContainerHandle container,
                                                         const char** names, uint32_t count, XAsyncBlock* async) {
    const XGameResult hr = XGameSaveReadBlobDataAsync(container, names, count, async);
    TraceGameSave("XGameSaveReadBlobDataAsync", hr);
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK SaveReadBlobDataResult(void*, XAsyncBlock* async, size_t blobs_size,
                                                          XGameSaveBlob* blobs, uint32_t* count) {
    return XGameSaveReadBlobDataResult(async, blobs_size, blobs, count);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveCreateUpdate(void*, XGameSaveContainerHandle container,
                                                    const char* display_name, XGameSaveUpdateHandle* update) {
    return XGameSaveCreateUpdate(container, display_name, update);
}

void SCARLETTX_GDK_CALLBACK SaveCloseUpdate(void*, XGameSaveUpdateHandle update) { XGameSaveCloseUpdate(update); }

XGameResult SCARLETTX_GDK_CALLBACK SaveSubmitBlobWrite(void*, XGameSaveUpdateHandle update, const char* blob_name,
                                                       const uint8_t* data, size_t byte_count) {
    return XGameSaveSubmitBlobWrite(update, blob_name, data, byte_count);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveSubmitBlobDelete(void*, XGameSaveUpdateHandle update, const char* blob_name) {
    return XGameSaveSubmitBlobDelete(update, blob_name);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveSubmitUpdate(void*, XGameSaveUpdateHandle update) {
    const XGameResult hr = XGameSaveSubmitUpdate(update);
    TraceGameSave("XGameSaveSubmitUpdate", hr);
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK SaveSubmitUpdateAsync(void*, XGameSaveUpdateHandle update, XAsyncBlock* async) {
    const XGameResult hr = XGameSaveSubmitUpdateAsync(update, async);
    TraceGameSave("XGameSaveSubmitUpdateAsync", hr);
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK SaveSubmitUpdateResult(void*, XAsyncBlock* async) {
    return XGameSaveSubmitUpdateResult(async);
}

XGameResult SCARLETTX_GDK_CALLBACK SaveFilesGetFolderWithUiAsync(void*, XUserHandle user, const char* configuration_id,
                                                                 XAsyncBlock* async) {
    const XGameResult hr = XGameSaveFilesGetFolderWithUiAsync(user, configuration_id, async);
    TraceGameSave("XGameSaveFilesGetFolderWithUiAsync", hr);
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK SaveFilesGetFolderWithUiResult(void*, XAsyncBlock* async, size_t folder_size,
                                                                  char* folder) {
    const XGameResult hr = XGameSaveFilesGetFolderWithUiResult(async, folder_size, folder);
    if (Trace()) {
        std::fprintf(stderr, "[ScarlettX][xgameruntime] XGameSaveFilesGetFolderWithUiResult -> 0x%08X (%s)\n",
                     static_cast<unsigned>(hr), hr == XGR_S_OK ? folder : "");
    }
    return hr;
}

XGameResult SCARLETTX_GDK_CALLBACK SaveFilesGetRemainingQuota(void*, XUserHandle user, const char* configuration_id,
                                                              int64_t* quota) {
    return XGameSaveFilesGetRemainingQuota(user, configuration_id, quota);
}

template <size_t... I>
std::array<Slot, kGameSaveSlots> MakeGameSaveVtbl(std::index_sequence<I...>) {
    std::array<Slot, kGameSaveSlots> vtbl = {reinterpret_cast<Slot>(&GameSaveUnimplemented<static_cast<int>(I)>)...};
    vtbl[0] = reinterpret_cast<Slot>(&TableQueryInterface);
    vtbl[1] = reinterpret_cast<Slot>(&TableAddRef);
    vtbl[2] = reinterpret_cast<Slot>(&TableRelease);
    vtbl[3] = reinterpret_cast<Slot>(&SaveInitializeProvider);
    vtbl[4] = reinterpret_cast<Slot>(&SaveInitializeProviderAsync);
    vtbl[5] = reinterpret_cast<Slot>(&SaveInitializeProviderResult);
    vtbl[6] = reinterpret_cast<Slot>(&SaveCloseProvider);
    vtbl[7] = reinterpret_cast<Slot>(&SaveGetRemainingQuota);
    vtbl[10] = reinterpret_cast<Slot>(&SaveDeleteContainer);
    vtbl[13] = reinterpret_cast<Slot>(&SaveGetContainerInfo);
    vtbl[14] = reinterpret_cast<Slot>(&SaveEnumerateContainerInfo);
    vtbl[15] = reinterpret_cast<Slot>(&SaveEnumerateContainerInfoByName);
    vtbl[16] = reinterpret_cast<Slot>(&SaveCreateContainer);
    vtbl[17] = reinterpret_cast<Slot>(&SaveCloseContainer);
    vtbl[18] = reinterpret_cast<Slot>(&SaveEnumerateBlobInfo);
    vtbl[19] = reinterpret_cast<Slot>(&SaveEnumerateBlobInfoByName);
    vtbl[20] = reinterpret_cast<Slot>(&SaveReadBlobData);
    vtbl[21] = reinterpret_cast<Slot>(&SaveReadBlobDataAsync);
    vtbl[22] = reinterpret_cast<Slot>(&SaveReadBlobDataResult);
    vtbl[23] = reinterpret_cast<Slot>(&SaveCreateUpdate);
    vtbl[24] = reinterpret_cast<Slot>(&SaveCloseUpdate);
    vtbl[25] = reinterpret_cast<Slot>(&SaveSubmitBlobWrite);
    vtbl[26] = reinterpret_cast<Slot>(&SaveSubmitBlobDelete);
    vtbl[27] = reinterpret_cast<Slot>(&SaveSubmitUpdate);
    vtbl[28] = reinterpret_cast<Slot>(&SaveSubmitUpdateAsync);
    vtbl[29] = reinterpret_cast<Slot>(&SaveSubmitUpdateResult);
    vtbl[30] = reinterpret_cast<Slot>(&SaveFilesGetFolderWithUiAsync);
    vtbl[31] = reinterpret_cast<Slot>(&SaveFilesGetFolderWithUiResult);
    vtbl[32] = reinterpret_cast<Slot>(&SaveFilesGetRemainingQuota);
    return vtbl;
}
const std::array<Slot, kGameSaveSlots> kGameSaveVtbl = MakeGameSaveVtbl(std::make_index_sequence<kGameSaveSlots>{});

// {8836FE87} slot 3, the table's only method. The console knows features 0
// to 21 (the public XGameRuntimeFeature values) and has them all; 10,
// XNetworking, depends on its network stack being up, which it is here.
constexpr uint32_t kLastRuntimeFeature = 21;

bool SCARLETTX_GDK_CALLBACK FeatureIsAvailable(void* /*self*/, uint32_t feature) {
    const bool available = feature <= kLastRuntimeFeature;
    if (Trace()) {
        std::fprintf(stderr, "[ScarlettX][xgameruntime] XGameRuntimeIsFeatureAvailable(%u) -> %d\n",
                     static_cast<unsigned>(feature), available ? 1 : 0);
    }
    return available;
}

const Slot kFeatureVtbl[] = {
    reinterpret_cast<Slot>(&TableQueryInterface),
    reinterpret_cast<Slot>(&TableAddRef),
    reinterpret_cast<Slot>(&TableRelease),
    reinterpret_cast<Slot>(&FeatureIsAvailable),
};

// {BBFBDCC7} slot 3, the table's only method. There is no service to send
// the event to, so it is accepted and dropped.
XGameResult SCARLETTX_GDK_CALLBACK GameEventWrite(void* /*self*/, XUserHandle user, const char* /*service_config_id*/,
                                                  const char* /*play_session_id*/, const char* event_name,
                                                  const char* /*dimensions_json*/, const char* /*measurements_json*/) {
    const XGameResult hr = user && event_name ? XGR_S_OK : XGR_E_INVALIDARG;
    if (Trace()) {
        std::fprintf(stderr, "[ScarlettX][xgameruntime] XGameEventWrite(%s) -> 0x%08X\n",
                     event_name ? event_name : "<null>", static_cast<unsigned>(hr));
    }
    return hr;
}

const Slot kGameEventVtbl[] = {
    reinterpret_cast<Slot>(&TableQueryInterface),
    reinterpret_cast<Slot>(&TableAddRef),
    reinterpret_cast<Slot>(&TableRelease),
    reinterpret_cast<Slot>(&GameEventWrite),
};

// {AF406016}: XPackage. The console object answers seven interface ids with
// one 39-entry table. Three methods are identified, by a title's calls and
// by the console code behind the slot; in the console table 20, 28 and 35
// are one function, as are 21, 29 and 36. The rest log the call and fail.
//   11       XPackageGetUserLocale(size, buffer)
//   20/28/35 XPackageEnumeratePackages(kind, scope, context, callback)
//   21/29/36 XPackageRegisterPackageInstalled(queue, context, callback, token)
constexpr int kPackageSlots = 39;

template <int Index>
XGameResult SCARLETTX_GDK_CALLBACK PackageUnimplemented(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    return LogUnimplemented("XPackage", "AF406016", Index, a1, a2, a3);
}

XGameResult SCARLETTX_GDK_CALLBACK PackageGetUserLocale(void* /*self*/, size_t size, char* locale) {
    const XGameResult hr = XPackageGetUserLocale(size, locale);
    if (Trace()) {
        std::fprintf(stderr, "[ScarlettX][xgameruntime] XPackageGetUserLocale -> 0x%08X (%s)\n",
                     static_cast<unsigned>(hr), hr == XGR_S_OK ? locale : "");
    }
    return hr;
}

// Only the running package is installed, and it is not reported: a title
// enumerates to find its add-on content, and there is none.
XGameResult SCARLETTX_GDK_CALLBACK PackageEnumeratePackages(void* /*self*/, uint32_t kind, uint32_t scope,
                                                            void* /*context*/, void* callback) {
    if (Trace()) {
        std::fprintf(stderr, "[ScarlettX][xgameruntime] XPackageEnumeratePackages(kind=%u, scope=%u) -> no packages\n",
                     static_cast<unsigned>(kind), static_cast<unsigned>(scope));
    }
    return callback ? XGR_S_OK : XGR_E_INVALIDARG;
}

// Nothing is ever installed while the title runs, so the callback is never
// invoked; the registration only hands out a token.
std::atomic<uint64_t> g_package_installed_tokens{0};

XGameResult SCARLETTX_GDK_CALLBACK PackageRegisterPackageInstalled(void* /*self*/, XTaskQueueHandle /*queue*/,
                                                                   void* /*context*/, void* callback,
                                                                   XTaskQueueRegistrationToken* token) {
    if (!callback || !token) return XGR_E_INVALIDARG;
    token->token = g_package_installed_tokens.fetch_add(1, std::memory_order_relaxed) + 1;
    return XGR_S_OK;
}

template <size_t... I>
std::array<Slot, kPackageSlots> MakePackageVtbl(std::index_sequence<I...>) {
    std::array<Slot, kPackageSlots> vtbl = {reinterpret_cast<Slot>(&PackageUnimplemented<static_cast<int>(I)>)...};
    vtbl[0] = reinterpret_cast<Slot>(&TableQueryInterface);
    vtbl[1] = reinterpret_cast<Slot>(&TableAddRef);
    vtbl[2] = reinterpret_cast<Slot>(&TableRelease);
    vtbl[11] = reinterpret_cast<Slot>(&PackageGetUserLocale);
    for (int slot : {20, 28, 35}) vtbl[slot] = reinterpret_cast<Slot>(&PackageEnumeratePackages);
    for (int slot : {21, 29, 36}) vtbl[slot] = reinterpret_cast<Slot>(&PackageRegisterPackageInstalled);
    return vtbl;
}
const std::array<Slot, kPackageSlots> kPackageVtbl = MakePackageVtbl(std::make_index_sequence<kPackageSlots>{});

// Discovery table for API ids nothing implements, enabled with
// SCARLETTX_XGR_DISCOVER=1: every method logs its slot and first arguments
// and fails, so a trace shows which slots a title uses.
constexpr int kDiscoverySlots = 96;

template <int Index>
XGameResult SCARLETTX_GDK_CALLBACK DiscoverySlot(void* self, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    char iid[40] = {};
    const Guid& g = static_cast<Table*>(self)->iid;
    std::snprintf(iid, sizeof(iid), "%08X-%04X-%04X", static_cast<unsigned>(g.data1), g.data2, g.data3);
    std::fprintf(stderr,
                 "[ScarlettX][xgameruntime] ApiImpl{%s}::+0x%X (slot %d)(a1=0x%llx, a2=0x%llx, a3=0x%llx) -> E_NOTIMPL\n",
                 iid, Index * 8, Index, static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                 static_cast<unsigned long long>(a3));
    std::fflush(stderr);
    return kNotImplemented;
}

template <size_t... I>
constexpr std::array<Slot, kDiscoverySlots> MakeDiscoveryVtbl(std::index_sequence<I...>) {
    return {reinterpret_cast<Slot>(&TableQueryInterface), reinterpret_cast<Slot>(&TableAddRef),
            reinterpret_cast<Slot>(&TableRelease), reinterpret_cast<Slot>(&DiscoverySlot<static_cast<int>(I) + 3>)...};
}
const std::array<Slot, kDiscoverySlots> kDiscoveryVtbl = MakeDiscoveryVtbl(std::make_index_sequence<kDiscoverySlots - 3>{});

bool DiscoveryEnabled() {
    const char* v = std::getenv("SCARLETTX_XGR_DISCOVER");
    return v && v[0] != '\0' && v[0] != '0';
}

void* NewTable(const Slot* vtbl, const void* api_id, const Guid& iid) {
    auto* table = new (std::nothrow) Table();
    if (!table) return nullptr;
    table->vtbl = vtbl;
    std::memcpy(&table->api, api_id, sizeof(Guid));
    table->iid = iid;
    return table;
}

const Slot* FindVtbl(const void* api_id, const void* riid, Guid* iid) {
    // An API's main interface has the API's own id.
    const bool main = std::memcmp(api_id, riid, sizeof(Guid)) == 0 || Equal(riid, kIidIUnknown);
    std::memcpy(iid, main ? api_id : riid, sizeof(Guid));
    if (Equal(api_id, kApiGame)) return main || Equal(riid, kIidGameLater) ? kGameVtbl : nullptr;
    if (Equal(api_id, kApiSystem)) return main || Equal(riid, kIidSystemLater) ? kSystemVtbl : nullptr;
    if (Equal(api_id, kApiGameSave)) {
        return main || Equal(riid, kIidGameSaveLater) || Equal(riid, kIidGameSave2610) ? kGameSaveVtbl.data() : nullptr;
    }
    if (Equal(api_id, kApiUser)) {
        if (main || Equal(riid, kIidUserLater)) return kUserVtbl.data();
        return Equal(riid, kIidUserGamertag) ? kUserGamertagVtbl : nullptr;
    }
    if (Equal(api_id, kApiFeature)) return main ? kFeatureVtbl : nullptr;
    if (Equal(api_id, kApiGameEvent)) return main ? kGameEventVtbl : nullptr;
    if (Equal(api_id, kApiPackage)) return main || Equal(riid, kIidPackageLater) ? kPackageVtbl.data() : nullptr;
    // An API nothing implements: titles ask for it by a later interface id
    // as often as by its own.
    return DiscoveryEnabled() ? kDiscoveryVtbl.data() : nullptr;
}

}  // namespace

void* CreateApiTable(const void* api_id, const void* riid) {
    if (!api_id || !riid) return nullptr;
    Guid iid{};
    const Slot* vtbl = FindVtbl(api_id, riid, &iid);
    return vtbl ? NewTable(vtbl, api_id, iid) : nullptr;
}

}  // namespace scarlettx::xgr
