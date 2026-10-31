#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace engrave {

// The unit a numeric value is expressed in.  The value codec derives None, Spatium
// and Absolute from the property value type; Millimetre and Degree are vocabulary
// for the per-property units a later slice can add.
enum struct Unit {
    None,
    Spatium,   // staff spaces
    Absolute,  // 1/1200 inch: the engine's DPI (a millimetre is DPMM of these)
    Millimetre,
    Degree,
};

// The shape of a property or style value.  One per Value alternative.
enum struct ValueKind { Bool, Int, Real, String, Point, Size, Color, Fraction, Enum, IntList, Opaque };

// A time position as a fraction of a whole note, e.g. {3, 4} for the third
// quarter. The denominator is positive; the value is not necessarily reduced.
struct Fraction {
    std::int64_t num = 0;
    std::int64_t den = 1;
    friend auto operator<=>(const Fraction&, const Fraction&) = default;
};

struct Point {
    double x = 0;
    double y = 0;

    bool operator==(const Point&) const = default;
};

struct Size {
    double w = 0;
    double h = 0;

    bool operator==(const Size&) const = default;
};

struct Color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;

    bool operator==(const Color&) const = default;
};

// An enum value as its MSCX token: the text the engine's TConv writes for it, so
// the vocabulary is the one of a saved score and survives a sync.  A few enums
// have no token list (SYMID's set is open, Align and OrnamentInterval build theirs
// from two tokens, AccidentalRole and HookType use numbers); their tokens are still
// accepted and produced.
struct Enum {
    std::string token;

    bool operator==(const Enum&) const = default;
};

// A value the engine keeps as an object and the API does not map: read-only.
// type is the property value type's name, e.g. "draw_path".
struct Opaque {
    std::string type;

    bool operator==(const Opaque&) const = default;
};

// The value of one property or style.  A default-constructed Value is
// std::monostate: no value at all.
using Value = std::variant<std::monostate, bool, std::int64_t, double, std::string, Fraction, Point, Size, Color,
                           Enum, std::vector<std::int64_t>, Opaque>;

} // namespace engrave
