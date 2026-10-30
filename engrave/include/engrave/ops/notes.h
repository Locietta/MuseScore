#pragma once

#include <tuple>

namespace engrave {

// Notes, rests and chords (A4 S7a): EnterNotes, SetPitch, SetDuration, Remove.
// The slice defines its operations here and lists them below; nothing else in
// the public headers changes.
using NoteOps = std::tuple<>;

} // namespace engrave
