#include "scarlettx/runtime/virtual_drive.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace scarlettx {
namespace vfs {

#if defined(_WIN32)

namespace {

// Fixed buffers: the crash path must not allocate.
char g_device[3] = {0, ':', 0};
char g_target[MAX_PATH + 1] = {};
char g_marker[MAX_PATH + 32] = {};
std::atomic<bool> g_mapped{false};
std::atomic<bool> g_handlers_installed{false};
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;

bool MarkerPath(char letter, char* out, size_t out_size) {
    char temp[MAX_PATH + 1] = {};
    const DWORD len = GetTempPathA(MAX_PATH, temp);
    if (len == 0 || len > MAX_PATH) {
        return false;
    }
    return std::snprintf(out, out_size, "%sscarlettx_drive_%c.txt", temp, letter) > 0;
}

bool SamePath(const char* a, const char* b) {
    return _stricmp(a, b) == 0;
}

// QueryDosDevice reports "<4-character NT prefix>C:<path>".
bool DriveTargets(const char* device, const char* target) {
    char current[MAX_PATH + 16] = {};
    if (QueryDosDeviceA(device, current, sizeof(current) - 1) == 0) {
        return false;
    }
    const char* path = current;
    if (std::strlen(current) > 4 && current[1] == '?' && current[2] == '?') {
        path = current + 4;
    }
    return SamePath(path, target);
}

// Serialises recovery and mapping of one drive letter across processes, so a
// process cannot act on a marker another one is about to replace. Recursive
// for the owning thread.
class DriveLock {
public:
    explicit DriveLock(char letter) {
        char name[40] = {};
        std::snprintf(name, sizeof(name), "ScarlettX_PackageDrive_%c", letter);
        handle_ = CreateMutexA(nullptr, FALSE, name);
        if (handle_) {
            const DWORD wait = WaitForSingleObject(handle_, 10000);
            owned_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        }
    }
    ~DriveLock() {
        if (handle_) {
            if (owned_) {
                ReleaseMutex(handle_);
            }
            CloseHandle(handle_);
        }
    }
    DriveLock(const DriveLock&) = delete;
    DriveLock& operator=(const DriveLock&) = delete;

private:
    HANDLE handle_ = nullptr;
    bool owned_ = false;
};

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
    UnmapPackageDrive();
    return g_previous_filter ? g_previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI ConsoleHandler(DWORD) {
    UnmapPackageDrive();
    return FALSE;
}

}  // namespace

bool RecoverStaleDrive(char letter) {
    DriveLock lock(letter);
    char marker[MAX_PATH + 32] = {};
    if (!MarkerPath(letter, marker, sizeof(marker))) {
        return false;
    }
    std::FILE* file = std::fopen(marker, "rb");
    if (!file) {
        return false;
    }
    unsigned long pid = 0;
    char target[MAX_PATH + 1] = {};
    const bool parsed = std::fscanf(file, "%lu", &pid) == 1 && std::fgetc(file) == 10 &&
                        std::fgets(target, sizeof(target), file) != nullptr;
    std::fclose(file);
    if (!parsed) {
        DeleteFileA(marker);
        return false;
    }
    if (pid == GetCurrentProcessId()) {
        return false;
    }
    if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
        const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
        CloseHandle(process);
        if (alive) {
            return false;  // another instance owns the mapping
        }
    }
    const char device[3] = {letter, ':', 0};
    bool removed = false;
    if (DriveTargets(device, target)) {
        removed = DefineDosDeviceA(DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE, device,
                                   target) != 0;
    }
    DeleteFileA(marker);
    return removed;
}

bool MapPackageDrive(char letter, const std::string& target) {
    if (g_mapped.load() || target.empty() || target.size() > MAX_PATH || letter < 'A' ||
        letter > 'Z') {
        return false;
    }
    DriveLock lock(letter);
    RecoverStaleDrive(letter);
    if (GetLogicalDrives() & (1u << (letter - 'A'))) {
        return false;
    }
    const char device[3] = {letter, ':', 0};
    if (!DefineDosDeviceA(0, device, target.c_str())) {
        return false;
    }
    g_device[0] = letter;
    std::memcpy(g_target, target.c_str(), target.size() + 1);
    g_marker[0] = 0;
    if (MarkerPath(letter, g_marker, sizeof(g_marker))) {
        if (std::FILE* file = std::fopen(g_marker, "wb")) {
            std::fprintf(file, "%lu%c%s", static_cast<unsigned long>(GetCurrentProcessId()), 10,
                         g_target);
            std::fclose(file);
        } else {
            g_marker[0] = 0;
        }
    }
    g_mapped.store(true);
    if (!g_handlers_installed.exchange(true)) {
        g_previous_filter = SetUnhandledExceptionFilter(&CrashFilter);
        SetConsoleCtrlHandler(&ConsoleHandler, TRUE);
        std::atexit(&UnmapPackageDrive);
    }
    return true;
}

bool DriveReaches(char letter, const std::string& target) {
    if (target.empty() || target.size() > MAX_PATH || letter < 'A' || letter > 'Z') {
        return false;
    }
    const std::string root = std::string(1, letter) + ":\\";
    const DWORD root_attributes = GetFileAttributesA(root.c_str());
    if (root_attributes == INVALID_FILE_ATTRIBUTES || !(root_attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        return false;
    }
    const std::string base = target.back() == '\\' || target.back() == '/' ? target : target + "\\";
    WIN32_FIND_DATAA entry{};
    const HANDLE find = FindFirstFileA((base + "*").c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool same = true;
    int compared = 0;
    do {
        if (std::strcmp(entry.cFileName, ".") == 0 || std::strcmp(entry.cFileName, "..") == 0) {
            continue;
        }
        WIN32_FILE_ATTRIBUTE_DATA seen{};
        const bool directory = (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        same = GetFileAttributesExA((root + entry.cFileName).c_str(), GetFileExInfoStandard, &seen) != 0 &&
               ((seen.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory &&
               (directory || (seen.nFileSizeLow == entry.nFileSizeLow && seen.nFileSizeHigh == entry.nFileSizeHigh));
        ++compared;
    } while (same && compared < 8 && FindNextFileA(find, &entry));
    FindClose(find);
    return same;
}

void UnmapPackageDrive() {
    if (!g_mapped.exchange(false)) {
        return;
    }
    DefineDosDeviceA(DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE, g_device, g_target);
    if (g_marker[0]) {
        DeleteFileA(g_marker);
    }
}

#else  // !_WIN32

bool MapPackageDrive(char, const std::string&) { return false; }
bool DriveReaches(char, const std::string&) { return false; }
void UnmapPackageDrive() {}
bool RecoverStaleDrive(char) { return false; }

#endif

}  // namespace vfs
}  // namespace scarlettx
