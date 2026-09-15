#include "p2d_text.hpp"

namespace p2d {
namespace {

const char kEscape = '\x1b';

bool matches(const std::string& text, size_t position, const char* tag, size_t length) {
  if (position + length > text.size()) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    if (text[position + i] != tag[i]) {
      return false;
    }
  }
  return true;
}

// Every two-letter tag the game recognises. Anything else is left alone, which
// matches parseCode rewinding the stream for an unknown code.
const char* const kTwoLetterTags[] = {
    "CC", "GC", "TM", "CA", "GA", "TB", "BS", "CU", "CD", "CL",
    "CR", "LU", "LD", "HM", "ST", "FX", "FY", "SH", "SV", "GM",
};

} // namespace

std::string strip_codes(const std::string& text) {
  std::string out;
  out.reserve(text.size());

  size_t position = 0;
  while (position < text.size()) {
    if (text[position] != kEscape) {
      out.push_back(text[position]);
      ++position;
      continue;
    }

    const size_t tag_start = position + 1;

    // "Z" is a single-letter tag carrying one extra byte.
    if (matches(text, tag_start, "Z", 1)) {
      position = tag_start + 2;
      continue;
    }

    const char* found = nullptr;
    for (const char* tag : kTwoLetterTags) {
      if (matches(text, tag_start, tag, 2)) {
        found = tag;
        break;
      }
    }

    if (found == nullptr) {
      // Not a recognised code; keep the escape byte out but resume normally.
      ++position;
      continue;
    }

    if (matches(text, tag_start, "TM", 2)) {
      position = tag_start + 2;
      continue;
    }

    const size_t closing = text.find(']', tag_start);
    position = (closing == std::string::npos) ? tag_start + 2 : closing + 1;
  }

  return out;
}

} // namespace p2d
