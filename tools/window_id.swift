// Prints the CoreGraphics window id of the emulator's game window.
//
// Visual checks capture that window by id rather than grabbing the screen,
// because `screencapture -l` reads a window even while it sits behind others.
// Raising the window instead needs accessibility permission the shell running
// these checks does not have.

import CoreGraphics
import Foundation

let match = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "PIKMINVIT"
guard let windows = CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID)
    as? [[String: Any]] else {
    exit(1)
}

for window in windows {
    let owner = window[kCGWindowOwnerName as String] as? String ?? ""
    let title = window[kCGWindowName as String] as? String ?? ""
    guard title.contains(match) || (owner.contains(match) && !title.isEmpty) else { continue }
    guard let number = window[kCGWindowNumber as String] as? Int else { continue }
    let width = (window[kCGWindowBounds as String] as? [String: Any])?["Width"] as? Double ?? 0
    // Skip the tiny helper windows the emulator also registers.
    if width < 200 { continue }
    print(number)
    exit(0)
}

exit(1)
