#pragma once

#include <string>

namespace scarlettx {
namespace vfs {

// Maps `letter`: to `target` for the lifetime of the process. The mapping is
// removed on normal exit, on an unhandled exception and on console close. A
// marker file records it, so that a later run can remove a mapping left behind
// by a process that was killed. Returns false if the letter is in use.
bool MapPackageDrive(char letter, const std::string& target);

// Whether the root of drive `letter` can be opened and shows `target`'s contents: each of the
// first few entries of `target` is there under the same name, with the same size. Reads only.
// A host may accept a mapping and still not serve it (CrossOver does: the drive is listed, and
// opening it fails), or the letter may already lead somewhere else. Either way the drive is of
// no use to a title.
bool DriveReaches(char letter, const std::string& target);

// Removes the mapping made by MapPackageDrive. Safe to call more than once.
void UnmapPackageDrive();

// Removes a mapping recorded by a ScarlettX process that no longer exists.
// Returns true if a stale mapping was removed.
bool RecoverStaleDrive(char letter);

}  // namespace vfs
}  // namespace scarlettx
