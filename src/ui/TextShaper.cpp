#include "ui/TextShaper.h"

#include "core/ArabicText.h"

#include <imgui.h>

#include <cfloat>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bz::ui {
namespace {

std::unordered_map<std::string, std::string>& Cache() {
    static std::unordered_map<std::string, std::string> cache;
    return cache;
}

float Width(ImFont* font, float size, const std::string& text) {
    if (text.empty()) return 0.0f;
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.data(), text.data() + text.size()).x;
}

std::vector<std::string_view> Split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t from = 0;
    for (;;) {
        const std::size_t at = text.find(separator, from);
        if (at == std::string_view::npos) break;
        parts.push_back(text.substr(from, at - from));
        from = at + 1;
    }
    parts.push_back(text.substr(from));
    return parts;
}

/// The words of one logical line packed into lines no wider than `wrap`, each in visual order.
void WrapLine(ImFont* font, float size, std::string_view line, float wrap, std::vector<std::string>& lines) {
    std::string current;
    for (const std::string_view word : Split(line, ' ')) {
        const std::string candidate = current.empty() ? std::string(word) : current + ' ' + std::string(word);
        if (!current.empty() && Width(font, size, arabic::ToVisual(candidate)) > wrap) {
            lines.push_back(arabic::ToVisual(current));
            current = std::string(word);
        } else {
            current = candidate;
        }
    }
    lines.push_back(arabic::ToVisual(current));
}

std::string Shape(ImFont* font, float size, std::string_view source, float wrap) {
    std::vector<std::string> lines;
    for (const std::string_view hard : Split(source, '\n')) {
        if (wrap > 0.0f && arabic::HasArabic(hard)) WrapLine(font, size, hard, wrap, lines);
        else lines.push_back(arabic::ToVisual(hard));
    }
    std::string out;
    // Lines of a wrapped block lean to the right edge, as right-to-left text does.
    std::vector<float> widths;
    float widest = 0.0f;
    if (wrap > 0.0f && lines.size() > 1) {
        for (const std::string& l : lines) {
            widths.push_back(Width(font, size, l));
            widest = widths.back() > widest ? widths.back() : widest;
        }
    }
    const float space = widths.empty() ? 0.0f : Width(font, size, " ");
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out += '\n';
        if (!widths.empty() && space > 0.0f && arabic::HasArabic(lines[i]))
            out.append(static_cast<std::size_t>(std::floor((widest - widths[i]) / space)), ' ');
        out += lines[i];
    }
    return out;
}

} // namespace

const char* ShapeText(ImFont* font, float fontSize, const char* text, const char* textEnd, float* wrapWidth,
                      const char** shapedEnd) {
    if (!text || !font) return nullptr;
    const std::string_view source = textEnd ? std::string_view(text, static_cast<std::size_t>(textEnd - text)) : std::string_view(text);
    if (!arabic::HasArabic(source)) return nullptr;

    static ImFont*     lastFont = nullptr;
    static std::string result;
    auto& cache = Cache();
    if (font != lastFont || cache.size() > 2000) {
        cache.clear();
        lastFont = font;
    }
    const float wrap = wrapWidth && *wrapWidth > 0.0f ? *wrapWidth : 0.0f;
    std::string key(source);
    key += '\x01';
    key += std::to_string(static_cast<int>(wrap));
    key += '\x01';
    key += std::to_string(static_cast<int>(fontSize * 10.0f));
    auto it = cache.find(key);
    if (it == cache.end()) it = cache.emplace(std::move(key), Shape(font, fontSize, source, wrap)).first;
    result = it->second;
    if (wrapWidth && wrap > 0.0f) *wrapWidth = 0.0f;
    if (shapedEnd) *shapedEnd = result.data() + result.size();
    return result.c_str();
}

void ResetTextShaper() { Cache().clear(); }

} // namespace bz::ui
