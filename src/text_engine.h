// text_engine.h (版本 2025-05-22 - 核心增强)
#ifndef TEXT_ENGINE_H
#define TEXT_ENGINE_H

#include "raylib.h" // 依赖 Raylib 的基本类型 (Vector2, Rectangle, Color, Texture2D, Matrix)
#include <vector>
#include <string>
#include <cstdint> // For uint8_t, uint32_t etc.
#include <memory>  // For std::unique_ptr
#include <variant> // For PositionedElementVariant (C++17)

// --- 配置与常量 ---
using FontId = int;
constexpr FontId INVALID_FONT_ID = -1;

// --- 枚举 ---

/**
 * @brief 定义字体的基本样式。可以组合使用。
 */
enum class FontStyle : uint8_t {
    Normal = 0,
    Bold = 1 << 0,
    Italic = 1 << 1,
};

// 位操作符重载，用于组合 FontStyle
inline FontStyle operator|(FontStyle a, FontStyle b) {
    return static_cast<FontStyle>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
inline FontStyle operator&(FontStyle a, FontStyle b) {
    return static_cast<FontStyle>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}
inline bool HasStyle(FontStyle combined, FontStyle single) {
    return (static_cast<uint8_t>(combined) & static_cast<uint8_t>(single)) != 0;
}

/**
 * @brief 文本引擎后端类型，用于创建特定实现的引擎实例。
 */
enum class TextEngineBackend {
    STB_SDF,
    FREETYPE_HARFBUZZ_ICU
};

/**
 * @brief 文本的基准书写方向。
 */
enum class TextDirection {
    AUTO_DETECT_FROM_TEXT,
    LTR,
    RTL
};

/**
 * @brief 断行策略。
 */
enum class LineBreakStrategy {
    SIMPLE_BY_WIDTH,
    ICU_WORD_BOUNDARIES,
    ICU_CHARACTER_BOUNDARIES
};

/**
 * @brief 制表符对齐方式。
 */
enum class TabAlignment {
    LEFT,
    RIGHT,
    CENTER,
    DECIMAL
};

/**
 * @brief 字形图集的内容类型提示。
 */
enum class GlyphAtlasType {
    ALPHA_ONLY_BITMAP,
    SDF_BITMAP
};


// --- 效果参数结构体 ---
struct EffectParameters {
    bool enabled = false;
    Color color = {0, 0, 0, 255}; // BLACK
};

struct OutlineEffectParams : public EffectParameters {
    float width = 0.05f;
};

struct GlowEffectParams : public EffectParameters {
    float range = 0.15f;
    float intensity = 0.7f;
};

struct ShadowEffectParams : public EffectParameters {
    Vector2 offset = {2.0f, 2.0f};
    float sdfSpread = 0.1f;
};

struct InnerEffectParams : public EffectParameters {
    float range = 0.05f;
    bool isShadow = true;
};

// --- 文本填充与制表符定义 ---
enum class FillType { SOLID_COLOR, LINEAR_GRADIENT };
struct GradientStop { Color color; float position; };

struct FillStyle {
    FillType type = FillType::SOLID_COLOR;
    Color solidColor = {0, 0, 0, 255}; // BLACK
    Vector2 linearGradientStart = {0.0f, 0.0f};
    Vector2 linearGradientEnd = {0.0f, 1.0f};
    std::vector<GradientStop> gradientStops;
};

struct TabStop {
    float position;
    TabAlignment alignment = TabAlignment::LEFT;
};

// --- 字符级/文本段级样式集 ---
struct CharacterStyle {
    FontId fontId = INVALID_FONT_ID;
    float fontSize = 16.0f;
    FillStyle fill;
    FontStyle basicStyle = FontStyle::Normal;
    std::string scriptTag;
    std::string languageTag;

    OutlineEffectParams outline;
    GlowEffectParams glow;
    ShadowEffectParams shadow;
    InnerEffectParams innerEffect;

    bool isImage = false;
    struct InlineImageParams {
        Texture2D texture = {0};
        float displayWidth = 0.0f;
        float displayHeight = 0.0f;
        enum class VAlign {
            BASELINE, MIDDLE_OF_TEXT, TEXT_TOP, TEXT_BOTTOM,
            LINE_TOP, LINE_BOTTOM // LINE_TOP/BOTTOM are resolved after line box height is known
        } vAlign = VAlign::BASELINE;
    } imageParams;

    CharacterStyle() {
        fill.solidColor = {0, 0, 0, 255}; // BLACK
    }
};

// --- 文本段 (Text Span / Run) ---
struct TextSpan {
    std::string text; // UTF-8 encoded string
    CharacterStyle style;
    void* userData = nullptr;
};

// --- 段落属性 ---
enum class HorizontalAlignment { LEFT, CENTER, RIGHT, JUSTIFY };
enum class LineHeightType : uint8_t {
    NORMAL_SCALED_FONT_METRICS,
    FACTOR_SCALED_FONT_SIZE,
    ABSOLUTE_POINTS,
    CONTENT_SCALED
};

struct ParagraphStyle {
    HorizontalAlignment alignment = HorizontalAlignment::LEFT;
    LineHeightType lineHeightType = LineHeightType::NORMAL_SCALED_FONT_METRICS;
    float lineHeightValue = 1.2f;
    float firstLineIndent = 0.0f;
    float wrapWidth = 0.0f;

    TextDirection baseDirection = TextDirection::AUTO_DETECT_FROM_TEXT;
    LineBreakStrategy lineBreakStrategy = LineBreakStrategy::SIMPLE_BY_WIDTH;

    std::vector<TabStop> customTabStops;
    float defaultTabWidthFactor = 4.0f;

    CharacterStyle defaultCharacterStyle;

    ParagraphStyle() {
        defaultCharacterStyle.fontId = INVALID_FONT_ID;
        defaultCharacterStyle.fontSize = 16.0f;
        defaultCharacterStyle.fill.solidColor = {0, 0, 0, 255}; // BLACK
    }
};


// --- 布局结果数据结构 ---

struct GlyphRenderInfo {
    Texture2D atlasTexture = {0};
    Rectangle atlasRect = {0,0,0,0};
    Vector2 drawOffset = {0,0};
    bool isSDF = false;
};

struct PositionedGlyph {
    uint32_t glyphId = 0;
    FontId sourceFont = INVALID_FONT_ID;      // **实际用于渲染此字形的字体ID (可能是主字体或回退字体)**
    // FontId requestedFontId = INVALID_FONT_ID; // (可选) 原始请求的字体ID，如果需要区分
    float sourceSize = 0.0f;

    Vector2 position = {0,0};
    float xAdvance = 0.0f;
    float yAdvance = 0.0f;
    float xOffset = 0.0f;
    float yOffset = 0.0f;

    GlyphRenderInfo renderInfo;

    uint32_t sourceSpanIndex = 0;
    uint32_t sourceCharByteOffsetInSpan = 0;
    uint16_t numSourceCharBytesInSpan = 0;

    CharacterStyle appliedStyle;

    float ascent = 0.0f;
    float descent = 0.0f;
    float visualLeft = 0.0f;
    float visualRight = 0.0f;

    enum class BiDiDirectionHint { UNSPECIFIED, LTR, RTL };
    BiDiDirectionHint visualRunDirectionHint = BiDiDirectionHint::UNSPECIFIED;
};

struct PositionedImage {
    Vector2 position = {0,0};
    float width = 0.0f;
    float height = 0.0f;
    float penAdvanceX = 0.0f;
    CharacterStyle::InlineImageParams imageParams;
    uint32_t sourceSpanIndex = 0;
    uint32_t sourceCharByteOffsetInSpan = 0;
    uint16_t numSourceCharBytesInSpan = 0;
    float ascent = 0.0f;
    float descent = 0.0f;
};

#if __cplusplus >= 201703L
using PositionedElementVariant = std::variant<PositionedGlyph, PositionedImage>;
#else
enum class PositionedElementType { GLYPH, IMAGE };
struct PositionedElementFallback {
    PositionedElementType type;
    union { PositionedGlyph glyph; PositionedImage image; };
    PositionedElementFallback() : type(PositionedElementType::GLYPH), glyph{} {}
};
using PositionedElementVariant = PositionedElementFallback;
#endif


struct VisualRun {
    size_t firstElementIndexInLineElements = 0;
    size_t numElementsInRun = 0;
    PositionedGlyph::BiDiDirectionHint direction = PositionedGlyph::BiDiDirectionHint::UNSPECIFIED;
    std::string scriptTagUsed;
    std::string languageTagUsed;
    FontId runFont = INVALID_FONT_ID;
    float runFontSize = 0.0f;
    float runVisualAdvanceX = 0.0f;
    int32_t logicalStartInOriginalSource = 0;
    int32_t logicalLengthInOriginalSource = 0;
};


struct LineLayoutInfo {
    size_t firstElementIndexInBlockElements = 0;
    size_t numElementsInLine = 0;
    float lineBoxY = 0.0f;
    float baselineYInBox = 0.0f;
    float lineWidth = 0.0f;
    float lineBoxHeight = 0.0f;
    float maxContentAscent = 0.0f;
    float maxContentDescent = 0.0f;
    uint32_t sourceTextByteStartIndexInBlockText = 0;
    uint32_t sourceTextByteEndIndexInBlockText = 0; // Exclusive

    std::vector<VisualRun> visualRuns;

    // **新增BiDi映射信息 (针对Freetype/HarfBuzz/ICU后端)**
    std::vector<int32_t> visualToLogicalMap; // Visual U16 index in line -> Logical U16 index in line
    std::vector<int32_t> logicalToVisualMap; // Logical U16 index in line -> Visual U16 index in line
    // (可选) 存储此行对应的UTF-16文本段，或其在段落UTF-16文本中的起止，以便解释映射。
    // int32_t lineLogicalStartInParagraphU16;
    // int32_t lineLogicalLengthU16;

    LineLayoutInfo() = default;
};

struct TextBlock {
    std::vector<PositionedElementVariant> elements;
    std::vector<LineLayoutInfo> lines;
    Rectangle overallBounds = {0,0,0,0};
    ParagraphStyle paragraphStyleUsed;
    std::string sourceTextConcatenated; // UTF-8
    std::vector<TextSpan> sourceSpansCopied;

    TextBlock() = default;
};

struct CursorLocationInfo {
    Vector2 visualPosition = {0,0};
    float cursorHeight = 0.0f;
    float cursorAscent = 0.0f;
    float cursorDescent = 0.0f;
    uint32_t byteOffset = 0;
    int lineIndex = -1;
    bool isAtLogicalLineEnd = false;
    bool isTrailingEdge = false;

    CursorLocationInfo() = default;
};


// --- 文本引擎接口 ---

class ITextEngine {
public:
    virtual ~ITextEngine() = default;

    // --- Font Management ---
    virtual FontId LoadFont(const char* filePath, int faceIndex = 0) = 0;
    virtual void UnloadFont(FontId fontId) = 0;
    virtual bool IsFontValid(FontId fontId) const = 0;
    virtual FontId GetDefaultFont() const = 0;
    virtual void SetDefaultFont(FontId fontId) = 0;

    /**
     * @brief 为指定的主字体设置字体回退链。
     * 当主字体无法渲染某个字符时，引擎将按顺序尝试回退链中的字体。
     * @param primaryFont 主字体的ID。
     * @param fallbackChain 回退字体的ID列表，按尝试顺序排列。
     */
    virtual void SetFontFallbackChain(FontId primaryFont, const std::vector<FontId>& fallbackChain) = 0;

    /**
     * @brief 检查指定字体（包括其回退链，如果checkFallback为true）是否能渲染给定的Unicode码点。
     * @param fontId 要检查的字体ID (通常是span请求的字体)。
     * @param codepoint Unicode码点。
     * @param checkFallback 是否同时检查此字体的回退链。
     * @return 如果可以渲染则为true，否则为false。
     */
    virtual bool IsCodepointAvailable(FontId fontId, uint32_t codepoint, bool checkFallback = true) const = 0;


    struct FontProperties {
        int unitsPerEm = 1000;
        bool hasTypoMetrics = false;
        int typoAscender = 0, typoDescender = 0, typoLineGap = 0;
        int hheaAscender = 0, hheaDescender = 0, hheaLineGap = 0;
        float capHeight = 0.0f;
        float xHeight = 0.0f;
        float underlinePosition = 0.0f;
        float underlineThickness = 0.0f;
        float strikeoutPosition = 0.0f;
        float strikeoutThickness = 0.0f;
        FontProperties() = default;
    };
    virtual FontProperties GetFontProperties(FontId fontId) const = 0;

    struct ScaledFontMetrics {
        float scale = 1.0f;
        float ascent = 0.0f;
        float descent = 0.0f;
        float lineGap = 0.0f;
        float recommendedLineHeight = 0.0f;
        float capHeight = 0.0f;
        float xHeight = 0.0f;
        float underlinePosition = 0.0f;
        float underlineThickness = 0.0f;
        float strikeoutPosition = 0.0f;
        float strikeoutThickness = 0.0f;
        ScaledFontMetrics() = default;
    };
    virtual ScaledFontMetrics GetScaledFontMetrics(FontId fontId, float fontSize) const = 0;

    // --- Text Layout ---
    virtual TextBlock LayoutStyledText(const std::vector<TextSpan>& spans, const ParagraphStyle& paragraphStyle) = 0;

    /**
     * @brief 获取给定文本块中指定字节范围的视觉边界矩形列表。
     * 对于跨行的范围，会返回多个矩形。矩形坐标相对于TextBlock的原点。
     * @param textBlock 已布局的文本块。
     * @param byteOffsetStart 范围的起始字节偏移 (UTF-8, 在textBlock.sourceTextConcatenated中)。
     * @param byteOffsetEnd 范围的结束字节偏移 (UTF-8, exclusive)。
     * @return 一系列描述选区视觉边界的矩形。
     */
    virtual std::vector<Rectangle> GetTextRangeBounds(const TextBlock& textBlock, uint32_t byteOffsetStart, uint32_t byteOffsetEnd) const = 0;


    // --- Text Drawing ---
    virtual void DrawTextBlock(const TextBlock& textBlock, const Matrix& transform, Color globalTint = WHITE, const Rectangle* clipRect = nullptr) = 0;

    /**
     * @brief 绘制给定文本块中指定字节范围的选区高亮。
     * @param textBlock 已布局的文本块。
     * @param selectionStartByte 选区起始字节偏移 (UTF-8)。
     * @param selectionEndByte 选区结束字节偏移 (UTF-8, exclusive)。
     * @param highlightColor 高亮颜色。
     * @param worldTransform 应用于文本块的世界变换矩阵，高亮将在此变换下绘制。
     */
    virtual void DrawTextSelectionHighlight(const TextBlock& textBlock,
                                            uint32_t selectionStartByte,
                                            uint32_t selectionEndByte,
                                            Color highlightColor,
                                            const Matrix& worldTransform
    ) const = 0;

    // --- Glyph Cache Management ---
    virtual void ClearGlyphCache() = 0;
    virtual void SetGlyphAtlasOptions(size_t maxGlyphsEstimate, int atlasWidth = 1024, int atlasHeight = 1024, GlyphAtlasType typeHint = GlyphAtlasType::ALPHA_ONLY_BITMAP) = 0;
    virtual Texture2D GetAtlasTextureForDebug(int atlasIndex = 0) const = 0;

    // --- Cursor and Hit-Testing ---
    virtual CursorLocationInfo GetCursorInfoFromByteOffset(const TextBlock& textBlock, uint32_t byteOffsetInConcatenatedText, bool preferLeadingEdge = true) const = 0;
    virtual uint32_t GetByteOffsetFromVisualPosition(const TextBlock& textBlock, Vector2 positionInBlockLocalCoords, bool* isTrailingEdge = nullptr, float* distanceToClosestEdge = nullptr) const = 0;
};

// --- Engine Factory ---
std::unique_ptr<ITextEngine> CreateTextEngine(); // 实现将位于 .cpp 文件中

// --- UTF-8 Helper ---
inline uint32_t GetNextCodepointFromUTF8(const char **textUtf8, int *byteCount) {
    const unsigned char *s = reinterpret_cast<const unsigned char *>(*textUtf8);
    if (s[0] == 0) { *byteCount = 0; return 0; }
    unsigned int codepoint = 0; int count = 0;
    if (s[0] < 0x80) { codepoint = s[0]; count = 1;
    } else if ((s[0] & 0xE0) == 0xC0) { if ((s[1] & 0xC0) == 0x80) { codepoint = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F); count = 2; }}
    else if ((s[0] & 0xF0) == 0xE0) { if (((s[1] & 0xC0) == 0x80) && ((s[2] & 0xC0) == 0x80)) { codepoint = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); count = 3; }}
    else if ((s[0] & 0xF8) == 0xF0) { if (((s[1] & 0xC0) == 0x80) && ((s[2] & 0xC0) == 0x80) && ((s[3] & 0xC0) == 0x80)) { codepoint = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F); count = 4; }}
    if (count > 0) { if ((codepoint < 0x80 && count > 1) || (codepoint < 0x800 && count > 2) || (codepoint < 0x10000 && count > 3) || (codepoint > 0x10FFFF) || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) { count = 0; }}
    if (count == 0) { *byteCount = (s[0] != 0); *textUtf8 = reinterpret_cast<const char *>(s + (*byteCount)); return 0xFFFD; }
    *textUtf8 = reinterpret_cast<const char *>(s + count); *byteCount = count; return codepoint;
}

#endif // TEXT_ENGINE_H