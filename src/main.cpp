// main.cpp (基于重构后的 text_engine.h 和 RaylibSDFText.cpp - 修正版)
#include "raylib.h"
#include "raymath.h"     // 用于矩阵操作, Vector2 等
#include "text_engine.h" // 我们的文本引擎头文件
#include <string>
#include <vector>
#include <algorithm> // 用于 std::min/max
#include <cmath>     // 用于 sinf, fabsf

// 全局变量，用于动态调整SDF平滑度 (与 RaylibSDFText.cpp 中的 extern 对应)
float dynamicSmoothnessAdd = 0.0f;

//------------------------------------------------------------------------------------
// 程序主入口点
//------------------------------------------------------------------------------------
int main(void) {
    // 初始化
    //--------------------------------------------------------------------------------------
    const int screenWidth = 1280;
    const int screenHeight = 720;

    InitWindow(screenWidth, screenHeight, "文本引擎 - HTML 对齐测试");

    // 使用工厂函数创建 STB 后端引擎
    std::unique_ptr<ITextEngine> textEngine = CreateTextEngine();
    if (!textEngine) {
        TraceLog(LOG_FATAL, "创建文本引擎失败!");
        CloseWindow();
        return -1;
    }

    const char* fontPathChineseMain = "resources/AlibabaPuHuiTi-3-55-Regular.ttf";   // 主中文字体 (也应包含英文)
    const char* fontPathArabic = "resources/NotoNaskhArabic-Regular.ttf"; // 阿拉伯文字体
//    const char* fontPathArabic = "resources/arial.ttf"; // 阿拉伯文字体
// const char* fontPathEnglishSpecific = "resources/MyPreferredLatinFont.ttf"; // (可选) 特定英文字体

    FontId chineseMainFont = textEngine->LoadFont(fontPathChineseMain);
    FontId arabicFont = textEngine->LoadFont(fontPathArabic);
// FontId englishSpecificFont = textEngine->LoadFont(fontPathEnglishSpecific); // (可选)

    if (chineseMainFont == INVALID_FONT_ID) { //
        TraceLog(LOG_FATAL, "加载主中文字体 '%s' 失败。", fontPathChineseMain);
        textEngine.reset(); CloseWindow(); return -1;
    }
    if (arabicFont == INVALID_FONT_ID) { //
        TraceLog(LOG_FATAL, "加载阿拉伯字体 '%s' 失败。", fontPathArabic);
        textEngine.reset(); CloseWindow(); return -1;
    }


    // 将主中文字体设为默认字体
    textEngine->SetDefaultFont(chineseMainFont); //

// --- 设置字体回退链 ---
    if (textEngine->IsFontValid(chineseMainFont) && textEngine->IsFontValid(arabicFont)) { //
        std::vector<FontId> fallbacksForChinese; //
        fallbacksForChinese.push_back(arabicFont);
        // 如果有特定的英文字体，并且希望它优先于阿拉伯字体中的英文字符 (如果阿拉伯字体也包含拉丁字符)
        // if (textEngine->IsFontValid(englishSpecificFont)) {
        //     fallbacksForChinese.push_back(englishSpecificFont);
        // }
        textEngine->SetFontFallbackChain(chineseMainFont, fallbacksForChinese); //
    }

    const char* imagePath = "resources/raylib_logo.png"; // Placeholder for img tags


    if (chineseMainFont == INVALID_FONT_ID) {
        TraceLog(LOG_FATAL, "加载默认字体 '%s' 失败。请确保字体文件存在。", chineseMainFont);
        textEngine.reset(); CloseWindow(); return -1;
    }


    // 加载内联图片
    Texture2D inlineTestImage = {0};
    if (FileExists(imagePath)){
        inlineTestImage = LoadTexture(imagePath);
        if (inlineTestImage.id == 0) {
            TraceLog(LOG_WARNING, "加载内联图片失败: %s.", imagePath);
        } else {
            SetTextureFilter(inlineTestImage, TEXTURE_FILTER_BILINEAR);
        }
    } else {
        TraceLog(LOG_WARNING, "内联图片文件未找到: %s", imagePath);
    }
    //--------------------------------------------------------------------------------------

    std::vector<TextSpan> spans;
    ParagraphStyle paraStyle;

    // --- 配置段落样式 ---
    paraStyle.wrapWidth = 200.0f;
    paraStyle.alignment = HorizontalAlignment::LEFT;
    paraStyle.baseDirection = TextDirection::AUTO_DETECT_FROM_TEXT;
    paraStyle.lineBreakStrategy = LineBreakStrategy::SIMPLE_BY_WIDTH;

    // 设置默认字符样式
    paraStyle.defaultCharacterStyle.fontId = chineseMainFont;
    paraStyle.defaultCharacterStyle.fontSize = 18.0f; // Default font size for paragraph text
    paraStyle.defaultCharacterStyle.fill.solidColor = DARKGRAY;
    paraStyle.defaultCharacterStyle.scriptTag = "Latn";
    paraStyle.defaultCharacterStyle.languageTag = "en";

    // 行高设置 (matches line-height: 1.5 from HTML)
    paraStyle.lineHeightType = LineHeightType::NORMAL_SCALED_FONT_METRICS;
    paraStyle.lineHeightValue = 1.5f;
    paraStyle.firstLineIndent = 0.0f; // No indent for these test cases
    paraStyle.defaultTabWidthFactor = 4.0f;

    // Base style for most text
    CharacterStyle baseStyle = paraStyle.defaultCharacterStyle;

    // Style for notes (smaller, gray)
    CharacterStyle noteStyle = baseStyle;
    noteStyle.fontSize = 13.0f;
    noteStyle.fill.solidColor = Color{100, 100, 100, 255}; // Darker gray for notes

    CharacterStyle arabicStyle = baseStyle; // 从基础样式开始，或从 paraStyle.defaultCharacterStyle 开始
    arabicStyle.fontId = arabicFont;        // 确保使用阿拉伯文字体
    arabicStyle.scriptTag = "Arab";         // **关键：设置正确的脚本标签**
    arabicStyle.languageTag = "ar";           // **关键：设置正确的语言标签**

    // --- 创建文本段 (Matching index.html) ---

    // <h1>🧪 混合文字内嵌图片：对齐测试</h1>
//    TextSpan h1Span;
//    h1Span.text = "🧪 混合文字内嵌图片：对齐测试\n\n";
//    h1Span.style = baseStyle;
//    h1Span.style.fontSize = 32.0f; // Larger font for H1
//    h1Span.style.fill.solidColor = BLACK;
//    spans.push_back(h1Span);

    // Helper function to add image spans
    auto addImageSpan = [&](CharacterStyle::InlineImageParams::VAlign vAlign, float width, float height) {
        TextSpan imgSpan;
        if (inlineTestImage.id > 0) {
            imgSpan.style.isImage = true;
            imgSpan.style.imageParams.texture = inlineTestImage;
            imgSpan.style.imageParams.displayWidth = width;
            imgSpan.style.imageParams.displayHeight = height;
            imgSpan.style.imageParams.vAlign = vAlign;
        } else { // Fallback if image not loaded
            imgSpan.text = "[IMG]";
            imgSpan.style = baseStyle;
        }
        spans.push_back(imgSpan);
    };

    auto addTextSpan = [&](const std::string& text, CharacterStyle style) {
        TextSpan span;
        span.text = text;
        span.style = style;
        spans.push_back(span);
    };

//    addTextSpan("1. 普通文字 ", baseStyle);
//    addImageSpan(CharacterStyle::InlineImageParams::VAlign::BASELINE, 30, 30);
//    addTextSpan(" baseline 图片\n", baseStyle);
//    addTextSpan("   图片默认 baseline，对齐文字基线\n\n", noteStyle);
//
//    addTextSpan("2. 普通文字 ", baseStyle);
//    addImageSpan(CharacterStyle::InlineImageParams::VAlign::MIDDLE_OF_TEXT, 30, 30);
//    addTextSpan(" middle 图片\n", baseStyle);
//    addTextSpan("   图片垂直居中对齐文字中线\n\n", noteStyle);
//
//    CharacterStyle case3Style = baseStyle; case3Style.fontSize = 12.0f;
//    addTextSpan("3. 小字号 ", case3Style);
//    addImageSpan(CharacterStyle::InlineImageParams::VAlign::BASELINE, 30, 30);
//    addTextSpan(" baseline\n", case3Style);
//    addTextSpan("   小文字 + baseline 图片\n\n", noteStyle);
//
//    CharacterStyle case4Style = baseStyle; case4Style.fontSize = 28.0f;
//    addTextSpan("4. 大字号 ", case4Style);
//    addImageSpan(CharacterStyle::InlineImageParams::VAlign::BASELINE, 30, 30);
//    addTextSpan(" baseline\n", case4Style);
//    addTextSpan("   大文字 + baseline 图片\n\n", noteStyle);
//
//    addTextSpan("5. 混合字号：", baseStyle);
//    CharacterStyle case5SmallStyle = baseStyle; case5SmallStyle.fontSize = 10.0f;
//    addTextSpan("小", case5SmallStyle);
//    CharacterStyle case5LargeStyle = baseStyle; case5LargeStyle.fontSize = 30.0f;
//    addTextSpan("大", case5LargeStyle);
//    addTextSpan(" ", baseStyle); // Space before image
//    addImageSpan(CharacterStyle::InlineImageParams::VAlign::MIDDLE_OF_TEXT, 30, 30);
//    addTextSpan("\n", baseStyle);
//    addTextSpan("   大小混合 + 图片中线对齐\n\n", noteStyle);
//
//    addTextSpan("6. 文前", baseStyle);
//    addImageSpan(CharacterStyle::InlineImageParams::VAlign::MIDDLE_OF_TEXT, 30, 30);
//    addTextSpan("文后\n", baseStyle);
//    addTextSpan("   图片嵌入两字中间，对比位置\n\n", noteStyle);

//    addTextSpan("7. 第一行文字 ", baseStyle);
//    addImageSpan(CharacterStyle::InlineImageParams::VAlign::MIDDLE_OF_TEXT, 30, 30);
//    addTextSpan("\n", baseStyle); // Simulating <br>
//    addTextSpan("   第二行文字:阿拉伯语", baseStyle); // 中文和拉丁文部分

    addTextSpan("第二行文字:阿拉伯语\n", baseStyle); // 中文和拉丁文部分
    TextSpan arabicWordSpan; //
    arabicStyle.fontSize =  32.0f;
    arabicWordSpan.text = "طويل"; //
    arabicWordSpan.style = arabicStyle; // **使用专门的阿拉伯文样式**
    spans.push_back(arabicWordSpan); //

//    addTextSpan("' \n", baseStyle); // 剩余的拉丁文和换行符
//    addTextSpan("   图片后换行，观察是否撑高第一行\n\n", noteStyle);
//
//    addTextSpan("8. 图片 ", baseStyle);
//    addImageSpan(CharacterStyle::InlineImageParams::VAlign::TEXT_TOP, 30, 30);
//    addTextSpan(" text-top 对齐\n", baseStyle);
//    addTextSpan("   图片顶部对齐文字顶部（不一定是字体盒的顶部）\n\n", noteStyle);
//
//    addTextSpan("9. 图片 ", baseStyle);
//    addImageSpan(CharacterStyle::InlineImageParams::VAlign::TEXT_BOTTOM, 30, 30);
//    addTextSpan(" text-bottom 对齐\n", baseStyle);
//    addTextSpan("   图片底部对齐文字底部\n\n", noteStyle);
//
//    addTextSpan("10. 图 ", baseStyle);
//    addImageSpan(CharacterStyle::InlineImageParams::VAlign::BASELINE, 30, 30);
//    addTextSpan(" 图 ", baseStyle);
//    addImageSpan(CharacterStyle::InlineImageParams::VAlign::BASELINE, 30, 30);
//    addTextSpan(" 图\n", baseStyle);
//    addTextSpan("    多图 inline 排列，观察是否 baseline 统一\n\n", noteStyle);
//
//    addTextSpan("11. 图1 ", baseStyle);
//    addImageSpan(CharacterStyle::InlineImageParams::VAlign::BASELINE, 30, 30);
//    addTextSpan(" 图2 ", baseStyle);
//    addImageSpan(CharacterStyle::InlineImageParams::VAlign::BASELINE, 60, 60);
//    addTextSpan(" baseline 对齐\n", baseStyle);
//    addTextSpan("    小图 + 大图 baseline 对齐，观察基线是否一致，行高是否被撑大\n\n", noteStyle);
//
//    addTextSpan("12. 图1 ", baseStyle);
//    addImageSpan(CharacterStyle::InlineImageParams::VAlign::MIDDLE_OF_TEXT, 30, 30);
//    addTextSpan(" 图2 ", baseStyle);
//    addImageSpan(CharacterStyle::InlineImageParams::VAlign::MIDDLE_OF_TEXT, 60, 60);
//    addTextSpan(" middle 对齐\n", baseStyle);
//    addTextSpan("    观察图像是否都居中对齐文字中线，尤其大图是否下沉\n\n", noteStyle);
//
//    // // HTML "top" usually means top of the line box. Using TEXT_TOP as the closest equivalent.
//    addTextSpan("13. 图1 ", baseStyle);
//    addImageSpan(CharacterStyle::InlineImageParams::VAlign::TEXT_TOP, 30, 30);
//    addTextSpan(" 图2 ", baseStyle);
//    addImageSpan(CharacterStyle::InlineImageParams::VAlign::TEXT_TOP, 60, 60);
//    addTextSpan(" top 对齐\n", baseStyle);
//    addTextSpan("    顶部对齐，注意两图是否平头 (using TEXT_TOP)\n\n", noteStyle);
//
//    addTextSpan("14. 图1 ", baseStyle);
//    addImageSpan(CharacterStyle::InlineImageParams::VAlign::BASELINE, 30, 30);
//    addTextSpan(" 图2 ", baseStyle);
//    addImageSpan(CharacterStyle::InlineImageParams::VAlign::MIDDLE_OF_TEXT, 60, 60);
//    addTextSpan(" 混合对齐\n", baseStyle);
//    addTextSpan("    不同图片使用不同对齐策略，观察对排版的影响\n\n", noteStyle);
//
//    addTextSpan("15. 图1 ", baseStyle);
//    addImageSpan(CharacterStyle::InlineImageParams::VAlign::MIDDLE_OF_TEXT, 30, 30);
//    CharacterStyle case15TextStyle = baseStyle; case15TextStyle.fontSize = 18.0f; // Explicitly 18px for "中间文字"
//    addTextSpan(" 中间文字 ", case15TextStyle);
//    addImageSpan(CharacterStyle::InlineImageParams::VAlign::MIDDLE_OF_TEXT, 60, 60);
//    addTextSpan("\n", baseStyle);
//    addTextSpan("    文字夹在两张图之间，观察整行对齐基准变化\n\n", noteStyle);
//

    // --- End of HTML content mapping ---


    TextBlock currentTextBlock;
    CursorLocationInfo cursorInfo;
    bool needsRelayout = true;
    uint32_t textEditCursorBytePosition = 0;

    // Calculate initial cursor position to be at the end of the text
    for(const auto& s : spans) {
        if (s.style.isImage && s.text.empty()) { // Images are represented by a placeholder in text
            textEditCursorBytePosition += 3; // Assuming U+FFFC (Object Replacement Character) or similar
        } else {
            textEditCursorBytePosition += s.text.length();
        }
    }

    Vector2 textBlockScreenPosition = { 50, 60 };
    float textBlockRotation = 0.0f;
    float textBlockScale = 1.0f;
    Vector2 textBlockTransformOrigin = { 0, 0 };

    float elapsedTime = 0.0f;
    float blinkTimer = 0.0f;
    bool showCursor = true;
    const float blinkInterval = 0.53f;
    bool animateScale = false;
    bool showDebugAtlas = false;

    SetTargetFPS(60);

    // 主游戏循环
    while (!WindowShouldClose()) {
        elapsedTime += GetFrameTime();
        blinkTimer += GetFrameTime();
        if (blinkTimer >= blinkInterval) { blinkTimer = 0.0f; showCursor = !showCursor; }
        if (animateScale) textBlockScale = 1.0f + 0.15f * sinf(elapsedTime * 3.0f); else textBlockScale = 1.0f;

        // --- 简单文本编辑逻辑 ---
        int charCodePoint = GetCharPressed();
        while (charCodePoint > 0) {
            uint32_t currentGlobalOffset = 0;
            int targetSpanIdx = -1;
            uint32_t relativeInsertOffsetInSpanBytes = 0;

            if (spans.empty()) {
                TextSpan newSpan;
                newSpan.style = paraStyle.defaultCharacterStyle;
                spans.push_back(newSpan);
                targetSpanIdx = 0;
                relativeInsertOffsetInSpanBytes = 0;
            } else {
                for (size_t i = 0; i < spans.size(); ++i) {
                    const auto& s = spans[i];
                    size_t spanByteLength = s.style.isImage && s.text.empty() ? 3 : s.text.length();
                    if (textEditCursorBytePosition >= currentGlobalOffset && textEditCursorBytePosition <= currentGlobalOffset + spanByteLength) {
                        if (s.style.isImage) {
                            // Prefer inserting into adjacent text span or create new one
                            if (textEditCursorBytePosition == currentGlobalOffset && i > 0 && !spans[i-1].style.isImage) {
                                targetSpanIdx = i - 1;
                                relativeInsertOffsetInSpanBytes = spans[i-1].text.length();
                            } else if (textEditCursorBytePosition == currentGlobalOffset + spanByteLength && i < spans.size() -1 && !spans[i+1].style.isImage){
                                targetSpanIdx = i + 1;
                                relativeInsertOffsetInSpanBytes = 0;
                            }
                            else { // Insert new span for text
                                TextSpan newCharSpan; newCharSpan.style = paraStyle.defaultCharacterStyle;
                                spans.insert(spans.begin() + i + (textEditCursorBytePosition > currentGlobalOffset ? 1:0), newCharSpan);
                                targetSpanIdx = i + (textEditCursorBytePosition > currentGlobalOffset ? 1:0);
                                if(targetSpanIdx < 0) targetSpanIdx = 0;
                                if(targetSpanIdx >= (int)spans.size()) targetSpanIdx = spans.size() -1;
                                relativeInsertOffsetInSpanBytes = 0;
                            }
                        } else { // It's a text span
                            targetSpanIdx = i;
                            relativeInsertOffsetInSpanBytes = textEditCursorBytePosition - currentGlobalOffset;
                        }
                        break;
                    }
                    currentGlobalOffset += spanByteLength;
                }
                if (targetSpanIdx == -1 && textEditCursorBytePosition >= currentGlobalOffset) {
                    // Cursor is at the very end of all spans
                    if (!spans.empty() && !spans.back().style.isImage) {
                        targetSpanIdx = spans.size() - 1;
                        relativeInsertOffsetInSpanBytes = spans.back().text.length();
                    } else { // Last span is image or spans is empty (though handled), create new text span
                        TextSpan newSpan; newSpan.style = paraStyle.defaultCharacterStyle;
                        spans.push_back(newSpan);
                        targetSpanIdx = spans.size() - 1;
                        relativeInsertOffsetInSpanBytes = 0;
                    }
                }
            }

            if (targetSpanIdx != -1 && targetSpanIdx < (int)spans.size() && !spans[targetSpanIdx].style.isImage) {
                int utf8Len = 0;
                const char* utf8Bytes = CodepointToUTF8(charCodePoint, &utf8Len);
                if (utf8Len > 0) {
                    if (relativeInsertOffsetInSpanBytes > spans[targetSpanIdx].text.length()) {
                        relativeInsertOffsetInSpanBytes = spans[targetSpanIdx].text.length();
                    }
                    spans[targetSpanIdx].text.insert(relativeInsertOffsetInSpanBytes, utf8Bytes, utf8Len);
                    textEditCursorBytePosition += utf8Len;
                    needsRelayout = true;
                }
            }
            charCodePoint = GetCharPressed();
        }

        if (IsKeyPressedRepeat(KEY_BACKSPACE) || IsKeyPressed(KEY_BACKSPACE)) {
            if (textEditCursorBytePosition > 0 && !spans.empty()) {
                uint32_t currentGlobalOffset = 0;
                int targetSpanIdx = -1;
                uint32_t relativeByteOffsetInSpanEnd = 0; // Byte offset from start of span to cursor

                for (size_t i = 0; i < spans.size(); ++i) {
                    const auto& s = spans[i];
                    size_t spanByteLength = s.style.isImage && s.text.empty() ? 3 : s.text.length();
                    if (textEditCursorBytePosition > currentGlobalOffset && textEditCursorBytePosition <= currentGlobalOffset + spanByteLength) {
                        targetSpanIdx = i;
                        relativeByteOffsetInSpanEnd = textEditCursorBytePosition - currentGlobalOffset;
                        break;
                    }
                    currentGlobalOffset += spanByteLength;
                }

                if (targetSpanIdx != -1) {
                    if (spans[targetSpanIdx].style.isImage && relativeByteOffsetInSpanEnd > 0) { // Cursor is after image or inside its placeholder
                        size_t imagePlaceholderLen = spans[targetSpanIdx].text.empty() ? 3 : spans[targetSpanIdx].text.length();
                        textEditCursorBytePosition -= imagePlaceholderLen;
                        spans.erase(spans.begin() + targetSpanIdx);
                        if (spans.empty()) { // Ensure there's always a span if everything is deleted
                            TextSpan emptySpan; emptySpan.style = paraStyle.defaultCharacterStyle;
                            spans.push_back(emptySpan);
                            textEditCursorBytePosition = 0;
                        }
                        needsRelayout = true;
                    } else if (!spans[targetSpanIdx].style.isImage) { // Text span
                        std::string& textToEdit = spans[targetSpanIdx].text;
                        if (relativeByteOffsetInSpanEnd > 0 && relativeByteOffsetInSpanEnd <= textToEdit.length()) {
                            uint32_t charToDeleteStartOffset = 0;
                            int charToDeleteByteLength = 0;

                            // Find the start of the UTF-8 character before relativeByteOffsetInSpanEnd
                            const char* ptr = textToEdit.c_str();
                            const char* endPtr = ptr + relativeByteOffsetInSpanEnd;
                            const char* lastCharStartPtr = ptr;
                            const char* prevLastCharStartPtr = ptr;

                            while(ptr < endPtr) {
                                prevLastCharStartPtr = lastCharStartPtr;
                                lastCharStartPtr = ptr;
                                int currentCharlen = 0;
                                GetNextCodepointFromUTF8(&ptr, &currentCharlen);
                                if(currentCharlen == 0) break; // Should not happen with valid UTF-8
                                charToDeleteByteLength = currentCharlen;
                            }
                            charToDeleteStartOffset = lastCharStartPtr - textToEdit.c_str();

                            if (charToDeleteByteLength > 0 && charToDeleteStartOffset < textToEdit.length()) {
                                textToEdit.erase(charToDeleteStartOffset, charToDeleteByteLength);
                                textEditCursorBytePosition -= charToDeleteByteLength;
                                needsRelayout = true;
                                // If a text span becomes empty and it's not the only span, consider removing it
                                // or merging with an adjacent text span if styles match. For simplicity, leaving it.
                            }
                        }
                    }
                }
            }
        }

        bool cursorMovedByKey = false;
        if (IsKeyPressedRepeat(KEY_LEFT) || IsKeyPressed(KEY_LEFT)){
            if (textEditCursorBytePosition > 0) {
                // This logic needs to be robust across multiple spans if currentTextBlock.sourceTextConcatenated is used.
                // For simplicity, we use sourceTextConcatenated from the last layout.
                // A more robust solution would map global byte offset to span and local offset.
                if (!currentTextBlock.sourceTextConcatenated.empty()) {
                    uint32_t prevCharStartOffset = 0;
                    uint32_t currentSearchOffset = 0;
                    const char* textPtr = currentTextBlock.sourceTextConcatenated.c_str();
                    while(currentSearchOffset < textEditCursorBytePosition) {
                        prevCharStartOffset = currentSearchOffset;
                        int bc = 0;
                        GetNextCodepointFromUTF8(&textPtr, &bc);
                        if(bc == 0) break; // End of string or invalid
                        currentSearchOffset += bc;
                        if(currentSearchOffset >= textEditCursorBytePosition) break;
                    }
                    textEditCursorBytePosition = prevCharStartOffset;
                } else if (textEditCursorBytePosition > 0) { // Fallback if no layout yet, just decrement (not UTF-8 safe)
                    textEditCursorBytePosition--;
                }
            }
            cursorMovedByKey=true;
        }
        if (IsKeyPressedRepeat(KEY_RIGHT) || IsKeyPressed(KEY_RIGHT)){
            if (!currentTextBlock.sourceTextConcatenated.empty() && textEditCursorBytePosition < currentTextBlock.sourceTextConcatenated.length()) {
                const char* textPtr = currentTextBlock.sourceTextConcatenated.c_str() + textEditCursorBytePosition;
                int bc = 0;
                GetNextCodepointFromUTF8(&textPtr, &bc);
                if (bc > 0) textEditCursorBytePosition += bc;
            } else if (textEditCursorBytePosition < 1000000) { // Arbitrary large number, if no layout yet (not UTF-8 safe)
                // This branch is problematic if sourceTextConcatenated is empty.
                // The cursor should not move beyond the logical end determined by spans.
            }
            cursorMovedByKey=true;
        }
        if (IsKeyPressed(KEY_HOME)) {
            if (cursorInfo.lineIndex != -1 && cursorInfo.lineIndex < (int)currentTextBlock.lines.size()) {
                textEditCursorBytePosition = currentTextBlock.lines[cursorInfo.lineIndex].sourceTextByteStartIndexInBlockText;
            } else { // Fallback or if no lines
                textEditCursorBytePosition = 0;
            }
            cursorMovedByKey = true;
        }
        if (IsKeyPressed(KEY_END)) {
            if (cursorInfo.lineIndex != -1 && cursorInfo.lineIndex < (int)currentTextBlock.lines.size()) {
                textEditCursorBytePosition = currentTextBlock.lines[cursorInfo.lineIndex].sourceTextByteEndIndexInBlockText;
                // Adjust if it's a trailing newline placeholder that shouldn't be selected
                if (textEditCursorBytePosition > 0 && cursorInfo.isTrailingEdge &&
                    currentTextBlock.sourceTextConcatenated[textEditCursorBytePosition-1] == '\n') {
                    // This might need more nuance depending on how line endings are handled.
                }
            } else { // Fallback or if no lines
                // Recalculate total length from spans if currentTextBlock is stale
                uint32_t totalLen = 0;
                for(const auto& s : spans) totalLen += (s.style.isImage && s.text.empty() ? 3 : s.text.length());
                textEditCursorBytePosition = totalLen;
            }
            cursorMovedByKey = true;
        }

        if (IsKeyPressedRepeat(KEY_UP) || IsKeyPressed(KEY_UP)) {
            if (cursorInfo.lineIndex > 0 && !currentTextBlock.lines.empty()) {
                Vector2 targetPos = {cursorInfo.visualPosition.x, cursorInfo.visualPosition.y - cursorInfo.cursorHeight * 0.9f}; // Go up one line
                textEditCursorBytePosition = textEngine->GetByteOffsetFromVisualPosition(currentTextBlock, targetPos, nullptr, nullptr);
            } else if (cursorInfo.lineIndex == 0 && !currentTextBlock.lines.empty()){ // Already at the first line
                textEditCursorBytePosition = currentTextBlock.lines[0].sourceTextByteStartIndexInBlockText;
            }
            cursorMovedByKey = true;
        }
        if (IsKeyPressedRepeat(KEY_DOWN) || IsKeyPressed(KEY_DOWN)) {
            if (cursorInfo.lineIndex != -1 && cursorInfo.lineIndex < (int)currentTextBlock.lines.size() - 1 && !currentTextBlock.lines.empty()) {
                Vector2 targetPos = {cursorInfo.visualPosition.x, cursorInfo.visualPosition.y + cursorInfo.cursorHeight * 1.1f}; // Go down one line
                textEditCursorBytePosition = textEngine->GetByteOffsetFromVisualPosition(currentTextBlock, targetPos, nullptr, nullptr);
            } else if (cursorInfo.lineIndex != -1 && cursorInfo.lineIndex == (int)currentTextBlock.lines.size() -1 && !currentTextBlock.lines.empty()){ // Already at the last line
                textEditCursorBytePosition = currentTextBlock.lines.back().sourceTextByteEndIndexInBlockText;
            }
            cursorMovedByKey = true;
        }


        if (cursorMovedByKey) { needsRelayout = true; blinkTimer = 0.0f; showCursor = true; }

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            Vector2 mousePos = GetMousePosition();
            // Transform mousePos from screen to local text block coordinates
            Matrix matInvTransform = MatrixInvert(MatrixMultiply(
                    MatrixMultiply(
                            MatrixMultiply(
                                    MatrixTranslate(-textBlockTransformOrigin.x, -textBlockTransformOrigin.y, 0),
                                    MatrixScale(textBlockScale, textBlockScale, 1.0f)
                            ),
                            MatrixRotateZ(textBlockRotation * DEG2RAD)
                    ),
                    MatrixTranslate(textBlockTransformOrigin.x + textBlockScreenPosition.x, textBlockTransformOrigin.y + textBlockScreenPosition.y, 0)
            ));

            // The matrix multiplication order for transforming mouse position should be:
            // 1. Translate to origin of text block on screen
            // 2. Translate by negative transform origin (pivot for scale/rotation)
            // 3. Apply inverse rotation
            // 4. Apply inverse scale
            // 5. Translate by positive transform origin
            // This is complex, simpler way:
            Matrix matScreenToTextBlock = MatrixIdentity();
            matScreenToTextBlock = MatrixMultiply(matScreenToTextBlock, MatrixTranslate(-textBlockScreenPosition.x, -textBlockScreenPosition.y, 0));
            matScreenToTextBlock = MatrixMultiply(matScreenToTextBlock, MatrixTranslate(-textBlockTransformOrigin.x, -textBlockTransformOrigin.y, 0)); // Translate to pivot
            matScreenToTextBlock = MatrixMultiply(matScreenToTextBlock, MatrixInvert(MatrixRotateZ(textBlockRotation * DEG2RAD))); // Rotate around pivot
            matScreenToTextBlock = MatrixMultiply(matScreenToTextBlock, MatrixInvert(MatrixScale(textBlockScale, textBlockScale, 1.0f))); // Scale around pivot
            matScreenToTextBlock = MatrixMultiply(matScreenToTextBlock, MatrixTranslate(textBlockTransformOrigin.x, textBlockTransformOrigin.y, 0)); // Translate back from pivot

            Vector2 relativeMousePos = Vector2Transform(mousePos, matScreenToTextBlock);


            if (needsRelayout || (currentTextBlock.sourceTextConcatenated.empty() && !spans.empty())) {
                currentTextBlock = textEngine->LayoutStyledText(spans, paraStyle);
            }
            textEditCursorBytePosition = textEngine->GetByteOffsetFromVisualPosition(currentTextBlock, relativeMousePos, nullptr, nullptr);
            needsRelayout = true; showCursor = true; blinkTimer = 0.0f;
        }


        // 调试按键
        if (IsKeyPressed(KEY_F1)) {
            bool anyOutline = false;
            if (!spans.empty()) anyOutline = spans[0].style.outline.enabled;
            for(auto& s : spans) { if(!s.style.isImage) s.style.outline.enabled = !anyOutline; }
            needsRelayout = true;
        }
        if (IsKeyPressed(KEY_F2)) {
            bool anyGlow = false;
            if (!spans.empty()) anyGlow = spans[0].style.glow.enabled;
            for(auto& s : spans) { if(!s.style.isImage) s.style.glow.enabled = !anyGlow; }
            needsRelayout = true;
        }
        if (IsKeyPressed(KEY_F5)) animateScale = !animateScale;
        if (IsKeyPressed(KEY_F6)) showDebugAtlas = !showDebugAtlas;
        if (IsKeyDown(KEY_PAGE_UP)) { dynamicSmoothnessAdd -= 0.0005f; dynamicSmoothnessAdd = std::max(-0.04f, dynamicSmoothnessAdd); needsRelayout = true;}
        if (IsKeyDown(KEY_PAGE_DOWN)) { dynamicSmoothnessAdd += 0.0005f; dynamicSmoothnessAdd = std::min(0.2f, dynamicSmoothnessAdd); needsRelayout = true;}


        // --- 布局与光标更新 ---
        if (needsRelayout) {
            currentTextBlock = textEngine->LayoutStyledText(spans, paraStyle);
            needsRelayout = false;

            // Recalculate transform origin based on the new layout's bounds
            if (currentTextBlock.overallBounds.width > 0 || currentTextBlock.overallBounds.height > 0) {
                textBlockTransformOrigin.x = currentTextBlock.overallBounds.x + currentTextBlock.overallBounds.width / 2.0f;
                textBlockTransformOrigin.y = currentTextBlock.overallBounds.y + currentTextBlock.overallBounds.height / 2.0f;
            } else {
                textBlockTransformOrigin = {0,0}; // Default if no content or zero size
            }
        }

        uint32_t totalConcatenatedLength = currentTextBlock.sourceTextConcatenated.length();
        textEditCursorBytePosition = std::min(textEditCursorBytePosition, totalConcatenatedLength);
        cursorInfo = textEngine->GetCursorInfoFromByteOffset(currentTextBlock, textEditCursorBytePosition, true);


        // 绘制
        //----------------------------------------------------------------------------------
        BeginDrawing();
        ClearBackground(RAYWHITE);

        // Construct the final transformation matrix for drawing the text block
        Matrix finalTransform = MatrixIdentity();
        // 1. Translate to the text block's local origin (pivot point for rotation/scaling)
        finalTransform = MatrixMultiply(MatrixTranslate(-textBlockTransformOrigin.x, -textBlockTransformOrigin.y, 0), finalTransform);
        // 2. Scale
        finalTransform = MatrixMultiply(MatrixScale(textBlockScale, textBlockScale, 1.0f), finalTransform);
        // 3. Rotate
        finalTransform = MatrixMultiply(MatrixRotateZ(textBlockRotation * DEG2RAD), finalTransform);
        // 4. Translate back from the local origin and then to the screen position
        finalTransform = MatrixMultiply(MatrixTranslate(textBlockTransformOrigin.x + textBlockScreenPosition.x,
                                                        textBlockTransformOrigin.y + textBlockScreenPosition.y, 0), finalTransform);

        textEngine->DrawTextBlock(currentTextBlock, finalTransform, WHITE);

        if (showCursor) {
            float cursorTopY = cursorInfo.visualPosition.y - cursorInfo.cursorAscent;
            Rectangle cursorRectLocal = { cursorInfo.visualPosition.x - 1.0f, cursorTopY, 2.0f, cursorInfo.cursorHeight };
            if (cursorInfo.cursorHeight < 1.0f) { // Ensure cursor is visible even for tiny/empty lines
                cursorRectLocal.height = paraStyle.defaultCharacterStyle.fontSize > 0 ? paraStyle.defaultCharacterStyle.fontSize * paraStyle.lineHeightValue * 0.8f : 16.0f;
                if (cursorInfo.cursorAscent == 0 && cursorInfo.cursorDescent == 0) { // Likely an empty line
                    cursorRectLocal.y = cursorInfo.visualPosition.y - cursorRectLocal.height * 0.7f; // Adjust based on typical ascent
                }
            }


            Vector2 p1 = Vector2Transform({cursorRectLocal.x, cursorRectLocal.y}, finalTransform);
            Vector2 p2 = Vector2Transform({cursorRectLocal.x + cursorRectLocal.width, cursorRectLocal.y}, finalTransform);
            Vector2 p3 = Vector2Transform({cursorRectLocal.x + cursorRectLocal.width, cursorRectLocal.y + cursorRectLocal.height}, finalTransform);
            Vector2 p4 = Vector2Transform({cursorRectLocal.x, cursorRectLocal.y + cursorRectLocal.height}, finalTransform);

            DrawTriangleStrip((Vector2[]){p1,p4,p2,p3}, 4, BLACK);
        }

        // Debug Text
        DrawText(TextFormat("Spans: %zu, Glyphs: %zu, Lines: %zu, TextBytes: %u",
                            spans.size(), currentTextBlock.elements.size(), currentTextBlock.lines.size(), (unsigned int)currentTextBlock.sourceTextConcatenated.length()),
                 10, 10, 10, GRAY);
        DrawText(TextFormat("CursorByte: %u (Line: %d, Trail: %s, X:%.1f Y:%.1f H:%.1f)",
                            textEditCursorBytePosition, cursorInfo.lineIndex,
                            cursorInfo.isTrailingEdge ? "T":"F", cursorInfo.visualPosition.x, cursorInfo.visualPosition.y, cursorInfo.cursorHeight),
                 10, 25, 10, GRAY);
        DrawText(TextFormat("SmoothnessAdd (PgUp/PgDn): %.4f", dynamicSmoothnessAdd), 10, screenHeight - 20, 10, GRAY);
        DrawText("F1:TglOutline F2:TglGlow F5:AnimScale F6:DebugAtlas", 10, 40, 10, GRAY);

        if (showDebugAtlas) {
            Texture2D atlasToDraw = textEngine->GetAtlasTextureForDebug(0);
            if (atlasToDraw.id > 0) {
                float dbgAtlasScale = 0.25f;
                DrawText("SDF Atlas 0:", 10, screenHeight - atlasToDraw.height * dbgAtlasScale - 55, 10, DARKGRAY);
                DrawTextureEx(atlasToDraw, {10, screenHeight - atlasToDraw.height * dbgAtlasScale - 40}, 0.0f, dbgAtlasScale, WHITE);
            }
        }
        DrawFPS(screenWidth - 90, 10);
        EndDrawing();
        //----------------------------------------------------------------------------------
    }

    // 清理
    //--------------------------------------------------------------------------------------
    if (inlineTestImage.id > 0) UnloadTexture(inlineTestImage);
    textEngine.reset();

    CloseWindow();
    //--------------------------------------------------------------------------------------

    return 0;
}