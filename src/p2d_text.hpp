#pragma once

// Handling for the inline formatting codes embedded in P2D text box strings.
//
// A code is ESC (0x1B), a one or two letter tag, then an optional bracketed
// argument. See P2DPrint::parseCode in
// upstream/pikmin/src/plugPikiYamashita/P2DPrint.cpp and the tag list in
// upstream/pikmin/src/plugPikiOgawa/ogSub.cpp.

#include <string>

namespace p2d {

// Returns the printable characters of `text`, dropping formatting codes.
// The port does not yet honour the per-character spacing and colour changes the
// codes request, so "\x1bSH[-3]Op..." renders as plain "Op...".
std::string strip_codes(const std::string& text);

} // namespace p2d
