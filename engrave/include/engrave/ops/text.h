#pragma once

#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <engrave/handle.h>
#include <engrave/value.h>

namespace engrave {

// Text and lyrics (A4 S7e).  The slice defines its operations here and lists them
// below; nothing else in the public headers changes.

// Adds a text of one text style at one staff and voice of a position (a
// SegmentAddress of kind "ChordRest").  style is the style's name as a saved score
// writes it; AddText writes "staff", "system", "tempo" and "lyrics_odd" (a lyrics
// line goes under the chord at the address, below the lyrics already there).  text
// is its initial text in the engine's markup (<sym>...</sym>, <b>, ...); an empty
// one is refused, because the engine stores no id for an empty text.
struct AddText {
    Address at;
    std::string style;
    std::string text;
};

// Adds the item a type names to the element or position the anchor names.  type and
// props use the descriptions' names (Engine::describe), so a tempo text is "Tempo"
// and a dynamic's kind is "DYNAMIC_TYPE".  The (anchor type, child type) pairs this
// may create are the table in src/api/ops/text.cpp: an articulation on a chord, and a
// fermata, dynamic, staff text or tempo text on a ChordRest segment.  Every other
// pair, and every other type, is an InvalidArgument.  props are set as SetProperty
// sets them; an articulation needs its "symbol" and a text its "text".
struct Add {
    Ref anchor;
    std::string type;
    std::vector<std::pair<std::string, Value>> props;
};

using TextOps = std::tuple<AddText, Add>;

} // namespace engrave
