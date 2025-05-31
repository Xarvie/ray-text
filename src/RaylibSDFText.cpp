// RaylibSDFText.cpp (STB SDF Backend - 2025-05-21 - 再次修正所有纯虚函数实现)
// 实现了 text_engine.h 接口，使用 STB TrueType 生成 SDF 字形。

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "text_engine.h"
#include "raymath.h"
#include "rlgl.h"

#include <fstream>
#include <vector>
#include <map>
#include <list>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

// 全局变量，用于在main.cpp中动态调整SDF平滑度
extern float dynamicSmoothnessAdd;


namespace { // 匿名命名空间

// --- 内部数据结构 ---
    struct STBFontData {
        std::vector<unsigned char> fontBuffer;
        stbtt_fontinfo fontInfo;
        int sdfPixelSizeHint;
        ITextEngine::FontProperties properties;
    };

    struct GlyphCacheKey {
        FontId fontId;
        uint32_t codepoint;
        int sdfPixelSize;

        bool operator==(const GlyphCacheKey& other) const {
            return fontId == other.fontId &&
                   codepoint == other.codepoint &&
                   sdfPixelSize == other.sdfPixelSize;
        }
    };

    struct GlyphCacheKeyHash {
        std::size_t operator()(const GlyphCacheKey& k) const {
            std::size_t h1 = std::hash<FontId>()(k.fontId);
            std::size_t h2 = std::hash<uint32_t>()(k.codepoint);
            std::size_t h3 = std::hash<int>()(k.sdfPixelSize);
            return h1 ^ (h2 << 1) ^ (h3 << 2);
        }
    };

    struct CachedGlyph {
        GlyphRenderInfo renderInfo;
        float xAdvanceUnscaled = 0.0f;
        float yAdvanceUnscaled = 0.0f;
        float xOffsetUnscaled = 0.0f;
        float yOffsetUnscaled = 0.0f;
        int codepointBoxX0 = 0, codepointBoxY0 = 0, codepointBoxX1 = 0, codepointBoxY1 = 0;
        int ascentUnscaled = 0;
        int descentUnscaled = 0;
    };


    const char* sdfMasterFragmentShaderSrc = R"(
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
)";

    struct BatchRenderState {
        Texture2D atlasTexture;
        FillStyle fill;
        FontStyle basicStyle;
        bool outlineEnabled; Color outlineColor; float outlineWidth;
        bool glowEnabled; Color glowColor; float glowRange; float glowIntensity;
        bool shadowEnabled; Color shadowColor; Vector2 shadowOffset; float shadowSdfSpread;
        bool innerEffectEnabled; Color innerEffectColor; float innerEffectRange; bool innerEffectIsShadow;
        float dynamicSmoothnessValue;

        BatchRenderState() : atlasTexture({0}), basicStyle(FontStyle::Normal),
                             outlineEnabled(false), outlineColor(BLANK), outlineWidth(0.0f),
                             glowEnabled(false), glowColor(BLANK), glowRange(0.0f), glowIntensity(0.0f),
                             shadowEnabled(false), shadowColor(BLANK), shadowOffset({0,0}), shadowSdfSpread(0.0f),
                             innerEffectEnabled(false), innerEffectColor(BLANK), innerEffectRange(0.0f), innerEffectIsShadow(false),
                             dynamicSmoothnessValue(0.05f) {
            fill.type = FillType::SOLID_COLOR;
            fill.solidColor = BLACK;
        }

        BatchRenderState(const PositionedGlyph& glyph, float currentSmoothness) :
                atlasTexture(glyph.renderInfo.atlasTexture),
                fill(glyph.appliedStyle.fill),
                basicStyle(glyph.appliedStyle.basicStyle),
                outlineEnabled(glyph.appliedStyle.outline.enabled), outlineColor(glyph.appliedStyle.outline.color), outlineWidth(glyph.appliedStyle.outline.width),
                glowEnabled(glyph.appliedStyle.glow.enabled), glowColor(glyph.appliedStyle.glow.color), glowRange(glyph.appliedStyle.glow.range), glowIntensity(glyph.appliedStyle.glow.intensity),
                shadowEnabled(glyph.appliedStyle.shadow.enabled), shadowColor(glyph.appliedStyle.shadow.color), shadowOffset(glyph.appliedStyle.shadow.offset), shadowSdfSpread(glyph.appliedStyle.shadow.sdfSpread),
                innerEffectEnabled(glyph.appliedStyle.innerEffect.enabled), innerEffectColor(glyph.appliedStyle.innerEffect.color), innerEffectRange(glyph.appliedStyle.innerEffect.range), innerEffectIsShadow(glyph.appliedStyle.innerEffect.isShadow),
                dynamicSmoothnessValue(currentSmoothness) {}
    private:
        bool ColorEquals(Color c1, Color c2) const { return c1.r == c2.r && c1.g == c2.g && c1.b == c2.b && c1.a == c2.a; }
        bool FloatEquals(float f1, float f2, float epsilon = 0.0001f) const { return fabsf(f1 - f2) < epsilon; }
        bool Vector2Equals(Vector2 v1, Vector2 v2, float epsilon = 0.001f) const { return FloatEquals(v1.x, v2.x, epsilon) && FloatEquals(v1.y, v2.y, epsilon); }
        bool GradientStopsEqual(const std::vector<GradientStop>& s1, const std::vector<GradientStop>& s2) const {
            if (s1.size() != s2.size()) return false;
            for (size_t i = 0; i < s1.size(); ++i) {
                if (!ColorEquals(s1[i].color, s2[i].color) || !FloatEquals(s1[i].position, s2[i].position)) return false;
            }
            return true;
        }
        bool FillStyleEquals(const FillStyle& fs1, const FillStyle& fs2) const {
            if (fs1.type != fs2.type) return false;
            if (fs1.type == FillType::SOLID_COLOR) { return ColorEquals(fs1.solidColor, fs2.solidColor); }
            else if (fs1.type == FillType::LINEAR_GRADIENT) {
                return Vector2Equals(fs1.linearGradientStart, fs2.linearGradientStart) &&
                       Vector2Equals(fs1.linearGradientEnd, fs2.linearGradientEnd) &&
                       GradientStopsEqual(fs1.gradientStops, fs2.gradientStops);
            }
            return true;
        }
    public:
        bool RequiresNewBatchComparedTo(const BatchRenderState& other) const {
            if (atlasTexture.id != other.atlasTexture.id) return true;
            if (!FillStyleEquals(fill, other.fill)) return true;
            if (basicStyle != other.basicStyle) return true;
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
    };

    class STBTextEngineImpl : public ITextEngine {
    private:
        std::map<FontId, STBFontData> loadedFonts_;
        FontId nextFontId_ = 1;
        FontId defaultFontId_ = INVALID_FONT_ID;

        std::list<GlyphCacheKey> lru_glyph_list_;
        std::unordered_map<GlyphCacheKey, std::pair<CachedGlyph, std::list<GlyphCacheKey>::iterator>, GlyphCacheKeyHash> glyph_cache_map_;
        size_t glyph_cache_capacity_;

        std::vector<Image> atlas_images_;
        std::vector<Texture2D> atlas_textures_;
        int current_atlas_idx_ = -1;
        Vector2 current_atlas_pen_pos_ = {0, 0};
        float current_atlas_max_row_height_ = 0.0f;
        int atlas_width_;
        int atlas_height_;
        GlyphAtlasType atlas_type_hint_;

        Shader sdfShader_ = {0};
        int uniform_sdfTexture_loc_ = -1, uniform_textColor_loc_ = -1, uniform_sdfEdgeValue_loc_ = -1, uniform_sdfSmoothness_loc_ = -1;
        int uniform_enableOutline_loc_ = -1, uniform_outlineColor_loc_ = -1, uniform_outlineWidth_loc_ = -1;
        int uniform_enableGlow_loc_ = -1, uniform_glowColor_loc_ = -1, uniform_glowRange_loc_ = -1, uniform_glowIntensity_loc_ = -1;
        int uniform_enableShadow_loc_ = -1, uniform_shadowColor_loc_ = -1, uniform_shadowTexCoordOffset_loc_ = -1, uniform_shadowSdfSpread_loc_ = -1;
        int uniform_enableInnerEffect_loc_ = -1, uniform_innerEffectColor_loc_ = -1, uniform_innerEffectRange_loc_ = -1, uniform_innerEffectIsShadow_loc_ = -1;
        int uniform_styleBold_loc_ = -1, uniform_boldStrength_loc_ = -1;

        static const int SDF_DEFAULT_PADDING_CONST = 5;
        static const unsigned char SDF_ONEDGE_VALUE_CONST = 128;//128禁止修改，他和freetype的sdf算法参数对齐
        static constexpr float SDF_PIXEL_DIST_SCALE_CONST = 12.0 ;

        const int TAB_SPACE_EQUIVALENT = 4;

        // --- Private Helper Method Definitions ---
        CachedGlyph GetOrGenerateGlyph(FontId fontId, uint32_t codepoint, int sdfPixelSizeForHint) {
            if (!IsFontValid(fontId)) {
                TraceLog(LOG_WARNING, "STBTextEngine: GetOrGenerateGlyph called with invalid FontID: %d", fontId);
                return {};
            }
            const auto& fontData = loadedFonts_.at(fontId);
            int actualSdfGenSize = fontData.sdfPixelSizeHint > 0 ? fontData.sdfPixelSizeHint : 64;

            GlyphCacheKey key = {fontId, codepoint, actualSdfGenSize};
            auto it = glyph_cache_map_.find(key);
            if (it != glyph_cache_map_.end()) {
                lru_glyph_list_.splice(lru_glyph_list_.begin(), lru_glyph_list_, it->second.second);
                return it->second.first;
            }

            CachedGlyph newGlyph;
            newGlyph.renderInfo.isSDF = (atlas_type_hint_ == GlyphAtlasType::SDF_BITMAP);

            int advanceUnscaledInt, lsbUnscaledInt;
            stbtt_GetCodepointHMetrics(&fontData.fontInfo, codepoint, &advanceUnscaledInt, &lsbUnscaledInt);
            newGlyph.xAdvanceUnscaled = (float)advanceUnscaledInt;

            stbtt_GetFontVMetrics(&fontData.fontInfo, &newGlyph.ascentUnscaled, &newGlyph.descentUnscaled, nullptr);
            stbtt_GetCodepointBitmapBox(&fontData.fontInfo, codepoint, 1.0f, 1.0f,
                                        &newGlyph.codepointBoxX0, &newGlyph.codepointBoxY0,
                                        &newGlyph.codepointBoxX1, &newGlyph.codepointBoxY1);

            newGlyph.xOffsetUnscaled = (float)newGlyph.codepointBoxX0;
            newGlyph.yOffsetUnscaled = (float)newGlyph.codepointBoxY0;


            if (codepoint == ' ' || codepoint == '\t' || codepoint == '\n' || codepoint == 0x3000 ) {
                newGlyph.renderInfo.atlasTexture.id = 0;
                newGlyph.renderInfo.atlasRect = {0,0,0,0};
                newGlyph.renderInfo.drawOffset = {0,0};
            } else {
                float scaleForSdfGen = stbtt_ScaleForPixelHeight(&fontData.fontInfo, (float)actualSdfGenSize);
                int stb_xoff = 0, stb_yoff = 0, stb_w = 0, stb_h = 0;
                unsigned char* glyph_bitmap_data = nullptr;

                if (newGlyph.renderInfo.isSDF) {
                    glyph_bitmap_data = stbtt_GetCodepointSDF(&fontData.fontInfo, scaleForSdfGen, codepoint,
                                                              SDF_DEFAULT_PADDING_CONST, SDF_ONEDGE_VALUE_CONST, SDF_PIXEL_DIST_SCALE_CONST,
                                                              &stb_w, &stb_h, &stb_xoff, &stb_yoff);
                } else {
                    glyph_bitmap_data = stbtt_GetCodepointBitmap(&fontData.fontInfo, scaleForSdfGen, scaleForSdfGen,
                                                                 codepoint, &stb_w, &stb_h, &stb_xoff, &stb_yoff);
                }

                if (glyph_bitmap_data && stb_w > 0 && stb_h > 0) {
                    Rectangle pack_rect = findSpaceInAtlasAndPack(stb_w, stb_h, glyph_bitmap_data,
                                                                  PIXELFORMAT_UNCOMPRESSED_GRAYSCALE);

                    if (pack_rect.width > 0) {
                        newGlyph.renderInfo.atlasTexture = atlas_textures_[current_atlas_idx_];
                        newGlyph.renderInfo.atlasRect = pack_rect;
                        newGlyph.renderInfo.drawOffset = {(float)stb_xoff, (float)stb_yoff};
                    } else {
                        TraceLog(LOG_WARNING, "STBTextEngine: Failed to pack glyph for codepoint %u into atlas.", codepoint);
                        newGlyph.renderInfo.atlasTexture.id = 0;
                    }
                    stbtt_FreeBitmap(glyph_bitmap_data, fontData.fontInfo.userdata);
                } else {
                    if(glyph_bitmap_data) stbtt_FreeBitmap(glyph_bitmap_data, fontData.fontInfo.userdata);
                    newGlyph.renderInfo.atlasTexture.id = 0;
                }
            }

            if (glyph_cache_map_.size() >= glyph_cache_capacity_ && !lru_glyph_list_.empty()) {
                GlyphCacheKey lru_key_to_evict = lru_glyph_list_.back();
                lru_glyph_list_.pop_back();
                glyph_cache_map_.erase(lru_key_to_evict);
            }
            lru_glyph_list_.push_front(key);
            glyph_cache_map_[key] = {newGlyph, lru_glyph_list_.begin()};
            return newGlyph;
        }

        Rectangle findSpaceInAtlasAndPack(int width, int height, const unsigned char* bitmapData, PixelFormat format) {
            if (width <= 0 || height <= 0 || !bitmapData) return {0,0,0,0};

            bool needs_new_atlas = (current_atlas_idx_ == -1);
            if (!needs_new_atlas) {
                if (current_atlas_pen_pos_.x + width > atlas_width_) {
                    current_atlas_pen_pos_.x = 0;
                    current_atlas_pen_pos_.y += current_atlas_max_row_height_;
                    current_atlas_max_row_height_ = 0;
                }
                if (current_atlas_pen_pos_.y + height > atlas_height_) {
                    needs_new_atlas = true;
                }
            }

            if (needs_new_atlas) {
                current_atlas_idx_++;
                if (static_cast<size_t>(current_atlas_idx_) >= atlas_images_.size()) {
                    Image new_atlas_image = GenImageColor(atlas_width_, atlas_height_, BLANK);
                    ImageFormat(&new_atlas_image, PIXELFORMAT_UNCOMPRESSED_GRAYSCALE);
                    if (new_atlas_image.data) {
                        memset(new_atlas_image.data, 0, (size_t)atlas_width_ * atlas_height_ * GetPixelDataSize(1,1,PIXELFORMAT_UNCOMPRESSED_GRAYSCALE));
                    } else {
                        TraceLog(LOG_ERROR, "STBTextEngine: Failed to GenImageColor or format for atlas %d", current_atlas_idx_);
                        current_atlas_idx_--;
                        return {0,0,0,0};
                    }

                    atlas_images_.push_back(new_atlas_image);
                    Texture2D new_texture = LoadTextureFromImage(atlas_images_.back());
                    if (new_texture.id == 0) {
                        TraceLog(LOG_ERROR, "STBTextEngine: Failed to load texture from new atlas image %d", current_atlas_idx_);
                        UnloadImage(atlas_images_.back());
                        atlas_images_.pop_back();
                        current_atlas_idx_--;
                        return {0,0,0,0};
                    }
                    SetTextureFilter(new_texture, TEXTURE_FILTER_BILINEAR);
                    atlas_textures_.push_back(new_texture);
                    TraceLog(LOG_INFO, "STBTextEngine: Created new glyph atlas #%d (%dx%d, Grayscale)", current_atlas_idx_, atlas_width_, atlas_height_);
                }
                current_atlas_pen_pos_ = {0, 0};
                current_atlas_max_row_height_ = 0;
            }

            if (width > atlas_width_ || height > atlas_height_) {
                TraceLog(LOG_WARNING, "STBTextEngine: Glyph %dx%d too large for atlas %dx%d.", width, height, atlas_width_, atlas_height_);
                return {0,0,0,0};
            }
            if (current_atlas_pen_pos_.y + height > atlas_height_){
                TraceLog(LOG_ERROR, "STBTextEngine: Atlas packing logic error, trying to pack %dx%d at Y %.0f in atlas H %d.", width, height, current_atlas_pen_pos_.y, atlas_height_);
                return {0,0,0,0};
            }

            Rectangle spot = {current_atlas_pen_pos_.x, current_atlas_pen_pos_.y, (float)width, (float)height};

            Image* currentImageToUpdate = &atlas_images_[current_atlas_idx_];
            Image glyphImage = {
                    (void*)bitmapData,
                    width,
                    height,
                    1,
                    format
            };
            ImageDraw(currentImageToUpdate, glyphImage,
                      {0,0,(float)width,(float)height},
                      spot,
                      WHITE);

            UpdateTextureRec(atlas_textures_[current_atlas_idx_], spot, bitmapData);

            current_atlas_pen_pos_.x += width;
            current_atlas_max_row_height_ = std::max(current_atlas_max_row_height_, (float)height);
            return spot;
        }

        void finalizeLine(TextBlock& textBlock, LineLayoutInfo& currentLineLayout, float finalPenXNoIndent, float& currentLineBoxTopY, const ScaledFontMetrics& paraDefaultMetrics, bool isCurrentLineFirstInPara, uint32_t nextCharGlobalByteIndex) {
            currentLineLayout.lineWidth = finalPenXNoIndent;
            textBlock.overallBounds.width = std::max(textBlock.overallBounds.width, finalPenXNoIndent + (isCurrentLineFirstInPara ? textBlock.paragraphStyleUsed.firstLineIndent : 0.0f));
            currentLineLayout.sourceTextByteEndIndexInBlockText = nextCharGlobalByteIndex;

            float contentActualHeight = currentLineLayout.maxContentAscent + currentLineLayout.maxContentDescent;
            if (currentLineLayout.numElementsInLine == 0 || contentActualHeight < 0.001f) {
                currentLineLayout.maxContentAscent = paraDefaultMetrics.ascent;
                currentLineLayout.maxContentDescent = paraDefaultMetrics.descent;
                contentActualHeight = currentLineLayout.maxContentAscent + currentLineLayout.maxContentDescent;
            }

            currentLineLayout.lineBoxHeight = calculateLineBoxHeight(textBlock.paragraphStyleUsed, paraDefaultMetrics, currentLineLayout.maxContentAscent, currentLineLayout.maxContentDescent, paraDefaultMetrics.ascent + paraDefaultMetrics.descent);
            currentLineLayout.baselineYInBox = currentLineLayout.maxContentAscent;

            currentLineLayout.lineBoxY = currentLineBoxTopY;
            textBlock.lines.push_back(currentLineLayout);

            currentLineBoxTopY += currentLineLayout.lineBoxHeight;

            currentLineLayout.firstElementIndexInBlockElements = textBlock.elements.size();
            currentLineLayout.numElementsInLine = 0;
            currentLineLayout.sourceTextByteStartIndexInBlockText = nextCharGlobalByteIndex;
            currentLineLayout.maxContentAscent = 0;
            currentLineLayout.maxContentDescent = 0;
        }

        float calculateLineBoxHeight(const ParagraphStyle& pStyle, const ScaledFontMetrics& defaultMetrics, float currentMaxAscent, float currentMaxDescent, float paraPrimaryFontSizeForFactor) const {
            float calculatedHeight = 0;
            float contentActualHeight = currentMaxAscent + currentMaxDescent;
            if (contentActualHeight < 0.001f) contentActualHeight = defaultMetrics.ascent + defaultMetrics.descent;

            switch (pStyle.lineHeightType) {
                case LineHeightType::NORMAL_SCALED_FONT_METRICS:
                    calculatedHeight = defaultMetrics.recommendedLineHeight * pStyle.lineHeightValue;
                    break;
                case LineHeightType::FACTOR_SCALED_FONT_SIZE:
                    calculatedHeight = paraPrimaryFontSizeForFactor * pStyle.lineHeightValue;
                    break;
                case LineHeightType::ABSOLUTE_POINTS:
                    calculatedHeight = pStyle.lineHeightValue;
                    break;
                case LineHeightType::CONTENT_SCALED:
                    calculatedHeight = contentActualHeight * pStyle.lineHeightValue;
                    break;
            }
            return std::max(calculatedHeight, contentActualHeight);
        }

        Color ColorAlphaMultiply(Color base, Color tint) {
            return {
                    (unsigned char)((base.r * tint.r) / 255),
                    (unsigned char)((base.g * tint.g) / 255),
                    (unsigned char)((base.b * tint.b) / 255),
                    (unsigned char)((base.a * tint.a) / 255)
            };
        }

    public:
        STBTextEngineImpl() :
                glyph_cache_capacity_(512),
                atlas_width_(1024),
                atlas_height_(1024),
                atlas_type_hint_(GlyphAtlasType::SDF_BITMAP)
        {
            // Shader loading and uniform location retrieval
            sdfShader_ = LoadShaderFromMemory(nullptr, sdfMasterFragmentShaderSrc);
            if (sdfShader_.id == rlGetShaderIdDefault()) {
                TraceLog(LOG_WARNING, "STBTextEngine: SDF shader failed to load.");
            } else {
                TraceLog(LOG_INFO, "STBTextEngine: SDF shader loaded successfully (ID: %d).", sdfShader_.id);
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
        }

        // --- Implementations for ITextEngine public virtual methods ---

        // Font Management
        FontId LoadFont(const char* filePath, int faceIndex = 0) override {
            std::ifstream file(filePath, std::ios::binary | std::ios::ate);
            if (!file.is_open()) {
                TraceLog(LOG_WARNING, "STBTextEngine: Failed to open font file: %s", filePath);
                return INVALID_FONT_ID;
            }
            std::streamsize fileSize = file.tellg();
            file.seekg(0, std::ios::beg);

            STBFontData fontData;
            fontData.fontBuffer.resize(fileSize);
            if (!file.read(reinterpret_cast<char*>(fontData.fontBuffer.data()), fileSize)) {
                TraceLog(LOG_WARNING, "STBTextEngine: Failed to read font file: %s", filePath);
                file.close();
                return INVALID_FONT_ID;
            }
            file.close();

            int fontOffset = stbtt_GetFontOffsetForIndex(fontData.fontBuffer.data(), faceIndex);
            if (fontOffset == -1 && faceIndex == 0) fontOffset = 0;
            if (fontOffset == -1) {
                TraceLog(LOG_WARNING, "STBTextEngine: Invalid face index %d for font: %s", faceIndex, filePath);
                return INVALID_FONT_ID;
            }

            if (!stbtt_InitFont(&fontData.fontInfo, fontData.fontBuffer.data(), fontOffset)) {
                TraceLog(LOG_WARNING, "STBTextEngine: Failed to initialize font: %s (face index %d)", filePath, faceIndex);
                return INVALID_FONT_ID;
            }

            fontData.sdfPixelSizeHint = 64;

            unsigned char* headTablePtr = fontData.fontInfo.data + fontData.fontInfo.head;
            if (fontData.fontInfo.head && (size_t)(fontData.fontInfo.head + 18 + 2) <= fontData.fontBuffer.size() ) {
                fontData.properties.unitsPerEm = ttUSHORT(headTablePtr + 18);
            } else {
                fontData.properties.unitsPerEm = 1000;
            }
            if (fontData.properties.unitsPerEm == 0) fontData.properties.unitsPerEm = 1000;

            int typoAscent, typoDescent, typoLineGap;
            if (stbtt_GetFontVMetricsOS2(&fontData.fontInfo, &typoAscent, &typoDescent, &typoLineGap)) {
                fontData.properties.hasTypoMetrics = true;
                fontData.properties.typoAscender = typoAscent;
                fontData.properties.typoDescender = typoDescent;
                fontData.properties.typoLineGap = typoLineGap;
            } else {
                fontData.properties.hasTypoMetrics = false;
            }
            stbtt_GetFontVMetrics(&fontData.fontInfo, &fontData.properties.hheaAscender, &fontData.properties.hheaDescender, &fontData.properties.hheaLineGap);

            FontId id = nextFontId_++;
            loadedFonts_[id] = std::move(fontData);

            if (defaultFontId_ == INVALID_FONT_ID) {
                SetDefaultFont(id);
            }
            TraceLog(LOG_INFO, "STBTextEngine: Font '%s' (face %d) loaded successfully (ID: %d).", filePath, faceIndex, id);
            return id;
        }

        void UnloadFont(FontId fontId) override {
            if (loadedFonts_.erase(fontId) > 0) {
                auto lru_it = lru_glyph_list_.begin();
                while (lru_it != lru_glyph_list_.end()) {
                    if (lru_it->fontId == fontId) {
                        glyph_cache_map_.erase(*lru_it);
                        lru_it = lru_glyph_list_.erase(lru_it);
                    } else {
                        ++lru_it;
                    }
                }
                TraceLog(LOG_INFO, "STBTextEngine: Font ID %d and its cached glyphs unloaded.", fontId);
                if (defaultFontId_ == fontId) {
                    defaultFontId_ = loadedFonts_.empty() ? INVALID_FONT_ID : loadedFonts_.begin()->first;
                }
            }
        }

        bool IsFontValid(FontId fontId) const override {
            return loadedFonts_.count(fontId) > 0;
        }

        FontId GetDefaultFont() const override {
            return defaultFontId_;
        }

        void SetDefaultFont(FontId fontId) override {
            if (IsFontValid(fontId) || fontId == INVALID_FONT_ID) {
                defaultFontId_ = fontId;
            } else {
                TraceLog(LOG_WARNING, "STBTextEngine: Attempted to set invalid FontID %d as default.", fontId);
            }
        }

        void SetFontFallbackChain(FontId primaryFont, const std::vector<FontId>& fallbackChain) override {
            TraceLog(LOG_INFO, "STBTextEngine: SetFontFallbackChain called (currently a NOP for STB backend).");
        }

        ITextEngine::FontProperties GetFontProperties(FontId fontId) const override {
            if (IsFontValid(fontId)) {
                return loadedFonts_.at(fontId).properties;
            }
            TraceLog(LOG_WARNING, "STBTextEngine: GetFontProperties called with invalid FontID: %d", fontId);
            return {};
        }

        ITextEngine::ScaledFontMetrics GetScaledFontMetrics(FontId fontId, float fontSize) const override {
            ScaledFontMetrics metrics;
            if (!IsFontValid(fontId) || fontSize <= 0) {
                TraceLog(LOG_WARNING, "STBTextEngine: GetScaledFontMetrics with invalid FontID %d or fontSize %.2f", fontId, fontSize);
                metrics.ascent = fontSize > 0 ? fontSize * 0.75f : 12.0f;
                metrics.descent = fontSize > 0 ? fontSize * 0.25f : 4.0f;
                metrics.recommendedLineHeight = metrics.ascent + metrics.descent;
                return metrics;
            }

            const auto& fontData = loadedFonts_.at(fontId);
            metrics.scale = stbtt_ScaleForPixelHeight(&fontData.fontInfo, fontSize);

            if (fontData.properties.hasTypoMetrics) {
                metrics.ascent = static_cast<float>(fontData.properties.typoAscender) * metrics.scale;
                metrics.descent = -static_cast<float>(fontData.properties.typoDescender) * metrics.scale;
                metrics.lineGap = static_cast<float>(fontData.properties.typoLineGap) * metrics.scale;
            } else {
                metrics.ascent = static_cast<float>(fontData.properties.hheaAscender) * metrics.scale;
                metrics.descent = -static_cast<float>(fontData.properties.hheaDescender) * metrics.scale;
                metrics.lineGap = static_cast<float>(fontData.properties.hheaLineGap) * metrics.scale;
            }
            metrics.recommendedLineHeight = metrics.ascent + metrics.descent + metrics.lineGap;

            metrics.capHeight = metrics.ascent * 0.7f;
            metrics.xHeight = metrics.ascent * 0.5f;
            metrics.underlinePosition = -metrics.descent * 0.5f;
            metrics.underlineThickness = fontSize / 15.0f;
            metrics.strikeoutPosition = metrics.ascent * 0.35f;
            metrics.strikeoutThickness = fontSize / 15.0f;

            return metrics;
        }


        // RaylibSDFText.cpp -> STBTextEngineImpl 类内部

        // RaylibSDFText.cpp -> STBTextEngineImpl 类内部
// 替换掉之前的 LayoutStyledText 方法

        TextBlock LayoutStyledText(const std::vector<TextSpan>& spans, const ParagraphStyle& paraStyle) override {
            TextBlock textBlock;
            textBlock.paragraphStyleUsed = paraStyle;
            textBlock.sourceSpansCopied = spans;

            // --- 1. 确定段落主字体和字号及其度量 ---
            FontId paraPrimaryFontId = paraStyle.defaultCharacterStyle.fontId;
            if (!IsFontValid(paraPrimaryFontId)) paraPrimaryFontId = defaultFontId_;
            if (!IsFontValid(paraPrimaryFontId) && !loadedFonts_.empty()) paraPrimaryFontId = loadedFonts_.begin()->first;

            float paraPrimaryFontSize = paraStyle.defaultCharacterStyle.fontSize;
            if (paraPrimaryFontSize <= 0) paraPrimaryFontSize = 16.0f;

            ScaledFontMetrics paraDefaultScaledMetrics;
            const STBFontData* pParaPrimaryFontData = nullptr;
            if (IsFontValid(paraPrimaryFontId)){
                paraDefaultScaledMetrics = GetScaledFontMetrics(paraPrimaryFontId, paraPrimaryFontSize);
                if (loadedFonts_.count(paraPrimaryFontId)) {
                    pParaPrimaryFontData = &loadedFonts_.at(paraPrimaryFontId);
                }
            } else {
                paraDefaultScaledMetrics.ascent = paraPrimaryFontSize * 0.75f;
                paraDefaultScaledMetrics.descent = paraPrimaryFontSize * 0.25f;
                paraDefaultScaledMetrics.recommendedLineHeight = paraPrimaryFontSize * 1.2f;
                paraDefaultScaledMetrics.scale = 1.0f;
                // 估算 xHeight (如果 ScaledFontMetrics 中有此字段)
                paraDefaultScaledMetrics.xHeight = paraPrimaryFontSize * 0.45f; // 粗略估算
            }

            float defaultTabWidthVal = paraStyle.defaultTabWidthFactor * ( (IsFontValid(paraPrimaryFontId) && pParaPrimaryFontData && paraDefaultScaledMetrics.scale > 0.0001f) ?
                                                                           (GetOrGenerateGlyph(paraPrimaryFontId, ' ', pParaPrimaryFontData->sdfPixelSizeHint).xAdvanceUnscaled * paraDefaultScaledMetrics.scale) :
                                                                           (paraPrimaryFontSize * 0.5f) );
            if (defaultTabWidthVal <= 0) defaultTabWidthVal = paraPrimaryFontSize * 2.0f;


            // --- 2. 构建源文本连接串 ---
            for (const auto& span : spans) {
                if (span.style.isImage && span.text.empty()) {
                    textBlock.sourceTextConcatenated += "\xEF\xBF\xBC"; // U+FFFC OBJECT REPLACEMENT CHARACTER
                } else {
                    textBlock.sourceTextConcatenated += span.text;
                }
            }

            // --- 3. 初始化布局状态 ---
            float currentLineBoxTopY = 0.0f;
            LineLayoutInfo currentLineLayout;
            currentLineLayout.firstElementIndexInBlockElements = 0;
            currentLineLayout.sourceTextByteStartIndexInBlockText = 0;
            // 初始化行升降部为段落默认值，确保空行或只有图片的行有基本高度参考
            currentLineLayout.maxContentAscent = paraDefaultScaledMetrics.ascent;
            currentLineLayout.maxContentDescent = paraDefaultScaledMetrics.descent;

            Vector2 linePenPos = {0.0f, 0.0f};
            bool isCurrentLineFirstInParagraph = true;
            uint32_t currentGlobalCharByteIndex = 0;
            textBlock.overallBounds.width = 0;

            // --- 4. 遍历 Spans 进行布局 ---
            for (size_t spanIdx = 0; spanIdx < spans.size(); ++spanIdx) {
                const auto& span = spans[spanIdx];
                CharacterStyle currentStyle = span.style;

                FontId activeFontId = currentStyle.fontId;
                if (!IsFontValid(activeFontId)) activeFontId = paraPrimaryFontId;

                float activeFontSize = currentStyle.fontSize;
                if (activeFontSize <= 0) activeFontSize = paraPrimaryFontSize;

                ScaledFontMetrics currentScaledFontMetrics;
                const STBFontData* pActiveFontData = nullptr;
                if(IsFontValid(activeFontId)) {
                    currentScaledFontMetrics = GetScaledFontMetrics(activeFontId, activeFontSize);
                    if (loadedFonts_.count(activeFontId)) {
                        pActiveFontData = &loadedFonts_.at(activeFontId);
                    }
                } else {
                    currentScaledFontMetrics = paraDefaultScaledMetrics;
                    activeFontSize = paraPrimaryFontSize;
                }

                // --- 4a. 处理图片元素 ---
                if (currentStyle.isImage) {
                    PositionedImage pImg;
                    pImg.imageParams = currentStyle.imageParams;
                    pImg.width = (pImg.imageParams.displayWidth > 0) ? pImg.imageParams.displayWidth : (pImg.imageParams.texture.id > 0 ? (float)pImg.imageParams.texture.width : activeFontSize);
                    pImg.height = (pImg.imageParams.displayHeight > 0) ? pImg.imageParams.displayHeight : (pImg.imageParams.texture.id > 0 ? (float)pImg.imageParams.texture.height : activeFontSize);
                    pImg.penAdvanceX = pImg.width;

                    float resolvedImgPosY_relativeToBaseline = 0.0f;

                    // 获取用于对齐的参考字体度量
                    // 对于 TEXT_TOP, TEXT_BOTTOM, MIDDLE_OF_TEXT (基于x-height)，我们使用段落默认字体的度量作为稳定参考。
                    float referenceAscentForVAlign = paraDefaultScaledMetrics.ascent;
                    float referenceDescentForVAlign = paraDefaultScaledMetrics.descent; // 正值
                    float referenceXHeightForVAlign = paraDefaultScaledMetrics.xHeight; // 可能需要估算

                    // 如果 ScaledFontMetrics 没有提供 xHeight，进行估算
                    if (referenceXHeightForVAlign <= 0.001f && referenceAscentForVAlign > 0.001f) {
                        referenceXHeightForVAlign = referenceAscentForVAlign * 0.45f;
                    }
                    if (referenceXHeightForVAlign <= 0.001f && paraPrimaryFontSize > 0) {
                        referenceXHeightForVAlign = paraPrimaryFontSize * 0.40f;
                    }


                    switch (pImg.imageParams.vAlign) {
                        case CharacterStyle::InlineImageParams::VAlign::BASELINE:
                            pImg.ascent = pImg.height;
                            pImg.descent = 0.0f;
                            resolvedImgPosY_relativeToBaseline = -pImg.height;
                            break;
                        case CharacterStyle::InlineImageParams::VAlign::MIDDLE_OF_TEXT:
                        {
                            float targetMidYFromBaseline = referenceXHeightForVAlign / 2.0f;
                            resolvedImgPosY_relativeToBaseline = -(targetMidYFromBaseline + pImg.height / 2.0f);
                            pImg.ascent = std::max(0.0f, targetMidYFromBaseline + pImg.height / 2.0f);
                            pImg.descent = std::max(0.0f, -(targetMidYFromBaseline - pImg.height / 2.0f));
                            break;
                        }
                        case CharacterStyle::InlineImageParams::VAlign::TEXT_TOP:
                            resolvedImgPosY_relativeToBaseline = -referenceAscentForVAlign;
                            pImg.ascent = referenceAscentForVAlign;
                            pImg.descent = std::max(0.0f, pImg.height - referenceAscentForVAlign);
                            break;
                        case CharacterStyle::InlineImageParams::VAlign::TEXT_BOTTOM:
                            resolvedImgPosY_relativeToBaseline = referenceDescentForVAlign - pImg.height;
                            pImg.descent = referenceDescentForVAlign;
                            pImg.ascent = std::max(0.0f, pImg.height - referenceDescentForVAlign);
                            break;
                        case CharacterStyle::InlineImageParams::VAlign::LINE_TOP:
                        case CharacterStyle::InlineImageParams::VAlign::LINE_BOTTOM:
                        default: // Fallback
                            pImg.ascent = pImg.height;
                            pImg.descent = 0.0f;
                            resolvedImgPosY_relativeToBaseline = -pImg.height;
                            break;
                    }
                    pImg.ascent = std::max(0.0f, pImg.ascent);
                    pImg.descent = std::max(0.0f, pImg.descent);

                    pImg.sourceSpanIndex = spanIdx;
                    pImg.sourceCharByteOffsetInSpan = 0;
                    pImg.numSourceCharBytesInSpan = span.text.empty() ? 3 : static_cast<uint16_t>(span.text.length());

                    float currentVisualPenX = linePenPos.x + (isCurrentLineFirstInParagraph ? paraStyle.firstLineIndent : 0.0f);
                    if (paraStyle.wrapWidth > 0 && (currentVisualPenX + pImg.penAdvanceX > paraStyle.wrapWidth) && currentLineLayout.numElementsInLine > 0) {
                        finalizeLine(textBlock, currentLineLayout, linePenPos.x, currentLineBoxTopY, paraDefaultScaledMetrics, isCurrentLineFirstInParagraph, currentGlobalCharByteIndex, paraPrimaryFontSize);
                        isCurrentLineFirstInParagraph = false;
                        linePenPos.x = 0;
                        currentLineLayout.maxContentAscent = paraDefaultScaledMetrics.ascent;
                        currentLineLayout.maxContentDescent = paraDefaultScaledMetrics.descent;
                    }

                    pImg.position = {linePenPos.x, resolvedImgPosY_relativeToBaseline};
                    textBlock.elements.emplace_back(pImg);
                    currentLineLayout.numElementsInLine++;

                    // 更新行的最大升降部
                    if (currentLineLayout.numElementsInLine == 1) {
                        // 如果是行内第一个元素，用它的升降部（或与段落默认比较）
                        if (isCurrentLineFirstInParagraph && currentLineLayout.sourceTextByteStartIndexInBlockText == 0) { // 段落的第一个元素
                            currentLineLayout.maxContentAscent = std::max(paraDefaultScaledMetrics.ascent, pImg.ascent);
                            currentLineLayout.maxContentDescent = std::max(paraDefaultScaledMetrics.descent, pImg.descent);
                        } else { // 换行后的第一个元素
                            currentLineLayout.maxContentAscent = pImg.ascent;
                            currentLineLayout.maxContentDescent = pImg.descent;
                        }
                    } else {
                        currentLineLayout.maxContentAscent = std::max(currentLineLayout.maxContentAscent, pImg.ascent);
                        currentLineLayout.maxContentDescent = std::max(currentLineLayout.maxContentDescent, pImg.descent);
                    }

                    linePenPos.x += pImg.penAdvanceX;
                    currentGlobalCharByteIndex += pImg.numSourceCharBytesInSpan;
                    continue;
                }

                // --- 4b. 处理文本元素 ---
                const char* textPtr = span.text.c_str();
                const char* textEnd = textPtr + span.text.length();
                uint32_t currentByteOffsetInSpan = 0;

                while (textPtr < textEnd) {
                    int codepointByteCount = 0;
                    const char* charStartIterator = textPtr;
                    uint32_t codepoint = GetNextCodepointFromUTF8(&textPtr, &codepointByteCount);

                    if (codepointByteCount == 0 && textPtr >= textEnd) break;
                    if (codepoint == 0 && codepointByteCount == 1 && *charStartIterator == 0) break;
                    if (codepoint == 0xFFFD && codepointByteCount == 1 && *charStartIterator == 0) break;


                    PositionedGlyph pGlyph;
                    pGlyph.sourceFont = activeFontId;
                    pGlyph.sourceSize = activeFontSize;
                    pGlyph.appliedStyle = currentStyle;
                    pGlyph.glyphId = codepoint;
                    pGlyph.sourceSpanIndex = spanIdx;
                    pGlyph.sourceCharByteOffsetInSpan = currentByteOffsetInSpan;
                    pGlyph.numSourceCharBytesInSpan = static_cast<uint16_t>(codepointByteCount);

                    CachedGlyph cachedGlyph;
                    if(pActiveFontData) {
                        cachedGlyph = GetOrGenerateGlyph(activeFontId, codepoint, pActiveFontData->sdfPixelSizeHint);
                    } else {
                        cachedGlyph.xAdvanceUnscaled = currentScaledFontMetrics.scale > 0.001f ? (activeFontSize * 0.5f / currentScaledFontMetrics.scale) : (activeFontSize * 0.5f) ;
                        cachedGlyph.ascentUnscaled = currentScaledFontMetrics.scale > 0.001f ? (activeFontSize * 0.75f / currentScaledFontMetrics.scale) : (activeFontSize * 0.75f);
                        cachedGlyph.descentUnscaled = currentScaledFontMetrics.scale > 0.001f ? (-activeFontSize * 0.25f / currentScaledFontMetrics.scale) : (-activeFontSize * 0.25f);
                        pGlyph.renderInfo.isSDF = false;
                    }
                    pGlyph.renderInfo = cachedGlyph.renderInfo;
                    pGlyph.xAdvance = cachedGlyph.xAdvanceUnscaled * currentScaledFontMetrics.scale;
                    pGlyph.yAdvance = 0;
                    pGlyph.xOffset = 0;
                    pGlyph.yOffset = 0;
                    pGlyph.ascent = cachedGlyph.ascentUnscaled * currentScaledFontMetrics.scale;
                    pGlyph.descent = -cachedGlyph.descentUnscaled * currentScaledFontMetrics.scale;
                    pGlyph.visualLeft = cachedGlyph.xOffsetUnscaled * currentScaledFontMetrics.scale;
                    pGlyph.visualRight = (cachedGlyph.xOffsetUnscaled + (cachedGlyph.codepointBoxX1 - cachedGlyph.codepointBoxX0)) * currentScaledFontMetrics.scale;
                    pGlyph.visualRunDirectionHint = PositionedGlyph::BiDiDirectionHint::LTR;

                    if (codepoint == '\n') {
                        finalizeLine(textBlock, currentLineLayout, linePenPos.x, currentLineBoxTopY, paraDefaultScaledMetrics, isCurrentLineFirstInParagraph, currentGlobalCharByteIndex + codepointByteCount, paraPrimaryFontSize);
                        isCurrentLineFirstInParagraph = false;
                        linePenPos.x = 0;
                        currentLineLayout.maxContentAscent = paraDefaultScaledMetrics.ascent;
                        currentLineLayout.maxContentDescent = paraDefaultScaledMetrics.descent;
                        currentByteOffsetInSpan += codepointByteCount;
                        currentGlobalCharByteIndex += codepointByteCount;
                        currentLineLayout.sourceTextByteStartIndexInBlockText = currentGlobalCharByteIndex;
                        continue;
                    }

                    float kerningAdvance = 0.0f;
                    if (pActiveFontData && currentLineLayout.numElementsInLine > 0 && !textBlock.elements.empty()) {
                        const auto& prevElementVariant = textBlock.elements.back();
                        if (prevElementVariant.index() == 0) {
                            const auto& prevGlyph = std::get<PositionedGlyph>(prevElementVariant);
                            if (prevGlyph.sourceFont == activeFontId && fabsf(prevGlyph.sourceSize - activeFontSize) < 0.1f) {
                                kerningAdvance = stbtt_GetCodepointKernAdvance(&pActiveFontData->fontInfo, prevGlyph.glyphId, codepoint) * currentScaledFontMetrics.scale;
                            }
                        }
                    }
                    linePenPos.x += kerningAdvance;

                    if (codepoint == '\t') {
                        float currentVisualPenX = linePenPos.x + (isCurrentLineFirstInParagraph ? paraStyle.firstLineIndent : 0.0f);
                        float tabTargetVisualX = (floorf(currentVisualPenX / defaultTabWidthVal) + 1.0f) * defaultTabWidthVal;
                        pGlyph.xAdvance = std::max(0.0f, tabTargetVisualX - currentVisualPenX);
                        if (pGlyph.xAdvance < currentScaledFontMetrics.scale * 0.1f) pGlyph.xAdvance = defaultTabWidthVal;
                    }

                    float currentVisualPenXForWrap = linePenPos.x + (isCurrentLineFirstInParagraph ? paraStyle.firstLineIndent : 0.0f);
                    if (paraStyle.wrapWidth > 0 && (currentVisualPenXForWrap + pGlyph.xAdvance > paraStyle.wrapWidth) && currentLineLayout.numElementsInLine > 0) {
                        finalizeLine(textBlock, currentLineLayout, linePenPos.x, currentLineBoxTopY, paraDefaultScaledMetrics, isCurrentLineFirstInParagraph, currentGlobalCharByteIndex, paraPrimaryFontSize);
                        isCurrentLineFirstInParagraph = false;
                        linePenPos.x = 0;
                        currentLineLayout.maxContentAscent = paraDefaultScaledMetrics.ascent;
                        currentLineLayout.maxContentDescent = paraDefaultScaledMetrics.descent;
                    }

                    pGlyph.position = {linePenPos.x, 0};
                    textBlock.elements.push_back(pGlyph);
                    currentLineLayout.numElementsInLine++;

                    // 更新行升降部
                    if (currentLineLayout.numElementsInLine == 1) { // 如果是行内第一个元素
                        // (段落首行或换行后的首个元素)
                        // 它的升降部直接成为当前行的基准，但要确保不小于段落默认值（针对段落首行，防止小元素导致行高过小）
                        if (isCurrentLineFirstInParagraph && currentLineLayout.sourceTextByteStartIndexInBlockText == 0) {
                            currentLineLayout.maxContentAscent = std::max(paraDefaultScaledMetrics.ascent, pGlyph.ascent);
                            currentLineLayout.maxContentDescent = std::max(paraDefaultScaledMetrics.descent, pGlyph.descent);
                        } else { // 换行后的第一个元素
                            currentLineLayout.maxContentAscent = pGlyph.ascent;
                            currentLineLayout.maxContentDescent = pGlyph.descent;
                        }
                    } else {
                        currentLineLayout.maxContentAscent = std::max(currentLineLayout.maxContentAscent, pGlyph.ascent);
                        currentLineLayout.maxContentDescent = std::max(currentLineLayout.maxContentDescent, pGlyph.descent);
                    }

                    linePenPos.x += pGlyph.xAdvance;
                    currentByteOffsetInSpan += codepointByteCount;
                    currentGlobalCharByteIndex += codepointByteCount;
                }
            }

            // --- 5. Finalize the last line ---
            if (currentLineLayout.numElementsInLine > 0) {
                finalizeLine(textBlock, currentLineLayout, linePenPos.x, currentLineBoxTopY, paraDefaultScaledMetrics, isCurrentLineFirstInParagraph, currentGlobalCharByteIndex, paraPrimaryFontSize);
            } else if (textBlock.lines.empty() && spans.empty()) {
                LineLayoutInfo emptyLine;
                emptyLine.firstElementIndexInBlockElements = textBlock.elements.size();
                emptyLine.numElementsInLine = 0;
                emptyLine.lineBoxY = currentLineBoxTopY;
                emptyLine.maxContentAscent = paraDefaultScaledMetrics.ascent;
                emptyLine.maxContentDescent = paraDefaultScaledMetrics.descent;
                emptyLine.lineBoxHeight = calculateLineBoxHeight(paraStyle, paraDefaultScaledMetrics, emptyLine.maxContentAscent, emptyLine.maxContentDescent, paraPrimaryFontSize);
                emptyLine.baselineYInBox = emptyLine.maxContentAscent;
                emptyLine.lineWidth = 0.0f;
                emptyLine.sourceTextByteStartIndexInBlockText = currentGlobalCharByteIndex;
                emptyLine.sourceTextByteEndIndexInBlockText = currentGlobalCharByteIndex;
                textBlock.lines.push_back(emptyLine);
                currentLineBoxTopY += emptyLine.lineBoxHeight;
            }

            // --- 6. Calculate overall bounds ---
            if (!textBlock.lines.empty()) {
                textBlock.overallBounds.x = 0;
                textBlock.overallBounds.y = textBlock.lines.front().lineBoxY;
                float maxVisualLineWidth = 0;
                for(size_t i=0; i < textBlock.lines.size(); ++i) {
                    const auto& l = textBlock.lines[i];
                    bool isActualFirst = (l.sourceTextByteStartIndexInBlockText == 0) ||
                                         (l.sourceTextByteStartIndexInBlockText > 0 && !textBlock.sourceTextConcatenated.empty() && l.sourceTextByteStartIndexInBlockText <= textBlock.sourceTextConcatenated.length() && textBlock.sourceTextConcatenated[l.sourceTextByteStartIndexInBlockText-1] == '\n');
                    maxVisualLineWidth = std::max(maxVisualLineWidth, l.lineWidth + (isActualFirst ? paraStyle.firstLineIndent : 0.0f));
                }
                textBlock.overallBounds.width = maxVisualLineWidth;
                textBlock.overallBounds.height = currentLineBoxTopY - textBlock.lines.front().lineBoxY; // currentLineBoxTopY 现在是下一行的起始Y
            } else {
                textBlock.overallBounds = {0,0,0,0};
            }
            return textBlock;
        }

// 辅助函数 finalizeLine 的签名也需要匹配调用时传递的参数
        void finalizeLine(TextBlock& textBlock, LineLayoutInfo& currentLineLayout, float finalPenXNoIndent, float& currentLineBoxTopY, const ScaledFontMetrics& paraDefaultMetrics, bool isCurrentLineFirstInPara, uint32_t nextCharGlobalByteIndex, float paraMainFontSize) {
            currentLineLayout.lineWidth = finalPenXNoIndent;
            textBlock.overallBounds.width = std::max(textBlock.overallBounds.width, finalPenXNoIndent + (isCurrentLineFirstInPara ? textBlock.paragraphStyleUsed.firstLineIndent : 0.0f));
            currentLineLayout.sourceTextByteEndIndexInBlockText = nextCharGlobalByteIndex;

            float contentActualHeight = currentLineLayout.maxContentAscent + currentLineLayout.maxContentDescent;
            if (currentLineLayout.numElementsInLine == 0 || contentActualHeight < 0.001f) {
                currentLineLayout.maxContentAscent = paraDefaultMetrics.ascent;
                currentLineLayout.maxContentDescent = paraDefaultMetrics.descent;
                contentActualHeight = currentLineLayout.maxContentAscent + currentLineLayout.maxContentDescent;
            }

            currentLineLayout.lineBoxHeight = calculateLineBoxHeight(textBlock.paragraphStyleUsed, paraDefaultMetrics, currentLineLayout.maxContentAscent, currentLineLayout.maxContentDescent, paraMainFontSize);

            // 近似垂直居中行内容 (如果行盒高于内容)
            if (textBlock.paragraphStyleUsed.lineHeightType != LineHeightType::CONTENT_SCALED && currentLineLayout.lineBoxHeight > contentActualHeight + 0.001f) {
                float extraSpace = currentLineLayout.lineBoxHeight - contentActualHeight;
                currentLineLayout.baselineYInBox = currentLineLayout.maxContentAscent + extraSpace / 2.0f;
            } else {
                currentLineLayout.baselineYInBox = currentLineLayout.maxContentAscent;
            }

            currentLineLayout.lineBoxY = currentLineBoxTopY;
            textBlock.lines.push_back(currentLineLayout);

            currentLineBoxTopY += currentLineLayout.lineBoxHeight;

            currentLineLayout.firstElementIndexInBlockElements = textBlock.elements.size();
            currentLineLayout.numElementsInLine = 0;
            currentLineLayout.sourceTextByteStartIndexInBlockText = nextCharGlobalByteIndex;
            // 为新行重置参考升降部为段落默认值
            currentLineLayout.maxContentAscent = paraDefaultMetrics.ascent;
            currentLineLayout.maxContentDescent = paraDefaultMetrics.descent;
        }

        void DrawTextBlock(const TextBlock& textBlock, const Matrix& transform, Color globalTint, const Rectangle* clipRect) override {
            if (textBlock.elements.empty() && textBlock.lines.empty()) return;

            bool useSDFShader = (sdfShader_.id > 0 && sdfShader_.id != rlGetShaderIdDefault());

            rlDrawRenderBatchActive();
            rlPushMatrix();
            rlMultMatrixf(MatrixToFloat(transform));

            bool scissorMode = false;
            // if (clipRect) { BeginScissorMode((int)clipRect->x, (int)clipRect->y, (int)clipRect->width, (int)clipRect->height); scissorMode = true; }


            if (useSDFShader) {
                BeginShaderMode(sdfShader_);
                float sdfEdgeTexVal = (float)SDF_ONEDGE_VALUE_CONST / 255.0f;
                if (uniform_sdfEdgeValue_loc_ != -1) {
                    SetShaderValue(sdfShader_, uniform_sdfEdgeValue_loc_, &sdfEdgeTexVal, SHADER_UNIFORM_FLOAT);
                }

                BatchRenderState currentBatchState;
                bool isFirstElementInBatch = true;

                for (const auto& line : textBlock.lines) {
                    float lineVisualBaselineY = line.lineBoxY + line.baselineYInBox;

                    float lineDrawStartX = 0.0f;
                    if (textBlock.paragraphStyleUsed.alignment == HorizontalAlignment::RIGHT) {
                        lineDrawStartX = (textBlock.paragraphStyleUsed.wrapWidth > 0 ? textBlock.paragraphStyleUsed.wrapWidth : line.lineWidth) - line.lineWidth;
                    } else if (textBlock.paragraphStyleUsed.alignment == HorizontalAlignment::CENTER) {
                        lineDrawStartX = ((textBlock.paragraphStyleUsed.wrapWidth > 0 ? textBlock.paragraphStyleUsed.wrapWidth : line.lineWidth) - line.lineWidth) / 2.0f;
                    }
                    bool isLineActuallyFirstInPara = (line.sourceTextByteStartIndexInBlockText == 0) ||
                                                     (line.sourceTextByteStartIndexInBlockText > 0 && !textBlock.sourceTextConcatenated.empty() && textBlock.sourceTextConcatenated[line.sourceTextByteStartIndexInBlockText-1] == '\n');
                    if (isLineActuallyFirstInPara) {
                        lineDrawStartX += textBlock.paragraphStyleUsed.firstLineIndent;
                    }


                    for (size_t i = 0; i < line.numElementsInLine; ++i) {
                        const auto& elementVariant = textBlock.elements[line.firstElementIndexInBlockElements + i];

                        if (elementVariant.index() == 0) { // PositionedGlyph
                            const auto& glyph = std::get<PositionedGlyph>(elementVariant);

                            if (glyph.renderInfo.atlasTexture.id == 0 || glyph.renderInfo.atlasRect.width == 0 || glyph.renderInfo.atlasRect.height == 0) {
                                continue;
                            }
                            if (!glyph.renderInfo.isSDF) {
                                if (!isFirstElementInBatch) rlDrawRenderBatchActive(); EndShaderMode();
                                DrawTexturePro(glyph.renderInfo.atlasTexture, glyph.renderInfo.atlasRect,
                                               {lineDrawStartX + glyph.position.x + glyph.renderInfo.drawOffset.x,
                                                lineVisualBaselineY + glyph.position.y + glyph.renderInfo.drawOffset.y,
                                                glyph.renderInfo.atlasRect.width, glyph.renderInfo.atlasRect.height},
                                               {0,0}, 0, ColorAlphaMultiply(glyph.appliedStyle.fill.solidColor, globalTint));
                                BeginShaderMode(sdfShader_);
                                if (uniform_sdfEdgeValue_loc_ != -1) SetShaderValue(sdfShader_, uniform_sdfEdgeValue_loc_, &sdfEdgeTexVal, SHADER_UNIFORM_FLOAT);
                                isFirstElementInBatch = true;
                                continue;
                            }

                            float currentSmoothness = 0.05f + dynamicSmoothnessAdd;
                            if (IsFontValid(glyph.sourceFont) && glyph.sourceSize > 0 && loadedFonts_.count(glyph.sourceFont) && loadedFonts_.at(glyph.sourceFont).sdfPixelSizeHint > 0) {
                                float sdfSizeHint = (float)loadedFonts_.at(glyph.sourceFont).sdfPixelSizeHint;
                                float scaleRatio = glyph.sourceSize / sdfSizeHint;
                                currentSmoothness = (0.05f / sqrtf(std::max(0.25f, scaleRatio))) + dynamicSmoothnessAdd;
                                currentSmoothness = std::max(0.001f, std::min(currentSmoothness, 0.25f));
                            }

                            BatchRenderState newState(glyph, currentSmoothness);

                            if (isFirstElementInBatch || newState.RequiresNewBatchComparedTo(currentBatchState)) {
                                if (!isFirstElementInBatch) rlDrawRenderBatchActive();
                                currentBatchState = newState;
                                isFirstElementInBatch = false;

                                rlSetTexture(currentBatchState.atlasTexture.id);

                                Vector4 normFillColor = ColorNormalize(currentBatchState.fill.solidColor);
                                Vector4 finalFillColor = { normFillColor.x * globalTint.r/255.f, normFillColor.y * globalTint.g/255.f, normFillColor.z * globalTint.b/255.f, normFillColor.w * globalTint.a/255.f };
                                if(uniform_textColor_loc_ != -1) SetShaderValue(sdfShader_, uniform_textColor_loc_, &finalFillColor, SHADER_UNIFORM_VEC4);
                                if(uniform_sdfSmoothness_loc_ != -1) SetShaderValue(sdfShader_, uniform_sdfSmoothness_loc_, &currentBatchState.dynamicSmoothnessValue, SHADER_UNIFORM_FLOAT);

                                int boldFlag = HasStyle(currentBatchState.basicStyle, FontStyle::Bold);
                                float boldStrengthVal = 0.03f;
                                if(uniform_styleBold_loc_ != -1) SetShaderValue(sdfShader_, uniform_styleBold_loc_, &boldFlag, SHADER_UNIFORM_INT);
                                if(uniform_boldStrength_loc_ != -1) SetShaderValue(sdfShader_, uniform_boldStrength_loc_, &boldStrengthVal, SHADER_UNIFORM_FLOAT);

                                int effectFlag = currentBatchState.outlineEnabled;
                                if(uniform_enableOutline_loc_ != -1) SetShaderValue(sdfShader_, uniform_enableOutline_loc_, &effectFlag, SHADER_UNIFORM_INT);
                                if (effectFlag) {
                                    Vector4 normOutlineColor = ColorNormalize(currentBatchState.outlineColor);
                                    Vector4 finalOutlineColor = { normOutlineColor.x * globalTint.r/255.f, normOutlineColor.y * globalTint.g/255.f, normOutlineColor.z * globalTint.b/255.f, normOutlineColor.w * globalTint.a/255.f };
                                    if(uniform_outlineColor_loc_ != -1) SetShaderValue(sdfShader_, uniform_outlineColor_loc_, &finalOutlineColor, SHADER_UNIFORM_VEC4);
                                    if(uniform_outlineWidth_loc_ != -1) SetShaderValue(sdfShader_, uniform_outlineWidth_loc_, &currentBatchState.outlineWidth, SHADER_UNIFORM_FLOAT);
                                }
                                effectFlag = currentBatchState.glowEnabled;
                                if(uniform_enableGlow_loc_ != -1) SetShaderValue(sdfShader_, uniform_enableGlow_loc_, &effectFlag, SHADER_UNIFORM_INT);
                                if (effectFlag) {
                                    Vector4 normGlowColor = ColorNormalize(currentBatchState.glowColor);
                                    Vector4 finalGlowColor = { normGlowColor.x * globalTint.r/255.f, normGlowColor.y * globalTint.g/255.f, normGlowColor.z * globalTint.b/255.f, normGlowColor.w * globalTint.a/255.f };
                                    if(uniform_glowColor_loc_ != -1) SetShaderValue(sdfShader_, uniform_glowColor_loc_, &finalGlowColor, SHADER_UNIFORM_VEC4);
                                    if(uniform_glowRange_loc_ != -1) SetShaderValue(sdfShader_, uniform_glowRange_loc_, &currentBatchState.glowRange, SHADER_UNIFORM_FLOAT);
                                    if(uniform_glowIntensity_loc_ != -1) SetShaderValue(sdfShader_, uniform_glowIntensity_loc_, &currentBatchState.glowIntensity, SHADER_UNIFORM_FLOAT);
                                }
                                effectFlag = currentBatchState.shadowEnabled;
                                if(uniform_enableShadow_loc_ != -1) SetShaderValue(sdfShader_, uniform_enableShadow_loc_, &effectFlag, SHADER_UNIFORM_INT);
                                if (effectFlag) {
                                    Vector4 normShadowColor = ColorNormalize(currentBatchState.shadowColor);
                                    Vector4 finalShadowColor = { normShadowColor.x * globalTint.r/255.f, normShadowColor.y * globalTint.g/255.f, normShadowColor.z * globalTint.b/255.f, normShadowColor.w * globalTint.a/255.f };
                                    Vector2 shadowTexOffset = {0,0};
                                    if (currentBatchState.atlasTexture.id > 0 && currentBatchState.atlasTexture.width > 0 && currentBatchState.atlasTexture.height > 0) {
                                        shadowTexOffset.x = currentBatchState.shadowOffset.x / (float)currentBatchState.atlasTexture.width;
                                        shadowTexOffset.y = currentBatchState.shadowOffset.y / (float)currentBatchState.atlasTexture.height;
                                    }
                                    if(uniform_shadowColor_loc_ != -1) SetShaderValue(sdfShader_, uniform_shadowColor_loc_, &finalShadowColor, SHADER_UNIFORM_VEC4);
                                    if(uniform_shadowTexCoordOffset_loc_ != -1) SetShaderValue(sdfShader_, uniform_shadowTexCoordOffset_loc_, &shadowTexOffset, SHADER_UNIFORM_VEC2);
                                    if(uniform_shadowSdfSpread_loc_ != -1) SetShaderValue(sdfShader_, uniform_shadowSdfSpread_loc_, &currentBatchState.shadowSdfSpread, SHADER_UNIFORM_FLOAT);
                                }
                                effectFlag = currentBatchState.innerEffectEnabled;
                                if(uniform_enableInnerEffect_loc_ != -1) SetShaderValue(sdfShader_, uniform_enableInnerEffect_loc_, &effectFlag, SHADER_UNIFORM_INT);
                                if (effectFlag) {
                                    Vector4 normInnerColor = ColorNormalize(currentBatchState.innerEffectColor);
                                    Vector4 finalInnerColor = { normInnerColor.x * globalTint.r/255.f, normInnerColor.y * globalTint.g/255.f, normInnerColor.z * globalTint.b/255.f, normInnerColor.w * globalTint.a/255.f };
                                    int isShadowFlag = currentBatchState.innerEffectIsShadow;
                                    if(uniform_innerEffectColor_loc_ != -1) SetShaderValue(sdfShader_, uniform_innerEffectColor_loc_, &finalInnerColor, SHADER_UNIFORM_VEC4);
                                    if(uniform_innerEffectRange_loc_ != -1) SetShaderValue(sdfShader_, uniform_innerEffectRange_loc_, &currentBatchState.innerEffectRange, SHADER_UNIFORM_FLOAT);
                                    if(uniform_innerEffectIsShadow_loc_ != -1) SetShaderValue(sdfShader_, uniform_innerEffectIsShadow_loc_, &isShadowFlag, SHADER_UNIFORM_INT);
                                }
                            }

                            float renderScaleFactor = 1.0f;
                            if (IsFontValid(glyph.sourceFont) && loadedFonts_.count(glyph.sourceFont) && loadedFonts_.at(glyph.sourceFont).sdfPixelSizeHint > 0 && glyph.sourceSize > 0) {
                                renderScaleFactor = glyph.sourceSize / (float)loadedFonts_.at(glyph.sourceFont).sdfPixelSizeHint;
                            }

                            float finalDrawOffsetX = glyph.renderInfo.drawOffset.x * renderScaleFactor;
                            float finalDrawOffsetY = glyph.renderInfo.drawOffset.y * renderScaleFactor;
                            float finalRectWidth = glyph.renderInfo.atlasRect.width * renderScaleFactor;
                            float finalRectHeight = glyph.renderInfo.atlasRect.height * renderScaleFactor;

                            float drawX = lineDrawStartX + glyph.position.x + finalDrawOffsetX;
                            float drawY = lineVisualBaselineY + glyph.position.y + finalDrawOffsetY;

                            Rectangle destRect = { drawX, drawY, finalRectWidth, finalRectHeight };
                            Rectangle srcRect = glyph.renderInfo.atlasRect;

                            float shearAmount = 0.0f;
                            if (HasStyle(glyph.appliedStyle.basicStyle, FontStyle::Italic)) {
                                shearAmount = 0.2f * destRect.height;
                            }

                            rlCheckRenderBatchLimit(4);
                            rlBegin(RL_QUADS);
                            rlColor4ub(255,255,255,255);

                            rlTexCoord2f(srcRect.x / currentBatchState.atlasTexture.width, srcRect.y / currentBatchState.atlasTexture.height);
                            rlVertex2f(destRect.x + shearAmount, destRect.y);

                            rlTexCoord2f(srcRect.x / currentBatchState.atlasTexture.width, (srcRect.y + srcRect.height) / currentBatchState.atlasTexture.height);
                            rlVertex2f(destRect.x, destRect.y + destRect.height);

                            rlTexCoord2f((srcRect.x + srcRect.width) / currentBatchState.atlasTexture.width, (srcRect.y + srcRect.height) / currentBatchState.atlasTexture.height);
                            rlVertex2f(destRect.x + destRect.width, destRect.y + destRect.height);

                            rlTexCoord2f((srcRect.x + srcRect.width) / currentBatchState.atlasTexture.width, srcRect.y / currentBatchState.atlasTexture.height);
                            rlVertex2f(destRect.x + destRect.width + shearAmount, destRect.y);
                            rlEnd();

                        } else if (elementVariant.index() == 1) { // PositionedImage
                            if (!isFirstElementInBatch) rlDrawRenderBatchActive();
                            EndShaderMode();

                            const auto& img = std::get<PositionedImage>(elementVariant);
                            if (img.imageParams.texture.id > 0) {
                                Rectangle srcImgRect = {0, 0, (float)img.imageParams.texture.width, (float)img.imageParams.texture.height};
                                Rectangle dstImgRect = {
                                        lineDrawStartX + img.position.x,
                                        lineVisualBaselineY + img.position.y,
                                        img.width,
                                        img.height
                                };
                                DrawTexturePro(img.imageParams.texture, srcImgRect, dstImgRect, {0,0}, 0.0f, globalTint);
                            }
                            BeginShaderMode(sdfShader_);
                            if (uniform_sdfEdgeValue_loc_ != -1) SetShaderValue(sdfShader_, uniform_sdfEdgeValue_loc_, &sdfEdgeTexVal, SHADER_UNIFORM_FLOAT);
                            isFirstElementInBatch = true;
                        }
                    }
                }
                if (!isFirstElementInBatch) rlDrawRenderBatchActive();
                EndShaderMode();
            } else {
                TraceLog(LOG_WARNING, "STBTextEngine: SDF Shader not available/functional for DrawTextBlock. Glyphs will not be rendered correctly.");
                for (const auto& line : textBlock.lines) {
                    float lineVisualBaselineY = line.lineBoxY + line.baselineYInBox;
                    float lineDrawStartX = 0.0f;
                    bool isLineActuallyFirstInPara = (line.sourceTextByteStartIndexInBlockText == 0) ||
                                                     (line.sourceTextByteStartIndexInBlockText > 0 && !textBlock.sourceTextConcatenated.empty() && textBlock.sourceTextConcatenated[line.sourceTextByteStartIndexInBlockText-1] == '\n');
                    if (isLineActuallyFirstInPara) {
                        lineDrawStartX += textBlock.paragraphStyleUsed.firstLineIndent;
                    }

                    for (size_t i = 0; i < line.numElementsInLine; ++i) {
                        const auto& elementVariant = textBlock.elements[line.firstElementIndexInBlockElements + i];
                        if (elementVariant.index() == 1) { // PositionedImage
                            const auto& img = std::get<PositionedImage>(elementVariant);
                            if (img.imageParams.texture.id > 0) {
                                DrawTextureV(img.imageParams.texture, {lineDrawStartX + img.position.x, lineVisualBaselineY + img.position.y}, globalTint);
                            }
                        }
                    }
                }
            }

            if (scissorMode) EndScissorMode();
            rlPopMatrix();
            rlDrawRenderBatchActive();
            rlSetTexture(0);
        }

        // --- Glyph Cache Management ---
        void ClearGlyphCache() override {
            for (Texture2D tex : atlas_textures_) {
                if (tex.id > 0) UnloadTexture(tex);
            }
            atlas_textures_.clear();
            for (Image img : atlas_images_) {
                if (img.data) UnloadImage(img);
            }
            atlas_images_.clear();

            glyph_cache_map_.clear();
            lru_glyph_list_.clear();
            current_atlas_idx_ = -1;
            current_atlas_pen_pos_ = {0, 0};
            current_atlas_max_row_height_ = 0.0f;
            TraceLog(LOG_INFO, "STBTextEngine: Glyph cache and atlases cleared.");
        }

        void SetGlyphAtlasOptions(size_t maxGlyphsEstimate, int atlasWidth, int atlasHeight, GlyphAtlasType typeHint) override {
            if (!atlas_textures_.empty() || !atlas_images_.empty()) {
                TraceLog(LOG_INFO, "STBTextEngine: Atlas options changed, clearing existing atlases and cache.");
                ClearGlyphCache();
            }

            glyph_cache_capacity_ = maxGlyphsEstimate > 0 ? maxGlyphsEstimate : 1;
            atlas_width_ = atlasWidth > 0 ? atlasWidth : 256;
            atlas_height_ = atlasHeight > 0 ? atlasHeight : 256;
            atlas_type_hint_ = typeHint;

            if (atlas_type_hint_ != GlyphAtlasType::SDF_BITMAP && atlas_type_hint_ != GlyphAtlasType::ALPHA_ONLY_BITMAP) {
                TraceLog(LOG_WARNING, "STBTextEngine: SetGlyphAtlasOptions: STB backend defaults to SDF/Alpha. Specified type hint might not be fully utilized if different.");
            }
            TraceLog(LOG_INFO, "STBTextEngine: Glyph atlas options set - Capacity: %zu, Atlas: %dx%d, TypeHint: %s",
                     glyph_cache_capacity_, atlas_width_, atlas_height_, (typeHint == GlyphAtlasType::SDF_BITMAP ? "SDF" : "Alpha"));
        }

        Texture2D GetAtlasTextureForDebug(int atlasIndex = 0) const override {
            if (atlasIndex >= 0 && static_cast<size_t>(atlasIndex) < atlas_textures_.size()) {
                return atlas_textures_[atlasIndex];
            }
            return {0};
        }

        // --- Cursor and Hit-Testing ---
        CursorLocationInfo GetCursorInfoFromByteOffset(const TextBlock& textBlock, uint32_t byteOffsetInConcatenatedText, bool preferLeadingEdge) const override {
            CursorLocationInfo cInfo;
            cInfo.byteOffset = std::min(byteOffsetInConcatenatedText, (uint32_t)textBlock.sourceTextConcatenated.length());

            FontId paraFontId = textBlock.paragraphStyleUsed.defaultCharacterStyle.fontId;
            if (!IsFontValid(paraFontId)) paraFontId = defaultFontId_;
            float paraFontSize = textBlock.paragraphStyleUsed.defaultCharacterStyle.fontSize;
            if (paraFontSize <= 0) paraFontSize = 16.0f;

            ScaledFontMetrics defaultMetrics;
            if(IsFontValid(paraFontId)) defaultMetrics = GetScaledFontMetrics(paraFontId, paraFontSize);
            else {
                defaultMetrics.ascent = paraFontSize * 0.75f;
                defaultMetrics.descent = paraFontSize * 0.25f;
                defaultMetrics.recommendedLineHeight = defaultMetrics.ascent + defaultMetrics.descent;
            }

            if (textBlock.lines.empty()) {
                cInfo.lineIndex = 0;
                float lineDrawStartX = textBlock.paragraphStyleUsed.firstLineIndent;
                cInfo.visualPosition.x = lineDrawStartX;
                cInfo.visualPosition.y = defaultMetrics.ascent;
                cInfo.cursorAscent = defaultMetrics.ascent;
                cInfo.cursorDescent = defaultMetrics.descent;
                cInfo.cursorHeight = defaultMetrics.ascent + defaultMetrics.descent;
                cInfo.isAtLogicalLineEnd = true;
                cInfo.isTrailingEdge = true;
                return cInfo;
            }

            for (size_t lineIdx = 0; lineIdx < textBlock.lines.size(); ++lineIdx) {
                const auto& line = textBlock.lines[lineIdx];
                bool isLastLine = (lineIdx == textBlock.lines.size() - 1);

                if ( (cInfo.byteOffset >= line.sourceTextByteStartIndexInBlockText && cInfo.byteOffset < line.sourceTextByteEndIndexInBlockText) ||
                     (cInfo.byteOffset == line.sourceTextByteEndIndexInBlockText && (isLastLine || (lineIdx + 1 < textBlock.lines.size() && cInfo.byteOffset < textBlock.lines[lineIdx+1].sourceTextByteStartIndexInBlockText )))
                        ) {
                    cInfo.lineIndex = lineIdx;
                    cInfo.visualPosition.y = line.lineBoxY + line.baselineYInBox;
                    cInfo.isAtLogicalLineEnd = (cInfo.byteOffset == line.sourceTextByteEndIndexInBlockText);

                    float lineDrawStartX = 0.0f;
                    if (textBlock.paragraphStyleUsed.alignment == HorizontalAlignment::RIGHT) {
                        lineDrawStartX = (textBlock.paragraphStyleUsed.wrapWidth > 0 ? textBlock.paragraphStyleUsed.wrapWidth : line.lineWidth) - line.lineWidth;
                    } else if (textBlock.paragraphStyleUsed.alignment == HorizontalAlignment::CENTER) {
                        lineDrawStartX = ((textBlock.paragraphStyleUsed.wrapWidth > 0 ? textBlock.paragraphStyleUsed.wrapWidth : line.lineWidth) - line.lineWidth) / 2.0f;
                    }
                    bool isLineActuallyFirstInPara = (line.sourceTextByteStartIndexInBlockText == 0) ||
                                                     (line.sourceTextByteStartIndexInBlockText > 0 && !textBlock.sourceTextConcatenated.empty() && textBlock.sourceTextConcatenated[line.sourceTextByteStartIndexInBlockText-1] == '\n');
                    if (isLineActuallyFirstInPara) {
                        lineDrawStartX += textBlock.paragraphStyleUsed.firstLineIndent;
                    }

                    bool foundElementForCursor = false;
                    for (size_t elIdx = 0; elIdx < line.numElementsInLine; ++elIdx) {
                        const auto& elementVariant = textBlock.elements[line.firstElementIndexInBlockElements + elIdx];
                        uint32_t elSrcSpanIdx = 0;
                        uint32_t elSrcByteOffsetInSpan = 0;
                        uint16_t elSrcNumBytesInSpan = 0;
                        float elAdvanceX = 0;
                        float elPosX = 0;
                        ScaledFontMetrics elMetrics;

                        if (elementVariant.index() == 0) { // PositionedGlyph
                            const auto& glyph = std::get<PositionedGlyph>(elementVariant);
                            elSrcSpanIdx = glyph.sourceSpanIndex;
                            elSrcByteOffsetInSpan = glyph.sourceCharByteOffsetInSpan;
                            elSrcNumBytesInSpan = glyph.numSourceCharBytesInSpan;
                            elAdvanceX = glyph.xAdvance;
                            elPosX = glyph.position.x;
                            if(IsFontValid(glyph.sourceFont)) elMetrics = GetScaledFontMetrics(glyph.sourceFont, glyph.sourceSize);
                            else elMetrics = defaultMetrics;
                        } else { // PositionedImage
                            const auto& img = std::get<PositionedImage>(elementVariant);
                            elSrcSpanIdx = img.sourceSpanIndex;
                            elSrcByteOffsetInSpan = img.sourceCharByteOffsetInSpan;
                            elSrcNumBytesInSpan = img.numSourceCharBytesInSpan;
                            elAdvanceX = img.penAdvanceX;
                            elPosX = img.position.x;
                            elMetrics.ascent = img.ascent; elMetrics.descent = img.descent;
                        }

                        uint32_t elStartByteInBlock = 0;
                        for(uint32_t k=0; k < elSrcSpanIdx; ++k) {
                            if (textBlock.sourceSpansCopied[k].style.isImage && textBlock.sourceSpansCopied[k].text.empty()) elStartByteInBlock += 3;
                            else elStartByteInBlock += textBlock.sourceSpansCopied[k].text.length();
                        }
                        elStartByteInBlock += elSrcByteOffsetInSpan;

                        if (cInfo.byteOffset >= elStartByteInBlock && cInfo.byteOffset < elStartByteInBlock + elSrcNumBytesInSpan) {
                            cInfo.visualPosition.x = lineDrawStartX + elPosX;
                            if (preferLeadingEdge) {
                                cInfo.isTrailingEdge = false;
                            } else {
                                const char* p_temp_cstr = textBlock.sourceTextConcatenated.c_str();
                                const char* charStart = p_temp_cstr + cInfo.byteOffset;
                                int bytesForThisChar = 0;
                                GetNextCodepointFromUTF8(&charStart, &bytesForThisChar);
                                if ( (cInfo.byteOffset - elStartByteInBlock) >= (uint32_t)(bytesForThisChar / 2) ) {
                                    cInfo.visualPosition.x = lineDrawStartX + elPosX + elAdvanceX;
                                    cInfo.isTrailingEdge = true;
                                } else {
                                    cInfo.isTrailingEdge = false;
                                }
                            }
                            cInfo.cursorAscent = elMetrics.ascent;
                            cInfo.cursorDescent = elMetrics.descent;
                            foundElementForCursor = true;
                            break;
                        } else if (cInfo.byteOffset == elStartByteInBlock + elSrcNumBytesInSpan) {
                            cInfo.visualPosition.x = lineDrawStartX + elPosX + elAdvanceX;
                            cInfo.isTrailingEdge = true;
                            cInfo.cursorAscent = elMetrics.ascent;
                            cInfo.cursorDescent = elMetrics.descent;
                            foundElementForCursor = true;
                            if (elIdx == line.numElementsInLine - 1 || preferLeadingEdge) break;
                        }
                    }

                    if (!foundElementForCursor) {
                        if(cInfo.byteOffset == line.sourceTextByteStartIndexInBlockText) {
                            cInfo.visualPosition.x = lineDrawStartX;
                            cInfo.isTrailingEdge = false;
                        } else {
                            cInfo.visualPosition.x = lineDrawStartX + line.lineWidth;
                            cInfo.isTrailingEdge = true;
                        }
                        cInfo.cursorAscent = (line.maxContentAscent > 0.001f) ? line.maxContentAscent : defaultMetrics.ascent;
                        cInfo.cursorDescent = (line.maxContentDescent > 0.001f) ? line.maxContentDescent : defaultMetrics.descent;
                    }
                    cInfo.cursorHeight = cInfo.cursorAscent + cInfo.cursorDescent;
                    if (cInfo.cursorHeight < 0.001f) cInfo.cursorHeight = defaultMetrics.ascent + defaultMetrics.descent;
                    return cInfo;
                }
            }

            if (cInfo.lineIndex == -1 && !textBlock.lines.empty()) {
                cInfo.lineIndex = textBlock.lines.size() - 1;
                const auto& lastLine = textBlock.lines.back();
                cInfo.visualPosition.y = lastLine.lineBoxY + lastLine.baselineYInBox;
                float lineDrawStartX = 0.0f;
                if (textBlock.paragraphStyleUsed.alignment == HorizontalAlignment::RIGHT) lineDrawStartX = (textBlock.paragraphStyleUsed.wrapWidth > 0 ? textBlock.paragraphStyleUsed.wrapWidth : lastLine.lineWidth) - lastLine.lineWidth;
                else if (textBlock.paragraphStyleUsed.alignment == HorizontalAlignment::CENTER) lineDrawStartX = ((textBlock.paragraphStyleUsed.wrapWidth > 0 ? textBlock.paragraphStyleUsed.wrapWidth : lastLine.lineWidth) - lastLine.lineWidth) / 2.0f;
                bool isLastLineActuallyFirstInPara = (lastLine.sourceTextByteStartIndexInBlockText == 0) ||
                                                     (lastLine.sourceTextByteStartIndexInBlockText > 0 && !textBlock.sourceTextConcatenated.empty() && textBlock.sourceTextConcatenated[lastLine.sourceTextByteStartIndexInBlockText-1] == '\n');
                if (isLastLineActuallyFirstInPara) {
                    lineDrawStartX += textBlock.paragraphStyleUsed.firstLineIndent;
                }
                cInfo.visualPosition.x = lineDrawStartX + lastLine.lineWidth;
                cInfo.isAtLogicalLineEnd = true;
                cInfo.isTrailingEdge = true;
                cInfo.cursorAscent = (lastLine.maxContentAscent > 0.001f) ? lastLine.maxContentAscent : defaultMetrics.ascent;
                cInfo.cursorDescent = (lastLine.maxContentDescent > 0.001f) ? lastLine.maxContentDescent : defaultMetrics.descent;
                cInfo.cursorHeight = cInfo.cursorAscent + cInfo.cursorDescent;
                if (cInfo.cursorHeight < 0.001f) cInfo.cursorHeight = defaultMetrics.ascent + defaultMetrics.descent;

            } else if (cInfo.lineIndex == -1 && textBlock.lines.empty()){
                cInfo.lineIndex = 0;
                cInfo.visualPosition.x = textBlock.paragraphStyleUsed.firstLineIndent;
                cInfo.visualPosition.y = defaultMetrics.ascent;
                cInfo.cursorAscent = defaultMetrics.ascent;
                cInfo.cursorDescent = defaultMetrics.descent;
                cInfo.cursorHeight = defaultMetrics.ascent + defaultMetrics.descent;
                cInfo.isAtLogicalLineEnd = true;
                cInfo.isTrailingEdge = true;
            }
            return cInfo;
        }

        uint32_t GetByteOffsetFromVisualPosition(const TextBlock& textBlock, Vector2 positionInBlockLocalCoords, bool* isTrailingEdge, float* distanceToClosestEdge) const override {
            if (isTrailingEdge) *isTrailingEdge = false;
            if (distanceToClosestEdge) *distanceToClosestEdge = 1e9f;

            if (textBlock.lines.empty()) return 0;

            int targetLineIdx = 0;
            float minDistYToLineCenter = 1e9f;

            for (size_t i = 0; i < textBlock.lines.size(); ++i) {
                const auto& line = textBlock.lines[i];
                float lineCenterY = line.lineBoxY + line.lineBoxHeight / 2.0f;
                float distY = fabsf(positionInBlockLocalCoords.y - lineCenterY);
                if (positionInBlockLocalCoords.y >= line.lineBoxY && positionInBlockLocalCoords.y < line.lineBoxY + line.lineBoxHeight) {
                    targetLineIdx = i;
                    minDistYToLineCenter = -1.0f;
                    break;
                }
                if (minDistYToLineCenter < 0 || distY < minDistYToLineCenter) {
                    minDistYToLineCenter = distY;
                    targetLineIdx = i;
                }
            }

            const auto& line = textBlock.lines[targetLineIdx];
            uint32_t closestByteOffset = line.sourceTextByteStartIndexInBlockText;
            float minAbsXDist = 1e9f;

            float lineDrawStartX = 0.0f;
            if (textBlock.paragraphStyleUsed.alignment == HorizontalAlignment::RIGHT) {
                lineDrawStartX = (textBlock.paragraphStyleUsed.wrapWidth > 0 ? textBlock.paragraphStyleUsed.wrapWidth : line.lineWidth) - line.lineWidth;
            } else if (textBlock.paragraphStyleUsed.alignment == HorizontalAlignment::CENTER) {
                lineDrawStartX = ((textBlock.paragraphStyleUsed.wrapWidth > 0 ? textBlock.paragraphStyleUsed.wrapWidth : line.lineWidth) - line.lineWidth) / 2.0f;
            }
            bool isLineActuallyFirstInPara = (line.sourceTextByteStartIndexInBlockText == 0) ||
                                             (line.sourceTextByteStartIndexInBlockText > 0 && !textBlock.sourceTextConcatenated.empty() && textBlock.sourceTextConcatenated[line.sourceTextByteStartIndexInBlockText-1] == '\n');
            if (isLineActuallyFirstInPara) {
                lineDrawStartX += textBlock.paragraphStyleUsed.firstLineIndent;
            }

            if (positionInBlockLocalCoords.x < lineDrawStartX) {
                if(isTrailingEdge) *isTrailingEdge = false;
                if(distanceToClosestEdge) *distanceToClosestEdge = fabsf(positionInBlockLocalCoords.x - lineDrawStartX);
                return line.sourceTextByteStartIndexInBlockText;
            }

            for (size_t elIdx = 0; elIdx < line.numElementsInLine; ++elIdx) {
                if ((line.firstElementIndexInBlockElements + elIdx) >= textBlock.elements.size()) break;
                const auto& elementVariant = textBlock.elements[line.firstElementIndexInBlockElements + elIdx];

                uint32_t elSrcSpanIdx = 0;
                uint32_t elSrcByteOffsetInSpan = 0;
                uint16_t elSrcNumBytesInSpan = 0;
                float elLogicalXStartOnLine = 0;
                float elAdvanceX = 0;

                if (elementVariant.index() == 0) { // PositionedGlyph
                    const auto& g = std::get<PositionedGlyph>(elementVariant);
                    elSrcSpanIdx = g.sourceSpanIndex;
                    elSrcByteOffsetInSpan = g.sourceCharByteOffsetInSpan;
                    elSrcNumBytesInSpan = g.numSourceCharBytesInSpan;
                    elLogicalXStartOnLine = g.position.x;
                    elAdvanceX = g.xAdvance;
                } else if (elementVariant.index() == 1) { // PositionedImage
                    const auto& img = std::get<PositionedImage>(elementVariant);
                    elSrcSpanIdx = img.sourceSpanIndex;
                    elSrcByteOffsetInSpan = img.sourceCharByteOffsetInSpan;
                    elSrcNumBytesInSpan = img.numSourceCharBytesInSpan;
                    elLogicalXStartOnLine = img.position.x;
                    elAdvanceX = img.penAdvanceX;
                } else {
                    continue;
                }

                uint32_t currentElementByteStartInBlock = 0;
                for(uint32_t k=0; k < elSrcSpanIdx; ++k) {
                    if (textBlock.sourceSpansCopied[k].style.isImage && textBlock.sourceSpansCopied[k].text.empty()) currentElementByteStartInBlock += 3;
                    else currentElementByteStartInBlock += textBlock.sourceSpansCopied[k].text.length();
                }
                currentElementByteStartInBlock += elSrcByteOffsetInSpan;

                float visualElementStartX = lineDrawStartX + elLogicalXStartOnLine;
                float visualElementEndX = visualElementStartX + elAdvanceX;
                float visualElementMidX = visualElementStartX + elAdvanceX / 2.0f;

                if (positionInBlockLocalCoords.x < visualElementMidX) {
                    float dist = fabsf(positionInBlockLocalCoords.x - visualElementStartX);
                    if (dist < minAbsXDist) {
                        minAbsXDist = dist;
                        closestByteOffset = currentElementByteStartInBlock;
                        if (isTrailingEdge) *isTrailingEdge = false;
                    }
                } else {
                    float dist = fabsf(positionInBlockLocalCoords.x - visualElementEndX);
                    if (dist < minAbsXDist) {
                        minAbsXDist = dist;
                        closestByteOffset = currentElementByteStartInBlock + elSrcNumBytesInSpan;
                        if (isTrailingEdge) *isTrailingEdge = true;
                    }
                }
            }

            if (line.numElementsInLine > 0) {
                const auto& lastElementVariant = textBlock.elements[line.firstElementIndexInBlockElements + line.numElementsInLine -1];
                float lastElVisualEndX = 0;
                if(lastElementVariant.index() == 0) {
                    const auto& glyph = std::get<PositionedGlyph>(lastElementVariant);
                    lastElVisualEndX = lineDrawStartX + glyph.position.x + glyph.xAdvance;
                } else {
                    const auto& img = std::get<PositionedImage>(lastElementVariant);
                    lastElVisualEndX = lineDrawStartX + img.position.x + img.penAdvanceX;
                }
                if (positionInBlockLocalCoords.x >= lastElVisualEndX) {
                    float dist = fabsf(positionInBlockLocalCoords.x - lastElVisualEndX);
                    if (dist < minAbsXDist) {
                        minAbsXDist = dist;
                        closestByteOffset = line.sourceTextByteEndIndexInBlockText;
                        if (isTrailingEdge) *isTrailingEdge = true;
                    }
                }
            } else {
                if (positionInBlockLocalCoords.x > lineDrawStartX) {
                    if (isTrailingEdge) *isTrailingEdge = true;
                } else {
                    if (isTrailingEdge) *isTrailingEdge = false;
                }
                minAbsXDist = fabsf(positionInBlockLocalCoords.x - lineDrawStartX);
                closestByteOffset = line.sourceTextByteStartIndexInBlockText;
            }

            if(distanceToClosestEdge) *distanceToClosestEdge = minAbsXDist;
            return std::min(closestByteOffset, (uint32_t)textBlock.sourceTextConcatenated.length());
        }

    }; // class STBTextEngineImpl

} // anonymous namespace

// --- Global Factory Function ---
std::unique_ptr<ITextEngine> CreateTextEngine() {
   return std::make_unique<STBTextEngineImpl>();
}