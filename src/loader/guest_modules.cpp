#include "guest_modules.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#  include <tlhelp32.h>
#endif

namespace scarlettx {
namespace guest {

#if defined(_WIN32)

namespace {

// ---------------------------------------------------------------------------
// Implicit TLS
//
// Guest code reads thread-locals as gs:[0x58][_tls_index] + offset. That array
// belongs to the Windows loader, which only allocates slots for modules it
// loaded itself. So the guest's TLS directory is handed to the loader inside a
// small generated carrier DLL: same template, same zero fill, same alignment.
// The loader then gives every existing and future thread a correctly
// initialised block, and the guest uses the carrier's index.
// ---------------------------------------------------------------------------
constexpr uint32_t kFileAlign = 0x200;
constexpr uint32_t kSectionAlign = 0x1000;
constexpr uint32_t kHeaderSize = 0x400;
constexpr uint32_t kTextRva = 0x1000;
constexpr uint32_t kDataRva = 0x2000;
constexpr uint32_t kTlsIndexOffset = 0x30;     // within .data; the directory is at 0
constexpr uint32_t kTlsCallbacksOffset = 0x38; // within .data: one null entry
constexpr uint32_t kImportDescOffset = 0x40;   // two descriptors, the second null
constexpr uint32_t kImportNamesOffset = 0x70;  // import lookup table, null-terminated
constexpr uint32_t kImportIatOffset = 0x80;
constexpr uint32_t kImportHintOffset = 0x90;
constexpr uint32_t kImportDllOffset = 0xA0;
constexpr uint32_t kTlsTemplateOffset = 0xC0;  // within .data
constexpr uint64_t kCarrierBase = 0x0000000171000000ull;
constexpr uint32_t kMaxTlsBytes = 64u * 1024u * 1024u;

struct TlsSlot {
    std::atomic<void*> helper{nullptr};
    uint8_t* guest_base = nullptr;
    const PIMAGE_TLS_CALLBACK* callbacks = nullptr;
    // Off until the mapper has made the image executable and run its
    // process-attach callbacks; stays off if the mapper was told not to run them.
    std::atomic<bool> callbacks_enabled{false};
    uint32_t index = 0;
    // Executables are compiled to read TLS slot 0 directly instead of going
    // through _tls_index. For those, slot 0 of every thread is pointed at the
    // carrier's block; the thread's own slot-0 pointer is parked in the last
    // 8 bytes of that block (at saved_offset) and put back on thread exit.
    std::atomic<bool> takes_slot0{false};
    uint32_t saved_offset = 0;
    bool used = false;  // guarded by g_tls_mutex
};

TlsSlot g_tls_slots[32];
std::mutex g_tls_mutex;
std::atomic<uint32_t> g_carrier_counter{0};

uint32_t AlignUp32(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

void** TlsArrayOf(void* teb) {
    return teb ? *reinterpret_cast<void***>(static_cast<uint8_t*>(teb) + 0x58) : nullptr;
}

void TakeSlot0(void* teb, const TlsSlot& slot) {
    void** tls = TlsArrayOf(teb);
    if (!tls || !tls[slot.index] || tls[0] == tls[slot.index]) {
        return;
    }
    std::memcpy(static_cast<uint8_t*>(tls[slot.index]) + slot.saved_offset, &tls[0], sizeof(void*));
    tls[0] = tls[slot.index];
}

void GiveBackSlot0(void* teb, const TlsSlot& slot) {
    void** tls = TlsArrayOf(teb);
    if (!tls || !tls[slot.index] || tls[0] != tls[slot.index]) {
        return;
    }
    std::memcpy(&tls[0], static_cast<uint8_t*>(tls[slot.index]) + slot.saved_offset, sizeof(void*));
}

// Applies fn to the TEB of every thread of this process. Other threads are
// suspended meanwhile; nothing in here may allocate or take a lock.
template <typename Fn>
void ForEachThreadTeb(Fn fn) {
    using QueryThreadFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    const auto query = reinterpret_cast<QueryThreadFn>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
    fn(NtCurrentTeb());
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (!query || snapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD pid = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) {
            continue;
        }
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, FALSE,
                                   entry.th32ThreadID);
        if (!thread) {
            continue;
        }
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            struct {
                LONG exit_status;
                void* teb;
                void* client_id[2];
                ULONG_PTR affinity;
                LONG priority;
                LONG base_priority;
            } info{};
            if (query(thread, 0 /* ThreadBasicInformation */, &info, sizeof(info), nullptr) >= 0 &&
                info.teb) {
                fn(info.teb);
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
}

// Entry point of every carrier DLL (reached through a jmp stub). Runs under the
// loader lock, like real TLS callbacks, and must not take g_tls_mutex.
BOOL WINAPI TlsCarrierMain(HINSTANCE carrier, DWORD reason, LPVOID /*reserved*/) {
    if (reason != DLL_THREAD_ATTACH && reason != DLL_THREAD_DETACH) {
        return TRUE;
    }
    for (auto& slot : g_tls_slots) {
        if (slot.helper.load(std::memory_order_acquire) != carrier) {
            continue;
        }
        if (slot.takes_slot0 && reason == DLL_THREAD_ATTACH) {
            TakeSlot0(NtCurrentTeb(), slot);
        }
        if (slot.callbacks_enabled.load(std::memory_order_acquire)) {
            for (const PIMAGE_TLS_CALLBACK* cb = slot.callbacks; cb && *cb; ++cb) {
                (*cb)(slot.guest_base, reason, nullptr);
            }
        }
        if (slot.takes_slot0 && reason == DLL_THREAD_DETACH) {
            // The loader frees every slot of an exiting thread exactly once.
            GiveBackSlot0(NtCurrentTeb(), slot);
        }
        break;
    }
    return TRUE;
}

// The template is stored at its full size, zeros included: the Windows loader
// sizes a thread's block from the raw data range alone and ignores
// SizeOfZeroFill.
std::vector<uint8_t> BuildTlsCarrier(const uint8_t* tls_template, uint32_t raw_size,
                                     uint32_t block_size, uint32_t characteristics) {
    const uint32_t data_vsize = kTlsTemplateOffset + block_size;
    const uint32_t data_raw = AlignUp32(data_vsize, kFileAlign);
    const uint32_t reloc_rva = kDataRva + AlignUp32(data_vsize, kSectionAlign);
    const uint32_t text_off = kHeaderSize;
    const uint32_t data_off = text_off + kFileAlign;
    const uint32_t reloc_off = data_off + data_raw;
    std::vector<uint8_t> file(reloc_off + kFileAlign, 0);

    IMAGE_DOS_HEADER dos{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = sizeof(IMAGE_DOS_HEADER);
    std::memcpy(file.data(), &dos, sizeof(dos));

    IMAGE_NT_HEADERS64 nt{};
    nt.Signature = IMAGE_NT_SIGNATURE;
    nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt.FileHeader.NumberOfSections = 3;
    nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt.FileHeader.Characteristics =
        IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE | IMAGE_FILE_DLL;
    auto& oh = nt.OptionalHeader;
    oh.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    oh.MajorLinkerVersion = 14;
    oh.SizeOfCode = kFileAlign;
    oh.SizeOfInitializedData = data_raw + kFileAlign;
    oh.AddressOfEntryPoint = kTextRva;
    oh.BaseOfCode = kTextRva;
    oh.ImageBase = kCarrierBase;
    oh.SectionAlignment = kSectionAlign;
    oh.FileAlignment = kFileAlign;
    oh.MajorOperatingSystemVersion = 6;
    oh.MajorSubsystemVersion = 6;
    oh.SizeOfImage = reloc_rva + kSectionAlign;
    oh.SizeOfHeaders = kHeaderSize;
    oh.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
    oh.DllCharacteristics = IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE |
                            IMAGE_DLLCHARACTERISTICS_NX_COMPAT |
                            IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA;
    oh.SizeOfStackReserve = 0x100000;
    oh.SizeOfStackCommit = 0x1000;
    oh.SizeOfHeapReserve = 0x100000;
    oh.SizeOfHeapCommit = 0x1000;
    oh.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = kDataRva + kImportDescOffset;
    oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size = 2 * sizeof(IMAGE_IMPORT_DESCRIPTOR);
    oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].VirtualAddress = kDataRva + kImportIatOffset;
    oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].Size = 16;
    oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress = kDataRva;
    oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size = sizeof(IMAGE_TLS_DIRECTORY64);
    oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress = reloc_rva;
    oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size = 16;
    std::memcpy(file.data() + dos.e_lfanew, &nt, sizeof(nt));

    IMAGE_SECTION_HEADER sections[3]{};
    std::memcpy(sections[0].Name, ".text", 5);
    sections[0].Misc.VirtualSize = 12;
    sections[0].VirtualAddress = kTextRva;
    sections[0].SizeOfRawData = kFileAlign;
    sections[0].PointerToRawData = text_off;
    sections[0].Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
    std::memcpy(sections[1].Name, ".data", 5);
    sections[1].Misc.VirtualSize = data_vsize;
    sections[1].VirtualAddress = kDataRva;
    sections[1].SizeOfRawData = data_raw;
    sections[1].PointerToRawData = data_off;
    sections[1].Characteristics =
        IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
    std::memcpy(sections[2].Name, ".reloc", 6);
    sections[2].Misc.VirtualSize = 16;
    sections[2].VirtualAddress = reloc_rva;
    sections[2].SizeOfRawData = kFileAlign;
    sections[2].PointerToRawData = reloc_off;
    sections[2].Characteristics =
        IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_DISCARDABLE | IMAGE_SCN_MEM_READ;
    std::memcpy(file.data() + dos.e_lfanew + sizeof(nt), sections, sizeof(sections));

    // .text: mov rax, &TlsCarrierMain ; jmp rax
    uint8_t* text = file.data() + text_off;
    const uint64_t target = reinterpret_cast<uint64_t>(&TlsCarrierMain);
    text[0] = 0x48;
    text[1] = 0xB8;
    std::memcpy(text + 2, &target, sizeof(target));
    text[10] = 0xFF;
    text[11] = 0xE0;

    // .data: TLS directory, index variable, template.
    IMAGE_TLS_DIRECTORY64 dir{};
    dir.StartAddressOfRawData = kCarrierBase + kDataRva + kTlsTemplateOffset;
    dir.EndAddressOfRawData = dir.StartAddressOfRawData + block_size;
    dir.AddressOfIndex = kCarrierBase + kDataRva + kTlsIndexOffset;
    // An empty, null-terminated callback array, as compiler-built images have.
    dir.AddressOfCallBacks = kCarrierBase + kDataRva + kTlsCallbacksOffset;
    dir.Characteristics = characteristics;
    std::memcpy(file.data() + data_off, &dir, sizeof(dir));
    if (raw_size != 0) {
        std::memcpy(file.data() + data_off + kTlsTemplateOffset, tls_template, raw_size);
    }

    // One import. The Windows loader sets up implicit TLS while it resolves a
    // module's imports, and skips it for an image that has none.
    IMAGE_IMPORT_DESCRIPTOR import{};
    import.OriginalFirstThunk = kDataRva + kImportNamesOffset;
    import.Name = kDataRva + kImportDllOffset;
    import.FirstThunk = kDataRva + kImportIatOffset;
    std::memcpy(file.data() + data_off + kImportDescOffset, &import, sizeof(import));
    const uint64_t hint_rva = kDataRva + kImportHintOffset;
    std::memcpy(file.data() + data_off + kImportNamesOffset, &hint_rva, sizeof(hint_rva));
    std::memcpy(file.data() + data_off + kImportIatOffset, &hint_rva, sizeof(hint_rva));
    std::memcpy(file.data() + data_off + kImportHintOffset + 2, "Sleep", 6);
    std::memcpy(file.data() + data_off + kImportDllOffset, "KERNEL32.dll", 13);

    // .reloc: the four absolute addresses of the directory.
    IMAGE_BASE_RELOCATION block{};
    block.VirtualAddress = kDataRva;
    block.SizeOfBlock = 16;
    const uint16_t entries[4] = {
        static_cast<uint16_t>((IMAGE_REL_BASED_DIR64 << 12) |
                              offsetof(IMAGE_TLS_DIRECTORY64, StartAddressOfRawData)),
        static_cast<uint16_t>((IMAGE_REL_BASED_DIR64 << 12) |
                              offsetof(IMAGE_TLS_DIRECTORY64, EndAddressOfRawData)),
        static_cast<uint16_t>((IMAGE_REL_BASED_DIR64 << 12) |
                              offsetof(IMAGE_TLS_DIRECTORY64, AddressOfIndex)),
        static_cast<uint16_t>((IMAGE_REL_BASED_DIR64 << 12) |
                              offsetof(IMAGE_TLS_DIRECTORY64, AddressOfCallBacks)),
    };
    std::memcpy(file.data() + reloc_off, &block, sizeof(block));
    std::memcpy(file.data() + reloc_off + sizeof(block), entries, sizeof(entries));
    return file;
}

}  // namespace

Status AttachTls(uint8_t* image_base, const void* tls_directory, bool is_exe, TlsBinding* out) {
    const auto* dir = static_cast<const IMAGE_TLS_DIRECTORY64*>(tls_directory);
    if (!image_base || !dir || !out || dir->AddressOfIndex == 0 ||
        dir->EndAddressOfRawData < dir->StartAddressOfRawData ||
        dir->EndAddressOfRawData - dir->StartAddressOfRawData > kMaxTlsBytes ||
        dir->SizeOfZeroFill > kMaxTlsBytes) {
        return Status::Error(StatusCode::ParseError, "Guest TLS directory is malformed");
    }
    const auto raw_size = static_cast<uint32_t>(dir->EndAddressOfRawData - dir->StartAddressOfRawData);
    const auto* tls_template = reinterpret_cast<const uint8_t*>(dir->StartAddressOfRawData);
    // A directory without data still needs a valid index for its callbacks.
    uint32_t zero_fill = (raw_size == 0 && dir->SizeOfZeroFill == 0) ? 8 : dir->SizeOfZeroFill;
    uint32_t saved_offset = 0;
    if (is_exe) {
        // Host code in this process's executable reads slot 0 too, so the block
        // must be at least as large as the one it replaces.
        uint32_t block_size = AlignUp32(raw_size + zero_fill, 16);
        auto* host = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
        const auto* host_nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
            host + reinterpret_cast<const IMAGE_DOS_HEADER*>(host)->e_lfanew);
        const auto& host_tls = host_nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
        if (host_tls.VirtualAddress != 0) {
            const auto* host_dir =
                reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(host + host_tls.VirtualAddress);
            const uint64_t host_size = host_dir->EndAddressOfRawData -
                                       host_dir->StartAddressOfRawData + host_dir->SizeOfZeroFill;
            if (host_size > kMaxTlsBytes) {
                return Status::Error(StatusCode::DeviceError, "Host TLS block is too large to shadow");
            }
            block_size = std::max(block_size, AlignUp32(static_cast<uint32_t>(host_size), 16));
        }
        saved_offset = block_size;
        zero_fill = block_size + 16 - raw_size;
    }
    const std::vector<uint8_t> carrier =
        BuildTlsCarrier(tls_template, raw_size, raw_size + zero_fill, dir->Characteristics);

    wchar_t temp_dir[MAX_PATH + 1] = {};
    const DWORD temp_len = GetTempPathW(MAX_PATH, temp_dir);
    if (temp_len == 0 || temp_len > MAX_PATH) {
        return Status::Error(StatusCode::DeviceError, "GetTempPathW failed for the guest TLS carrier");
    }
    const std::wstring path = std::wstring(temp_dir) + L"scarlettx_tls_" +
                              std::to_wstring(GetCurrentProcessId()) + L"_" +
                              std::to_wstring(g_carrier_counter.fetch_add(1)) + L".dll";

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return Status::Error(StatusCode::DeviceError, "Cannot create the guest TLS carrier file");
    }
    DWORD written = 0;
    const BOOL write_ok = WriteFile(file, carrier.data(), static_cast<DWORD>(carrier.size()),
                                    &written, nullptr);
    CloseHandle(file);
    if (!write_ok || written != carrier.size()) {
        DeleteFileW(path.c_str());
        return Status::Error(StatusCode::DeviceError, "Cannot write the guest TLS carrier file");
    }

    std::lock_guard<std::mutex> lock(g_tls_mutex);
    int slot_index = -1;
    for (int i = 0; i < static_cast<int>(sizeof(g_tls_slots) / sizeof(g_tls_slots[0])); ++i) {
        if (!g_tls_slots[i].used) {
            slot_index = i;
            break;
        }
    }
    if (slot_index < 0) {
        DeleteFileW(path.c_str());
        return Status::Error(StatusCode::DeviceError, "Too many guest images with TLS");
    }

    HMODULE module = LoadLibraryW(path.c_str());
    if (!module) {
        const DWORD err = GetLastError();
        DeleteFileW(path.c_str());
        return Status::Error(StatusCode::DeviceError,
                             "Loading the guest TLS carrier failed (Win32 error " +
                                 std::to_string(err) + ")");
    }

    DWORD index = 0;
    std::memcpy(&index, reinterpret_cast<const uint8_t*>(module) + kDataRva + kTlsIndexOffset,
                sizeof(index));

    // Check what the loader built for this thread before trusting it.
    void** tls_array = *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(NtCurrentTeb()) + 0x58);
    const uint8_t* block = tls_array ? static_cast<const uint8_t*>(tls_array[index]) : nullptr;
    if (!block || (raw_size != 0 && std::memcmp(block, tls_template, raw_size) != 0)) {
        FreeLibrary(module);
        DeleteFileW(path.c_str());
        return Status::Error(StatusCode::DeviceError,
                             "The host loader did not provide a TLS block for the guest image");
    }

    std::memcpy(reinterpret_cast<void*>(dir->AddressOfIndex), &index, sizeof(index));

    TlsSlot& slot = g_tls_slots[slot_index];
    slot.used = true;
    slot.guest_base = image_base;
    slot.callbacks = reinterpret_cast<const PIMAGE_TLS_CALLBACK*>(dir->AddressOfCallBacks);
    slot.index = index;
    slot.takes_slot0 = is_exe && index != 0;
    slot.saved_offset = saved_offset;
    slot.helper.store(module, std::memory_order_release);
    if (slot.takes_slot0) {
        // Threads started from here on are handled in TlsCarrierMain.
        ForEachThreadTeb([&](void* teb) { TakeSlot0(teb, slot); });
    }

    out->helper_module = module;
    out->helper_path = path;
    out->index = index;
    out->slot = slot_index;
    return Status::Ok();
}

void EnableTlsCallbacks(TlsBinding* binding) {
    if (!binding || !binding->helper_module || binding->slot < 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_tls_mutex);
    g_tls_slots[binding->slot].callbacks_enabled.store(true, std::memory_order_release);
}

void DetachTls(TlsBinding* binding) {
    if (!binding || !binding->helper_module) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_tls_mutex);
        if (binding->slot >= 0) {
            TlsSlot& slot = g_tls_slots[binding->slot];
            if (slot.takes_slot0) {
                // Must happen before the carrier unloads and its blocks are freed.
                // TlsCarrierMain runs under the loader lock, so holding it here
                // keeps a starting thread from taking slot 0 after it was given
                // back.
                using LockFn = LONG(NTAPI*)(ULONG, ULONG*, ULONG_PTR*);
                using UnlockFn = LONG(NTAPI*)(ULONG, ULONG_PTR);
                const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
                const auto lock_loader = reinterpret_cast<LockFn>(
                    reinterpret_cast<void*>(GetProcAddress(ntdll, "LdrLockLoaderLock")));
                const auto unlock_loader = reinterpret_cast<UnlockFn>(
                    reinterpret_cast<void*>(GetProcAddress(ntdll, "LdrUnlockLoaderLock")));
                ULONG_PTR cookie = 0;
                const bool locked =
                    lock_loader && unlock_loader && lock_loader(0, nullptr, &cookie) >= 0;
                slot.takes_slot0 = false;
                ForEachThreadTeb([&](void* teb) { GiveBackSlot0(teb, slot); });
                if (locked) {
                    unlock_loader(0, cookie);
                }
            }
            slot.callbacks_enabled.store(false, std::memory_order_release);
            slot.helper.store(nullptr, std::memory_order_release);
            slot.callbacks = nullptr;
            slot.guest_base = nullptr;
            slot.used = false;
        }
    }
    // Takes the loader lock, so a thread still inside TlsCarrierMain finishes first.
    FreeLibrary(static_cast<HMODULE>(binding->helper_module));
    DeleteFileW(binding->helper_path.c_str());
    *binding = TlsBinding{};
}

// ---------------------------------------------------------------------------
// Guest module registry and the host queries answered from it
// ---------------------------------------------------------------------------
namespace {

struct Module {
    uint8_t* base = nullptr;
    uint32_t size = 0;
    std::string path;
    std::wstring wide_path;
    std::string name_lower;
    bool is_exe = false;
};

std::mutex g_modules_mutex;
std::vector<Module> g_modules;

const char kBackslash = static_cast<char>(92);

std::string BaseNameLower(const std::string& path) {
    size_t start = 0;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '/' || path[i] == kBackslash) {
            start = i + 1;
        }
    }
    std::string name = path.substr(start);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    });
    return name;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) {
        return std::wstring();
    }
    const int len = MultiByteToWideChar(CP_ACP, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(len > 0 ? len : 0), L'x');
    if (len > 0) {
        MultiByteToWideChar(CP_ACP, 0, s.c_str(), static_cast<int>(s.size()), &out[0], len);
    }
    return out;
}

std::string Narrow(const wchar_t* s) {
    if (!s) {
        return std::string();
    }
    const int len = WideCharToMultiByte(CP_ACP, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len > 1 ? len - 1 : 0), 'x');
    if (len > 1) {
        WideCharToMultiByte(CP_ACP, 0, s, -1, &out[0], len, nullptr, nullptr);
    }
    return out;
}

bool FindByAddress(const void* address, Module* out) {
    const auto* p = static_cast<const uint8_t*>(address);
    std::lock_guard<std::mutex> lock(g_modules_mutex);
    for (const auto& m : g_modules) {
        if (p >= m.base && p < m.base + m.size) {
            *out = m;
            return true;
        }
    }
    return false;
}

bool FindPrimaryExe(Module* out) {
    std::lock_guard<std::mutex> lock(g_modules_mutex);
    for (const auto& m : g_modules) {
        if (m.is_exe) {
            *out = m;
            return true;
        }
    }
    return false;
}

bool FindByName(const std::string& name, Module* out) {
    const std::string wanted = BaseNameLower(name);
    std::lock_guard<std::mutex> lock(g_modules_mutex);
    for (const auto& m : g_modules) {
        if (m.name_lower == wanted || m.name_lower == wanted + ".dll" ||
            m.name_lower == wanted + ".exe") {
            *out = m;
            return true;
        }
    }
    return false;
}

// NULL means "the process image", which for guest code is the guest executable.
bool ResolveHandle(HMODULE handle, Module* out) {
    if (!handle) {
        return FindPrimaryExe(out);
    }
    Module m;
    if (FindByAddress(handle, &m) && m.base == reinterpret_cast<uint8_t*>(handle)) {
        *out = m;
        return true;
    }
    return false;
}

HMODULE WINAPI GuestGetModuleHandleA(LPCSTR name) {
    Module m;
    if (name ? FindByName(name, &m) : FindPrimaryExe(&m)) {
        return reinterpret_cast<HMODULE>(m.base);
    }
    return GetModuleHandleA(name);
}

HMODULE WINAPI GuestGetModuleHandleW(LPCWSTR name) {
    Module m;
    if (name ? FindByName(Narrow(name), &m) : FindPrimaryExe(&m)) {
        return reinterpret_cast<HMODULE>(m.base);
    }
    return GetModuleHandleW(name);
}

bool ResolveHandleEx(DWORD flags, const void* name, bool wide, HMODULE* module) {
    Module m;
    bool found = false;
    if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
        found = FindByAddress(name, &m);
    } else if (!name) {
        found = FindPrimaryExe(&m);
    } else {
        found = FindByName(wide ? Narrow(static_cast<const wchar_t*>(name))
                                : std::string(static_cast<const char*>(name)), &m);
    }
    if (found && module) {
        *module = reinterpret_cast<HMODULE>(m.base);
    }
    return found && module;
}

BOOL WINAPI GuestGetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE* module) {
    if (ResolveHandleEx(flags, name, false, module)) {
        return TRUE;
    }
    return GetModuleHandleExA(flags, name, module);
}

BOOL WINAPI GuestGetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE* module) {
    if (ResolveHandleEx(flags, name, true, module)) {
        return TRUE;
    }
    return GetModuleHandleExW(flags, name, module);
}

template <typename Char, typename String>
DWORD CopyModulePath(const String& path, Char* buffer, DWORD size) {
    if (!buffer || size == 0) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    const DWORD length = static_cast<DWORD>(path.size());
    const DWORD copied = length < size ? length : size - 1;
    std::copy(path.begin(), path.begin() + copied, buffer);
    buffer[copied] = 0;
    if (length >= size) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return size;
    }
    SetLastError(ERROR_SUCCESS);
    return copied;
}

DWORD WINAPI GuestGetModuleFileNameA(HMODULE module, LPSTR buffer, DWORD size) {
    Module m;
    if (ResolveHandle(module, &m)) {
        return CopyModulePath(m.path, buffer, size);
    }
    return GetModuleFileNameA(module, buffer, size);
}

DWORD WINAPI GuestGetModuleFileNameW(HMODULE module, LPWSTR buffer, DWORD size) {
    Module m;
    if (ResolveHandle(module, &m)) {
        return CopyModulePath(m.wide_path, buffer, size);
    }
    return GetModuleFileNameW(module, buffer, size);
}

PVOID WINAPI GuestRtlPcToFileHeader(PVOID pc, PVOID* base) {
    Module m;
    if (FindByAddress(pc, &m)) {
        if (base) {
            *base = m.base;
        }
        return m.base;
    }
    // Taken from ntdll itself, so that no redirected import can lead back here.
    using Fn = PVOID(WINAPI*)(PVOID, PVOID*);
    static const Fn real = reinterpret_cast<Fn>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlPcToFileHeader")));
    if (!real) {
        if (base) {
            *base = nullptr;
        }
        return nullptr;
    }
    return real(pc, base);
}

}  // namespace

void RegisterModule(uint8_t* base, uint32_t size, const std::string& path, bool is_exe) {
    Module m;
    m.base = base;
    m.size = size;
    m.path = path;
    m.wide_path = Widen(path);
    m.name_lower = BaseNameLower(path);
    m.is_exe = is_exe;
    std::lock_guard<std::mutex> lock(g_modules_mutex);
    g_modules.push_back(std::move(m));
}

void UnregisterModule(uint8_t* base) {
    std::lock_guard<std::mutex> lock(g_modules_mutex);
    g_modules.erase(std::remove_if(g_modules.begin(), g_modules.end(),
                                   [&](const Module& m) { return m.base == base; }),
                    g_modules.end());
}

void* FindModuleBase(const void* address) {
    Module m;
    return FindByAddress(address, &m) ? m.base : nullptr;
}

void* LookupImportOverride(const std::string& target_dll_lower, const std::string& symbol) {
    if (target_dll_lower != "kernel32.dll" && target_dll_lower != "kernelbase.dll" &&
        target_dll_lower != "ntdll.dll") {
        return nullptr;
    }
    struct Entry {
        const char* name;
        void* address;
    };
    static const Entry kEntries[] = {
        {"GetModuleHandleA", reinterpret_cast<void*>(&GuestGetModuleHandleA)},
        {"GetModuleHandleW", reinterpret_cast<void*>(&GuestGetModuleHandleW)},
        {"GetModuleHandleExA", reinterpret_cast<void*>(&GuestGetModuleHandleExA)},
        {"GetModuleHandleExW", reinterpret_cast<void*>(&GuestGetModuleHandleExW)},
        {"GetModuleFileNameA", reinterpret_cast<void*>(&GuestGetModuleFileNameA)},
        {"GetModuleFileNameW", reinterpret_cast<void*>(&GuestGetModuleFileNameW)},
        {"RtlPcToFileHeader", reinterpret_cast<void*>(&GuestRtlPcToFileHeader)},
    };
    for (const auto& e : kEntries) {
        if (symbol == e.name) {
            return e.address;
        }
    }
    return nullptr;
}

namespace {

std::wstring LowerWide(std::wstring s) {
    for (auto& c : s) {
        if (c >= L'A' && c <= L'Z') {
            c = static_cast<wchar_t>(c + 32);
        }
    }
    return s;
}

// True for a host-loaded DLL that lives in the guest executable's directory
// tree, i.e. one that shipped with the title.
bool IsPackageModule(void* host_module) {
    Module exe;
    if (!FindPrimaryExe(&exe)) {
        return false;
    }
    std::wstring dir = LowerWide(exe.wide_path);
    size_t cut = std::wstring::npos;
    for (size_t i = 0; i < dir.size(); ++i) {
        if (dir[i] == L'/' || dir[i] == static_cast<wchar_t>(kBackslash)) {
            cut = i;
        }
    }
    if (cut == std::wstring::npos) {
        return false;
    }
    dir.resize(cut + 1);
    wchar_t path[MAX_PATH + 1] = {};
    const DWORD len = GetModuleFileNameW(static_cast<HMODULE>(host_module), path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return false;
    }
    return LowerWide(path).compare(0, dir.size(), dir) == 0;
}


// Rewrites the import table entries of one host module (see PatchHostModuleForGuests).
void RedirectImports(uint8_t* base, bool package_module) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) {
        return;
    }
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) {
        return;
    }
    for (const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
         desc->Name != 0; ++desc) {
        if (desc->OriginalFirstThunk == 0) {
            continue;
        }
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + desc->OriginalFirstThunk);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData != 0; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) {
                continue;
            }
            const auto* by_name =
                reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            const char* symbol = reinterpret_cast<const char*>(by_name->Name);
            void* replacement = nullptr;
            if (std::strcmp(symbol, "RtlPcToFileHeader") == 0) {
                replacement = reinterpret_cast<void*>(&GuestRtlPcToFileHeader);
            } else if (package_module) {
                replacement = LookupImportOverride("kernel32.dll", symbol);
            }
            if (!replacement) {
                continue;
            }
            DWORD old_protect = 0;
            if (VirtualProtect(&iat->u1.Function, sizeof(ULONGLONG), PAGE_READWRITE, &old_protect)) {
                iat->u1.Function = reinterpret_cast<ULONGLONG>(replacement);
                VirtualProtect(&iat->u1.Function, sizeof(ULONGLONG), old_protect, &old_protect);
            }
        }
    }
}

}  // namespace

void PatchHostModuleForGuests(void* host_module) {
    auto* base = static_cast<uint8_t*>(host_module);
    // Low bits are set on data-file and image-resource handles.
    if (!base || (reinterpret_cast<uintptr_t>(base) & 0xFFFF) != 0) {
        return;
    }
    // The overrides fall back to the real functions through this module's own
    // imports, so those must stay as they are.
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&GuestRtlPcToFileHeader), &self) &&
        self == host_module) {
        return;
    }
    // Only two kinds of host-loaded module are touched: the C++ runtime, whose
    // throw looks up the image that owns the exception's type data, and DLLs
    // that shipped with the title. System DLLs keep their imports.
    const bool package_module = IsPackageModule(host_module);
    wchar_t module_path[MAX_PATH + 1] = {};
    GetModuleFileNameW(static_cast<HMODULE>(host_module), module_path, MAX_PATH);
    const bool runtime_module = BaseNameLower(Narrow(module_path)).compare(0, 9, "vcruntime") == 0;
    if (!package_module && !runtime_module) {
        return;
    }
    RedirectImports(base, package_module);
    if (runtime_module) {
        // The lookup has to be redirected in the module that implements the throw. On Windows
        // that is vcruntime140.dll itself. Wine and CrossOver export _CxxThrowException from
        // vcruntime140.dll as a forward to their ucrtbase.dll, whose own RtlPcToFileHeader import
        // would otherwise report no module for a guest image and leave the throw's image base 0.
        HMODULE thrower = nullptr;
        const FARPROC throw_fn = GetProcAddress(static_cast<HMODULE>(host_module), "_CxxThrowException");
        if (throw_fn &&
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(throw_fn), &thrower) &&
            thrower && thrower != host_module && thrower != self) {
            RedirectImports(reinterpret_cast<uint8_t*>(thrower), false);
        }
    }
}

#else  // !_WIN32

Status AttachTls(uint8_t*, const void*, bool, TlsBinding*) {
    return Status::Error(StatusCode::NotImplemented, "Guest TLS requires Windows x64");
}
void EnableTlsCallbacks(TlsBinding*) {}
void DetachTls(TlsBinding*) {}
void RegisterModule(uint8_t*, uint32_t, const std::string&, bool) {}
void UnregisterModule(uint8_t*) {}
void* FindModuleBase(const void*) { return nullptr; }
void* LookupImportOverride(const std::string&, const std::string&) { return nullptr; }
void PatchHostModuleForGuests(void*) {}

#endif

}  // namespace guest
}  // namespace scarlettx
