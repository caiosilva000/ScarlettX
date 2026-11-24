import CoreGraphics
import Foundation
// Prints "<windowID>\t<owner>\t<title>\t<x>,<y>,<w>,<h>\t<onscreen>" for every window whose title or owner matches argv[1].
let needle = CommandLine.arguments.count > 1 ? CommandLine.arguments[1].lowercased() : ""
let list = CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID) as? [[String: Any]] ?? []
for w in list {
    let owner = w[kCGWindowOwnerName as String] as? String ?? ""
    let title = w[kCGWindowName as String] as? String ?? ""
    if !needle.isEmpty && !owner.lowercased().contains(needle) && !title.lowercased().contains(needle) { continue }
    let b = w[kCGWindowBounds as String] as? [String: Any] ?? [:]
    let id = w[kCGWindowNumber as String] as? Int ?? 0
    let on = w[kCGWindowIsOnscreen as String] as? Bool ?? false
    print("\(id)\t\(owner)\t\(title)\t\(b["X"] ?? 0),\(b["Y"] ?? 0),\(b["Width"] ?? 0),\(b["Height"] ?? 0)\t\(on)")
}
