// RaylibSDFTextEx.cpp (FTTextEngineImpl - FreeType-HarfBuzz-ICU Backend - Enhanced Version 2025-05-22)
// Implements text_engine.h interface, using FreeType, HarfBuzz, and ICU.
// Enhanced with:
// 1. Font Fallback mechanism.
// 2. GetTextRangeBounds for selection.
// 3. Storage for BiDi maps in LineLayoutInfo (population in LayoutStyledText).
// 4. DrawTextSelectionHighlight.

#include "text_engine.h" // Engine interface and core data structure definitions
#include "raymath.h"     // Raylib math library
#include "rlgl.h"        // Raylib low-level graphics interface

// C/C++ Standard Libraries
#include <fstream>
#include <vector>
#include <string>
#include <map>
#include <list>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <variant> // For std::holds_alternative / std::get

// FreeType Headers
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_OUTLINE_H
#include FT_SYSTEM_H     // For FT_Stream
#include <freetype/tttables.h> // For TT_OS2 and other table definitions
#include <freetype/ftsnames.h> // For FT_Sfnt_Tag

// HarfBuzz Headers
#include <harfbuzz/hb.h>
#include <harfbuzz/hb-ft.h> // HarfBuzz FreeType integration

// ICU Headers
#include <unicode/utypes.h>
#include <unicode/ustring.h> // For UTF-16 string functions (u_strFromUTF8, u_strToUTF8 etc.)
#include <unicode/ubidi.h>   // For BiDi algorithm
#include <unicode/ubrk.h>    // For line breaking
#include <unicode/uscript.h> // For script detection
#include <unicode/uloc.h>    // For locales
#include <unicode/utf16.h>   // For U16_... macros

// Global variable for dynamically adjusting SDF smoothness (from main.cpp)
extern float dynamicSmoothnessAdd;

// Custom HarfBuzz glyph advances callback (remains the same)
static void my_custom_get_glyph_h_advances_callback(
        hb_font_t *hb_font,
        void *font_data,
        unsigned int count,
        const hb_codepoint_t *first_glyph_gid,
        unsigned int glyph_stride,
        hb_position_t *first_advance,
        unsigned int advance_stride,
        void *user_data
) {
    FT_Face ft_face = static_cast<FT_Face>(font_data);
    if (!ft_face || count == 0) return;

    const char *glyph_gid_ptr = reinterpret_cast<const char *>(first_glyph_gid);
    char *advance_ptr = reinterpret_cast<char *>(first_advance);

    for (unsigned int i = 0; i < count; i++) {
        hb_codepoint_t current_gid = *reinterpret_cast<const hb_codepoint_t*>(glyph_gid_ptr);
        hb_position_t *current_pos_output = reinterpret_cast<hb_position_t*>(advance_ptr);
        FT_Error error = FT_Load_Glyph(ft_face, current_gid, FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP);
        if (error) {
            *current_pos_output = 0;
        } else {
            *current_pos_output = ft_face->glyph->advance.x;
        }
        glyph_gid_ptr += glyph_stride;
        advance_ptr += advance_stride;
    }
}

namespace { // Anonymous namespace start

// --- Internal Data Structures ---

    struct FTFontData {
        std::vector<unsigned char> fontBuffer;
        FT_Face ftFace = nullptr;
        hb_font_t* hbFont = nullptr;
        ITextEngine::FontProperties properties; //
        int sdfPixelSizeHint = 64;
        int16_t yStrikeoutPosition_fontUnits = 0;
        int16_t yStrikeoutSize_fontUnits = 0;
    };

    struct FTGlyphCacheKey {
        FontId fontId;       // The actual font ID used to render this glyph (could be a fallback)
        uint32_t glyphIndex; // The GID within that fontId
        int sdfPixelSize;    // The size at which the SDF/bitmap was generated
        bool isSDF;

        bool operator==(const FTGlyphCacheKey& other) const {
            return fontId == other.fontId &&
                   glyphIndex == other.glyphIndex &&
                   sdfPixelSize == other.sdfPixelSize &&
                   isSDF == other.isSDF;
        }
    };

    struct FTGlyphCacheKeyHash {
        std::size_t operator()(const FTGlyphCacheKey& k) const {
            std::size_t h1 = std::hash<FontId>()(k.fontId);
            std::size_t h2 = std::hash<uint32_t>()(k.glyphIndex);
            std::size_t h3 = std::hash<int>()(k.sdfPixelSize);
            std::size_t h4 = std::hash<bool>()(k.isSDF);
            return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3);
        }
    };

    struct FTCachedGlyph {
        GlyphRenderInfo renderInfo; //
        // Metrics at the size the glyph was cached (sdfPixelSize)
        float advanceX_at_cached_size = 0.0f;
        float ascent_at_cached_size = 0.0f;   // Positive upwards from baseline
        float descent_at_cached_size = 0.0f;  // Positive downwards from baseline
        // uint32_t originalCodepoint = 0; // Might be useful for debugging fallback
    };

    const char* ftSdfMasterFragmentShaderSrc = R"(
#version 330 core
in vec2 fragTexCoord;
uniform sampler2D sdfTexture;
uniform vec4 textColor;
uniform float sdfEdgeValue;
uniform float sdfSmoothness;
uniform bool enableOutline;
uniform vec4 outlineColor;
uniform float outlineWidth;
uniform bool enableGlow;
uniform vec4 glowColor;
uniform float glowRange;
uniform float glowIntensity;
uniform bool enableShadow;
uniform vec4 shadowColor;
uniform vec2 shadowTexCoordOffset;
uniform float shadowSdfSpread;
uniform bool enableInnerEffect;
uniform vec4 innerEffectColor;
uniform float innerEffectRange;
uniform bool innerEffectIsShadow;
uniform bool styleBold;
uniform float boldStrength;
out vec4 finalFragColor;
vec4 alphaBlend(vec4 newColor, vec4 oldColor) {
    float outAlpha = newColor.a + oldColor.a * (1.0 - newColor.a);
    if (outAlpha < 0.0001) return vec4(0.0, 0.0, 0.0, 0.0);
    vec3 outRGB = (newColor.rgb * newColor.a + oldColor.rgb * oldColor.a * (1.0 - newColor.a)) / outAlpha;
    return vec4(outRGB, outAlpha);
}
void main() {
    float mainDistance = texture(sdfTexture, fragTexCoord).r;
    vec4 accumulatedColor = vec4(0.0, 0.0, 0.0, 0.0);
    float effectiveSdfEdge = sdfEdgeValue;
    if (styleBold) { effectiveSdfEdge -= boldStrength; }
    if (enableShadow) {
        float shadowDistance = texture(sdfTexture, fragTexCoord - shadowTexCoordOffset).r;
        float shadowAlpha = smoothstep(sdfEdgeValue - shadowSdfSpread, sdfEdgeValue + shadowSdfSpread, shadowDistance);
        shadowAlpha *= shadowColor.a;
        accumulatedColor = alphaBlend(vec4(shadowColor.rgb, shadowAlpha), accumulatedColor);
    }
    if (enableGlow && glowRange > 0.0) {
        float glowEffectiveOutlineWidth = enableOutline ? outlineWidth : 0.0;
        float glowStartEdge = effectiveSdfEdge - glowEffectiveOutlineWidth;
        float distanceFromObjectEdgeForGlow = glowStartEdge - mainDistance;
        float rawGlowAlpha = 0.0;
        if (distanceFromObjectEdgeForGlow > 0.0) {
            rawGlowAlpha = pow(1.0 - clamp(distanceFromObjectEdgeForGlow / glowRange, 0.0, 1.0), 2.0);
        }
        float finalGlowAlpha = rawGlowAlpha * glowIntensity * glowColor.a;
        accumulatedColor = alphaBlend(vec4(glowColor.rgb, finalGlowAlpha), accumulatedColor);
    }
    if (enableOutline && outlineWidth > 0.0) {
        float outlineOuterEdge = effectiveSdfEdge - outlineWidth;
        float outlineInnerEdge = effectiveSdfEdge;
        float alphaOuter = smoothstep(outlineOuterEdge - sdfSmoothness, outlineOuterEdge + sdfSmoothness, mainDistance);
        float alphaInner = smoothstep(outlineInnerEdge - sdfSmoothness, outlineInnerEdge + sdfSmoothness, mainDistance);
        float outlineAlpha = alphaOuter - alphaInner;
        outlineAlpha = clamp(outlineAlpha, 0.0, 1.0);
        outlineAlpha *= outlineColor.a;
        accumulatedColor = alphaBlend(vec4(outlineColor.rgb, outlineAlpha), accumulatedColor);
    }
    vec4 currentFillRenderColor = textColor;
    float fillAlphaFactor = smoothstep(effectiveSdfEdge - sdfSmoothness, effectiveSdfEdge + sdfSmoothness, mainDistance);
    vec4 fillPixelColor = vec4(currentFillRenderColor.rgb, currentFillRenderColor.a * fillAlphaFactor);
    if (enableInnerEffect && innerEffectRange > 0.0 && fillAlphaFactor > 0.001) {
        float innerEffectTargetEdge = effectiveSdfEdge + innerEffectRange;
        float alphaAtInnerTarget = smoothstep(innerEffectTargetEdge - sdfSmoothness, innerEffectTargetEdge + sdfSmoothness, mainDistance);
        float innerEffectAlpha = fillAlphaFactor - alphaAtInnerTarget;
        innerEffectAlpha = clamp(innerEffectAlpha, 0.0, 1.0);
        innerEffectAlpha *= innerEffectColor.a;
        if (innerEffectIsShadow) {
            fillPixelColor.rgb = mix(fillPixelColor.rgb, fillPixelColor.rgb * innerEffectColor.rgb, innerEffectAlpha);
        } else {
            fillPixelColor.rgb = mix(fillPixelColor.rgb, innerEffectColor.rgb, innerEffectAlpha);
        }
    }
    accumulatedColor = alphaBlend(fillPixelColor, accumulatedColor);
    finalFragColor = accumulatedColor;
}
)"; //

    // BatchRenderState structure remains the same as in your provided RaylibSDFTextEx.cpp
    struct BatchRenderState {
        Texture2D atlasTexture;
        FillStyle fill; //
        FontStyle basicStyle; //
        bool outlineEnabled; Color outlineColor; float outlineWidth;
        bool glowEnabled; Color glowColor; float glowRange; float glowIntensity;
        bool shadowEnabled; Color shadowColor; Vector2 shadowOffset; float shadowSdfSpread;
        bool innerEffectEnabled; Color innerEffectColor; float innerEffectRange; bool innerEffectIsShadow;
        float dynamicSmoothnessValue;

        BatchRenderState() : atlasTexture({0}), basicStyle(FontStyle::Normal), //
                             outlineEnabled(false), outlineColor(BLANK), outlineWidth(0.0f),
                             glowEnabled(false), glowColor(BLANK), glowRange(0.0f), glowIntensity(0.0f),
                             shadowEnabled(false), shadowColor(BLANK), shadowOffset({0,0}), shadowSdfSpread(0.0f),
                             innerEffectEnabled(false), innerEffectColor(BLANK), innerEffectRange(0.0f), innerEffectIsShadow(false),
                             dynamicSmoothnessValue(0.005f) {
            fill.type = FillType::SOLID_COLOR; //
            fill.solidColor = BLACK; //
        }

        BatchRenderState(const PositionedGlyph& glyph, float currentSmoothness) : //
                atlasTexture(glyph.renderInfo.atlasTexture), //
                fill(glyph.appliedStyle.fill), //
                basicStyle(glyph.appliedStyle.basicStyle), //
                outlineEnabled(glyph.appliedStyle.outline.enabled), outlineColor(glyph.appliedStyle.outline.color), outlineWidth(glyph.appliedStyle.outline.width), //
                glowEnabled(glyph.appliedStyle.glow.enabled), glowColor(glyph.appliedStyle.glow.color), glowRange(glyph.appliedStyle.glow.range), glowIntensity(glyph.appliedStyle.glow.intensity), //
                shadowEnabled(glyph.appliedStyle.shadow.enabled), shadowColor(glyph.appliedStyle.shadow.color), shadowOffset(glyph.appliedStyle.shadow.offset), shadowSdfSpread(glyph.appliedStyle.shadow.sdfSpread), //
                innerEffectEnabled(glyph.appliedStyle.innerEffect.enabled), innerEffectColor(glyph.appliedStyle.innerEffect.color), innerEffectRange(glyph.appliedStyle.innerEffect.range), innerEffectIsShadow(glyph.appliedStyle.innerEffect.isShadow), //
                dynamicSmoothnessValue(currentSmoothness) {}
    private:
        bool ColorEquals(Color c1, Color c2) const { return c1.r == c2.r && c1.g == c2.g && c1.b == c2.b && c1.a == c2.a; }
        bool FloatEquals(float f1, float f2, float epsilon = 0.0001f) const { return fabsf(f1 - f2) < epsilon; }
        bool Vector2Equals(Vector2 v1, Vector2 v2, float epsilon = 0.001f) const { return FloatEquals(v1.x, v2.x, epsilon) && FloatEquals(v1.y, v2.y, epsilon); }
        bool GradientStopsEqual(const std::vector<GradientStop>& s1, const std::vector<GradientStop>& s2) const { //
            if (s1.size() != s2.size()) return false;
            for (size_t i = 0; i < s1.size(); ++i) {
                if (!ColorEquals(s1[i].color, s2[i].color) || !FloatEquals(s1[i].position, s2[i].position)) return false;
            }
            return true;
        }
        bool FillStyleEquals(const FillStyle& fs1, const FillStyle& fs2) const { //
            if (fs1.type != fs2.type) return false; //
            if (fs1.type == FillType::SOLID_COLOR) { return ColorEquals(fs1.solidColor, fs2.solidColor); } //
            else if (fs1.type == FillType::LINEAR_GRADIENT) { //
                return Vector2Equals(fs1.linearGradientStart, fs2.linearGradientStart) && //
                       Vector2Equals(fs1.linearGradientEnd, fs2.linearGradientEnd) && //
                       GradientStopsEqual(fs1.gradientStops, fs2.gradientStops); //
            }
            return true;
        }
    public:
        bool RequiresNewBatchComparedTo(const BatchRenderState& other) const {
            if (atlasTexture.id != other.atlasTexture.id) return true;
            if (!FillStyleEquals(fill, other.fill)) return true;
            if (basicStyle != other.basicStyle) return true; //
            if (outlineEnabled != other.outlineEnabled) return true;
            if (outlineEnabled && (!ColorEquals(outlineColor, other.outlineColor) || !FloatEquals(outlineWidth, other.outlineWidth))) return true;
            if (glowEnabled != other.glowEnabled) return true;
            if (glowEnabled && (!ColorEquals(glowColor, other.glowColor) || !FloatEquals(glowRange, other.glowRange) || !FloatEquals(glowIntensity, other.glowIntensity))) return true;
            if (shadowEnabled != other.shadowEnabled) return true;
            if (shadowEnabled && (!ColorEquals(shadowColor, other.shadowColor) || !Vector2Equals(shadowOffset, other.shadowOffset) || !FloatEquals(shadowSdfSpread, other.shadowSdfSpread))) return true;
            if (innerEffectEnabled != other.innerEffectEnabled) return true;
            if (innerEffectEnabled && (!ColorEquals(innerEffectColor, other.innerEffectColor) || !FloatEquals(innerEffectRange, other.innerEffectRange) || innerEffectIsShadow != other.innerEffectIsShadow)) return true;
            if (!FloatEquals(dynamicSmoothnessValue, other.dynamicSmoothnessValue)) return true;
            return false;
        }
    }; //

    class FTTextEngineImpl : public ITextEngine {
    private:
        FT_Library ftLibrary_ = nullptr;
        std::map<FontId, FTFontData> loadedFonts_;
        FontId nextFontId_ = 1;
        FontId defaultFontId_ = INVALID_FONT_ID; //
        std::map<FontId, std::vector<FontId>> fontFallbackChains_; // For font fallback

        std::list<FTGlyphCacheKey> lru_glyph_list_;
        std::unordered_map<FTGlyphCacheKey, std::pair<FTCachedGlyph, std::list<FTGlyphCacheKey>::iterator>, FTGlyphCacheKeyHash> glyph_cache_map_;
        size_t glyph_cache_capacity_ = 512;

        std::vector<Image> atlas_images_;
        std::vector<Texture2D> atlas_textures_;
        int current_atlas_idx_ = -1;
        Vector2 current_atlas_pen_pos_ = {0, 0};
        float current_atlas_max_row_height_ = 0.0f;
        int atlas_width_ = 1024;
        int atlas_height_ = 1024;
        GlyphAtlasType atlas_type_hint_ = GlyphAtlasType::SDF_BITMAP; //

        Shader sdfShader_ = {0};
        // Shader uniform locations (remains the same)
        int uniform_sdfTexture_loc_ = -1, uniform_textColor_loc_ = -1, uniform_sdfEdgeValue_loc_ = -1, uniform_sdfSmoothness_loc_ = -1;
        int uniform_enableOutline_loc_ = -1, uniform_outlineColor_loc_ = -1, uniform_outlineWidth_loc_ = -1;
        int uniform_enableGlow_loc_ = -1, uniform_glowColor_loc_ = -1, uniform_glowRange_loc_ = -1, uniform_glowIntensity_loc_ = -1;
        int uniform_enableShadow_loc_ = -1, uniform_shadowColor_loc_ = -1, uniform_shadowTexCoordOffset_loc_ = -1, uniform_shadowSdfSpread_loc_ = -1;
        int uniform_enableInnerEffect_loc_ = -1, uniform_innerEffectColor_loc_ = -1, uniform_innerEffectRange_loc_ = -1, uniform_innerEffectIsShadow_loc_ = -1;
        int uniform_styleBold_loc_ = -1, uniform_boldStrength_loc_ = -1;


        // UTF-8/16 conversion helpers (remains the same)
        std::u16string Utf8ToUtf16(const std::string& u8_str) const {
            if (u8_str.empty()) return std::u16string();
            std::u16string u16_str;
            UErrorCode error_code = U_ZERO_ERROR;
            int32_t u16_len = 0;
            u_strFromUTF8(nullptr, 0, &u16_len, u8_str.c_str(), static_cast<int32_t>(u8_str.length()), &error_code);
            if (error_code == U_BUFFER_OVERFLOW_ERROR || (error_code == U_ZERO_ERROR && u16_len > 0)) {
                error_code = U_ZERO_ERROR;
                u16_str.resize(u16_len);
                u_strFromUTF8(reinterpret_cast<UChar*>(&u16_str[0]), u16_len, nullptr, u8_str.c_str(), static_cast<int32_t>(u8_str.length()), &error_code);
                if (U_FAILURE(error_code)) {
                    TraceLog(LOG_WARNING, "FTTextEngine: ICU u_strFromUTF8 failed: %s", u_errorName(error_code));
                    return std::u16string();
                }
            } else if (error_code == U_ZERO_ERROR && u16_len == 0) { // Empty input string
                return std::u16string();
            }
            else {
                TraceLog(LOG_WARNING, "FTTextEngine: ICU u_strFromUTF8 preflight failed: %s (len: %d, input_len: %zu)", u_errorName(error_code), u16_len, u8_str.length());
                return std::u16string();
            }
            return u16_str;
        }

        std::string Utf16ToUtf8(const std::u16string& u16_str) const {
            if (u16_str.empty()) return std::string();
            std::string u8_str;
            UErrorCode error_code = U_ZERO_ERROR;
            int32_t u8_len = 0;
            u_strToUTF8(nullptr, 0, &u8_len, reinterpret_cast<const UChar*>(u16_str.data()), static_cast<int32_t>(u16_str.length()), &error_code);
            if (error_code == U_BUFFER_OVERFLOW_ERROR || (error_code == U_ZERO_ERROR && u8_len > 0)) {
                error_code = U_ZERO_ERROR;
                u8_str.resize(u8_len);
                u_strToUTF8(&u8_str[0], u8_len, nullptr, reinterpret_cast<const UChar*>(u16_str.data()), static_cast<int32_t>(u16_str.length()), &error_code);
                if (U_FAILURE(error_code)) {
                    TraceLog(LOG_WARNING, "FTTextEngine: ICU u_strToUTF8 failed: %s", u_errorName(error_code));
                    return std::string();
                }
            } else if (error_code == U_ZERO_ERROR && u8_len == 0) { // Empty input string
                return std::string();
            } else {
                TraceLog(LOG_WARNING, "FTTextEngine: ICU u_strToUTF8 preflight failed: %s", u_errorName(error_code));
                return std::string();
            }
            return u8_str;
        }
        // Helper: Given a UTF-8 string and a byte offset, find the UTF-16 code unit offset.
        int32_t Utf8ByteOffsetToUtf16CodeUnitOffset(const std::string& utf8RunText, uint32_t utf8ByteOffsetInRun) const {
            if (utf8ByteOffsetInRun == 0) return 0;
            if (utf8RunText.empty()) return 0;

            // Ensure the offset is within the bounds of the string to avoid substr errors
            uint32_t clampedOffset = std::min(utf8ByteOffsetInRun, (uint32_t)utf8RunText.length());

            std::string prefixU8 = utf8RunText.substr(0, clampedOffset);
            std::u16string prefixU16 = Utf8ToUtf16(prefixU8);
            return static_cast<int32_t>(prefixU16.length());
        }


        hb_script_t HbScriptFromString(const char* s) const { //
            if (!s || s[0] == '\0') return HB_SCRIPT_UNKNOWN;
            return hb_script_from_string(s, -1);
        }

        hb_language_t HbLanguageFromString(const char* s) const { //
            if (!s || s[0] == '\0') return hb_language_get_default();
            return hb_language_from_string(s, -1);
        }

        // findSpaceInAtlasAndPack (remains the same)
        Rectangle findSpaceInAtlasAndPack(int width, int height, const unsigned char* bitmapData, PixelFormat format) {
            if (width <= 0 || height <= 0 || !bitmapData) return {0,0,0,0};

            bool packed_in_current_atlas = false;
            if (current_atlas_idx_ != -1) {
                if (current_atlas_pen_pos_.x + width <= atlas_width_ && current_atlas_pen_pos_.y + height <= atlas_height_) {
                    // Fits in current row
                } else { // Try next row in current atlas
                    current_atlas_pen_pos_.x = 0;
                    current_atlas_pen_pos_.y += current_atlas_max_row_height_;
                    current_atlas_max_row_height_ = 0;
                }
                if (current_atlas_pen_pos_.y + height <= atlas_height_ && current_atlas_pen_pos_.x + width <= atlas_width_) {
                    // Check again after potentially moving to next row
                    packed_in_current_atlas = true;
                }
            }

            if (!packed_in_current_atlas) { // Needs new atlas or first atlas
                current_atlas_idx_++;
                current_atlas_pen_pos_ = {0, 0};
                current_atlas_max_row_height_ = 0;

                if (static_cast<size_t>(current_atlas_idx_) >= atlas_images_.size()) {
                    // Create new atlas image and texture
                    Image new_atlas_image = GenImageColor(atlas_width_, atlas_height_, BLANK);
                    ImageFormat(&new_atlas_image, PIXELFORMAT_UNCOMPRESSED_GRAYSCALE); // SDFs are grayscale
                    if (new_atlas_image.data) {
                        // Initialize to 0 (transparent for SDF, black for alpha)
                        memset(new_atlas_image.data, 0, (size_t)atlas_width_ * atlas_height_ * GetPixelDataSize(1,1,PIXELFORMAT_UNCOMPRESSED_GRAYSCALE));
                    } else {
                        TraceLog(LOG_ERROR, "FTTextEngine: Failed to GenImageColor or format for new atlas %d", current_atlas_idx_);
                        current_atlas_idx_--; // Revert index increment
                        return {0,0,0,0};
                    }
                    atlas_images_.push_back(new_atlas_image);

                    Texture2D new_texture = LoadTextureFromImage(atlas_images_.back());
                    if (new_texture.id == 0) {
                        TraceLog(LOG_ERROR, "FTTextEngine: Failed to load texture from new atlas image %d", current_atlas_idx_);
                        UnloadImage(atlas_images_.back()); atlas_images_.pop_back();
                        current_atlas_idx_--;
                        return {0,0,0,0};
                    }
                    SetTextureFilter(new_texture, TEXTURE_FILTER_BILINEAR); // Good for SDF
                    atlas_textures_.push_back(new_texture);
                }
            }
            // Final check if it can be packed after potentially selecting/creating an atlas
            if (current_atlas_idx_ < 0 || static_cast<size_t>(current_atlas_idx_) >= atlas_textures_.size() ||
                width > atlas_width_ || height > atlas_height_ || current_atlas_pen_pos_.y + height > atlas_height_ || current_atlas_pen_pos_.x + width > atlas_width_) {
                TraceLog(LOG_WARNING, "FTTextEngine: Glyph %dx%d cannot be packed into atlas %d (%dx%d) at current pos (%.0f, %.0f). Might need larger/more atlases.",
                         width, height, current_atlas_idx_, atlas_width_, atlas_height_, current_atlas_pen_pos_.x, current_atlas_pen_pos_.y);
                return {0,0,0,0};
            }


            Rectangle spot = {current_atlas_pen_pos_.x, current_atlas_pen_pos_.y, (float)width, (float)height};
            Image* currentImageToUpdate = &atlas_images_[current_atlas_idx_];
            Image glyphImage = { const_cast<unsigned char*>(bitmapData), width, height, 1, format };

            ImageDraw(currentImageToUpdate, glyphImage, {0,0,(float)width,(float)height}, spot, WHITE); // Draw new glyph onto atlas image
            UpdateTextureRec(atlas_textures_[current_atlas_idx_], spot, bitmapData); // Update GPU texture

            current_atlas_pen_pos_.x += width;
            current_atlas_max_row_height_ = std::max(current_atlas_max_row_height_, (float)height);
            return spot;
        }

        // **MODIFIED/NEW** getOrCacheGlyph - now takes codepoint and handles fallback.
        // actualFontIdUsed will be populated with the FontId that actually provided the glyph.
        FTCachedGlyph getOrCacheGlyph(FontId requestedFontId, uint32_t codepoint, float fontSizeForRender, FontId& actualFontIdUsed) {
            actualFontIdUsed = INVALID_FONT_ID;
            uint32_t glyphIndex = 0;
            bool isWhitespace = (codepoint == ' ' || codepoint == '\t' || codepoint == '\n' || codepoint == 0x3000);

            // 1. Try the requested font
            if (IsFontValid(requestedFontId)) {
                const auto& fontData = loadedFonts_.at(requestedFontId);
                glyphIndex = FT_Get_Char_Index(fontData.ftFace, codepoint);
                if (glyphIndex != 0 || isWhitespace) { // Found or is whitespace (whitespace GID might be 0 but is valid)
                    actualFontIdUsed = requestedFontId;
                }
            }

            // 2. If not found in requested font (and not whitespace that was "found" as GID 0), try fallback chain
            if (actualFontIdUsed == INVALID_FONT_ID && !isWhitespace) {
                auto fallbackIt = fontFallbackChains_.find(requestedFontId);
                if (fallbackIt != fontFallbackChains_.end()) {
                    for (FontId fallbackFontId : fallbackIt->second) {
                        if (IsFontValid(fallbackFontId)) {
                            const auto& fallbackFontData = loadedFonts_.at(fallbackFontId);
                            glyphIndex = FT_Get_Char_Index(fallbackFontData.ftFace, codepoint);
                            if (glyphIndex != 0) {
                                actualFontIdUsed = fallbackFontId;
                                TraceLog(LOG_DEBUG, "FTTextEngine: Codepoint %u (char '%lc') found in fallback FontID %d (GID %u)", codepoint, (wchar_t)codepoint, actualFontIdUsed, glyphIndex);
                                break;
                            }
                        }
                    }
                }
            }

            // 3. If still not found (and not whitespace), try default font
            if (actualFontIdUsed == INVALID_FONT_ID && !isWhitespace && defaultFontId_ != INVALID_FONT_ID && defaultFontId_ != requestedFontId) {
                // Check if default font was already tried in fallback chain for requestedFontId
                bool defaultWasInFallback = false;
                auto fallbackIt = fontFallbackChains_.find(requestedFontId);
                if (fallbackIt != fontFallbackChains_.end()) {
                    for (FontId fid : fallbackIt->second) if (fid == defaultFontId_) defaultWasInFallback = true;
                }

                if (!defaultWasInFallback && IsFontValid(defaultFontId_)) {
                    const auto& defaultFontData = loadedFonts_.at(defaultFontId_);
                    glyphIndex = FT_Get_Char_Index(defaultFontData.ftFace, codepoint);
                    if (glyphIndex != 0) {
                        actualFontIdUsed = defaultFontId_;
                        TraceLog(LOG_DEBUG, "FTTextEngine: Codepoint %u (char '%lc') found in default FontID %d (GID %u)", codepoint, (wchar_t)codepoint, actualFontIdUsed, glyphIndex);
                    }
                }
            }

            // 4. If still not found (could be whitespace from an invalid initial font, or truly missing char), use .notdef from a valid font
            if (actualFontIdUsed == INVALID_FONT_ID) {
                FontId fontForNotdef = INVALID_FONT_ID;
                if (IsFontValid(requestedFontId)) fontForNotdef = requestedFontId;
                else if (IsFontValid(defaultFontId_)) fontForNotdef = defaultFontId_;
                // Else, pick first available font if any? This case should be rare if default font is always set.

                if (IsFontValid(fontForNotdef)) {
                    const auto& fontData = loadedFonts_.at(fontForNotdef);
                    // For whitespace, FT_Get_Char_Index on a valid font is fine even if it returns 0.
                    // For non-whitespace, GID 0 is the .notdef glyph.
                    glyphIndex = FT_Get_Char_Index(fontData.ftFace, codepoint);
                    if (!isWhitespace && glyphIndex != 0) { // It's not a whitespace, but we found a specific glyph for it in the 'notdef' font choice. Use it.
                        TraceLog(LOG_DEBUG, "FTTextEngine: Codepoint %u found in designated notdef font %d (GID %u) before forcing notdef GID.", codepoint, fontForNotdef, glyphIndex);
                    } else if (!isWhitespace && glyphIndex == 0) { // It's not whitespace and font doesn't have it, explicitly use GID 0.
                        TraceLog(LOG_DEBUG, "FTTextEngine: Codepoint %u NOT found. Using .notdef (GID 0) from FontID %d", codepoint, fontForNotdef);
                    }
                    // If it IS whitespace, glyphIndex (possibly 0) is what we use.
                    actualFontIdUsed = fontForNotdef;
                } else {
                    TraceLog(LOG_ERROR, "FTTextEngine: No valid font available to render codepoint %u or its .notdef glyph.", codepoint);
                    return {}; // Cannot proceed
                }
            }
            // At this point, actualFontIdUsed should be valid, and glyphIndex is set.

            const auto& chosenFontData = loadedFonts_.at(actualFontIdUsed);
            int sdfGenSize = chosenFontData.sdfPixelSizeHint > 0 ? chosenFontData.sdfPixelSizeHint : 64; // Use actual font's hint

            FTGlyphCacheKey key = {actualFontIdUsed, glyphIndex, sdfGenSize, (atlas_type_hint_ == GlyphAtlasType::SDF_BITMAP)};

            auto cacheIt = glyph_cache_map_.find(key);
            if (cacheIt != glyph_cache_map_.end()) {
                lru_glyph_list_.splice(lru_glyph_list_.begin(), lru_glyph_list_, cacheIt->second.second);
                return cacheIt->second.first;
            }

            FTCachedGlyph newCachedGlyph;
            // newCachedGlyph.originalCodepoint = codepoint; // For debugging
            newCachedGlyph.renderInfo.isSDF = key.isSDF;
            FT_Face faceToRender = chosenFontData.ftFace;

            FT_Error error = FT_Set_Pixel_Sizes(faceToRender, 0, (FT_UInt)sdfGenSize);
            if (error) { TraceLog(LOG_WARNING, "FTTextEngine: FT_Set_Pixel_Sizes failed (glyph %u, font %d, size %d): %s", glyphIndex, actualFontIdUsed, sdfGenSize, FT_Error_String(error)); return {}; }

            int load_flags = FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING;
            error = FT_Load_Glyph(faceToRender, glyphIndex, load_flags);
            if (error) {
                TraceLog(LOG_WARNING, "FTTextEngine: FT_Load_Glyph failed (glyph %u, font %d): %s", glyphIndex, actualFontIdUsed, FT_Error_String(error));
                // If loading GID 0 itself failed, something is very wrong with the font.
                if (glyphIndex != 0) return getOrCacheGlyph(actualFontIdUsed, 0 /*.notdef GID*/, fontSizeForRender, actualFontIdUsed); // Try .notdef of this font
                return {};
            }

            FT_GlyphSlot slot = faceToRender->glyph;
            if (key.isSDF) error = FT_Render_Glyph(slot, FT_RENDER_MODE_SDF);
            else error = FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL);
            if (error) { TraceLog(LOG_WARNING, "FTTextEngine: FT_Render_Glyph (%s) failed (glyph %u, font %d): %s", key.isSDF?"SDF":"Normal", glyphIndex, actualFontIdUsed, FT_Error_String(error)); return {};}

            // Store metrics at the cached size (sdfGenSize)
            newCachedGlyph.advanceX_at_cached_size = (float)slot->metrics.horiAdvance / 64.0f;
            newCachedGlyph.ascent_at_cached_size = (float)slot->metrics.horiBearingY / 64.0f;
            newCachedGlyph.descent_at_cached_size = (float)(slot->metrics.height - slot->metrics.horiBearingY) / 64.0f;


            if (slot->bitmap.buffer && slot->bitmap.width > 0 && slot->bitmap.rows > 0) {
                Rectangle pack_rect = findSpaceInAtlasAndPack(slot->bitmap.width, slot->bitmap.rows, slot->bitmap.buffer, PIXELFORMAT_UNCOMPRESSED_GRAYSCALE);
                if (pack_rect.width > 0) {
                    newCachedGlyph.renderInfo.atlasTexture = atlas_textures_[current_atlas_idx_];
                    newCachedGlyph.renderInfo.atlasRect = pack_rect;
                    newCachedGlyph.renderInfo.drawOffset.x = (float)slot->bitmap_left;
                    newCachedGlyph.renderInfo.drawOffset.y = -(float)slot->bitmap_top;
                } else { newCachedGlyph.renderInfo.atlasTexture.id = 0; }
            } else { // For whitespace or empty glyphs
                newCachedGlyph.renderInfo.atlasTexture.id = 0;
                newCachedGlyph.renderInfo.atlasRect = {0,0,0,0};
                newCachedGlyph.renderInfo.drawOffset = {0,0};
            }

            // Cache management (remains the same)
            if (glyph_cache_map_.size() >= glyph_cache_capacity_ && !lru_glyph_list_.empty()) {
                glyph_cache_map_.erase(lru_glyph_list_.back());
                lru_glyph_list_.pop_back();
            }
            lru_glyph_list_.push_front(key);
            glyph_cache_map_[key] = {newCachedGlyph, lru_glyph_list_.begin()};
            return newCachedGlyph;
        }


        void performCacheCleanup() { //
            for (Texture2D tex : atlas_textures_) if (tex.id > 0) UnloadTexture(tex);
            atlas_textures_.clear();
            for (Image img : atlas_images_) if (img.data) UnloadImage(img);
            atlas_images_.clear();
            glyph_cache_map_.clear(); lru_glyph_list_.clear();
            current_atlas_idx_ = -1; current_atlas_pen_pos_ = {0,0}; current_atlas_max_row_height_ = 0.0f;
        }


    public:
        FTTextEngineImpl() { // Constructor remains mostly the same
            if (FT_Init_FreeType(&ftLibrary_)) {
                TraceLog(LOG_FATAL, "FTTextEngine: Could not initialize FreeType library");
                ftLibrary_ = nullptr; return;
            }
            sdfShader_ = LoadShaderFromMemory(nullptr, ftSdfMasterFragmentShaderSrc);
            if (sdfShader_.id == rlGetShaderIdDefault()) { TraceLog(LOG_WARNING, "FTTextEngine: SDF shader failed to load."); }
            else {
                TraceLog(LOG_INFO, "FTTextEngine: SDF shader loaded (ID: %d).", sdfShader_.id);
                uniform_sdfTexture_loc_ = GetShaderLocation(sdfShader_, "sdfTexture");
                uniform_textColor_loc_ = GetShaderLocation(sdfShader_, "textColor");
                uniform_sdfEdgeValue_loc_ = GetShaderLocation(sdfShader_, "sdfEdgeValue");
                uniform_sdfSmoothness_loc_ = GetShaderLocation(sdfShader_, "sdfSmoothness");
                uniform_enableOutline_loc_ = GetShaderLocation(sdfShader_, "enableOutline");
                uniform_outlineColor_loc_ = GetShaderLocation(sdfShader_, "outlineColor");
                uniform_outlineWidth_loc_ = GetShaderLocation(sdfShader_, "outlineWidth");
                uniform_enableGlow_loc_ = GetShaderLocation(sdfShader_, "enableGlow");
                uniform_glowColor_loc_ = GetShaderLocation(sdfShader_, "glowColor");
                uniform_glowRange_loc_ = GetShaderLocation(sdfShader_, "glowRange");
                uniform_glowIntensity_loc_ = GetShaderLocation(sdfShader_, "glowIntensity");
                uniform_enableShadow_loc_ = GetShaderLocation(sdfShader_, "enableShadow");
                uniform_shadowColor_loc_ = GetShaderLocation(sdfShader_, "shadowColor");
                uniform_shadowTexCoordOffset_loc_ = GetShaderLocation(sdfShader_, "shadowTexCoordOffset");
                uniform_shadowSdfSpread_loc_ = GetShaderLocation(sdfShader_, "shadowSdfSpread");
                uniform_enableInnerEffect_loc_ = GetShaderLocation(sdfShader_, "enableInnerEffect");
                uniform_innerEffectColor_loc_ = GetShaderLocation(sdfShader_, "innerEffectColor");
                uniform_innerEffectRange_loc_ = GetShaderLocation(sdfShader_, "innerEffectRange");
                uniform_innerEffectIsShadow_loc_ = GetShaderLocation(sdfShader_, "innerEffectIsShadow");
                uniform_styleBold_loc_ = GetShaderLocation(sdfShader_, "styleBold");
                uniform_boldStrength_loc_ = GetShaderLocation(sdfShader_, "boldStrength");
            }
            glyph_cache_capacity_ = 512; atlas_width_ = 1024; atlas_height_ = 1024;
            atlas_type_hint_ = GlyphAtlasType::SDF_BITMAP; //
        }

        ~FTTextEngineImpl() override { // Destructor remains the same
            performCacheCleanup();
            for (auto& pair : loadedFonts_) {
                if (pair.second.hbFont) hb_font_destroy(pair.second.hbFont);
                if (pair.second.ftFace) FT_Done_Face(pair.second.ftFace);
            }
            loadedFonts_.clear();
            fontFallbackChains_.clear();
            if (ftLibrary_) FT_Done_FreeType(ftLibrary_);
            if (sdfShader_.id > 0 && sdfShader_.id != rlGetShaderIdDefault()) UnloadShader(sdfShader_);
        }

        // --- Font Management ---
        FontId LoadFont(const char* filePath, int faceIndex = 0) override { //
            if (!ftLibrary_) {
                TraceLog(LOG_ERROR, "FTTextEngine: FreeType library not initialized. Cannot load font.");
                return INVALID_FONT_ID; //
            }

            std::ifstream file(filePath, std::ios::binary | std::ios::ate);
            if (!file.is_open()) {
                TraceLog(LOG_WARNING, "FTTextEngine: Failed to open font file: %s", filePath);
                return INVALID_FONT_ID; //
            }
            std::streamsize fileSize = file.tellg();
            file.seekg(0, std::ios::beg);

            FTFontData fontData;
            fontData.fontBuffer.resize(fileSize);
            if (!file.read(reinterpret_cast<char*>(fontData.fontBuffer.data()), fileSize)) {
                TraceLog(LOG_WARNING, "FTTextEngine: Failed to read font file: %s", filePath);
                file.close();
                return INVALID_FONT_ID; //
            }
            file.close();

            FT_Error error = FT_New_Memory_Face(ftLibrary_,
                                                fontData.fontBuffer.data(),
                                                static_cast<FT_Long>(fontData.fontBuffer.size()),
                                                faceIndex,
                                                &fontData.ftFace);
            if (error) {
                TraceLog(LOG_WARNING, "FTTextEngine: FT_New_Memory_Face failed for %s (face %d): %s", filePath, faceIndex, FT_Error_String(error));
                return INVALID_FONT_ID; //
            }
            float initialPixelSizeForHbSetup = (float)(fontData.sdfPixelSizeHint > 0 ? fontData.sdfPixelSizeHint : 64);
            error = FT_Set_Pixel_Sizes(fontData.ftFace, 0, static_cast<FT_UInt>(roundf(initialPixelSizeForHbSetup)));
            if (error) {
                TraceLog(LOG_WARNING, "FTTextEngine: LoadFont: Initial FT_Set_Pixel_Sizes (%.1fpx) failed: %s.", initialPixelSizeForHbSetup, FT_Error_String(error));
            }

            hb_face_t* hbFace = hb_ft_face_create_referenced(fontData.ftFace);
            if (!hbFace || hbFace == hb_face_get_empty()) {
                TraceLog(LOG_WARNING, "FTTextEngine: hb_ft_face_create_referenced failed for %s", filePath);
                FT_Done_Face(fontData.ftFace); fontData.ftFace = nullptr;
                return INVALID_FONT_ID; //
            }

            fontData.hbFont = hb_font_create(hbFace);
            hb_face_destroy(hbFace);
            hbFace = nullptr;

            if (!fontData.hbFont || fontData.hbFont == hb_font_get_empty()) {
                TraceLog(LOG_WARNING, "FTTextEngine: hb_font_create failed for %s", filePath);
                FT_Done_Face(fontData.ftFace); fontData.ftFace = nullptr;
                return INVALID_FONT_ID; //
            }
            hb_font_t* hb_ft_parent = hb_ft_font_create_referenced(fontData.ftFace);
            if (!hb_ft_parent || hb_ft_parent == hb_font_get_empty()) {
                TraceLog(LOG_WARNING, "FTTextEngine: hb_ft_font_create_referenced (for parent) failed for %s", filePath);
                hb_font_destroy(fontData.hbFont); fontData.hbFont = nullptr;
                FT_Done_Face(fontData.ftFace); fontData.ftFace = nullptr;
                return INVALID_FONT_ID; //
            }
            hb_font_set_parent(fontData.hbFont, hb_ft_parent);
            hb_font_destroy(hb_ft_parent);
            hb_ft_parent = nullptr;
            hb_ft_font_set_load_flags(fontData.hbFont, FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING);

            hb_font_funcs_t *custom_funcs = hb_font_funcs_create();
            if (!custom_funcs) {
                TraceLog(LOG_ERROR, "FTTextEngine: hb_font_funcs_create failed for custom_funcs!");
                hb_font_destroy(fontData.hbFont); fontData.hbFont = nullptr;
                FT_Done_Face(fontData.ftFace); fontData.ftFace = nullptr;
                return INVALID_FONT_ID; //
            }
            hb_font_funcs_set_glyph_h_advances_func(custom_funcs, my_custom_get_glyph_h_advances_callback, nullptr,nullptr); //
            hb_font_set_funcs(fontData.hbFont, custom_funcs, fontData.ftFace, nullptr);
            hb_font_funcs_destroy(custom_funcs);
            custom_funcs = nullptr;

            fontData.properties.unitsPerEm = fontData.ftFace->units_per_EM; //
            if (fontData.properties.unitsPerEm == 0) fontData.properties.unitsPerEm = 1000; //

            TT_OS2* os2 = (TT_OS2*)FT_Get_Sfnt_Table(fontData.ftFace, ft_sfnt_os2);
            if (os2 && os2->version != 0xFFFF) {
                fontData.properties.hasTypoMetrics = true; //
                fontData.properties.typoAscender = os2->sTypoAscender; //
                fontData.properties.typoDescender = os2->sTypoDescender; //
                fontData.properties.typoLineGap = os2->sTypoLineGap; //
                fontData.yStrikeoutPosition_fontUnits = os2->yStrikeoutPosition;
                fontData.yStrikeoutSize_fontUnits = os2->yStrikeoutSize;
            } else {
                fontData.properties.hasTypoMetrics = false; //
                fontData.yStrikeoutPosition_fontUnits = (fontData.ftFace->ascender * 2) / 5 ;
                fontData.yStrikeoutSize_fontUnits = fontData.ftFace->underline_thickness > 0 ? fontData.ftFace->underline_thickness : fontData.ftFace->units_per_EM / 20;
            }
            fontData.properties.hheaAscender = fontData.ftFace->ascender;  //
            fontData.properties.hheaDescender = fontData.ftFace->descender; //
            fontData.properties.hheaLineGap = fontData.ftFace->height - (fontData.ftFace->ascender - fontData.ftFace->descender); //
            //fontData.sdfPixelSizeHint = 64; // Default value

            FontId id = nextFontId_++;
            loadedFonts_[id] = std::move(fontData);

            if (defaultFontId_ == INVALID_FONT_ID) { //
                SetDefaultFont(id); //
            }
            TraceLog(LOG_INFO, "FTTextEngine: Font '%s' (face %d) loaded (ID: %d).", filePath, faceIndex, id);
            return id;
        }


        void UnloadFont(FontId fontId) override { //
            auto it = loadedFonts_.find(fontId);
            if (it != loadedFonts_.end()) {
                if (it->second.hbFont) hb_font_destroy(it->second.hbFont);
                if (it->second.ftFace) FT_Done_Face(it->second.ftFace);
                loadedFonts_.erase(it);

                fontFallbackChains_.erase(fontId); // Remove any fallback chains defined for this font
                for(auto& pair : fontFallbackChains_){ // Remove this font if it's in any other font's fallback chain
                    pair.second.erase(std::remove(pair.second.begin(), pair.second.end(), fontId), pair.second.end());
                }

                auto lru_it = lru_glyph_list_.begin();
                while (lru_it != lru_glyph_list_.end()) {
                    if (lru_it->fontId == fontId) {
                        glyph_cache_map_.erase(*lru_it);
                        lru_it = lru_glyph_list_.erase(lru_it);
                    } else {
                        ++lru_it;
                    }
                }
                TraceLog(LOG_INFO, "FTTextEngine: Font ID %d unloaded.", fontId);
                if (defaultFontId_ == fontId) defaultFontId_ = loadedFonts_.empty() ? INVALID_FONT_ID : loadedFonts_.begin()->first; //
            }
        }

        bool IsFontValid(FontId fontId) const override { return loadedFonts_.count(fontId) > 0; } //
        FontId GetDefaultFont() const override { return defaultFontId_; } //
        void SetDefaultFont(FontId fontId) override { //
            if (IsFontValid(fontId) || fontId == INVALID_FONT_ID) defaultFontId_ = fontId; //
            else TraceLog(LOG_WARNING, "FTTextEngine: Invalid FontID %d for default.", fontId);
        }

        // **IMPLEMENTED**
        void SetFontFallbackChain(FontId primaryFont, const std::vector<FontId>& fallbackChain) override {
            if (!IsFontValid(primaryFont)) {
                TraceLog(LOG_WARNING, "FTTextEngine: SetFontFallbackChain: Invalid primaryFont ID: %d", primaryFont);
                return;
            }
            std::vector<FontId> validFallbackChain;
            for (FontId fallbackFontId : fallbackChain) {
                if (IsFontValid(fallbackFontId)) {
                    validFallbackChain.push_back(fallbackFontId);
                } else {
                    TraceLog(LOG_WARNING, "FTTextEngine: SetFontFallbackChain: Invalid fallbackFont ID: %d for primaryFont ID: %d. Skipping.", fallbackFontId, primaryFont);
                }
            }
            fontFallbackChains_[primaryFont] = validFallbackChain;
            TraceLog(LOG_INFO, "FTTextEngine: Fallback chain set for FontID %d with %zu valid fallbacks.", primaryFont, validFallbackChain.size());
        }

        // **IMPLEMENTED**
        bool IsCodepointAvailable(FontId fontId, uint32_t codepoint, bool checkFallback = true) const override {
            if (IsFontValid(fontId)) {
                const auto& fontDataIt = loadedFonts_.find(fontId);
                if (fontDataIt != loadedFonts_.end() && fontDataIt->second.ftFace) {
                    if (FT_Get_Char_Index(fontDataIt->second.ftFace, codepoint) != 0) {
                        return true;
                    }
                }
            }

            if (checkFallback) {
                auto fallbackChainIt = fontFallbackChains_.find(fontId);
                if (fallbackChainIt != fontFallbackChains_.end()) {
                    for (FontId fallbackFontId : fallbackChainIt->second) {
                        if (IsFontValid(fallbackFontId)) {
                            const auto& fallbackFontDataIt = loadedFonts_.find(fallbackFontId);
                            if (fallbackFontDataIt != loadedFonts_.end() && fallbackFontDataIt->second.ftFace) {
                                if (FT_Get_Char_Index(fallbackFontDataIt->second.ftFace, codepoint) != 0) {
                                    return true;
                                }
                            }
                        }
                    }
                }
            }
            // As a last resort, check the application's default font if it's different from the primary and not already checked via fallback
            if (defaultFontId_ != INVALID_FONT_ID && defaultFontId_ != fontId) { //
                bool checked_default_in_fallback = false;
                if (checkFallback) {
                    auto fallbackChainIt = fontFallbackChains_.find(fontId);
                    if (fallbackChainIt != fontFallbackChains_.end()) {
                        for(FontId fid : fallbackChainIt->second) if(fid == defaultFontId_) checked_default_in_fallback = true;
                    }
                }
                if(!checked_default_in_fallback && IsFontValid(defaultFontId_)){
                    const auto& defaultFontDataIt = loadedFonts_.find(defaultFontId_);
                    if (defaultFontDataIt != loadedFonts_.end() && defaultFontDataIt->second.ftFace) {
                        if (FT_Get_Char_Index(defaultFontDataIt->second.ftFace, codepoint) != 0) {
                            return true;
                        }
                    }
                }
            }
            return false;
        }

        FontProperties GetFontProperties(FontId fontId) const override { if (IsFontValid(fontId)) return loadedFonts_.at(fontId).properties; return {}; } //
        ScaledFontMetrics GetScaledFontMetrics(FontId fontId, float fontSize) const override { //
            ScaledFontMetrics metrics; //
            if (!IsFontValid(fontId) || fontSize <= 0) { metrics.ascent= (fontSize > 0 ? fontSize : 16.f)*0.75f; metrics.descent= (fontSize > 0 ? fontSize : 16.f)*0.25f; metrics.recommendedLineHeight=metrics.ascent+metrics.descent; return metrics;} //
            const auto& fontData = loadedFonts_.at(fontId); FT_Face face = fontData.ftFace;
            FT_Error error = FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(roundf(fontSize)));
            if (error) { metrics.ascent=(fontSize > 0 ? fontSize : 16.f)*0.75f; metrics.descent=(fontSize > 0 ? fontSize : 16.f)*0.25f; metrics.recommendedLineHeight=metrics.ascent+metrics.descent; return metrics;} //

            if (face->units_per_EM > 0) metrics.scale = fontSize / (float)face->units_per_EM; else metrics.scale = 1.0f; //

            if (fontData.properties.hasTypoMetrics) { //
                metrics.ascent = (float)fontData.properties.typoAscender * metrics.scale; metrics.descent = -(float)fontData.properties.typoDescender * metrics.scale; metrics.lineGap = (float)fontData.properties.typoLineGap * metrics.scale; //
            } else {
                metrics.ascent = (float)fontData.properties.hheaAscender * metrics.scale; metrics.descent = -(float)fontData.properties.hheaDescender * metrics.scale; metrics.lineGap = (float)fontData.properties.hheaLineGap * metrics.scale; //
            }
            metrics.recommendedLineHeight = metrics.ascent + metrics.descent + metrics.lineGap; //
            if (metrics.recommendedLineHeight <= 0.001f) metrics.recommendedLineHeight = fontSize * 1.2f;

            TT_OS2* os2 = (TT_OS2*)FT_Get_Sfnt_Table(face, ft_sfnt_os2);
            if (os2 && os2->version != 0xFFFF) {
                if (os2->sCapHeight != 0) metrics.capHeight = (float)os2->sCapHeight * metrics.scale; else metrics.capHeight = metrics.ascent * 0.7f; //
                if (os2->sxHeight != 0) metrics.xHeight = (float)os2->sxHeight * metrics.scale; else metrics.xHeight = metrics.ascent * 0.45f; //
                metrics.strikeoutPosition = (float)fontData.yStrikeoutPosition_fontUnits * metrics.scale; //
                metrics.strikeoutThickness = (float)fontData.yStrikeoutSize_fontUnits * metrics.scale; //
            } else {
                metrics.capHeight = metrics.ascent * 0.7f; metrics.xHeight = metrics.ascent * 0.45f; //
                metrics.strikeoutPosition = metrics.xHeight / 2.0f; metrics.strikeoutThickness = fontSize / 20.0f; //
            }
            metrics.underlinePosition = (float)face->underline_position * metrics.scale; //
            metrics.underlineThickness = (float)face->underline_thickness * metrics.scale; //
            if (metrics.underlineThickness > 0 && metrics.underlineThickness < 1.0f) metrics.underlineThickness = 1.0f;
            if (metrics.strikeoutThickness > 0 && metrics.strikeoutThickness < 1.0f) metrics.strikeoutThickness = 1.0f;
            return metrics;
        }
        FTCachedGlyph getCachedGlyphByGID(FontId fontId, uint32_t glyphID_from_harfbuzz, float fontSizeForRender, FontId& actualFontIdUsed) {
            // 1. 确定实际使用的字体 ID（通常就是传入的 fontId，因为 GID 是针对特定字体的）
            actualFontIdUsed = fontId;
            if (!IsFontValid(actualFontIdUsed)) {
                // 处理错误，或者尝试用默认字体渲染 GID 0 (notdef)
                TraceLog(LOG_ERROR, "getCachedGlyphByGID: Invalid FontID %d passed.", fontId);
                // Fallback to .notdef from default font or handle error
                if (IsFontValid(defaultFontId_)) {
                    actualFontIdUsed = defaultFontId_;
                    glyphID_from_harfbuzz = 0; // .notdef
                } else {
                    return {}; // 返回空的 FTCachedGlyph
                }
            }

            const auto& chosenFontData = loadedFonts_.at(actualFontIdUsed);
            int sdfGenSize = chosenFontData.sdfPixelSizeHint > 0 ? chosenFontData.sdfPixelSizeHint : 64;

            // 2. 创建缓存键，直接使用 glyphID_from_harfbuzz
            FTGlyphCacheKey key = {actualFontIdUsed, glyphID_from_harfbuzz, sdfGenSize, (atlas_type_hint_ == GlyphAtlasType::SDF_BITMAP)};

            // 3. 查找缓存 (与 getOrCacheGlyph 相同)
            auto cacheIt = glyph_cache_map_.find(key);
            if (cacheIt != glyph_cache_map_.end()) {
                lru_glyph_list_.splice(lru_glyph_list_.begin(), lru_glyph_list_, cacheIt->second.second);
                return cacheIt->second.first;
            }

            // 4. 如果未缓存，则加载和渲染字形 (核心区别在这里)
            FTCachedGlyph newCachedGlyph;
            newCachedGlyph.renderInfo.isSDF = key.isSDF;
            FT_Face faceToRender = chosenFontData.ftFace;

            FT_Error error = FT_Set_Pixel_Sizes(faceToRender, 0, (FT_UInt)sdfGenSize);
            // ... (错误处理) ...

            // 直接使用 HarfBuzz 提供的 GID (key.glyphIndex == glyphID_from_harfbuzz)
            int load_flags = FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING; // 通常HarfBuzz已处理hinting相关
            error = FT_Load_Glyph(faceToRender, key.glyphIndex, load_flags);
            // ... (错误处理, 如果加载特定GID失败，可以尝试加载GID 0即.notdef) ...

            FT_GlyphSlot slot = faceToRender->glyph;
            if (key.isSDF) error = FT_Render_Glyph(slot, FT_RENDER_MODE_SDF);
            else error = FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL);
            // ... (错误处理) ...

            // 存储基于缓存尺寸的度量 (与 getOrCacheGlyph 相同)
            newCachedGlyph.advanceX_at_cached_size = (float)slot->metrics.horiAdvance / 64.0f;
            newCachedGlyph.ascent_at_cached_size = (float)slot->metrics.horiBearingY / 64.0f; // FT y向上为正
            newCachedGlyph.descent_at_cached_size = (float)(slot->metrics.height - slot->metrics.horiBearingY) / 64.0f; // FT y向上为正，descent通常为正值

            // 图集打包 (与 getOrCacheGlyph 相同)
            if (slot->bitmap.buffer && slot->bitmap.width > 0 && slot->bitmap.rows > 0) {
                Rectangle pack_rect = findSpaceInAtlasAndPack(slot->bitmap.width, slot->bitmap.rows, slot->bitmap.buffer, PIXELFORMAT_UNCOMPRESSED_GRAYSCALE);
                if (pack_rect.width > 0) {
                    newCachedGlyph.renderInfo.atlasTexture = atlas_textures_[current_atlas_idx_];
                    newCachedGlyph.renderInfo.atlasRect = pack_rect;
                    newCachedGlyph.renderInfo.drawOffset.x = (float)slot->bitmap_left;
                    // FreeType 的 bitmap_top 是从基线到栅格图顶部的距离。
                    // 如果您的渲染坐标系Y轴向下，而字体度量Y轴向上，转换可能需要注意。
                    // 通常 yOffset (PositionedGlyph的) 和 drawOffset.y 的关系是：
                    // PositionedGlyph.yOffset (来自HarfBuzz, y向上为正)
                    // drawOffset.y (FT bitmap_top, y向上为正，指栅格图顶部相对于基线的偏移)
                    // 最终绘制时： screen_y = baseline_y - PositionedGlyph.yOffset - drawOffset.y (如果都以字体y向上为准)
                    // 或者：screen_y = baseline_y + PositionedGlyph.yOffset_screen + drawOffset.y_screen (如果都转为屏幕y向下)
                    // 您代码中 PositionedGlyph.position.y = ... -pGlyph.yOffset (HB yOffset)
                    // 然后 DrawTextBlock 中 finalDrawY = ... + glyph.position.y + (glyph.renderInfo.drawOffset.y * renderScaleFactor)
                    // 如果 renderInfo.drawOffset.y 直接用了 slot->bitmap_top (FT y向上), 那么需要注意符号。
                    // 通常 renderInfo.drawOffset.y 应该是 slot->bitmap_top (如果你想让它表示基线到字形顶部的距离)
                    // 或者 -slot->bitmap_top (如果你的绘制逻辑是从左上角开始，并且希望这个offset是向下的)
                    // 您目前的 `newCachedGlyph.renderInfo.drawOffset.y = -(float)slot->bitmap_top;` 可能是为了适配Y轴向下的屏幕坐标系，这通常是正确的。
                    newCachedGlyph.renderInfo.drawOffset.y = -(float)slot->bitmap_top;
                } else { newCachedGlyph.renderInfo.atlasTexture.id = 0; }
            } else { /* ... whitespace or empty glyphs ... */ }

            // 缓存管理 (与 getOrCacheGlyph 相同)
            if (glyph_cache_map_.size() >= glyph_cache_capacity_ && !lru_glyph_list_.empty()) {
                glyph_cache_map_.erase(lru_glyph_list_.back());
                lru_glyph_list_.pop_back();
            }
            lru_glyph_list_.push_front(key);
            glyph_cache_map_[key] = {newCachedGlyph, lru_glyph_list_.begin()};
            return newCachedGlyph;
        }



        // RaylibSDFTextEx.cpp -> FTTextEngineImpl 类内部
// 请用这个版本替换你现有的 LayoutStyledText 函数

        TextBlock LayoutStyledText(const std::vector<TextSpan>& spans, const ParagraphStyle& paragraphStyle) {
            TextBlock textBlock;
            textBlock.paragraphStyleUsed = paragraphStyle;
            textBlock.sourceSpansCopied = spans;

            if (!ftLibrary_) {
                TraceLog(LOG_ERROR, "FTTextEngine: FT lib not init in Layout.");
                return textBlock;
            }

            FontId paraDefFontId = paragraphStyle.defaultCharacterStyle.fontId;
            if (!IsFontValid(paraDefFontId)) paraDefFontId = defaultFontId_;
            float paraDefFontSize = paragraphStyle.defaultCharacterStyle.fontSize > 0 ? paragraphStyle.defaultCharacterStyle.fontSize : 16.0f;
            ScaledFontMetrics paraDefaultMetrics;
            if (IsFontValid(paraDefFontId)) {
                paraDefaultMetrics = GetScaledFontMetrics(paraDefFontId, paraDefFontSize);
            } else {
                paraDefaultMetrics.ascent = paraDefFontSize * 0.75f; paraDefaultMetrics.descent = paraDefFontSize * 0.25f;
                paraDefaultMetrics.recommendedLineHeight = paraDefaultMetrics.ascent + paraDefaultMetrics.descent;
                paraDefaultMetrics.scale = 1.0f; paraDefaultMetrics.xHeight = paraDefFontSize * 0.45f;
            }

            if (spans.empty()) { // Handle case of no spans
                LineLayoutInfo emptyLine;
                emptyLine.firstElementIndexInBlockElements = 0; emptyLine.numElementsInLine = 0;
                emptyLine.sourceTextByteStartIndexInBlockText = 0; emptyLine.sourceTextByteEndIndexInBlockText = 0;
                emptyLine.maxContentAscent = paraDefaultMetrics.ascent; emptyLine.maxContentDescent = paraDefaultMetrics.descent;
                emptyLine.lineBoxHeight = calculateLineBoxHeight(paragraphStyle, paraDefaultMetrics, emptyLine.maxContentAscent, emptyLine.maxContentDescent, paraDefFontSize);
                emptyLine.baselineYInBox = emptyLine.maxContentAscent;
                if (emptyLine.lineBoxHeight > (emptyLine.maxContentAscent + emptyLine.maxContentDescent) + 0.001f && paragraphStyle.lineHeightType != LineHeightType::CONTENT_SCALED) {
                    emptyLine.baselineYInBox += (emptyLine.lineBoxHeight - (emptyLine.maxContentAscent + emptyLine.maxContentDescent)) / 2.0f;
                }
                emptyLine.lineWidth = 0.0f; emptyLine.lineBoxY = 0.0f;
                textBlock.lines.push_back(emptyLine);
                textBlock.overallBounds = {0, 0, paragraphStyle.firstLineIndent, emptyLine.lineBoxHeight};
                return textBlock;
            }

            std::string fullUtf8Text_local;
            struct SpanMapEntry {
                uint32_t u8_start_offset_in_full; uint32_t u8_length_in_full;
                uint32_t u16_start_offset_in_full; uint32_t u16_length_in_full;
                size_t originalSpanIndex;
            };
            std::vector<SpanMapEntry> spanMap_local;
            uint32_t currentU8BytePosInFull = 0; uint32_t currentU16CodeUnitPosInFull = 0;

            for (size_t i = 0; i < spans.size(); ++i) {
                const auto& span = spans[i];
                std::string text_to_process = span.text;
                if (span.style.isImage && span.text.empty()) text_to_process = "\xEF\xBF\xBC"; // U+FFFC
                fullUtf8Text_local += text_to_process;
                uint32_t u8LenOfSpanText = text_to_process.length();
                std::u16string u16SpanText = Utf8ToUtf16(text_to_process);
                uint32_t u16LenOfSpanText = u16SpanText.length();
                spanMap_local.push_back({currentU8BytePosInFull, u8LenOfSpanText, currentU16CodeUnitPosInFull, u16LenOfSpanText, i});
                currentU8BytePosInFull += u8LenOfSpanText; currentU16CodeUnitPosInFull += u16LenOfSpanText;
            }
            textBlock.sourceTextConcatenated = fullUtf8Text_local;
            std::u16string fullU16Text_local = Utf8ToUtf16(fullUtf8Text_local);
            if (fullU16Text_local.empty() && !textBlock.sourceTextConcatenated.empty()) {
                TraceLog(LOG_ERROR, "FTTextEngine: Full text UTF-16 conversion failed."); return textBlock;
            }

            UErrorCode icu_status = U_ZERO_ERROR;
            UBiDi* paraBiDi = ubidi_openSized(fullU16Text_local.length() + 1, 0, &icu_status);
            if (U_FAILURE(icu_status)) { TraceLog(LOG_ERROR, "FTTextEngine: ubidi_openSized failed: %s", u_errorName(icu_status)); return textBlock; }
            UBiDiLevel paraLvlUBIDI = (paragraphStyle.baseDirection == TextDirection::RTL) ? UBIDI_DEFAULT_RTL : UBIDI_DEFAULT_LTR;
            if (paragraphStyle.baseDirection == TextDirection::AUTO_DETECT_FROM_TEXT) paraLvlUBIDI = UBIDI_DEFAULT_LTR;
            ubidi_setPara(paraBiDi, reinterpret_cast<const UChar*>(fullU16Text_local.data()), fullU16Text_local.length(), paraLvlUBIDI, nullptr, &icu_status);
            if (U_FAILURE(icu_status)) { TraceLog(LOG_ERROR, "FTTextEngine: ubidi_setPara failed: %s", u_errorName(icu_status)); ubidi_close(paraBiDi); return textBlock; }
            UBiDiLevel actualParaLevel = ubidi_getParaLevel(paraBiDi);

            const char* localeForBreaks = paragraphStyle.defaultCharacterStyle.languageTag.empty() ? uloc_getDefault() : paragraphStyle.defaultCharacterStyle.languageTag.c_str();
            UBreakIterator* icuBreakIter = nullptr;
            if (paragraphStyle.lineBreakStrategy == LineBreakStrategy::ICU_CHARACTER_BOUNDARIES){
                icuBreakIter = ubrk_open(UBRK_CHARACTER, localeForBreaks, nullptr, 0, &icu_status);
            } else { icuBreakIter = ubrk_open(UBRK_WORD, localeForBreaks, nullptr, 0, &icu_status); }
            if (U_FAILURE(icu_status) || !icuBreakIter) {
                icu_status = U_ZERO_ERROR; icuBreakIter = ubrk_open(UBRK_WORD, uloc_getDefault(), nullptr, 0, &icu_status);
                if (U_FAILURE(icu_status) || !icuBreakIter) { TraceLog(LOG_FATAL, "FTTextEngine: All ubrk_open attempts failed: %s", u_errorName(icu_status)); if(paraBiDi) ubidi_close(paraBiDi); return textBlock;}
            }
            ubrk_setText(icuBreakIter, reinterpret_cast<const UChar*>(fullU16Text_local.data()), fullU16Text_local.length(), &icu_status);
            if (U_FAILURE(icu_status)) { TraceLog(LOG_ERROR, "FTTextEngine: ubrk_setText failed: %s", u_errorName(icu_status)); ubrk_close(icuBreakIter); ubidi_close(paraBiDi); return textBlock; }

            float currentLineBoxTopY = 0.0f; bool isFirstLineOfParagraph = true; float overallMaxVisualLineWidth = 0.0f;
            std::vector<PositionedElementVariant> pendingLineElements;
            float currentLineCommittedWidth = 0.0f;
            float currentLineMaxAscent = paraDefaultMetrics.ascent; float currentLineMaxDescent = paraDefaultMetrics.descent;
            uint32_t currentLineU8StartIndexInFull_for_lineinfo = 0;
            LineLayoutInfo currentLineInfoTemplate;
            currentLineInfoTemplate.firstElementIndexInBlockElements = textBlock.elements.size();
            currentLineInfoTemplate.sourceTextByteStartIndexInBlockText = currentLineU8StartIndexInFull_for_lineinfo;
            currentLineInfoTemplate.maxContentAscent = paraDefaultMetrics.ascent; currentLineInfoTemplate.maxContentDescent = paraDefaultMetrics.descent;

            int32_t currentU16BreakPos = 0; int32_t lastU16BreakPos = 0;
            while (lastU16BreakPos < (int32_t)fullU16Text_local.length() || (lastU16BreakPos == 0 && fullU16Text_local.empty() && textBlock.lines.empty()) ) {
                if (lastU16BreakPos >= (int32_t)fullU16Text_local.length() && !fullU16Text_local.empty()) break;
                currentU16BreakPos = ubrk_following(icuBreakIter, lastU16BreakPos);
                bool atEndOfParagraph = false;
                if (currentU16BreakPos == UBRK_DONE) { currentU16BreakPos = fullU16Text_local.length(); atEndOfParagraph = true; }
                if (lastU16BreakPos == currentU16BreakPos && !atEndOfParagraph && currentU16BreakPos < (int32_t)fullU16Text_local.length() ) { currentU16BreakPos++; }
                if (currentU16BreakPos > (int32_t)fullU16Text_local.length()) currentU16BreakPos = (int32_t)fullU16Text_local.length();

                std::u16string segmentU16 = fullU16Text_local.substr(lastU16BreakPos, currentU16BreakPos - lastU16BreakPos);
                uint32_t segmentU8StartByteInFull = Utf16ToUtf8(fullU16Text_local.substr(0, lastU16BreakPos)).length();
                bool containsHardNewline = false; std::u16string segmentToShapeU16 = segmentU16;
                size_t newlinePosInSegmentU16 = segmentU16.find(u'\n');
                if (newlinePosInSegmentU16 != std::u16string::npos) { containsHardNewline = true; segmentToShapeU16 = segmentU16.substr(0, newlinePosInSegmentU16); }

                std::vector<PositionedElementVariant> elements_for_this_segment;
                float width_of_this_segment = 0;
                float max_ascent_for_this_segment = 0, max_descent_for_this_segment = 0;
                float penXWithinSegment_for_icu_runs = 0.0f;

                if (!segmentToShapeU16.empty()) {
                    UBiDi* segmentBiDi = ubidi_openSized(segmentToShapeU16.length() + 1, 0, &icu_status);
                    ubidi_setPara(segmentBiDi, reinterpret_cast<const UChar*>(segmentToShapeU16.data()), segmentToShapeU16.length(), actualParaLevel, nullptr, &icu_status);
                    int32_t visualRunCountOnSegment = ubidi_countRuns(segmentBiDi, &icu_status);
                    if (U_FAILURE(icu_status)) { visualRunCountOnSegment = 0; }

                    for (int32_t i_run = 0; i_run < visualRunCountOnSegment; ++i_run) {
                        VisualRun current_visual_run_props;
                        int32_t logicalStartU16InSegment, runLengthU16InSegment;
                        UBiDiDirection runDirectionUBIDI = ubidi_getVisualRun(segmentBiDi, i_run, &logicalStartU16InSegment, &runLengthU16InSegment);
                        if (runLengthU16InSegment == 0) continue;

                        std::u16string runU16 = segmentToShapeU16.substr(logicalStartU16InSegment, runLengthU16InSegment);
                        std::string runU8 = Utf16ToUtf8(runU16);

                        if (runU8 == "طويل") { // Your test log
                            TraceLog(LOG_INFO, "LayoutStyledText: For run '%s', runDirectionUBIDI is: %s", runU8.c_str(), (runDirectionUBIDI == UBIDI_RTL ? "RTL" : "LTR"));
                        }
                        current_visual_run_props.direction = (runDirectionUBIDI == UBIDI_LTR) ? PositionedGlyph::BiDiDirectionHint::LTR : PositionedGlyph::BiDiDirectionHint::RTL;
                        current_visual_run_props.logicalStartInOriginalSource = lastU16BreakPos + logicalStartU16InSegment;
                        current_visual_run_props.logicalLengthInOriginalSource = runLengthU16InSegment;
                        uint32_t runU8StartByteInFull = Utf16ToUtf8(fullU16Text_local.substr(0, current_visual_run_props.logicalStartInOriginalSource)).length();

                        size_t runDominantSpanIdx = 0;
                        for(const auto& mapEntry : spanMap_local) { /* ... Find runDominantSpanIdx ... */
                            uint32_t spanU8EndInFull = mapEntry.u8_start_offset_in_full + mapEntry.u8_length_in_full;
                            if (runU8StartByteInFull >= mapEntry.u8_start_offset_in_full && runU8StartByteInFull < spanU8EndInFull) {
                                runDominantSpanIdx = mapEntry.originalSpanIndex; break;
                            }
                            if (runU8StartByteInFull == spanU8EndInFull && spanU8EndInFull == textBlock.sourceTextConcatenated.length() && mapEntry.originalSpanIndex == spans.size()-1){
                                runDominantSpanIdx = mapEntry.originalSpanIndex; break;
                            }
                        }
                        CharacterStyle runStyle = (runDominantSpanIdx < spans.size()) ? spans[runDominantSpanIdx].style : paragraphStyle.defaultCharacterStyle;
                        FontId runFontId = runStyle.fontId; if (!IsFontValid(runFontId)) runFontId = paraDefFontId;
                        float runFontSize = runStyle.fontSize > 0 ? runStyle.fontSize : paraDefFontSize;

                        if (!IsFontValid(runFontId)) { continue; }
                        const auto& fontData = loadedFonts_.at(runFontId);
                        ScaledFontMetrics runFontMetrics = GetScaledFontMetrics(runFontId, runFontSize);

                        current_visual_run_props.runFont = runFontId; current_visual_run_props.runFontSize = runFontSize;
                        current_visual_run_props.scriptTagUsed = runStyle.scriptTag.empty() ? "auto" : runStyle.scriptTag;
                        current_visual_run_props.languageTagUsed = runStyle.languageTag.empty() ? "und" : runStyle.languageTag;

                        hb_buffer_t* hb_buf = hb_buffer_create();
                        hb_buffer_add_utf8(hb_buf, runU8.c_str(), runU8.length(), 0, runU8.length());
                        hb_buffer_set_direction(hb_buf, (runDirectionUBIDI == UBIDI_LTR) ? HB_DIRECTION_LTR : HB_DIRECTION_RTL);
                        hb_buffer_set_script(hb_buf, HbScriptFromString(current_visual_run_props.scriptTagUsed.c_str()));
                        hb_buffer_set_language(hb_buf, HbLanguageFromString(current_visual_run_props.languageTagUsed.c_str()));
                        if (runStyle.scriptTag.empty()) { hb_buffer_guess_segment_properties(hb_buf); }

                        FT_Set_Pixel_Sizes(fontData.ftFace, 0, static_cast<FT_UInt>(roundf(runFontSize)));
                        hb_shape(fontData.hbFont, hb_buf, nullptr, 0);

                        unsigned int hb_glyph_count;
                        hb_glyph_info_t* hb_glyph_info = hb_buffer_get_glyph_infos(hb_buf, &hb_glyph_count);
                        hb_glyph_position_t* hb_glyph_pos = hb_buffer_get_glyph_positions(hb_buf, &hb_glyph_count);

                        float current_hb_run_pen_x = 0.0f; float current_hb_run_pen_y = 0.0f;

                        for (unsigned int j = 0; j < hb_glyph_count; ++j) {
                            uint32_t cluster_u8_offset_in_runU8 = hb_glyph_info[j].cluster;

                            int num_bytes_for_this_glyph_original_source = 0;
                            std::string char_in_cluster_utf8_preview; // For logging

                            if (cluster_u8_offset_in_runU8 < runU8.length()) {
                                const char* char_start_ptr_for_len_calc = runU8.c_str() + cluster_u8_offset_in_runU8;
                                GetNextCodepointFromUTF8(&char_start_ptr_for_len_calc, &num_bytes_for_this_glyph_original_source);
                                if (cluster_u8_offset_in_runU8 + num_bytes_for_this_glyph_original_source > runU8.length()){
                                    num_bytes_for_this_glyph_original_source = runU8.length() - cluster_u8_offset_in_runU8;
                                }
                                if (num_bytes_for_this_glyph_original_source > 0) {
                                    char_in_cluster_utf8_preview = runU8.substr(cluster_u8_offset_in_runU8, num_bytes_for_this_glyph_original_source);
                                }
                            }
                            // If num_bytes is still 0 (e.g. end of string, or mark without base), HarfBuzz might still produce a glyph.
                            // Default to 1 if it's a mark or something that GetNextCodepointFromUTF8 might miss if not advancing.
                            // But usually, a glyph from HB corresponds to some non-zero byte range from source.
                            // For now, we rely on GetNextCodepointFromUTF8. If it's 0, it might be an issue or an empty cluster.

                            uint32_t original_codepoint_for_glyph = 0;
                            if (!char_in_cluster_utf8_preview.empty()) {
                                const char* temp_char_ptr = char_in_cluster_utf8_preview.c_str();
                                int temp_byte_count_ignored = 0;
                                original_codepoint_for_glyph = GetNextCodepointFromUTF8(&temp_char_ptr, &temp_byte_count_ignored);
                            }

                            if (original_codepoint_for_glyph == 0xFFFC) { /* ... Image Handling ... */
                                // (Your existing image handling logic, ensure pImg.position.x uses penXWithinSegment_for_icu_runs + current_hb_run_pen_x + hb_glyph_pos[j].x_offset/64.0f
                                // and current_hb_run_pen_x is advanced by image's width or HarfBuzz's x_advance for 0xFFFC)
                                uint32_t placeholder_global_u8_start = runU8StartByteInFull + cluster_u8_offset_in_runU8;
                                size_t imageOriginalSpanIdx = runDominantSpanIdx; bool foundImageSpan = false;
                                for(const auto& mapEntry : spanMap_local){ /* ... find image span ... */
                                    if (placeholder_global_u8_start >= mapEntry.u8_start_offset_in_full && placeholder_global_u8_start < (mapEntry.u8_start_offset_in_full + mapEntry.u8_length_in_full)) {
                                        if (spans[mapEntry.originalSpanIndex].style.isImage) { imageOriginalSpanIdx = mapEntry.originalSpanIndex; foundImageSpan = true; break;}
                                    }
                                }
                                if(foundImageSpan){
                                    PositionedImage pImg; /* ... setup pImg ... */
                                    const auto& imgSpanStyle = spans[imageOriginalSpanIdx].style;
                                    pImg.imageParams = imgSpanStyle.imageParams;
                                    pImg.width = (pImg.imageParams.displayWidth > 0) ? pImg.imageParams.displayWidth : (pImg.imageParams.texture.id > 0 ? (float)pImg.imageParams.texture.width : runFontSize);
                                    pImg.height = (pImg.imageParams.displayHeight > 0) ? pImg.imageParams.displayHeight : (pImg.imageParams.texture.id > 0 ? (float)pImg.imageParams.texture.height : runFontSize);
                                    pImg.sourceSpanIndex = imageOriginalSpanIdx; pImg.sourceCharByteOffsetInSpan = 0; pImg.numSourceCharBytesInSpan = 3;
                                    float imgRelBaselineY = 0; ScaledFontMetrics refMetricsForImgVAlign = runFontMetrics;
                                    switch(pImg.imageParams.vAlign) { /* ... VAlign logic ... */
                                        case CharacterStyle::InlineImageParams::VAlign::BASELINE: pImg.ascent = pImg.height; pImg.descent = 0; imgRelBaselineY = -pImg.height; break;
                                        case CharacterStyle::InlineImageParams::VAlign::MIDDLE_OF_TEXT: { float tMidY = (refMetricsForImgVAlign.xHeight > 0.01f ? refMetricsForImgVAlign.xHeight / 2.0f : (refMetricsForImgVAlign.ascent - refMetricsForImgVAlign.descent)/2.0f); imgRelBaselineY=-(tMidY+pImg.height/2.0f); pImg.ascent=std::max(0.f, tMidY+pImg.height/2.f); pImg.descent=std::max(0.f,pImg.height/2.f - tMidY); break;}
                                        case CharacterStyle::InlineImageParams::VAlign::TEXT_TOP: imgRelBaselineY=-refMetricsForImgVAlign.ascent; pImg.ascent=refMetricsForImgVAlign.ascent; pImg.descent=std::max(0.f,pImg.height-refMetricsForImgVAlign.ascent); break;
                                        case CharacterStyle::InlineImageParams::VAlign::TEXT_BOTTOM: imgRelBaselineY=refMetricsForImgVAlign.descent-pImg.height; pImg.descent=refMetricsForImgVAlign.descent; pImg.ascent=std::max(0.f, pImg.height-refMetricsForImgVAlign.descent); break;
                                        default: pImg.ascent = pImg.height; pImg.descent = 0; imgRelBaselineY = -pImg.height; break;
                                    }
                                    pImg.ascent = std::max(0.0f, pImg.ascent); pImg.descent = std::max(0.0f, pImg.descent);
                                    float image_draw_x_in_hb_run = current_hb_run_pen_x + ((float)hb_glyph_pos[j].x_offset / 64.0f);
                                    pImg.position = { penXWithinSegment_for_icu_runs + image_draw_x_in_hb_run, imgRelBaselineY };
                                    elements_for_this_segment.push_back(pImg);
                                    max_ascent_for_this_segment = std::max(max_ascent_for_this_segment, pImg.ascent);
                                    max_descent_for_this_segment = std::max(max_descent_for_this_segment, pImg.descent);
                                    current_hb_run_pen_x += (float)hb_glyph_pos[j].x_advance / 64.0f;
                                    current_hb_run_pen_y += (float)hb_glyph_pos[j].y_advance / 64.0f;
                                    continue;
                                }
                            }

                            PositionedGlyph pGlyph;
                            pGlyph.glyphId = hb_glyph_info[j].codepoint;
                            pGlyph.sourceFont = runFontId; pGlyph.sourceSize = runFontSize;
                            pGlyph.appliedStyle = runStyle;
                            pGlyph.xOffset = (float)hb_glyph_pos[j].x_offset / 64.0f;
                            pGlyph.yOffset = (float)hb_glyph_pos[j].y_offset / 64.0f;
                            pGlyph.xAdvance = (float)hb_glyph_pos[j].x_advance / 64.0f;
                            pGlyph.yAdvance = (float)hb_glyph_pos[j].y_advance / 64.0f;
                            pGlyph.visualRunDirectionHint = current_visual_run_props.direction; // Set this from the run

                            // --- Corrected source text mapping ---
                            pGlyph.sourceSpanIndex = runDominantSpanIdx;
                            pGlyph.numSourceCharBytesInSpan = static_cast<uint16_t>(num_bytes_for_this_glyph_original_source); // Use calculated byte length

                            uint32_t cluster_absolute_start_in_full_u8 = runU8StartByteInFull + cluster_u8_offset_in_runU8;
                            const auto& originalSpanMapEntry = spanMap_local[pGlyph.sourceSpanIndex];
                            uint32_t originalSpan_start_in_full_u8 = originalSpanMapEntry.u8_start_offset_in_full;
                            if (cluster_absolute_start_in_full_u8 >= originalSpan_start_in_full_u8) {
                                pGlyph.sourceCharByteOffsetInSpan = cluster_absolute_start_in_full_u8 - originalSpan_start_in_full_u8;
                            } else {
                                pGlyph.sourceCharByteOffsetInSpan = 0;
                                if (pGlyph.numSourceCharBytesInSpan !=0) { /* Log inconsistent state */ }
                                pGlyph.numSourceCharBytesInSpan = 0; // Safety
                                TraceLog(LOG_ERROR, "LayoutText: Cluster mapping error for GID %u", pGlyph.glyphId);
                            }
                            // Boundary checks for sourceCharByteOffsetInSpan and numSourceCharBytesInSpan
                            const std::string* originalSpanTextPtr = nullptr;
                            if (pGlyph.sourceSpanIndex < spans.size()) { originalSpanTextPtr = &(spans[pGlyph.sourceSpanIndex].text); }
                            std::string tempImagePlaceholderTextCheck_glyph;
                            if (pGlyph.sourceSpanIndex < spans.size() && spans[pGlyph.sourceSpanIndex].style.isImage && spans[pGlyph.sourceSpanIndex].text.empty()){ tempImagePlaceholderTextCheck_glyph = "\xEF\xBF\xBC"; originalSpanTextPtr = &tempImagePlaceholderTextCheck_glyph; }
                            if (originalSpanTextPtr) {
                                const std::string& effectiveOriginalSpanText = *originalSpanTextPtr;
                                if (pGlyph.sourceCharByteOffsetInSpan > effectiveOriginalSpanText.length()) {
                                    pGlyph.sourceCharByteOffsetInSpan = effectiveOriginalSpanText.length(); pGlyph.numSourceCharBytesInSpan = 0;
                                } else if (pGlyph.sourceCharByteOffsetInSpan + pGlyph.numSourceCharBytesInSpan > effectiveOriginalSpanText.length()) {
                                    pGlyph.numSourceCharBytesInSpan = effectiveOriginalSpanText.length() - pGlyph.sourceCharByteOffsetInSpan;
                                }
                                if (static_cast<int>(pGlyph.numSourceCharBytesInSpan) < 0) pGlyph.numSourceCharBytesInSpan = 0;
                            } else {
                                pGlyph.sourceCharByteOffsetInSpan = 0; pGlyph.numSourceCharBytesInSpan = 0;
                                TraceLog(LOG_ERROR, "LayoutText: Invalid sourceSpanIndex %u for pGlyph text checks.", pGlyph.sourceSpanIndex);
                            }

                            if (current_visual_run_props.direction == PositionedGlyph::BiDiDirectionHint::RTL) {
                                TraceLog(LOG_INFO, "LayoutText[RTL Glyph]: Char:'%s'(GID:%u) | SpanIdx:%u, OffInSpan:%u, NumBytes:%u | ClustInRun:%u",
                                         char_in_cluster_utf8_preview.c_str(), pGlyph.glyphId,
                                         pGlyph.sourceSpanIndex, pGlyph.sourceCharByteOffsetInSpan, pGlyph.numSourceCharBytesInSpan,
                                         cluster_u8_offset_in_runU8);
                            }

                            FontId actualFontForGIDCacheLookup = runFontId;
                            FTCachedGlyph shapedGlyphRenderData = getCachedGlyphByGID(runFontId, pGlyph.glyphId, runFontSize, actualFontForGIDCacheLookup);
                            pGlyph.sourceFont = actualFontForGIDCacheLookup;
                            pGlyph.renderInfo = shapedGlyphRenderData.renderInfo;

                            float scaleFactorForMetrics = 1.0f;
                            const auto& actualFontDataUsed = loadedFonts_.at(pGlyph.sourceFont);
                            int sdfSizeForActualFont = actualFontDataUsed.sdfPixelSizeHint > 0 ? actualFontDataUsed.sdfPixelSizeHint : 64;
                            if (sdfSizeForActualFont > 0 && runFontSize > 0) { scaleFactorForMetrics = runFontSize / (float)sdfSizeForActualFont; }
                            pGlyph.ascent = shapedGlyphRenderData.ascent_at_cached_size * scaleFactorForMetrics;
                            pGlyph.descent = shapedGlyphRenderData.descent_at_cached_size * scaleFactorForMetrics;

                            if (IsFontValid(pGlyph.sourceFont)){ /* ... Calculate visualLeft/Right ... */
                                FT_Face tempFace = loadedFonts_.at(pGlyph.sourceFont).ftFace;
                                FT_Set_Pixel_Sizes(tempFace, 0, static_cast<FT_UInt>(roundf(runFontSize)));
                                FT_Error err = FT_Load_Glyph(tempFace, pGlyph.glyphId, FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP);
                                if (!err) {
                                    pGlyph.visualLeft = ((float)tempFace->glyph->metrics.horiBearingX / 64.0f);
                                    pGlyph.visualRight = pGlyph.visualLeft + ((float)tempFace->glyph->metrics.width / 64.0f);
                                } else { pGlyph.visualLeft = 0; pGlyph.visualRight = pGlyph.xAdvance; }
                            } else { pGlyph.visualLeft = 0; pGlyph.visualRight = pGlyph.xAdvance; }

                            float glyph_draw_origin_x_in_run = current_hb_run_pen_x + pGlyph.xOffset;
                            float glyph_draw_origin_y_in_run = current_hb_run_pen_y - pGlyph.yOffset;
                            pGlyph.position = { penXWithinSegment_for_icu_runs + glyph_draw_origin_x_in_run, glyph_draw_origin_y_in_run };

                            elements_for_this_segment.push_back(pGlyph);
                            max_ascent_for_this_segment = std::max(max_ascent_for_this_segment, pGlyph.ascent - pGlyph.yOffset);
                            max_descent_for_this_segment = std::max(max_descent_for_this_segment, pGlyph.descent + pGlyph.yOffset);

                            current_hb_run_pen_x += pGlyph.xAdvance;
                            current_hb_run_pen_y += pGlyph.yAdvance;
                        }
                        hb_buffer_destroy(hb_buf);
                        current_visual_run_props.runVisualAdvanceX = current_hb_run_pen_x;
                        penXWithinSegment_for_icu_runs += current_hb_run_pen_x;
                    } // End for each ICU visual run (i_run)
                    width_of_this_segment = penXWithinSegment_for_icu_runs;
                    ubidi_close(segmentBiDi);
                } // End if (!segmentToShapeU16.empty())

                // --- Line breaking and element commitment logic ---
                float lineStartXWithIndent = (isFirstLineOfParagraph ? paragraphStyle.firstLineIndent : 0.0f);
                if (paragraphStyle.wrapWidth > 0 &&
                    (lineStartXWithIndent + currentLineCommittedWidth + width_of_this_segment > paragraphStyle.wrapWidth) &&
                    !pendingLineElements.empty() && width_of_this_segment > 0.001f) {
                    finalizeCurrentLine(textBlock, pendingLineElements, currentLineInfoTemplate, currentLineCommittedWidth,
                                        currentLineMaxAscent, currentLineMaxDescent, currentLineBoxTopY,
                                        isFirstLineOfParagraph, paragraphStyle, paraDefaultMetrics, paraDefFontSize,
                                        segmentU8StartByteInFull, overallMaxVisualLineWidth,
                                        fullU16Text_local, actualParaLevel);
                    pendingLineElements.clear(); currentLineCommittedWidth = 0; isFirstLineOfParagraph = false;
                    currentLineMaxAscent = paraDefaultMetrics.ascent; currentLineMaxDescent = paraDefaultMetrics.descent;
                    currentLineU8StartIndexInFull_for_lineinfo = segmentU8StartByteInFull;
                    currentLineInfoTemplate = {}; // Reset for new line
                    currentLineInfoTemplate.firstElementIndexInBlockElements = textBlock.elements.size();
                    currentLineInfoTemplate.sourceTextByteStartIndexInBlockText = currentLineU8StartIndexInFull_for_lineinfo;
                    currentLineInfoTemplate.maxContentAscent = paraDefaultMetrics.ascent; currentLineInfoTemplate.maxContentDescent = paraDefaultMetrics.descent;
                }

                if (!elements_for_this_segment.empty()) {
                    float basePenXForSegmentElements = currentLineCommittedWidth;
                    for (auto& el_var : elements_for_this_segment) {
                        std::visit([basePenXForSegmentElements](auto&& arg){ arg.position.x += basePenXForSegmentElements; }, el_var);
                        pendingLineElements.push_back(el_var);
                    }
                    currentLineCommittedWidth += width_of_this_segment;
                    currentLineMaxAscent = std::max(currentLineMaxAscent, max_ascent_for_this_segment);
                    currentLineMaxDescent = std::max(currentLineMaxDescent, max_descent_for_this_segment);
                }

                if (containsHardNewline) {
                    uint32_t u8OffsetAfterNewline = Utf16ToUtf8(fullU16Text_local.substr(0, lastU16BreakPos + segmentToShapeU16.length() + 1)).length();
                    finalizeCurrentLine(textBlock, pendingLineElements, currentLineInfoTemplate, currentLineCommittedWidth,
                                        currentLineMaxAscent, currentLineMaxDescent, currentLineBoxTopY,
                                        isFirstLineOfParagraph, paragraphStyle, paraDefaultMetrics, paraDefFontSize,
                                        u8OffsetAfterNewline, overallMaxVisualLineWidth,
                                        fullU16Text_local, actualParaLevel);
                    pendingLineElements.clear(); currentLineCommittedWidth = 0; isFirstLineOfParagraph = false;
                    currentLineMaxAscent = paraDefaultMetrics.ascent; currentLineMaxDescent = paraDefaultMetrics.descent;
                    currentLineU8StartIndexInFull_for_lineinfo = u8OffsetAfterNewline;
                    currentLineInfoTemplate = {};
                    currentLineInfoTemplate.firstElementIndexInBlockElements = textBlock.elements.size();
                    currentLineInfoTemplate.sourceTextByteStartIndexInBlockText = currentLineU8StartIndexInFull_for_lineinfo;
                    currentLineInfoTemplate.maxContentAscent = paraDefaultMetrics.ascent; currentLineInfoTemplate.maxContentDescent = paraDefaultMetrics.descent;
                    lastU16BreakPos = lastU16BreakPos + segmentToShapeU16.length() + 1;
                } else {
                    lastU16BreakPos = currentU16BreakPos;
                }
                if (atEndOfParagraph && lastU16BreakPos >= (int32_t)fullU16Text_local.length()) break;
            } // End while u16 break pos

            if (!pendingLineElements.empty() || textBlock.lines.empty()) {
                finalizeCurrentLine(textBlock, pendingLineElements, currentLineInfoTemplate, currentLineCommittedWidth,
                                    currentLineMaxAscent, currentLineMaxDescent, currentLineBoxTopY,
                                    isFirstLineOfParagraph, paragraphStyle, paraDefaultMetrics, paraDefFontSize,
                                    textBlock.sourceTextConcatenated.length(), overallMaxVisualLineWidth,
                                    fullU16Text_local, actualParaLevel);
            }

            if (icuBreakIter) ubrk_close(icuBreakIter);
            if (paraBiDi) ubidi_close(paraBiDi);

            // Calculate overall bounds (same as before)
            textBlock.overallBounds.x = 0;
            textBlock.overallBounds.y = textBlock.lines.empty() ? 0 : textBlock.lines.front().lineBoxY;
            textBlock.overallBounds.width = overallMaxVisualLineWidth;
            textBlock.overallBounds.height = currentLineBoxTopY - (textBlock.lines.empty() ? 0 : textBlock.lines.front().lineBoxY);
            if (textBlock.lines.empty() && !spans.empty() && textBlock.overallBounds.height < 0.01f) {
                textBlock.overallBounds.height = paraDefaultMetrics.recommendedLineHeight > 0 ? paraDefaultMetrics.recommendedLineHeight : paraDefFontSize * 1.2f;
            }

            // Post-process LINE_TOP/LINE_BOTTOM image alignment
            for (auto& line_info : textBlock.lines) {
                for (size_t i = 0; i < line_info.numElementsInLine; ++i) {
                    size_t globalElementIdx = line_info.firstElementIndexInBlockElements + i;
                    if (globalElementIdx < textBlock.elements.size() && std::holds_alternative<PositionedImage>(textBlock.elements[globalElementIdx])) {
                        PositionedImage& pImg = std::get<PositionedImage>(textBlock.elements[globalElementIdx]);
                        if (pImg.imageParams.vAlign == CharacterStyle::InlineImageParams::VAlign::LINE_TOP) {
                            pImg.position.y = -line_info.baselineYInBox;
                            pImg.ascent = line_info.baselineYInBox;
                            pImg.descent = std::max(0.0f, pImg.height - pImg.ascent);
                        } else if (pImg.imageParams.vAlign == CharacterStyle::InlineImageParams::VAlign::LINE_BOTTOM) {
                            pImg.position.y = (line_info.lineBoxHeight - line_info.baselineYInBox) - pImg.height;
                            pImg.descent = line_info.lineBoxHeight - line_info.baselineYInBox;
                            pImg.ascent = std::max(0.0f, pImg.height - pImg.descent);
                        }
                    }
                }
            }
            return textBlock;
        }

        // In RaylibSDFTextEx.cpp, within FTTextEngineImpl class

        void finalizeCurrentLine(
                TextBlock& textBlock,
                std::vector<PositionedElementVariant>& pendingElements,
                LineLayoutInfo& lineInfoTemplate, // Contains line number, byte start, etc. already set
                float committedLineWidthNoIndent, // Visual width of elements in pendingElements
                float lineMaxAscent,              // Max ascent from pendingElements
                float lineMaxDescent,             // Max descent from pendingElements
                float& currentLineBoxTopY,        // Y position for the top of this line box, updated for next line
                bool isFirstLineOfPara,
                const ParagraphStyle& pStyle,
                const ScaledFontMetrics& defaultPStyleMetrics, // Correctly named parameter
                float paraDefaultFontSize,                     // Correctly named parameter
                uint32_t nextLineU8StartOffsetInFull, // Byte offset in full text where the next line would start, or end of text
                float& overallMaxVisualLineWidthInOut,
                const std::u16string& paragraphFullU16Text, // For BiDi map on the line
                UBiDiLevel paragraphBiDiLevelForLineMap    // The resolved paragraph level for this line's BiDi context
        ) {
            // Skip finalization if this segment didn't actually advance text position and wasn't the very first line attempt
            if (pendingElements.empty() && !(textBlock.lines.empty() && textBlock.sourceTextConcatenated.empty() && lineInfoTemplate.sourceTextByteStartIndexInBlockText == 0)) {
                if (lineInfoTemplate.sourceTextByteStartIndexInBlockText >= nextLineU8StartOffsetInFull && !textBlock.sourceTextConcatenated.empty() && lineInfoTemplate.sourceTextByteStartIndexInBlockText != 0) {
                    // This case might occur if a line break happened exactly at a segment boundary without consuming the segment.
                    // TraceLog(LOG_DEBUG, "finalizeCurrentLine: Skipping finalization of an empty segment that didn't advance text position.");
                    return;
                }
            }

            LineLayoutInfo finalizedLine = lineInfoTemplate; // Copy template info (like start byte index)
            finalizedLine.lineWidth = committedLineWidthNoIndent;
            finalizedLine.maxContentAscent = (lineMaxAscent > 0.001f || pendingElements.empty()) ? lineMaxAscent : defaultPStyleMetrics.ascent;
            finalizedLine.maxContentDescent = (lineMaxDescent > 0.001f || pendingElements.empty()) ? lineMaxDescent : defaultPStyleMetrics.descent;
            finalizedLine.sourceTextByteEndIndexInBlockText = nextLineU8StartOffsetInFull;

            // Move pendingElements to textBlock.elements
            finalizedLine.firstElementIndexInBlockElements = textBlock.elements.size();
            for(const auto& el : pendingElements) {
                textBlock.elements.push_back(el);
            }
            finalizedLine.numElementsInLine = pendingElements.size();


            // Line alignment calculation
            float linePhysicalStartX = isFirstLineOfPara ? pStyle.firstLineIndent : 0.0f;
            float visualLineWidthWithIndentActual = linePhysicalStartX + finalizedLine.lineWidth;
            float lineShiftX = 0;
            float effectiveWrapWidthForAlign = pStyle.wrapWidth > 0 ? pStyle.wrapWidth : visualLineWidthWithIndentActual;
            // Ensure effectiveWrapWidth is not zero if there's content, to avoid division by zero or huge shifts
            if (effectiveWrapWidthForAlign < 0.01f && visualLineWidthWithIndentActual > 0.01f) effectiveWrapWidthForAlign = visualLineWidthWithIndentActual;

            if (pStyle.alignment == HorizontalAlignment::RIGHT && visualLineWidthWithIndentActual < effectiveWrapWidthForAlign) {
                lineShiftX = effectiveWrapWidthForAlign - visualLineWidthWithIndentActual;
            } else if (pStyle.alignment == HorizontalAlignment::CENTER && visualLineWidthWithIndentActual < effectiveWrapWidthForAlign) {
                lineShiftX = (effectiveWrapWidthForAlign - visualLineWidthWithIndentActual) / 2.0f;
            }

            // Apply alignment shift to elements of this line
            if (fabsf(lineShiftX) > 0.001f) {
                for(size_t i = 0; i < finalizedLine.numElementsInLine; ++i) {
                    size_t el_idx = finalizedLine.firstElementIndexInBlockElements + i;
                    if (el_idx < textBlock.elements.size()) { // Ensure element exists
                        std::visit([lineShiftX](auto&& arg) { arg.position.x += lineShiftX; }, textBlock.elements[el_idx]);
                    }
                }
            }
            overallMaxVisualLineWidthInOut = std::max(overallMaxVisualLineWidthInOut, visualLineWidthWithIndentActual + (lineShiftX > 0 ? lineShiftX : 0.0f) );


            // Line height and baseline calculation
            finalizedLine.lineBoxHeight = calculateLineBoxHeight(pStyle, defaultPStyleMetrics, finalizedLine.maxContentAscent, finalizedLine.maxContentDescent, paraDefaultFontSize);
            finalizedLine.baselineYInBox = finalizedLine.maxContentAscent; // Default baseline at max ascent
            // Adjust baseline if line box is taller due to fixed line height settings (distribute extra space)
            if (finalizedLine.lineBoxHeight > (finalizedLine.maxContentAscent + finalizedLine.maxContentDescent) + 0.001f &&
                pStyle.lineHeightType != LineHeightType::CONTENT_SCALED) {
                finalizedLine.baselineYInBox += (finalizedLine.lineBoxHeight - (finalizedLine.maxContentAscent + finalizedLine.maxContentDescent)) / 2.0f;
            }
            finalizedLine.lineBoxY = currentLineBoxTopY;


            // --- Build VisualRun list for this line ---
            finalizedLine.visualRuns.clear();
            if (!pendingElements.empty()) {
                size_t currentRunStartIdxInLine = 0; // Index within pendingElements for this line

                // Initialize current run properties from the first element or paragraph defaults
                PositionedGlyph::BiDiDirectionHint currentRunDir = PositionedGlyph::BiDiDirectionHint::UNSPECIFIED;
                FontId currentRunFont = pStyle.defaultCharacterStyle.fontId;
                if (!IsFontValid(currentRunFont)) currentRunFont = defaultFontId_; // Engine default
                if (!IsFontValid(currentRunFont) && !loadedFonts_.empty()) currentRunFont = loadedFonts_.begin()->first; // Absolute fallback

                float currentRunSize = pStyle.defaultCharacterStyle.fontSize;
                if (currentRunSize <= 0) currentRunSize = paraDefaultFontSize;

                std::string currentRunScript = pStyle.defaultCharacterStyle.scriptTag;
                if (currentRunScript.empty()) currentRunScript = "auto"; // Default for HarfBuzz if not specified
                std::string currentRunLang = pStyle.defaultCharacterStyle.languageTag;
                if (currentRunLang.empty()) currentRunLang = "und";


                std::visit([&](const auto& el_v){ // Initialize from the first element's actual properties
                    using T = std::decay_t<decltype(el_v)>;
                    if constexpr (std::is_same_v<T, PositionedGlyph>) {
                        currentRunDir = el_v.visualRunDirectionHint; // This was set in LayoutStyledText
                        currentRunFont = el_v.sourceFont;
                        currentRunSize = el_v.sourceSize;
                        currentRunScript = el_v.appliedStyle.scriptTag.empty() ? "auto" : el_v.appliedStyle.scriptTag;
                        currentRunLang = el_v.appliedStyle.languageTag.empty() ? "und" : el_v.appliedStyle.languageTag;
                    } else if constexpr (std::is_same_v<T, PositionedImage>) {
                        // Image run: direction is neutral, other properties can be inherited or default
                        currentRunDir = PositionedGlyph::BiDiDirectionHint::UNSPECIFIED;
                    }
                }, pendingElements[0]);


                for (size_t i = 0; i < pendingElements.size(); ++i) {
                    bool splitRun = false;
                    PositionedGlyph::BiDiDirectionHint elemDir = PositionedGlyph::BiDiDirectionHint::UNSPECIFIED;
                    FontId elemFont = INVALID_FONT_ID; float elemSize = 0.f;
                    std::string elemScript, elemLang;
                    bool currentElementIsImage = false;

                    std::visit([&](const auto& el_v){
                        using T = std::decay_t<decltype(el_v)>;
                        if constexpr (std::is_same_v<T, PositionedGlyph>) {
                            elemDir = el_v.visualRunDirectionHint;
                            elemFont = el_v.sourceFont; elemSize = el_v.sourceSize;
                            elemScript = el_v.appliedStyle.scriptTag.empty() ? "auto" : el_v.appliedStyle.scriptTag;
                            elemLang = el_v.appliedStyle.languageTag.empty() ? "und" : el_v.appliedStyle.languageTag;
                            currentElementIsImage = false;
                        } else if constexpr (std::is_same_v<T, PositionedImage>) {
                            elemDir = PositionedGlyph::BiDiDirectionHint::UNSPECIFIED;
                            currentElementIsImage = true;
                        }
                    }, pendingElements[i]);

                    if (i > currentRunStartIdxInLine) {
                        bool prevElementWasImage = std::holds_alternative<PositionedImage>(textBlock.elements[finalizedLine.firstElementIndexInBlockElements + i -1]); // Check type of previous element in textBlock
                        if (currentElementIsImage != prevElementWasImage) {
                            splitRun = true;
                        } else if (!currentElementIsImage) { // Current is text, compare with current text run properties
                            if (elemDir != currentRunDir) splitRun = true;
                            else if (elemFont != currentRunFont) splitRun = true;
                            else if (fabsf(elemSize - currentRunSize) > 0.1f) splitRun = true;
                            else if (elemScript != currentRunScript) splitRun = true;
                            else if (elemLang != currentRunLang) splitRun = true;
                        }
                    }

                    if (splitRun) {
                        if (i > currentRunStartIdxInLine) {
                            VisualRun run;
                            run.firstElementIndexInLineElements = currentRunStartIdxInLine;
                            run.numElementsInRun = i - currentRunStartIdxInLine;
                            run.direction = currentRunDir;
                            run.runFont = currentRunFont; run.runFontSize = currentRunSize;
                            run.scriptTagUsed = currentRunScript; run.languageTagUsed = currentRunLang;

                            run.runVisualAdvanceX = 0;
                            for(size_t k=0; k < run.numElementsInRun; ++k){
                                std::visit([&run](const auto& el_v_for_adv){
                                    using T_adv = std::decay_t<decltype(el_v_for_adv)>;
                                    if constexpr (std::is_same_v<T_adv, PositionedGlyph>) run.runVisualAdvanceX += el_v_for_adv.xAdvance;
                                    else if constexpr (std::is_same_v<T_adv, PositionedImage>) run.runVisualAdvanceX += el_v_for_adv.penAdvanceX;
                                }, textBlock.elements[finalizedLine.firstElementIndexInBlockElements + currentRunStartIdxInLine + k]);
                            }
                            finalizedLine.visualRuns.push_back(run);
                        }
                        currentRunStartIdxInLine = i;
                        currentRunDir = elemDir;
                        currentRunFont = elemFont; currentRunSize = elemSize;
                        currentRunScript = elemScript; currentRunLang = elemLang;
                        if(currentElementIsImage) {
                            currentRunDir = PositionedGlyph::BiDiDirectionHint::UNSPECIFIED;
                            // For an image run, reset/default font/size/script/lang if they are text-specific
                            currentRunFont = pStyle.defaultCharacterStyle.fontId; if (!IsFontValid(currentRunFont)) currentRunFont = defaultFontId_;
                            currentRunSize = paraDefaultFontSize;
                            currentRunScript = "auto"; currentRunLang = "und";
                        }
                    }
                }
                // Add the last run
                if (pendingElements.size() > currentRunStartIdxInLine) {
                    VisualRun run;
                    run.firstElementIndexInLineElements = currentRunStartIdxInLine;
                    run.numElementsInRun = pendingElements.size() - currentRunStartIdxInLine;
                    run.direction = currentRunDir;
                    run.runFont = currentRunFont; run.runFontSize = currentRunSize;
                    run.scriptTagUsed = currentRunScript; run.languageTagUsed = currentRunLang;

                    run.runVisualAdvanceX = 0;
                    for(size_t k=0; k < run.numElementsInRun; ++k){
                        std::visit([&run](const auto& el_v_for_adv){
                            using T_adv = std::decay_t<decltype(el_v_for_adv)>;
                            if constexpr (std::is_same_v<T_adv, PositionedGlyph>) run.runVisualAdvanceX += el_v_for_adv.xAdvance;
                            else if constexpr (std::is_same_v<T_adv, PositionedImage>) run.runVisualAdvanceX += el_v_for_adv.penAdvanceX;
                        }, textBlock.elements[finalizedLine.firstElementIndexInBlockElements + currentRunStartIdxInLine + k]);
                    }
                    finalizedLine.visualRuns.push_back(run);
                }
            }
            // --- VisualRun 构建结束 ---

            // --- Line-level BiDi map population ---
            uint32_t lineU8Len = finalizedLine.sourceTextByteEndIndexInBlockText - finalizedLine.sourceTextByteStartIndexInBlockText;
            if (lineU8Len > 0 && finalizedLine.sourceTextByteEndIndexInBlockText <= textBlock.sourceTextConcatenated.length()) {
                std::string lineU8_for_map = textBlock.sourceTextConcatenated.substr(finalizedLine.sourceTextByteStartIndexInBlockText, lineU8Len);
                std::u16string lineU16_for_map = Utf8ToUtf16(lineU8_for_map);

                if (!lineU16_for_map.empty()) {
                    UErrorCode status_line_bidi = U_ZERO_ERROR;
                    UBiDi* lineBiDiObj = ubidi_openSized(lineU16_for_map.length() + 1, 0, &status_line_bidi);
                    if (U_SUCCESS(status_line_bidi)) {
                        // Use paragraphBiDiLevelForLineMap which is the resolved paragraph level
                        ubidi_setPara(lineBiDiObj, reinterpret_cast<const UChar*>(lineU16_for_map.data()), lineU16_for_map.length(), paragraphBiDiLevelForLineMap, nullptr, &status_line_bidi);
                        if (U_SUCCESS(status_line_bidi)) {
                            int32_t line_logical_len_for_map_icu = ubidi_getLength(lineBiDiObj);
                            if (line_logical_len_for_map_icu > 0) {
                                finalizedLine.visualToLogicalMap.resize(line_logical_len_for_map_icu);
                                finalizedLine.logicalToVisualMap.resize(line_logical_len_for_map_icu);
                                ubidi_getVisualMap(lineBiDiObj, finalizedLine.visualToLogicalMap.data(), &status_line_bidi);
                                if(U_FAILURE(status_line_bidi)) TraceLog(LOG_WARNING, "ICU ubidi_getVisualMap failed: %s", u_errorName(status_line_bidi));
                                status_line_bidi = U_ZERO_ERROR;
                                ubidi_getLogicalMap(lineBiDiObj, finalizedLine.logicalToVisualMap.data(), &status_line_bidi);
                                if(U_FAILURE(status_line_bidi)) TraceLog(LOG_WARNING, "ICU ubidi_getLogicalMap failed: %s", u_errorName(status_line_bidi));
                            } else {
                                finalizedLine.visualToLogicalMap.clear();
                                finalizedLine.logicalToVisualMap.clear();
                            }
                        } else { TraceLog(LOG_WARNING, "ICU ubidi_setPara for line map failed: %s", u_errorName(status_line_bidi)); }
                        ubidi_close(lineBiDiObj);
                    } else { TraceLog(LOG_WARNING, "ICU ubidi_openSized for line map failed: %s", u_errorName(status_line_bidi)); }
                }
            }

            textBlock.lines.push_back(finalizedLine);
            currentLineBoxTopY += finalizedLine.lineBoxHeight;
        }


        // DrawTextBlock (remains mostly the same as your original, ensure it uses updated PositionedGlyph.sourceFont)
        void DrawTextBlock(const TextBlock& textBlock, const Matrix& transform, Color globalTint, const Rectangle* clipRect) override { //
            if (textBlock.elements.empty() && textBlock.lines.empty()) return; //

            bool useSDFShader = (sdfShader_.id > 0 && sdfShader_.id != rlGetShaderIdDefault());

            rlDrawRenderBatchActive();
            rlPushMatrix();
            rlMultMatrixf(MatrixToFloat(transform));

            bool scissorActive = false;
            if (clipRect) { //
                // For simplicity, this assumes clipRect is in the transformed space.
                // Correct scissoring with arbitrary transforms is complex with Raylib's BeginScissorMode.
                // Consider rlglScissor for more control if needed.
                // BeginScissorMode((int)clipRect->x, (int)clipRect->y, (int)clipRect->width, (int)clipRect->height);
                // scissorActive = true;
            }

            if (useSDFShader) {
                BeginShaderMode(sdfShader_);
                float sdfEdgeTexVal =  128.0f /  255.0f; // Default for FT_RENDER_MODE_SDF
                if (uniform_sdfEdgeValue_loc_ != -1) {
                    SetShaderValue(sdfShader_, uniform_sdfEdgeValue_loc_, &sdfEdgeTexVal, SHADER_UNIFORM_FLOAT);
                }

                BatchRenderState currentBatchState; //
                bool isFirstElementInBatch = true;

                for (size_t lineIdx = 0; lineIdx < textBlock.lines.size(); ++lineIdx) { //
                    const auto& line = textBlock.lines[lineIdx]; //
                    float lineVisualBaselineY = line.lineBoxY + line.baselineYInBox; //

                    for (size_t i = 0; i < line.numElementsInLine; ++i) { //
                        if ((line.firstElementIndexInBlockElements + i) >= textBlock.elements.size()) { //
                            continue;
                        }
                        const auto& elementVariant = textBlock.elements[line.firstElementIndexInBlockElements + i]; //

                        if (std::holds_alternative<PositionedGlyph>(elementVariant)) { //
                            const auto& glyph = std::get<PositionedGlyph>(elementVariant); //

                            if (glyph.renderInfo.atlasTexture.id == 0 || glyph.renderInfo.atlasRect.width == 0 || glyph.renderInfo.atlasRect.height == 0) { //
                                continue;
                            }

                            if (!glyph.renderInfo.isSDF) { //
                                if (!isFirstElementInBatch) rlDrawRenderBatchActive();
                                EndShaderMode(); // Fallback to default shader for non-SDF

                                float renderScaleFactor_alpha = 1.0f;
                                if (IsFontValid(glyph.sourceFont) && loadedFonts_.count(glyph.sourceFont)) { //
                                    const auto& fontData = loadedFonts_.at(glyph.sourceFont);
                                    if (fontData.sdfPixelSizeHint > 0 && glyph.sourceSize > 0) { //
                                        renderScaleFactor_alpha = glyph.sourceSize / (float)fontData.sdfPixelSizeHint; //
                                    }
                                }
                                float finalDrawOffsetX_alpha = glyph.renderInfo.drawOffset.x * renderScaleFactor_alpha; //
                                float finalDrawOffsetY_alpha = glyph.renderInfo.drawOffset.y * renderScaleFactor_alpha; //
                                float finalRectWidth_alpha = glyph.renderInfo.atlasRect.width * renderScaleFactor_alpha; //
                                float finalRectHeight_alpha = glyph.renderInfo.atlasRect.height * renderScaleFactor_alpha; //

                                DrawTexturePro(glyph.renderInfo.atlasTexture, glyph.renderInfo.atlasRect, //
                                               {glyph.position.x + glyph.xOffset + finalDrawOffsetX_alpha, //
                                                lineVisualBaselineY + glyph.position.y + finalDrawOffsetY_alpha, // yOffset is negative up, position.y is -yOffset
                                                finalRectWidth_alpha, finalRectHeight_alpha},
                                               {0,0}, 0, ColorAlphaMultiply(glyph.appliedStyle.fill.solidColor, globalTint)); //

                                BeginShaderMode(sdfShader_); // Restore SDF shader
                                if (uniform_sdfEdgeValue_loc_ != -1) SetShaderValue(sdfShader_, uniform_sdfEdgeValue_loc_, &sdfEdgeTexVal, SHADER_UNIFORM_FLOAT);
                                isFirstElementInBatch = true; // Force state re-check
                                continue;
                            }
                            // SDF Path
                            float currentSmoothness = 0.02f + dynamicSmoothnessAdd; //
                            if (IsFontValid(glyph.sourceFont) && glyph.sourceSize > 0 && loadedFonts_.count(glyph.sourceFont)) { //
                                const auto& fontData = loadedFonts_.at(glyph.sourceFont); // Use actual sourceFont
                                if (fontData.sdfPixelSizeHint > 0) {
                                    float sdfSizeHint = (float)fontData.sdfPixelSizeHint;
                                    float scaleRatioRenderToSDFGen = glyph.sourceSize / sdfSizeHint; //
                                    currentSmoothness = (0.02f / std::max(0.5f, sqrtf(std::max(0.25f, scaleRatioRenderToSDFGen)))) + dynamicSmoothnessAdd;
                                    currentSmoothness = std::max(0.001f, std::min(currentSmoothness, 0.1f));
                                }
                            }
                            BatchRenderState newState(glyph, currentSmoothness); //
                            if (isFirstElementInBatch || newState.RequiresNewBatchComparedTo(currentBatchState)) {
                                if (!isFirstElementInBatch) rlDrawRenderBatchActive();
                                currentBatchState = newState; isFirstElementInBatch = false;
                                rlSetTexture(currentBatchState.atlasTexture.id);
                                Vector4 normFillColor = ColorNormalize(currentBatchState.fill.solidColor); //
                                Vector4 finalFillColor = { normFillColor.x*globalTint.r/255.f, normFillColor.y*globalTint.g/255.f, normFillColor.z*globalTint.b/255.f, normFillColor.w*globalTint.a/255.f };
                                if(uniform_textColor_loc_ != -1) SetShaderValue(sdfShader_, uniform_textColor_loc_, &finalFillColor, SHADER_UNIFORM_VEC4);
                                if(uniform_sdfSmoothness_loc_ != -1) SetShaderValue(sdfShader_, uniform_sdfSmoothness_loc_, &currentBatchState.dynamicSmoothnessValue, SHADER_UNIFORM_FLOAT);
                                int boldFlag = HasStyle(currentBatchState.basicStyle, FontStyle::Bold); //
                                float boldStrengthVal = 0.03f;
                                if(uniform_styleBold_loc_ != -1) SetShaderValue(sdfShader_, uniform_styleBold_loc_, &boldFlag, SHADER_UNIFORM_INT);
                                if(uniform_boldStrength_loc_ != -1) SetShaderValue(sdfShader_, uniform_boldStrength_loc_, &boldStrengthVal, SHADER_UNIFORM_FLOAT);
                                // Other effect uniforms (same as before)
                                int effectFlag = currentBatchState.outlineEnabled; if(uniform_enableOutline_loc_ != -1) SetShaderValue(sdfShader_, uniform_enableOutline_loc_, &effectFlag, SHADER_UNIFORM_INT); if (effectFlag) { Vector4 oC=ColorNormalize(currentBatchState.outlineColor); Vector4 fOC={oC.x*globalTint.r/255.f, oC.y*globalTint.g/255.f, oC.z*globalTint.b/255.f, oC.w*globalTint.a/255.f}; if(uniform_outlineColor_loc_ != -1) SetShaderValue(sdfShader_, uniform_outlineColor_loc_, &fOC, SHADER_UNIFORM_VEC4); if(uniform_outlineWidth_loc_ != -1) SetShaderValue(sdfShader_, uniform_outlineWidth_loc_, &currentBatchState.outlineWidth, SHADER_UNIFORM_FLOAT); }
                                effectFlag = currentBatchState.glowEnabled; if(uniform_enableGlow_loc_ != -1) SetShaderValue(sdfShader_, uniform_enableGlow_loc_, &effectFlag, SHADER_UNIFORM_INT); if (effectFlag) { Vector4 gC=ColorNormalize(currentBatchState.glowColor); Vector4 fGC={gC.x*globalTint.r/255.f, gC.y*globalTint.g/255.f, gC.z*globalTint.b/255.f, gC.w*globalTint.a/255.f}; if(uniform_glowColor_loc_ != -1) SetShaderValue(sdfShader_, uniform_glowColor_loc_, &fGC, SHADER_UNIFORM_VEC4); if(uniform_glowRange_loc_ != -1) SetShaderValue(sdfShader_, uniform_glowRange_loc_, &currentBatchState.glowRange, SHADER_UNIFORM_FLOAT); if(uniform_glowIntensity_loc_ != -1) SetShaderValue(sdfShader_, uniform_glowIntensity_loc_, &currentBatchState.glowIntensity, SHADER_UNIFORM_FLOAT); }
                                effectFlag = currentBatchState.shadowEnabled; if(uniform_enableShadow_loc_ != -1) SetShaderValue(sdfShader_, uniform_enableShadow_loc_, &effectFlag, SHADER_UNIFORM_INT); if (effectFlag) { Vector4 sC=ColorNormalize(currentBatchState.shadowColor); Vector4 fSC={sC.x*globalTint.r/255.f, sC.y*globalTint.g/255.f, sC.z*globalTint.b/255.f, sC.w*globalTint.a/255.f}; Vector2 sTO={0,0}; if(currentBatchState.atlasTexture.width >0) sTO.x = currentBatchState.shadowOffset.x/(float)currentBatchState.atlasTexture.width; if(currentBatchState.atlasTexture.height>0) sTO.y = currentBatchState.shadowOffset.y/(float)currentBatchState.atlasTexture.height; if(uniform_shadowColor_loc_ != -1) SetShaderValue(sdfShader_, uniform_shadowColor_loc_, &fSC, SHADER_UNIFORM_VEC4); if(uniform_shadowTexCoordOffset_loc_ != -1) SetShaderValue(sdfShader_, uniform_shadowTexCoordOffset_loc_, &sTO, SHADER_UNIFORM_VEC2); if(uniform_shadowSdfSpread_loc_ != -1) SetShaderValue(sdfShader_, uniform_shadowSdfSpread_loc_, &currentBatchState.shadowSdfSpread, SHADER_UNIFORM_FLOAT); }
                                effectFlag = currentBatchState.innerEffectEnabled; if(uniform_enableInnerEffect_loc_ != -1) SetShaderValue(sdfShader_, uniform_enableInnerEffect_loc_, &effectFlag, SHADER_UNIFORM_INT); if (effectFlag) { Vector4 ieC=ColorNormalize(currentBatchState.innerEffectColor); Vector4 fieC={ieC.x*globalTint.r/255.f, ieC.y*globalTint.g/255.f, ieC.z*globalTint.b/255.f, ieC.w*globalTint.a/255.f}; int ieIS = currentBatchState.innerEffectIsShadow; if(uniform_innerEffectColor_loc_ != -1) SetShaderValue(sdfShader_, uniform_innerEffectColor_loc_, &fieC, SHADER_UNIFORM_VEC4); if(uniform_innerEffectRange_loc_ != -1) SetShaderValue(sdfShader_, uniform_innerEffectRange_loc_, &currentBatchState.innerEffectRange, SHADER_UNIFORM_FLOAT); if(uniform_innerEffectIsShadow_loc_ != -1) SetShaderValue(sdfShader_, uniform_innerEffectIsShadow_loc_, &ieIS, SHADER_UNIFORM_INT); }

                            }

                            float renderScaleFactor = 1.0f; //
                            if (IsFontValid(glyph.sourceFont) && loadedFonts_.count(glyph.sourceFont)) { //
                                const auto& fontData = loadedFonts_.at(glyph.sourceFont);
                                if (fontData.sdfPixelSizeHint > 0 && glyph.sourceSize > 0) { //
                                    renderScaleFactor = glyph.sourceSize / (float)fontData.sdfPixelSizeHint; //
                                }
                            }
                            // The glyph.position already includes HarfBuzz x_offset and y_offset (as -yOffset)
                            float finalDrawX = glyph.position.x + glyph.renderInfo.drawOffset.x * renderScaleFactor; //
                            float finalDrawY = lineVisualBaselineY + glyph.position.y + (glyph.renderInfo.drawOffset.y * renderScaleFactor) ; //

                            Rectangle destRect = { finalDrawX, finalDrawY,
                                                   glyph.renderInfo.atlasRect.width * renderScaleFactor, //
                                                   glyph.renderInfo.atlasRect.height * renderScaleFactor //
                            };
                            Rectangle srcRect = glyph.renderInfo.atlasRect; //

                            float shearAmount = HasStyle(glyph.appliedStyle.basicStyle, FontStyle::Italic) ? 0.2f * destRect.height : 0.0f; //
                            rlCheckRenderBatchLimit(4); rlBegin(RL_QUADS); rlColor4ub(255,255,255,255);
                            rlTexCoord2f(srcRect.x/currentBatchState.atlasTexture.width, srcRect.y/currentBatchState.atlasTexture.height); rlVertex2f(destRect.x+shearAmount, destRect.y);
                            rlTexCoord2f(srcRect.x/currentBatchState.atlasTexture.width, (srcRect.y+srcRect.height)/currentBatchState.atlasTexture.height); rlVertex2f(destRect.x, destRect.y+destRect.height);
                            rlTexCoord2f((srcRect.x+srcRect.width)/currentBatchState.atlasTexture.width, (srcRect.y+srcRect.height)/currentBatchState.atlasTexture.height); rlVertex2f(destRect.x+destRect.width, destRect.y+destRect.height);
                            rlTexCoord2f((srcRect.x+srcRect.width)/currentBatchState.atlasTexture.width, srcRect.y/currentBatchState.atlasTexture.height); rlVertex2f(destRect.x+destRect.width+shearAmount, destRect.y);
                            rlEnd();

                        } else if (std::holds_alternative<PositionedImage>(elementVariant)) { //
                            if (!isFirstElementInBatch) rlDrawRenderBatchActive(); EndShaderMode(); // Non-SDF
                            const auto& img = std::get<PositionedImage>(elementVariant); //
                            if (img.imageParams.texture.id > 0) { //
                                Rectangle srcImgR = {0,0,(float)img.imageParams.texture.width, (float)img.imageParams.texture.height}; //
                                Rectangle dstImgR = {img.position.x, lineVisualBaselineY + img.position.y, img.width, img.height}; //
                                DrawTexturePro(img.imageParams.texture, srcImgR, dstImgR, {0,0},0.0f,globalTint); //
                            }
                            BeginShaderMode(sdfShader_); // Restore SDF shader
                            if (uniform_sdfEdgeValue_loc_ != -1) SetShaderValue(sdfShader_, uniform_sdfEdgeValue_loc_, &sdfEdgeTexVal, SHADER_UNIFORM_FLOAT);
                            isFirstElementInBatch = true; // Force state re-check
                        }
                    }
                }
                if (!isFirstElementInBatch) rlDrawRenderBatchActive();
                EndShaderMode();
            } else { // Fallback non-SDF drawing (same as before)
                for (size_t lineIdx = 0; lineIdx < textBlock.lines.size(); ++lineIdx) { //
                    const auto& line = textBlock.lines[lineIdx]; //
                    float lineVisualBaselineY = line.lineBoxY + line.baselineYInBox; //
                    for (size_t i = 0; i < line.numElementsInLine; ++i) { //
                        const auto& elVar = textBlock.elements[line.firstElementIndexInBlockElements + i]; //
                        if (std::holds_alternative<PositionedGlyph>(elVar)){ //
                            // Simplified non-SDF glyph drawing - this needs more robust handling for alpha bitmaps
                            const auto& glyph = std::get<PositionedGlyph>(elVar); //
                            if(glyph.renderInfo.atlasTexture.id > 0){ //
                                DrawTextureRec(glyph.renderInfo.atlasTexture, glyph.renderInfo.atlasRect, //
                                               {glyph.position.x + glyph.xOffset + glyph.renderInfo.drawOffset.x, lineVisualBaselineY + glyph.position.y + glyph.renderInfo.drawOffset.y}, //
                                               ColorAlphaMultiply(glyph.appliedStyle.fill.solidColor, globalTint)); //
                            }
                        } else if (std::holds_alternative<PositionedImage>(elVar)){ //
                            const auto& img = std::get<PositionedImage>(elVar); //
                            if (img.imageParams.texture.id > 0) { //
                                Rectangle srcImgR = {0,0,(float)img.imageParams.texture.width, (float)img.imageParams.texture.height}; //
                                Rectangle dstImgR = {img.position.x, lineVisualBaselineY + img.position.y, img.width, img.height}; //
                                DrawTexturePro(img.imageParams.texture, srcImgR, dstImgR, {0,0},0.0f,globalTint); //
                            }
                        }
                    }
                }
            }

            if (scissorActive) EndScissorMode();
            rlPopMatrix();
            rlDrawRenderBatchActive(); // Finish any active batch
            rlSetTexture(0); // Reset texture binding
        }

        // **NEWLY IMPLEMENTED**
        std::vector<Rectangle> GetTextRangeBounds(const TextBlock& textBlock, uint32_t byteOffsetStart, uint32_t byteOffsetEnd) const override {
            std::vector<Rectangle> boundsList;
            if (byteOffsetStart >= byteOffsetEnd || textBlock.lines.empty()) { //
                return boundsList;
            }

            for (const auto& line : textBlock.lines) { //
                uint32_t lineByteStart = line.sourceTextByteStartIndexInBlockText; //
                uint32_t lineByteEnd = line.sourceTextByteEndIndexInBlockText; //

                // Determine overlap between query range and line range
                uint32_t effectiveRangeStart = std::max(byteOffsetStart, lineByteStart);
                uint32_t effectiveRangeEnd = std::min(byteOffsetEnd, lineByteEnd);

                if (effectiveRangeStart >= effectiveRangeEnd) {
                    continue; // No overlap with this line
                }

                float lineVisualBaselineY = line.lineBoxY + line.baselineYInBox; //
                float currentRunMinX = -1.0f; // Use -1 to indicate no active run on this line yet
                float currentRunMaxX = 0.0f;
                float currentRunMaxAscent = 0.0f;
                float currentRunMaxDescent = 0.0f;

                for (size_t i = 0; i < line.numElementsInLine; ++i) { //
                    const auto& elementVariant = textBlock.elements[line.firstElementIndexInBlockElements + i]; //

                    uint32_t elGlobalByteStart = 0;
                    uint16_t elNumBytes = 0;
                    std::visit([&](const auto& el_v){ //
                        uint32_t span_start_offset = 0;
                        for(size_t k=0; k < el_v.sourceSpanIndex; ++k) { //
                            span_start_offset += textBlock.sourceSpansCopied[k].text.empty() && textBlock.sourceSpansCopied[k].style.isImage ? 3 : textBlock.sourceSpansCopied[k].text.length(); //
                        }
                        elGlobalByteStart = span_start_offset + el_v.sourceCharByteOffsetInSpan; //
                        elNumBytes = el_v.numSourceCharBytesInSpan; //
                    }, elementVariant);
                    uint32_t elGlobalByteEnd = elGlobalByteStart + elNumBytes;

                    // Check if this element is part of the effective range for this line
                    if (elGlobalByteEnd > effectiveRangeStart && elGlobalByteStart < effectiveRangeEnd) {
                        // This element is (at least partially) in the selection for this line
                        float elVisualX = 0, elVisualWidth = 0, elAscent = 0, elDescent = 0;
                        float elDrawOffsetX = 0, elDrawOffsetY = 0; // For SDF glyphs, relative to its logical origin

                        std::visit([&](const auto& el_val){
                            elVisualX = el_val.position.x; // This is the logical pen position (before SDF drawOffset)
                            elAscent = el_val.ascent; //
                            elDescent = el_val.descent; //
                            using T = std::decay_t<decltype(el_val)>;
                            if constexpr (std::is_same_v<T, PositionedGlyph>) { //
                                elVisualX += el_val.xOffset; // Apply HarfBuzz x_offset
                                if (el_val.renderInfo.isSDF && IsFontValid(el_val.sourceFont) && loadedFonts_.count(el_val.sourceFont)) { //
                                    const auto& fontData = loadedFonts_.at(el_val.sourceFont);
                                    if (fontData.sdfPixelSizeHint > 0 && el_val.sourceSize > 0) {
                                        float renderScale = el_val.sourceSize / (float)fontData.sdfPixelSizeHint;
                                        elVisualWidth = el_val.renderInfo.atlasRect.width * renderScale; //
                                        elDrawOffsetX = el_val.renderInfo.drawOffset.x * renderScale; //
                                        // elDrawOffsetY already incorporated into ascent/descent effectively or handled by y_offset.
                                    } else { elVisualWidth = el_val.xAdvance; } //
                                } else { // Non-SDF or missing info
                                    elVisualWidth = el_val.xAdvance; //
                                }
                            } else if constexpr (std::is_same_v<T, PositionedImage>) { //
                                elVisualWidth = el_val.width; //
                            }
                        }, elementVariant);

                        float actualVisualStartX = elVisualX + elDrawOffsetX;

                        if (currentRunMinX < 0.0f) { // Start of a new selected run on this line
                            currentRunMinX = actualVisualStartX;
                            currentRunMaxX = actualVisualStartX + elVisualWidth;
                        } else { // Extend current run
                            currentRunMinX = std::min(currentRunMinX, actualVisualStartX);
                            currentRunMaxX = std::max(currentRunMaxX, actualVisualStartX + elVisualWidth);
                        }
                        currentRunMaxAscent = std::max(currentRunMaxAscent, elAscent);
                        currentRunMaxDescent = std::max(currentRunMaxDescent, elDescent);

                    } else if (currentRunMinX >= 0.0f) { // Element is outside range, but a run was active
                        boundsList.push_back({currentRunMinX, lineVisualBaselineY - currentRunMaxAscent,
                                              currentRunMaxX - currentRunMinX, currentRunMaxAscent + currentRunMaxDescent});
                        currentRunMinX = -1.0f; // Reset for next potential run on this line
                        currentRunMaxAscent = 0.0f; currentRunMaxDescent = 0.0f;
                    }
                } // End element loop for line

                if (currentRunMinX >= 0.0f) { // Add any pending run at the end of the line
                    boundsList.push_back({currentRunMinX, lineVisualBaselineY - currentRunMaxAscent,
                                          currentRunMaxX - currentRunMinX, currentRunMaxAscent + currentRunMaxDescent});
                }
            } // End line loop
            return boundsList;
        }

        // **NEWLY IMPLEMENTED**
        void DrawTextSelectionHighlight(const TextBlock& textBlock,
                                        uint32_t selectionStartByte,
                                        uint32_t selectionEndByte,
                                        Color highlightColor,
                                        const Matrix& worldTransform
        ) const override {
            if (selectionStartByte >= selectionEndByte || textBlock.lines.empty()) return; //

            std::vector<Rectangle> selectionRects = GetTextRangeBounds(textBlock, selectionStartByte, selectionEndByte);
            if (selectionRects.empty()) return;

            rlDrawRenderBatchActive();
            rlEnableScissorTest(); // Enable scissor test if not already
            rlPushMatrix();
            rlMultMatrixf(MatrixToFloat(worldTransform));

            for (const auto& rec : selectionRects) {
                // DrawRectangleRec is fine if worldTransform is identity or only translation.
                // For rotation/scale, direct rlgl calls are more robust under transform.
                rlBegin(RL_QUADS);
                rlColor4ub(highlightColor.r, highlightColor.g, highlightColor.b, highlightColor.a);
                rlVertex2f(rec.x, rec.y);
                rlVertex2f(rec.x, rec.y + rec.height);
                rlVertex2f(rec.x + rec.width, rec.y + rec.height);
                rlVertex2f(rec.x + rec.width, rec.y);
                rlEnd();
                rlDrawRenderBatchActive(); // Ensure this segment is drawn
            }
            rlPopMatrix();
            rlDisableScissorTest(); // Disable scissor test if it was enabled by this function
            rlDrawRenderBatchActive(); // Final flush
        }


        // --- Glyph Cache Management ---
        void ClearGlyphCache() override { performCacheCleanup(); } //
        void SetGlyphAtlasOptions(size_t maxGlyphsEstimate, int atlasWidth, int atlasHeight, GlyphAtlasType typeHint) override { //
            bool optionsChanged = (glyph_cache_capacity_ != maxGlyphsEstimate || atlas_width_ != atlasWidth || atlas_height_ != atlasHeight || atlas_type_hint_ != typeHint);
            if (optionsChanged && (!atlas_textures_.empty() || !atlas_images_.empty())) { performCacheCleanup(); }
            glyph_cache_capacity_ = maxGlyphsEstimate > 0 ? maxGlyphsEstimate : 1;
            atlas_width_ = atlasWidth > 0 ? atlasWidth : 256; atlas_height_ = atlasHeight > 0 ? atlasHeight : 256;
            atlas_type_hint_ = typeHint; //
            if (atlas_type_hint_ != GlyphAtlasType::SDF_BITMAP && atlas_type_hint_ != GlyphAtlasType::ALPHA_ONLY_BITMAP) { //
                TraceLog(LOG_WARNING, "FTTextEngine: Unsupported GlyphAtlasType. Defaulting to SDF."); atlas_type_hint_ = GlyphAtlasType::SDF_BITMAP; //
            }
        }
        Texture2D GetAtlasTextureForDebug(int atlasIndex = 0) const override { if (atlasIndex >= 0 && static_cast<size_t>(atlasIndex) < atlas_textures_.size()) return atlas_textures_[atlasIndex]; return {0}; } //

        // --- Cursor and Hit-Testing ---
        // GetCursorInfoFromByteOffset and GetByteOffsetFromVisualPosition remain largely the same logic
        // as in your original RaylibSDFTextEx.cpp, but should be reviewed for BiDi correctness
        // if the LineLayoutInfo's BiDi maps are populated and used.
        // For brevity here, I'm including the existing logic structure.
        // They will benefit from the BiDi maps for more precise LTR/RTL boundary handling.
        CursorLocationInfo GetCursorInfoFromByteOffset(
                const TextBlock& textBlock, //
                uint32_t byteOffsetInConcatenatedText,
                bool preferLeadingEdge) const override { //

            CursorLocationInfo cInfo; //
            cInfo.byteOffset = std::min(byteOffsetInConcatenatedText, (uint32_t)textBlock.sourceTextConcatenated.length()); //

            FontId paraFontId = textBlock.paragraphStyleUsed.defaultCharacterStyle.fontId; //
            if (!IsFontValid(paraFontId)) paraFontId = defaultFontId_;
            float paraFontSize = textBlock.paragraphStyleUsed.defaultCharacterStyle.fontSize; //
            if (paraFontSize <= 0) paraFontSize = 16.0f;
            ScaledFontMetrics defaultMetrics = GetScaledFontMetrics(paraFontId, paraFontSize); //

            if (textBlock.lines.empty()) { //
                cInfo.lineIndex = 0; //
                float lineDrawStartX = textBlock.paragraphStyleUsed.firstLineIndent; //
                cInfo.visualPosition.x = lineDrawStartX; //
                cInfo.visualPosition.y = defaultMetrics.ascent; // Baseline Y
                cInfo.cursorAscent = defaultMetrics.ascent; //
                cInfo.cursorDescent = defaultMetrics.descent; //
                cInfo.cursorHeight = defaultMetrics.ascent + defaultMetrics.descent; //
                if (cInfo.cursorHeight < 1.0f) cInfo.cursorHeight = paraFontSize;
                cInfo.isAtLogicalLineEnd = true; //
                cInfo.isTrailingEdge = true; // At end of (empty) text
                return cInfo;
            }

            int targetLineIdx = -1;
            for (size_t i = 0; i < textBlock.lines.size(); ++i) { //
                const auto& line = textBlock.lines[i]; //
                if ((cInfo.byteOffset >= line.sourceTextByteStartIndexInBlockText && cInfo.byteOffset < line.sourceTextByteEndIndexInBlockText) || //
                    (cInfo.byteOffset == line.sourceTextByteEndIndexInBlockText && (i == textBlock.lines.size() - 1 || cInfo.byteOffset < textBlock.lines[i + 1].sourceTextByteStartIndexInBlockText)) || //
                    (cInfo.byteOffset == textBlock.sourceTextConcatenated.length() && i == textBlock.lines.size() - 1) ) { //
                    targetLineIdx = i; break;
                }
            }
            if (targetLineIdx == -1) { targetLineIdx = textBlock.lines.size() - 1; cInfo.byteOffset = textBlock.lines[targetLineIdx].sourceTextByteEndIndexInBlockText; } //
            if (targetLineIdx < 0) targetLineIdx = 0;

            const auto& line = textBlock.lines[targetLineIdx]; //
            cInfo.lineIndex = targetLineIdx; //
            cInfo.visualPosition.y = line.lineBoxY + line.baselineYInBox; // Y is baseline
            cInfo.isAtLogicalLineEnd = (cInfo.byteOffset == line.sourceTextByteEndIndexInBlockText); //

            float lineDrawStartX = 0.0f;
            bool isThisLineFirstInParagraph = (line.sourceTextByteStartIndexInBlockText == 0) || (line.sourceTextByteStartIndexInBlockText > 0 && !textBlock.sourceTextConcatenated.empty() && textBlock.sourceTextConcatenated[line.sourceTextByteStartIndexInBlockText-1] == '\n'); //
            if (isThisLineFirstInParagraph) lineDrawStartX += textBlock.paragraphStyleUsed.firstLineIndent; //
            float lineShiftX = 0; float visualLineWidthWithIndent = lineDrawStartX + line.lineWidth; //
            float effectiveWrapWidth = textBlock.paragraphStyleUsed.wrapWidth > 0 ? textBlock.paragraphStyleUsed.wrapWidth : visualLineWidthWithIndent; //
            if (effectiveWrapWidth < 0.01f && visualLineWidthWithIndent > 0.01f) effectiveWrapWidth = visualLineWidthWithIndent;
            if (textBlock.paragraphStyleUsed.alignment == HorizontalAlignment::RIGHT && visualLineWidthWithIndent < effectiveWrapWidth) lineShiftX = effectiveWrapWidth - visualLineWidthWithIndent; //
            else if (textBlock.paragraphStyleUsed.alignment == HorizontalAlignment::CENTER && visualLineWidthWithIndent < effectiveWrapWidth) lineShiftX = (effectiveWrapWidth - visualLineWidthWithIndent) / 2.0f; //
            lineDrawStartX += lineShiftX; // This is the visual start X of the line's content box

            if (line.numElementsInLine == 0) { //
                cInfo.visualPosition.x = lineDrawStartX; //
                cInfo.cursorAscent = line.maxContentAscent > 0.001f ? line.maxContentAscent : defaultMetrics.ascent; //
                cInfo.cursorDescent = line.maxContentDescent > 0.001f ? line.maxContentDescent : defaultMetrics.descent; //
                cInfo.isTrailingEdge = preferLeadingEdge ? false : true; //
            } else {
                // This part needs to carefully use the BiDi maps if available and runs are LTR/RTL mixed.
                // The existing loop structure can be adapted.
                // For simplicity, the BiDi map usage is omitted here but would be an enhancement.
                bool cursorPositionFound = false;
                // Iterate through visual runs and then elements within those runs
                // (same logic as your original RaylibSDFTextEx.cpp for finding the element)
                // When an element is found, its pGlyph.position.x is already relative to the unaligned line start.
                // We just need to add lineDrawStartX to it.
                for (const auto& visualRun : line.visualRuns) { //
                    if (cursorPositionFound) break;
                    for (size_t i_el_in_run = 0; i_el_in_run < visualRun.numElementsInRun; ++i_el_in_run) { //
                        size_t elementIdxInLine = visualRun.firstElementIndexInLineElements + i_el_in_run; //
                        size_t currentElementGlobalIdx = line.firstElementIndexInBlockElements + elementIdxInLine; //
                        const auto& elementVariant = textBlock.elements[currentElementGlobalIdx]; //

                        uint32_t el_byte_start_in_block = 0; uint16_t el_num_bytes = 0;
                        float el_pos_x_in_line = 0, el_advance = 0; // el_pos_x_in_line is before line alignment
                        float el_ascent = defaultMetrics.ascent, el_descent = defaultMetrics.descent;

                        std::visit([&](const auto& el_v){ //
                            uint32_t current_byte_offset_for_span = 0;
                            for(size_t k=0; k < el_v.sourceSpanIndex; ++k) current_byte_offset_for_span += textBlock.sourceSpansCopied[k].text.empty() && textBlock.sourceSpansCopied[k].style.isImage ? 3 : textBlock.sourceSpansCopied[k].text.length(); //
                            el_byte_start_in_block = current_byte_offset_for_span + el_v.sourceCharByteOffsetInSpan; //
                            el_num_bytes = el_v.numSourceCharBytesInSpan; //
                            el_pos_x_in_line = el_v.position.x; // This is element's X relative to unaligned line start
                            el_ascent = el_v.ascent; el_descent = el_v.descent; //
                            using T = std::decay_t<decltype(el_v)>;
                            if constexpr (std::is_same_v<T, PositionedGlyph>) el_advance = el_v.xAdvance; //
                            else if constexpr (std::is_same_v<T, PositionedImage>) el_advance = el_v.penAdvanceX; //
                        }, elementVariant);

                        if (cInfo.byteOffset >= el_byte_start_in_block && cInfo.byteOffset <= el_byte_start_in_block + el_num_bytes) {
                            cInfo.cursorAscent = el_ascent > 0.001f ? el_ascent : defaultMetrics.ascent; //
                            cInfo.cursorDescent = el_descent > 0.001f ? el_descent : defaultMetrics.descent; //
                            bool isAtElementStart = (cInfo.byteOffset == el_byte_start_in_block);
                            // For LTR:
                            if ((isAtElementStart && preferLeadingEdge) || (cInfo.byteOffset < el_byte_start_in_block + el_num_bytes / 2.0f)) { // Simplified edge preference
                                cInfo.visualPosition.x = lineDrawStartX + el_pos_x_in_line; //
                                cInfo.isTrailingEdge = false; //
                            } else {
                                cInfo.visualPosition.x = lineDrawStartX + el_pos_x_in_line + el_advance; //
                                cInfo.isTrailingEdge = true; //
                            }
                            // TODO: Add proper BiDi logic here using visualRun.direction and LineLayoutInfo BiDi maps
                            cursorPositionFound = true; break;
                        }
                    }
                }
                if (!cursorPositionFound) { // Fallback to end of line
                    cInfo.visualPosition.x = lineDrawStartX + line.lineWidth; //
                    cInfo.cursorAscent = line.maxContentAscent > 0.001f ? line.maxContentAscent : defaultMetrics.ascent; //
                    cInfo.cursorDescent = line.maxContentDescent > 0.001f ? line.maxContentDescent : defaultMetrics.descent; //
                    cInfo.isTrailingEdge = true; //
                }
            }
            cInfo.cursorHeight = cInfo.cursorAscent + cInfo.cursorDescent; //
            if (cInfo.cursorHeight < 1.0f) { cInfo.cursorHeight = defaultMetrics.recommendedLineHeight > 0 ? defaultMetrics.recommendedLineHeight : paraFontSize; if (cInfo.cursorAscent < 0.01f && cInfo.cursorDescent < 0.01f) { cInfo.cursorAscent = cInfo.cursorHeight * 0.75f; cInfo.cursorDescent = cInfo.cursorHeight * 0.25f; }}
            return cInfo;
        }

// In RaylibSDFTextEx.cpp, within FTTextEngineImpl class
// 请用这个版本替换你现有的 GetByteOffsetFromVisualPosition 函数

        uint32_t GetByteOffsetFromVisualPosition(
                const TextBlock& textBlock,
                Vector2 positionInBlockLocalCoords,
                bool* isTrailingEdgeOut,
                float* distanceToClosestEdgeOut) const {

            // Initialize output parameters
            if (isTrailingEdgeOut) *isTrailingEdgeOut = false;
            if (distanceToClosestEdgeOut) *distanceToClosestEdgeOut = 1e9f;

            TraceLog(LOG_DEBUG, "GetByteOffset: ===== Function Start =====");
            TraceLog(LOG_DEBUG, "GetByteOffset: Input Click Coords (Block Local): X=%.2f, Y=%.2f", positionInBlockLocalCoords.x, positionInBlockLocalCoords.y);

            if (textBlock.lines.empty()) {
                TraceLog(LOG_INFO, "GetByteOffset: TextBlock has no lines. Defaulting to offset 0.");
                if (isTrailingEdgeOut && positionInBlockLocalCoords.x > 0) *isTrailingEdgeOut = true; // Simplistic trailing for empty block
                if (distanceToClosestEdgeOut) *distanceToClosestEdgeOut = fabsf(positionInBlockLocalCoords.x);
                return 0;
            }

            // 1. Determine the target line based on Y coordinate
            int targetLineIdx = 0;
            float minDistYToLineCenter = 1e9f;
            bool clickIsDirectlyOnLine = false;

            for (size_t i = 0; i < textBlock.lines.size(); ++i) {
                const auto& line = textBlock.lines[i];
                float lineTopY = line.lineBoxY;
                float lineBottomY = line.lineBoxY + line.lineBoxHeight;
                if (positionInBlockLocalCoords.y >= lineTopY && positionInBlockLocalCoords.y < lineBottomY) {
                    targetLineIdx = i;
                    clickIsDirectlyOnLine = true; // Click is within the Y-bounds of this line
                    minDistYToLineCenter = 0;
                    break;
                }
                float lineCenterY = lineTopY + line.lineBoxHeight / 2.0f;
                float distY = fabsf(positionInBlockLocalCoords.y - lineCenterY);
                if (distY < minDistYToLineCenter) {
                    minDistYToLineCenter = distY;
                    targetLineIdx = i;
                }
            }
            const auto& line = textBlock.lines[targetLineIdx];
            TraceLog(LOG_INFO, "GetByteOffset: TargetLine Index: %d (YBox: %.1f, Height: %.1f, ClickDirectlyOnLine: %s, MinDistY: %.2f)",
                     targetLineIdx, line.lineBoxY, line.lineBoxHeight, clickIsDirectlyOnLine ? "Yes" : "No", minDistYToLineCenter);

            // 2. Get the UTF-16 representation of the current line's text for BiDi maps
            std::u16string lineU16;
            uint32_t lineU8StartInBlock = line.sourceTextByteStartIndexInBlockText;
            uint32_t lineU8EndInBlock = line.sourceTextByteEndIndexInBlockText; // Exclusive
            if (lineU8EndInBlock > lineU8StartInBlock && lineU8EndInBlock <= textBlock.sourceTextConcatenated.length()) {
                lineU16 = Utf8ToUtf16(textBlock.sourceTextConcatenated.substr(lineU8StartInBlock, lineU8EndInBlock - lineU8StartInBlock));
            }
            TraceLog(LOG_DEBUG, "GetByteOffset: Line U16 Content (len %zu): \"%.*s...\" (U8 Range in Block: [%u, %u))",
                     lineU16.length(), lineU16.length() > 20 ? 20 : (int)lineU16.length() , Utf16ToUtf8(lineU16).c_str(), lineU8StartInBlock, lineU8EndInBlock);

            // 3. Calculate lineVisualContentStartX and handle clicks outside horizontal bounds or on empty lines
            float lineVisualContentStartX = 0.0f; // X where content visually starts in block coordinates
            bool isThisLineFirstInParagraph = (line.sourceTextByteStartIndexInBlockText == 0) ||
                                              (line.sourceTextByteStartIndexInBlockText > 0 &&
                                               !textBlock.sourceTextConcatenated.empty() &&
                                               line.sourceTextByteStartIndexInBlockText <= textBlock.sourceTextConcatenated.length() &&
                                               textBlock.sourceTextConcatenated[line.sourceTextByteStartIndexInBlockText - 1] == '\n');
            if (isThisLineFirstInParagraph) {
                lineVisualContentStartX += textBlock.paragraphStyleUsed.firstLineIndent;
            }
            float lineShiftX = 0; // Alignment shift
            float visualLineWidthWithIndent = (isThisLineFirstInParagraph ? textBlock.paragraphStyleUsed.firstLineIndent : 0.0f) + line.lineWidth;
            float effectiveWrapWidth = textBlock.paragraphStyleUsed.wrapWidth > 0 ? textBlock.paragraphStyleUsed.wrapWidth : visualLineWidthWithIndent;
            if (effectiveWrapWidth < 0.01f && visualLineWidthWithIndent > 0.01f) effectiveWrapWidth = visualLineWidthWithIndent;

            if (textBlock.paragraphStyleUsed.alignment == HorizontalAlignment::RIGHT && visualLineWidthWithIndent < effectiveWrapWidth) {
                lineShiftX = effectiveWrapWidth - visualLineWidthWithIndent;
            } else if (textBlock.paragraphStyleUsed.alignment == HorizontalAlignment::CENTER && visualLineWidthWithIndent < effectiveWrapWidth) {
                lineShiftX = (effectiveWrapWidth - visualLineWidthWithIndent) / 2.0f;
            }
            lineVisualContentStartX += lineShiftX;
            float lineVisualContentEndX = lineVisualContentStartX + line.lineWidth; // Visual end X of content on the line (in block coords)

            TraceLog(LOG_DEBUG, "GetByteOffset: Line Visual Content X Range (Block Coords): Start=%.1f, End=%.1f (LineWidth=%.1f, Indent=%.1f, AlignShift=%.1f)",
                     lineVisualContentStartX, lineVisualContentEndX, line.lineWidth,
                     (isThisLineFirstInParagraph ? textBlock.paragraphStyleUsed.firstLineIndent : 0.0f), lineShiftX);

            if (lineU16.empty() || line.numElementsInLine == 0) {
                if (isTrailingEdgeOut) *isTrailingEdgeOut = (positionInBlockLocalCoords.x > lineVisualContentStartX + line.lineWidth / 2.0f);
                if (distanceToClosestEdgeOut) *distanceToClosestEdgeOut = fabsf(positionInBlockLocalCoords.x - (lineVisualContentStartX + ((*isTrailingEdgeOut) ? line.lineWidth : 0.0f)));
                TraceLog(LOG_INFO, "GetByteOffset: Empty or No-Element Line. ByteOffset: %u. Trailing: %s", line.sourceTextByteStartIndexInBlockText, (*isTrailingEdgeOut ? "Y":"N"));
                return line.sourceTextByteStartIndexInBlockText;
            }

            if (positionInBlockLocalCoords.x < lineVisualContentStartX && !line.visualToLogicalMap.empty()) {
                if (isTrailingEdgeOut) *isTrailingEdgeOut = false;
                if (distanceToClosestEdgeOut) *distanceToClosestEdgeOut = fabsf(positionInBlockLocalCoords.x - lineVisualContentStartX);

                int32_t firstLogicalIdxInLine = line.visualToLogicalMap.empty() ? 0 : line.visualToLogicalMap[0];
                std::string prefixU8ToConvert;
                if (firstLogicalIdxInLine >= 0 && firstLogicalIdxInLine <= (int32_t)lineU16.length()) { // Ensure substr is valid
                    prefixU8ToConvert = Utf16ToUtf8(lineU16.substr(0, firstLogicalIdxInLine));
                }
                uint32_t resultOffset = lineU8StartInBlock + prefixU8ToConvert.length();
                TraceLog(LOG_INFO, "GetByteOffset: Click Left of Content. ByteOffset: %u. Trailing: false", resultOffset);
                return resultOffset;
            }

            // 4. Iterate through visual elements
            int32_t bestHitVisualU16IndexInLine = 0;
            bool determinedTrailingForBestHit = false;
            float minDistanceToConsideredEdge = 1e9f;

            if (line.visualRuns.empty() && line.numElementsInLine > 0) {
                TraceLog(LOG_WARNING, "GetByteOffset: Line %d has %zu elements but NO visual runs. BiDi info might be missing.", targetLineIdx, line.numElementsInLine);
            }

            for (const auto& visualRun : line.visualRuns) {
                TraceLog(LOG_DEBUG, "GetByteOffset: Processing VisualRun (Dir: %s, NumElements: %zu, RunFont: %d, RunSize: %.1f)",
                         (visualRun.direction == PositionedGlyph::BiDiDirectionHint::RTL ? "RTL" : (visualRun.direction == PositionedGlyph::BiDiDirectionHint::LTR ? "LTR" : "UNSPEC")),
                         visualRun.numElementsInRun, visualRun.runFont, visualRun.runFontSize);

                for (size_t i_el_in_run = 0; i_el_in_run < visualRun.numElementsInRun; ++i_el_in_run) {
                    size_t elementIdxInLineElements = visualRun.firstElementIndexInLineElements + i_el_in_run;
                    size_t currentElementGlobalIdx = line.firstElementIndexInBlockElements + elementIdxInLineElements;

                    if (currentElementGlobalIdx >= textBlock.elements.size()) {
                        TraceLog(LOG_WARNING, "GetByteOffset: Element index %zu out of bounds (Total elements: %zu)", currentElementGlobalIdx, textBlock.elements.size());
                        continue;
                    }
                    const auto& elementVariant = textBlock.elements[currentElementGlobalIdx];

                    float el_block_x = 0.0f;
                    float el_visual_width = 0.0f;
                    uint32_t el_u8_start_in_block_abs = 0;
                    uint16_t el_u8_num_bytes_in_span_for_el = 0;
                    uint32_t current_el_source_span_idx = 0;

                    std::visit([&](const auto& el_v){
                        el_block_x = el_v.position.x;
                        el_u8_num_bytes_in_span_for_el = el_v.numSourceCharBytesInSpan;
                        current_el_source_span_idx = el_v.sourceSpanIndex;

                        uint32_t span_start_offset_in_block = 0;
                        for(size_t k=0; k < el_v.sourceSpanIndex; ++k) {
                            if (k < textBlock.sourceSpansCopied.size()) {
                                const auto& prev_span_style = textBlock.sourceSpansCopied[k].style;
                                const auto& prev_span_text = textBlock.sourceSpansCopied[k].text;
                                span_start_offset_in_block += (prev_span_style.isImage && prev_span_text.empty() ? 3 : prev_span_text.length());
                            }
                        }
                        el_u8_start_in_block_abs = span_start_offset_in_block + el_v.sourceCharByteOffsetInSpan;

                        using T = std::decay_t<decltype(el_v)>;
                        if constexpr (std::is_same_v<T, PositionedGlyph>) el_visual_width = el_v.xAdvance;
                        else if constexpr (std::is_same_v<T, PositionedImage>) el_visual_width = el_v.width;
                    }, elementVariant);

                    float elementVisualLeftXInBlock = el_block_x;
                    float elementVisualRightXInBlock = el_block_x + el_visual_width;
                    float elementVisualMidXInBlock = el_block_x + el_visual_width / 2.0f; // Midpoint in block coordinates

                    bool clickIsOnVisualLeftHalfOfElement = (positionInBlockLocalCoords.x < elementVisualMidXInBlock);
                    float distToEdgeOfThisElement;
                    if (clickIsOnVisualLeftHalfOfElement) {
                        distToEdgeOfThisElement = fabsf(positionInBlockLocalCoords.x - elementVisualLeftXInBlock);
                    } else {
                        distToEdgeOfThisElement = fabsf(positionInBlockLocalCoords.x - elementVisualRightXInBlock);
                    }

                    std::string el_text_debug = "N/A";
                    if (el_u8_start_in_block_abs + el_u8_num_bytes_in_span_for_el <= textBlock.sourceTextConcatenated.length() && el_u8_num_bytes_in_span_for_el > 0){
                        size_t len_to_substr = 0;
                        const char* start_ptr_debug = textBlock.sourceTextConcatenated.c_str() + el_u8_start_in_block_abs;
                        const char* end_ptr_debug_limit = textBlock.sourceTextConcatenated.c_str() + el_u8_start_in_block_abs + el_u8_num_bytes_in_span_for_el;
                        int char_count_debug = 0;
                        while(char_count_debug < 3 && start_ptr_debug < end_ptr_debug_limit) {
                            int bytes_in_char_debug = 0;
                            const char* temp_sptr = start_ptr_debug; // Use temp for GetNextCodepointFromUTF8
                            GetNextCodepointFromUTF8(&temp_sptr, &bytes_in_char_debug);
                            if (bytes_in_char_debug == 0 || start_ptr_debug + bytes_in_char_debug > end_ptr_debug_limit) break; // ensure not past element
                            len_to_substr += bytes_in_char_debug;
                            start_ptr_debug += bytes_in_char_debug; // Manually advance original pointer
                            char_count_debug++;
                        }
                        if (len_to_substr > 0) {
                            el_text_debug = textBlock.sourceTextConcatenated.substr(el_u8_start_in_block_abs, len_to_substr);
                        }
                    }

                    // 【核心日志：评估每个元素】
                    TraceLog(LOG_DEBUG, "GetByteOffset: [%s EVAL] ClickX_B:%.1f | El:'%s'(U8St:%u,Len:%u) | VisL:%.1f,Mid:%.1f,VisR:%.1f | ClickOnLeftH:%s | CalcDist:%.2f | CurMinDist:%.2f",
                             (visualRun.direction == PositionedGlyph::BiDiDirectionHint::RTL ? "RTL" : "LTR"),
                             positionInBlockLocalCoords.x, el_text_debug.c_str(), el_u8_start_in_block_abs, el_u8_num_bytes_in_span_for_el,
                             elementVisualLeftXInBlock, elementVisualMidXInBlock, elementVisualRightXInBlock,
                             clickIsOnVisualLeftHalfOfElement ? "Y" : "N", distToEdgeOfThisElement, minDistanceToConsideredEdge);


                    if (distToEdgeOfThisElement < minDistanceToConsideredEdge) {
                        minDistanceToConsideredEdge = distToEdgeOfThisElement;

                        uint32_t el_u8_start_in_line_val = el_u8_start_in_block_abs - lineU8StartInBlock;
                        std::string el_source_u8_prefix_in_line;
                        if (el_u8_start_in_line_val <= (textBlock.sourceTextConcatenated.length() - lineU8StartInBlock) && lineU8StartInBlock + el_u8_start_in_line_val <= textBlock.sourceTextConcatenated.length() ) {
                            el_source_u8_prefix_in_line = textBlock.sourceTextConcatenated.substr(lineU8StartInBlock, el_u8_start_in_line_val);
                        }
                        int32_t logical_u16_start_for_element = Utf8ToUtf16(el_source_u8_prefix_in_line).length();

                        std::string el_source_u8_text_for_glyph_local;
                        if (el_u8_start_in_block_abs + el_u8_num_bytes_in_span_for_el <= textBlock.sourceTextConcatenated.length() && el_u8_num_bytes_in_span_for_el > 0) {
                            el_source_u8_text_for_glyph_local = textBlock.sourceTextConcatenated.substr(el_u8_start_in_block_abs, el_u8_num_bytes_in_span_for_el);
                        }
                        int32_t logical_u16_length_for_element = Utf8ToUtf16(el_source_u8_text_for_glyph_local).length();
                        if (logical_u16_length_for_element == 0 && el_u8_num_bytes_in_span_for_el > 0) logical_u16_length_for_element = 1;
// 新增日志：
                        if (visualRun.direction == PositionedGlyph::BiDiDirectionHint::RTL) {
                            TraceLog(LOG_INFO, "    [RTL El U16 Info] SourceU8:'%s', U8BytesForEl:%u -> U16LenForEl:%d",
                                     el_source_u8_text_for_glyph_local.c_str(),
                                     el_u8_num_bytes_in_span_for_el,
                                     logical_u16_length_for_element);
                        }
                        int32_t logical_u16_end_for_element = logical_u16_start_for_element + logical_u16_length_for_element;

                        logical_u16_start_for_element = std::max(0, std::min(logical_u16_start_for_element, (int32_t)lineU16.length()));
                        logical_u16_end_for_element   = std::max(logical_u16_start_for_element, std::min(logical_u16_end_for_element, (int32_t)lineU16.length()));

                        int32_t temp_best_hit_visual_u16_idx_before = bestHitVisualU16IndexInLine;
                        bool temp_determined_trailing_before = determinedTrailingForBestHit;

                        if (visualRun.direction == PositionedGlyph::BiDiDirectionHint::RTL) {
                            if (clickIsOnVisualLeftHalfOfElement) {
                                int32_t target_logical_idx = logical_u16_end_for_element > logical_u16_start_for_element ? (logical_u16_end_for_element - 1) : logical_u16_start_for_element;
                                target_logical_idx = std::max(0, std::min(target_logical_idx, (int32_t)line.logicalToVisualMap.size() -1 ));
                                if (!line.logicalToVisualMap.empty()) bestHitVisualU16IndexInLine = line.logicalToVisualMap[target_logical_idx];
                                else if (!lineU16.empty()) bestHitVisualU16IndexInLine = target_logical_idx; // Fallback if map empty but U16 text exists
                                else bestHitVisualU16IndexInLine = 0;
                                determinedTrailingForBestHit = true;
                            } else {
                                int32_t target_logical_idx = logical_u16_start_for_element;
                                target_logical_idx = std::max(0, std::min(target_logical_idx, (int32_t)line.logicalToVisualMap.size() -1 ));
                                if (!line.logicalToVisualMap.empty()) bestHitVisualU16IndexInLine = line.logicalToVisualMap[target_logical_idx];
                                else if (!lineU16.empty()) bestHitVisualU16IndexInLine = target_logical_idx;
                                else bestHitVisualU16IndexInLine = 0;
                                determinedTrailingForBestHit = false;
                            }
                        } else { // LTR
                            if (clickIsOnVisualLeftHalfOfElement) {
                                int32_t target_logical_idx = logical_u16_start_for_element;
                                target_logical_idx = std::max(0, std::min(target_logical_idx, (int32_t)line.logicalToVisualMap.size() -1 ));
                                if (!line.logicalToVisualMap.empty()) bestHitVisualU16IndexInLine = line.logicalToVisualMap[target_logical_idx];
                                else if (!lineU16.empty()) bestHitVisualU16IndexInLine = target_logical_idx;
                                else bestHitVisualU16IndexInLine = 0;
                                determinedTrailingForBestHit = false;
                            } else {
                                int32_t target_logical_idx = logical_u16_end_for_element > logical_u16_start_for_element ? (logical_u16_end_for_element - 1) : logical_u16_start_for_element;
                                target_logical_idx = std::max(0, std::min(target_logical_idx, (int32_t)line.logicalToVisualMap.size() -1 ));
                                if (!line.logicalToVisualMap.empty()) bestHitVisualU16IndexInLine = line.logicalToVisualMap[target_logical_idx];
                                else if (!lineU16.empty()) bestHitVisualU16IndexInLine = target_logical_idx;
                                else bestHitVisualU16IndexInLine = 0;
                                determinedTrailingForBestHit = true;
                            }
                        }
                        // 【核心日志：当一个元素被选为最佳匹配时】
                        TraceLog(LOG_INFO, "    -> [%s NEW BEST FIT!] El:'%s'. Dist:%.2f. LogU16Start:%d,LogU16End:%d -> VisU16Idx:%d, Trail:%s",
                                 (visualRun.direction == PositionedGlyph::BiDiDirectionHint::RTL ? "RTL" : "LTR"),
                                 el_text_debug.c_str(), minDistanceToConsideredEdge,
                                 logical_u16_start_for_element, logical_u16_end_for_element,
                                 bestHitVisualU16IndexInLine, determinedTrailingForBestHit ? "Y":"N");
                    }
                }
            }

            // TraceLog(LOG_DEBUG, "GetByteOffset: Pre-check Right Boundary: ClickX_Block %.1f vs LineVisEnd %.1f. CurrentMinDist: %.2f", positionInBlockLocalCoords.x, lineVisualContentEndX, minDistanceToConsideredEdge);
            if (positionInBlockLocalCoords.x >= lineVisualContentEndX && !lineU16.empty()) {
                float dist_to_visual_line_end = fabsf(positionInBlockLocalCoords.x - lineVisualContentEndX);
                if (dist_to_visual_line_end < minDistanceToConsideredEdge) {
                    minDistanceToConsideredEdge = dist_to_visual_line_end;

                    bool lineIsEffectivelyRTL = false;
                    if (!line.visualRuns.empty()) {
                        lineIsEffectivelyRTL = (line.visualRuns.front().direction == PositionedGlyph::BiDiDirectionHint::RTL);
                    } else if (textBlock.paragraphStyleUsed.baseDirection == TextDirection::RTL) {
                        lineIsEffectivelyRTL = true;
                    }

                    if (!line.logicalToVisualMap.empty()) {
                        if (lineIsEffectivelyRTL) {
                            int32_t target_logical_idx = 0;
                            target_logical_idx = std::max(0, std::min(target_logical_idx, (int32_t)line.logicalToVisualMap.size() -1 ));
                            bestHitVisualU16IndexInLine = line.logicalToVisualMap[target_logical_idx];
                            determinedTrailingForBestHit = false;
                        } else {
                            int32_t last_logical_char_idx = lineU16.length() > 0 ? lineU16.length() - 1 : 0;
                            last_logical_char_idx = std::max(0, std::min(last_logical_char_idx, (int32_t)line.logicalToVisualMap.size() -1));
                            bestHitVisualU16IndexInLine = line.logicalToVisualMap[last_logical_char_idx];
                            determinedTrailingForBestHit = true;
                        }
                    } else {
                        bestHitVisualU16IndexInLine = lineU16.length() > 0 ? (lineU16.length() -1) : 0;
                        determinedTrailingForBestHit = true;
                    }
                    TraceLog(LOG_INFO, "GetByteOffset: [LINE END HIT by dist] EffRTL:%s. VisU16Idx:%d. Trail:%s",
                             lineIsEffectivelyRTL ? "Y":"N", bestHitVisualU16IndexInLine, determinedTrailingForBestHit ? "Y":"N");
                }
            }

            // Sanity check bestHitVisualU16IndexInLine against map bounds
            if (bestHitVisualU16IndexInLine < 0) bestHitVisualU16IndexInLine = 0;
            if (!line.visualToLogicalMap.empty() && (size_t)bestHitVisualU16IndexInLine >= line.visualToLogicalMap.size()) {
                bestHitVisualU16IndexInLine = line.visualToLogicalMap.size() - 1;
                TraceLog(LOG_WARNING, "GetByteOffset: Clamped bestHitVisualU16IndexInLine to %d (map size %zu)", bestHitVisualU16IndexInLine, line.visualToLogicalMap.size());
            } else if (line.visualToLogicalMap.empty() && bestHitVisualU16IndexInLine != 0) {
                TraceLog(LOG_WARNING, "GetByteOffset: visualToLogicalMap is empty, but bestHitVisualU16IndexInLine is %d. Resetting to 0.", bestHitVisualU16IndexInLine);
                bestHitVisualU16IndexInLine = 0;
            }


            // 5. Convert the best hit visual U16 index to a logical U16 index
            int32_t finalLogicalU16IndexInLine = 0;
            if (!line.visualToLogicalMap.empty()) { // Check map is not empty before accessing
                finalLogicalU16IndexInLine = line.visualToLogicalMap[bestHitVisualU16IndexInLine];
                // TraceLog(LOG_DEBUG, "GetByteOffset: Mapped Visual %d to Logical %d", bestHitVisualU16IndexInLine, finalLogicalU16IndexInLine);
                if (determinedTrailingForBestHit) {
                    if (finalLogicalU16IndexInLine < (int32_t)lineU16.length()) {
                        int32_t tempIdx = finalLogicalU16IndexInLine;
                        if (!lineU16.empty()) {
                            U16_FWD_1(lineU16.data(), tempIdx, lineU16.length());
                        }
                        finalLogicalU16IndexInLine = tempIdx;
                        // TraceLog(LOG_DEBUG, "GetByteOffset: Trailing=true, advanced finalLogicalU16IndexInLine to %d", finalLogicalU16IndexInLine);
                    } else {
                        finalLogicalU16IndexInLine = lineU16.length();
                        // TraceLog(LOG_DEBUG, "GetByteOffset: Trailing=true, already at/past end, set to U16.length %d", finalLogicalU16IndexInLine);
                    }
                }
            } else if (!lineU16.empty()) {
                finalLogicalU16IndexInLine = determinedTrailingForBestHit ? lineU16.length() : 0;
                // TraceLog(LOG_DEBUG, "GetByteOffset: VisualToLogicalMap empty. finalLogicalU16IndexInLine set to %d", finalLogicalU16IndexInLine);
            } else {
                finalLogicalU16IndexInLine = 0;
                determinedTrailingForBestHit = (positionInBlockLocalCoords.x > lineVisualContentStartX);
                // TraceLog(LOG_DEBUG, "GetByteOffset: LineU16 empty. finalLogicalU16IndexInLine set to 0, Trailing: %s", determinedTrailingForBestHit ? "Y" : "N");
            }
            finalLogicalU16IndexInLine = std::max(0, std::min(finalLogicalU16IndexInLine, (int32_t)lineU16.length()));

            // 6. Convert the line-local logical UTF-16 index to a global UTF-8 byte offset
            std::string prefixU8ToConvert;
            if (finalLogicalU16IndexInLine > 0 && finalLogicalU16IndexInLine <= (int32_t)lineU16.length()) {
                prefixU8ToConvert = Utf16ToUtf8(lineU16.substr(0, finalLogicalU16IndexInLine));
            }
            uint32_t finalByteOffset = lineU8StartInBlock + prefixU8ToConvert.length();

            if (isTrailingEdgeOut) *isTrailingEdgeOut = determinedTrailingForBestHit;
            if (distanceToClosestEdgeOut) *distanceToClosestEdgeOut = minDistanceToConsideredEdge;

            TraceLog(LOG_INFO, "GetByteOffset: Final Result - Line:%d, ByteOffset:%u, IsTrailing:%s, DistToEdge:%.2f, ClickX:%.1f",
                     targetLineIdx, finalByteOffset, (determinedTrailingForBestHit ? "Y" : "N"), minDistanceToConsideredEdge, positionInBlockLocalCoords.x);
            TraceLog(LOG_DEBUG, "GetByteOffset: ===== Function End =====");

            return std::min(finalByteOffset, (uint32_t)textBlock.sourceTextConcatenated.length());
        }


        // Helper to multiply color components (used in DrawTextBlock)
        Color ColorAlphaMultiply(Color base, Color tint) { //
            return {(unsigned char)((base.r*tint.r)/255),(unsigned char)((base.g*tint.g)/255),(unsigned char)((base.b*tint.b)/255),(unsigned char)((base.a*tint.a)/255)};
        }
        // Helper to calculate line box height (used in LayoutStyledText and finalizeCurrentLine)
        float calculateLineBoxHeight(const ParagraphStyle& pStyle, const ScaledFontMetrics& defaultMetrics, float currentMaxAscent, float currentMaxDescent, float paraPrimaryFontSizeForFactor) const { //
            float calculatedHeight = 0; float contentActualHeight = currentMaxAscent + currentMaxDescent;
            if (contentActualHeight < 0.001f) { if ((defaultMetrics.ascent + defaultMetrics.descent) > 0.001f) contentActualHeight = defaultMetrics.ascent + defaultMetrics.descent; else contentActualHeight = paraPrimaryFontSizeForFactor > 0 ? paraPrimaryFontSizeForFactor * 1.2f : 16.0f * 1.2f; } //
            switch (pStyle.lineHeightType) { //
                case LineHeightType::NORMAL_SCALED_FONT_METRICS: calculatedHeight = defaultMetrics.recommendedLineHeight * pStyle.lineHeightValue; break; //
                case LineHeightType::FACTOR_SCALED_FONT_SIZE: calculatedHeight = paraPrimaryFontSizeForFactor * pStyle.lineHeightValue; break; //
                case LineHeightType::ABSOLUTE_POINTS: calculatedHeight = pStyle.lineHeightValue; break; //
                case LineHeightType::CONTENT_SCALED: calculatedHeight = contentActualHeight * pStyle.lineHeightValue; break; //
            } return std::max(calculatedHeight, contentActualHeight);
        }


    }; // class FTTextEngineImpl
} // anonymous namespace end

std::unique_ptr<ITextEngine> CreateTextEngine() { //
    return std::make_unique<FTTextEngineImpl>(); //
}