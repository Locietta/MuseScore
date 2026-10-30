// SPDX-License-Identifier: GPL-3.0-only
// Shared native collection-face selection for engraving metrics and Cairo.
#pragma once

#include <string>
#include <ft2build.h>
#include FT_FREETYPE_H
#include "draw/types/fontstypes.h"

namespace muse::draw {

// Single-face fonts retain upstream alias behavior. Collections select an exact
// family/bold/italic key, so registering a later face never renders face zero.
inline FT_Error newMatchingMemoryFace(FT_Library library, const FT_Byte* bytes,
                                     FT_Long size, const FontDataKey& key, FT_Face* result)
{
    if (!result || !bytes || size <= 0) return FT_Err_Invalid_Argument;
    *result = nullptr;
    FT_Face face = nullptr;
    FT_Error error = FT_New_Memory_Face(library, bytes, size, 0, &face);
    if (error) return error;
    const FT_Long count = face->num_faces;
    if (count == 1) {
        *result = face;
        return 0;
    }
    if (count < 1 || count > 32) {
        FT_Done_Face(face);
        return FT_Err_Invalid_Argument;
    }
    for (FT_Long index = 0; index < count; ++index) {
        if (index != 0) {
            error = FT_New_Memory_Face(library, bytes, size, index, &face);
            if (error) return error;
        }
        const bool matches = face->family_name
            && Font::FontFamily(std::string(face->family_name)) == key.family()
            && bool(face->style_flags & FT_STYLE_FLAG_BOLD) == key.bold()
            && bool(face->style_flags & FT_STYLE_FLAG_ITALIC) == key.italic();
        if (matches) {
            *result = face;
            return 0;
        }
        FT_Done_Face(face);
        face = nullptr;
    }
    return FT_Err_Invalid_Argument;
}

} // namespace muse::draw
