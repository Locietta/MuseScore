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
// SegmentAddress of kind "ChordRest").  style is the style's MSCX name, e.g.
// "staff", "system", "tempo" or "lyrics_odd"; text is its initial text, in the
// engine's markup (<sym>...</sym>, <b>, ...).  A lyrics line is the "lyrics_odd"
// style: it goes under the chord at the address, below the lyrics already there.
struct AddText {
    Address at;
    std::string style;
    std::string text;
};

// Adds the item a type names to the element or position the anchor names: an
// articulation on a chord or a rest, or a fermata, dynamic, staff text or tempo
// text on a ChordRest segment.  props are the properties of the new item, set the
// way SetProperty sets them; an articulation needs its "symbol" and a text its
// "text".  The (anchor type, child type) pairs this may create are the table in
// src/api/ops/text.cpp; every other pair, and every other type, is an
// InvalidArgument.
struct Add {
    Ref anchor;
    std::string type;
    std::vector<std::pair<std::string, Value>> props;
};

using TextOps = std::tuple<AddText, Add>;

} // namespace engrave
