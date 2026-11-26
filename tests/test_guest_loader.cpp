// Host-process services the in-process loader provides for mapped guest
// images: implicit TLS, module queries and the package drive mapping.
#include <atomic>
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include <windows.h>

#include "../src/loader/guest_modules.h"
#include "scarlettx/runtime/virtual_drive.h"
#include "sx_check.h"

using namespace scarlettx;

namespace {

// Shaped like an MSVC image's TLS data: the magic-static epoch (INT_MIN) at +4.
const uint8_t kTemplate[16] = {0, 0, 0, 0, 0, 0, 0, 0x80, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};

struct FakeImage {
    uint8_t tls_data[16];
    uint32_t tls_index;
    PIMAGE_TLS_CALLBACK callbacks[2];
    IMAGE_TLS_DIRECTORY64 directory;
};

std::atomic<int> g_thread_attach{0};
std::atomic<int> g_thread_detach{0};
std::atomic<void*> g_callback_image{nullptr};

void NTAPI CountingTlsCallback(PVOID image, DWORD reason, PVOID) {
    g_callback_image.store(image);
    if (reason == DLL_THREAD_ATTACH) {
        g_thread_attach.fetch_add(1);
    } else if (reason == DLL_THREAD_DETACH) {
        g_thread_detach.fetch_add(1);
    }
}

void InitFakeImage(FakeImage* image) {
    std::memset(image, 0, sizeof(*image));
    std::memcpy(image->tls_data, kTemplate, sizeof(kTemplate));
    image->tls_index = 0xFFFFFFFFu;
    image->callbacks[0] = &CountingTlsCallback;
    image->directory.StartAddressOfRawData = reinterpret_cast<ULONGLONG>(image->tls_data);
    image->directory.EndAddressOfRawData = image->directory.StartAddressOfRawData + sizeof(kTemplate);
    image->directory.AddressOfIndex = reinterpret_cast<ULONGLONG>(&image->tls_index);
    image->directory.AddressOfCallBacks = reinterpret_cast<ULONGLONG>(image->callbacks);
    image->directory.SizeOfZeroFill = 48;
}

void** TlsArray() {
    return *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(NtCurrentTeb()) + 0x58);
}

// The block as compiled guest code reaches it: DLLs through _tls_index,
// executables through slot 0.
uint8_t* GuestBlock(const FakeImage& image, bool is_exe) {
    return static_cast<uint8_t*>(TlsArray()[is_exe ? 0 : image.tls_index]);
}

bool BlockIsFreshTemplate(const uint8_t* block) {
    if (!block || std::memcmp(block, kTemplate, sizeof(kTemplate)) != 0) {
        return false;
    }
    for (size_t i = sizeof(kTemplate); i < sizeof(kTemplate) + 48; ++i) {
        if (block[i] != 0) {
            return false;
        }
    }
    return true;
}

void TestImplicitTls(bool is_exe) {
    FakeImage image;
    InitFakeImage(&image);
    g_thread_attach = 0;
    g_thread_detach = 0;
    void* const original_slot0 = TlsArray()[0];

    guest::TlsBinding binding;
    Status st = guest::AttachTls(reinterpret_cast<uint8_t*>(&image), &image.directory, is_exe, &binding);
    if (!st.ok()) {
        std::fprintf(stderr, "AttachTls failed: %s\n", st.message.c_str());
    }
    SX_REQUIRE(st.ok());
    SX_REQUIRE(image.tls_index == binding.index);
    SX_REQUIRE(image.tls_index != 0xFFFFFFFFu);

    uint8_t* main_block = GuestBlock(image, is_exe);
    SX_REQUIRE(BlockIsFreshTemplate(main_block));

    // Until the mapper enables them, thread callbacks must not run: the image
    // is not executable yet and has not seen process attach.
    std::thread early([] {});
    early.join();
    SX_REQUIRE(g_thread_attach.load() == 0 && g_thread_detach.load() == 0);
    guest::EnableTlsCallbacks(&binding);
    if (is_exe) {
        SX_REQUIRE(TlsArray()[0] == TlsArray()[binding.index]);
    }
    main_block[8] = 0xAA;  // this thread's copy only

    for (int round = 0; round < 8; ++round) {
        uint8_t* seen = nullptr;
        bool fresh = false;
        std::thread worker([&] {
            seen = GuestBlock(image, is_exe);
            fresh = BlockIsFreshTemplate(seen);
            if (seen) {
                seen[9] = 0xBB;
            }
        });
        worker.join();
        SX_REQUIRE(fresh);
        SX_REQUIRE(seen != main_block);
    }
    SX_REQUIRE(main_block[8] == 0xAA && main_block[9] == kTemplate[9]);
    SX_REQUIRE(g_thread_attach.load() == 8);
    SX_REQUIRE(g_thread_detach.load() == 8);
    SX_REQUIRE(g_callback_image.load() == &image);

    guest::DetachTls(&binding);
    SX_REQUIRE(binding.helper_module == nullptr);
    SX_REQUIRE(TlsArray()[0] == original_slot0);

    // A thread started after detach must not reach the unloaded image.
    std::thread late([] {});
    late.join();
    SX_REQUIRE(g_thread_attach.load() == 8);
}

void TestModuleQueries() {
    using GetHandleW = HMODULE(WINAPI*)(LPCWSTR);
    using GetHandleA = HMODULE(WINAPI*)(LPCSTR);
    using GetHandleExW = BOOL(WINAPI*)(DWORD, LPCWSTR, HMODULE*);
    using GetFileNameA = DWORD(WINAPI*)(HMODULE, LPSTR, DWORD);
    using PcToFileHeader = PVOID(WINAPI*)(PVOID, PVOID*);
    const auto handle_w = reinterpret_cast<GetHandleW>(guest::LookupImportOverride("kernel32.dll", "GetModuleHandleW"));
    const auto handle_a = reinterpret_cast<GetHandleA>(guest::LookupImportOverride("kernelbase.dll", "GetModuleHandleA"));
    const auto handle_ex = reinterpret_cast<GetHandleExW>(guest::LookupImportOverride("kernel32.dll", "GetModuleHandleExW"));
    const auto file_name = reinterpret_cast<GetFileNameA>(guest::LookupImportOverride("kernel32.dll", "GetModuleFileNameA"));
    const auto pc_to_header = reinterpret_cast<PcToFileHeader>(guest::LookupImportOverride("ntdll.dll", "RtlPcToFileHeader"));
    SX_REQUIRE(handle_w && handle_a && handle_ex && file_name && pc_to_header);
    SX_REQUIRE(guest::LookupImportOverride("kernel32.dll", "CreateFileW") == nullptr);
    SX_REQUIRE(guest::LookupImportOverride("user32.dll", "GetModuleHandleW") == nullptr);

    // Without a guest executable the host answers.
    SX_REQUIRE(handle_w(nullptr) == GetModuleHandleW(nullptr));

    static uint8_t fake_exe[0x3000];
    static uint8_t fake_dll[0x2000];
    const std::string exe_path = "X:/package/Mount/Game.exe";
    guest::RegisterModule(fake_exe, sizeof(fake_exe), exe_path, true);
    guest::RegisterModule(fake_dll, sizeof(fake_dll), "X:/package/Mount/Plugin.dll", false);

    SX_REQUIRE(handle_w(nullptr) == reinterpret_cast<HMODULE>(fake_exe));
    SX_REQUIRE(handle_a(nullptr) == reinterpret_cast<HMODULE>(fake_exe));
    SX_REQUIRE(handle_a("GAME.EXE") == reinterpret_cast<HMODULE>(fake_exe));
    SX_REQUIRE(handle_a("plugin") == reinterpret_cast<HMODULE>(fake_dll));
    SX_REQUIRE(handle_w(L"kernel32.dll") == GetModuleHandleW(L"kernel32.dll"));

    HMODULE from_address = nullptr;
    SX_REQUIRE(handle_ex(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                         reinterpret_cast<LPCWSTR>(fake_dll + 0x1234), &from_address));
    SX_REQUIRE(from_address == reinterpret_cast<HMODULE>(fake_dll));

    char path[MAX_PATH] = {};
    SX_REQUIRE(file_name(nullptr, path, sizeof(path)) == exe_path.size());
    SX_REQUIRE(exe_path == path);
    char tiny[8] = {};
    SX_REQUIRE(file_name(nullptr, tiny, sizeof(tiny)) == sizeof(tiny));
    SX_REQUIRE(GetLastError() == ERROR_INSUFFICIENT_BUFFER && tiny[7] == 0);

    PVOID base = nullptr;
    SX_REQUIRE(pc_to_header(fake_exe + 0x2FFF, &base) == fake_exe && base == fake_exe);
    SX_REQUIRE(pc_to_header(fake_exe + sizeof(fake_exe) + sizeof(fake_dll) + 0x100000, &base) !=
               fake_exe);
    SX_REQUIRE(guest::FindModuleBase(fake_dll) == fake_dll);

    guest::UnregisterModule(fake_exe);
    guest::UnregisterModule(fake_dll);
    SX_REQUIRE(guest::FindModuleBase(fake_dll) == nullptr);
    SX_REQUIRE(handle_w(nullptr) == GetModuleHandleW(nullptr));
}

char FreeDriveLetter() {
    const DWORD drives = GetLogicalDrives();
    for (char letter = 'Y'; letter >= 'H'; --letter) {
        if ((drives & (1u << (letter - 'A'))) == 0) {
            return letter;
        }
    }
    return 0;
}

bool DriveExists(char letter) {
    return (GetLogicalDrives() & (1u << (letter - 'A'))) != 0;
}

int CrashChild(char letter, const char* target) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (!vfs::MapPackageDrive(letter, target)) {
        return 3;
    }
    volatile int* bad = nullptr;
    *bad = 1;  // unhandled access violation
    return 4;
}

void TestPackageDrive(const char* self_path) {
    const char letter = FreeDriveLetter();
    SX_REQUIRE(letter != 0);
    const std::string target = std::filesystem::temp_directory_path().string();
    const std::string trimmed =
        (target.size() > 3 && (target.back() == '/' || target.back() == 92))
            ? target.substr(0, target.size() - 1) : target;

    // A drive reaches the folder it shows and no other: the temporary folder's own drive reaches
    // its root and not the temporary folder, and a letter nothing is mapped to reaches nothing.
    {
        const char own = static_cast<char>(std::toupper(static_cast<unsigned char>(trimmed[0])));
        SX_REQUIRE(trimmed.size() > 3 && trimmed[1] == ':');
        const std::string probe = trimmed + "\\scarlettx_reach_probe";
        std::filesystem::create_directories(probe);
        SX_REQUIRE(vfs::DriveReaches(own, std::string(1, own) + ":\\"));
        SX_REQUIRE(!vfs::DriveReaches(own, trimmed));
        SX_REQUIRE(!vfs::DriveReaches(letter, trimmed));
        std::filesystem::remove(probe);
    }

    // Normal lifetime. A host may list the new drive and still not serve it (CrossOver): the
    // mapping is made either way, and DriveReaches says which it was.
    SX_REQUIRE(vfs::MapPackageDrive(letter, trimmed));
    SX_REQUIRE(DriveExists(letter));
    {
        const std::string root = std::string(1, letter) + ":\\";
        const bool opens = GetFileAttributesA(root.c_str()) != INVALID_FILE_ATTRIBUTES;
        const bool reaches = vfs::DriveReaches(letter, trimmed);
        std::printf("[test_guest_loader] drive %c: mapped; its root opens: %s; reaches its target: %s\n", letter,
                    opens ? "yes" : "no", reaches ? "yes" : "no");
        SX_REQUIRE(reaches == opens);
    }
    vfs::UnmapPackageDrive();
    vfs::UnmapPackageDrive();
    SX_REQUIRE(!DriveExists(letter));

    // A process that dies on an unhandled exception removes its mapping.
    std::string command = std::string(1, '"') + self_path + std::string(1, '"') + " --crash-child " +
                          std::string(1, letter) + " " + std::string(1, '"') + trimmed + std::string(1, '"');
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    SX_REQUIRE(CreateProcessA(nullptr, &command[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                              &startup, &child));
    SX_REQUIRE(WaitForSingleObject(child.hProcess, 60000) == WAIT_OBJECT_0);
    DWORD exit_code = 0;
    GetExitCodeProcess(child.hProcess, &exit_code);
    const DWORD dead_pid = child.dwProcessId;
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    SX_REQUIRE(exit_code != 0 && exit_code != 3 && exit_code != 4);
    SX_REQUIRE(!DriveExists(letter));

    // A mapping left by a killed process is removed by the next run.
    const char device[3] = {letter, ':', 0};
    SX_REQUIRE(DefineDosDeviceA(0, device, trimmed.c_str()));
    const std::string marker = target + (target.back() == 92 || target.back() == '/' ? "" : "/") +
                               "scarlettx_drive_" + std::string(1, letter) + ".txt";
    std::FILE* file = std::fopen(marker.c_str(), "wb");
    SX_REQUIRE(file != nullptr);
    std::fprintf(file, "%lu%c%s", static_cast<unsigned long>(dead_pid), 10, trimmed.c_str());
    std::fclose(file);
    SX_REQUIRE(vfs::RecoverStaleDrive(letter));
    SX_REQUIRE(!DriveExists(letter));
    SX_REQUIRE(!std::filesystem::exists(marker));
    SX_REQUIRE(!vfs::RecoverStaleDrive(letter));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 4 && std::strcmp(argv[1], "--crash-child") == 0) {
        return CrashChild(argv[2][0], argv[3]);
    }
    TestImplicitTls(false);
    TestImplicitTls(true);
    TestImplicitTls(true);  // attach again after a full detach
    TestModuleQueries();
    TestPackageDrive(argv[0]);
    std::printf("[test_guest_loader] all checks passed\n");
    return 0;
}
