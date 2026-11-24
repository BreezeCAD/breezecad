#include "core/ArabicText.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace bz::arabic {
namespace {

enum class Join : unsigned char { None, Right, Dual };

/// Contextual forms of a letter: isolated, final, initial, medial (0: none).
struct Letter {
    char32_t iso, fin, ini, med;
    Join     join;
};

const std::unordered_map<char32_t, Letter>& Letters() {
    static const std::unordered_map<char32_t, Letter> table = {
        {0x0621, {0xFE80, 0, 0, 0, Join::None}},
        {0x0622, {0xFE81, 0xFE82, 0, 0, Join::Right}},
        {0x0623, {0xFE83, 0xFE84, 0, 0, Join::Right}},
        {0x0624, {0xFE85, 0xFE86, 0, 0, Join::Right}},
        {0x0625, {0xFE87, 0xFE88, 0, 0, Join::Right}},
        {0x0626, {0xFE89, 0xFE8A, 0xFE8B, 0xFE8C, Join::Dual}},
        {0x0627, {0xFE8D, 0xFE8E, 0, 0, Join::Right}},
        {0x0628, {0xFE8F, 0xFE90, 0xFE91, 0xFE92, Join::Dual}},
        {0x0629, {0xFE93, 0xFE94, 0, 0, Join::Right}},
        {0x062A, {0xFE95, 0xFE96, 0xFE97, 0xFE98, Join::Dual}},
        {0x062B, {0xFE99, 0xFE9A, 0xFE9B, 0xFE9C, Join::Dual}},
        {0x062C, {0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0, Join::Dual}},
        {0x062D, {0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4, Join::Dual}},
        {0x062E, {0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8, Join::Dual}},
        {0x062F, {0xFEA9, 0xFEAA, 0, 0, Join::Right}},
        {0x0630, {0xFEAB, 0xFEAC, 0, 0, Join::Right}},
        {0x0631, {0xFEAD, 0xFEAE, 0, 0, Join::Right}},
        {0x0632, {0xFEAF, 0xFEB0, 0, 0, Join::Right}},
        {0x0633, {0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4, Join::Dual}},
        {0x0634, {0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8, Join::Dual}},
        {0x0635, {0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC, Join::Dual}},
        {0x0636, {0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0, Join::Dual}},
        {0x0637, {0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4, Join::Dual}},
        {0x0638, {0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8, Join::Dual}},
        {0x0639, {0xFEC9, 0xFECA, 0xFECB, 0xFECC, Join::Dual}},
        {0x063A, {0xFECD, 0xFECE, 0xFECF, 0xFED0, Join::Dual}},
        {0x0640, {0x0640, 0x0640, 0x0640, 0x0640, Join::Dual}},   // tatweel
        {0x0641, {0xFED1, 0xFED2, 0xFED3, 0xFED4, Join::Dual}},
        {0x0642, {0xFED5, 0xFED6, 0xFED7, 0xFED8, Join::Dual}},
        {0x0643, {0xFED9, 0xFEDA, 0xFEDB, 0xFEDC, Join::Dual}},
        {0x0644, {0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0, Join::Dual}},
        {0x0645, {0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4, Join::Dual}},
        {0x0646, {0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8, Join::Dual}},
        {0x0647, {0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC, Join::Dual}},
        {0x0648, {0xFEED, 0xFEEE, 0, 0, Join::Right}},
        {0x0649, {0xFEEF, 0xFEF0, 0, 0, Join::Right}},
        {0x064A, {0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4, Join::Dual}},
        // Persian and Urdu letters.
        {0x067E, {0xFB56, 0xFB57, 0xFB58, 0xFB59, Join::Dual}},
        {0x0686, {0xFB7A, 0xFB7B, 0xFB7C, 0xFB7D, Join::Dual}},
        {0x0698, {0xFB8A, 0xFB8B, 0, 0, Join::Right}},
        {0x06A9, {0xFB8E, 0xFB8F, 0xFB90, 0xFB91, Join::Dual}},
        {0x06AF, {0xFB92, 0xFB93, 0xFB94, 0xFB95, Join::Dual}},
        {0x06CC, {0xFBFC, 0xFBFD, 0xFBFE, 0xFBFF, Join::Dual}},
    };
    return table;
}

/// Marks that sit on the letter before them (fatha, damma, shadda, dagger alef…).
bool IsMark(char32_t c) {
    return (c >= 0x064B && c <= 0x065F) || c == 0x0670 || (c >= 0x06D6 && c <= 0x06DC) || (c >= 0x06DF && c <= 0x06E4) ||
           c == 0x06E7 || c == 0x06E8 || (c >= 0x06EA && c <= 0x06ED);
}

bool IsArabicBlock(char32_t c) {
    return (c >= 0x0600 && c <= 0x06FF) || (c >= 0x0750 && c <= 0x077F) || (c >= 0xFB50 && c <= 0xFDFF) ||
           (c >= 0xFE70 && c <= 0xFEFF);
}

std::u32string Decode(std::string_view s) {
    std::u32string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char b = static_cast<unsigned char>(s[i]);
        int len = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 1;
        if (i + static_cast<std::size_t>(len) > s.size()) len = 1;
        char32_t c = len == 1 ? b : static_cast<char32_t>(b & (0xFF >> (len + 1)));
        for (int k = 1; k < len; ++k) c = (c << 6) | (static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]) & 0x3F);
        out.push_back(c);
        i += static_cast<std::size_t>(len);
    }
    return out;
}

std::string Encode(const std::u32string& s) {
    std::string out;
    out.reserve(s.size() * 2);
    for (const char32_t c : s) {
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

// ---- joining ---------------------------------------------------------------

struct Unit {
    char32_t  cp   = 0;
    Join      join = Join::None;
    const Letter* letter = nullptr;
    char32_t  ligIso = 0, ligFin = 0;   // lam + alef: one glyph
};

/// Letters into their forms; marks dropped; lam + alef into one glyph.
std::u32string ShapeLetters(const std::u32string& in) {
    const auto& table = Letters();
    std::vector<Unit> units;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char32_t c = in[i];
        if (IsMark(c)) continue;
        Unit u;
        u.cp = c;
        if (const auto it = table.find(c); it != table.end()) {
            u.letter = &it->second;
            u.join   = it->second.join;
            if (c == 0x0644) {
                std::size_t j = i + 1;
                while (j < in.size() && IsMark(in[j])) ++j;
                if (j < in.size()) {
                    const char32_t a = in[j];
                    const bool lig = a == 0x0622 || a == 0x0623 || a == 0x0625 || a == 0x0627;
                    if (lig) {
                        u.ligIso = a == 0x0622 ? 0xFEF5 : a == 0x0623 ? 0xFEF7 : a == 0x0625 ? 0xFEF9 : 0xFEFB;
                        u.ligFin = u.ligIso + 1;
                        u.join   = Join::Right;
                        i        = j;
                    }
                }
            }
        }
        units.push_back(u);
    }
    std::u32string out;
    out.reserve(units.size());
    for (std::size_t i = 0; i < units.size(); ++i) {
        const Unit& u = units[i];
        if (!u.letter) {
            out.push_back(u.cp);
            continue;
        }
        const bool prevJoins = i > 0 && units[i - 1].join == Join::Dual;
        const bool joinPrev  = prevJoins && (u.join == Join::Dual || u.join == Join::Right);
        const bool joinNext  = u.join == Join::Dual && i + 1 < units.size() &&
                               (units[i + 1].join == Join::Dual || units[i + 1].join == Join::Right);
        if (u.ligIso) {
            out.push_back(joinPrev ? u.ligFin : u.ligIso);
            continue;
        }
        const Letter& l = *u.letter;
        char32_t form = l.iso;
        if (joinPrev && joinNext) form = l.med ? l.med : (l.fin ? l.fin : l.iso);
        else if (joinPrev) form = l.fin ? l.fin : l.iso;
        else if (joinNext) form = l.ini ? l.ini : l.iso;
        out.push_back(form);
    }
    return out;
}

// ---- bidirectional ordering -------------------------------------------------

enum class Kind : unsigned char { L, R, EN, ES, ET, CS, ON };

bool IsAsciiDigit(char32_t c) { return c >= '0' && c <= '9'; }

Kind Classify(char32_t c) {
    if (IsAsciiDigit(c) || c == 0xB2 || c == 0xB3 || c == 0xB9 || (c >= 0x0660 && c <= 0x0669)) return Kind::EN;
    if (c == 0x060C || c == 0x061B || c == 0x061F) return Kind::ON;   // Arabic comma, semicolon, question mark
    if (IsArabicBlock(c)) return Kind::R;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return Kind::L;
    if (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) return Kind::L;
    if (c >= 0x0370 && c <= 0x052F) return Kind::L;   // Greek, Cyrillic
    if (c == '+' || c == '-') return Kind::ES;
    if (c == '%' || c == '$' || c == '#' || c == 0xB0 || c == 0xB1) return Kind::ET;
    if (c == ',' || c == '.' || c == ':' || c == '/' || c == 0xA0) return Kind::CS;
    return Kind::ON;
}

char32_t Mirror(char32_t c) {
    switch (c) {
    case '(': return ')';
    case ')': return '(';
    case '[': return ']';
    case ']': return '[';
    case '{': return '}';
    case '}': return '{';
    case '<': return '>';
    case '>': return '<';
    case 0xAB: return 0xBB;
    case 0xBB: return 0xAB;
    case 0x2039: return 0x203A;
    case 0x203A: return 0x2039;
    default: return c;
    }
}

/// `text` (already joined) in visual order; right to left is the base direction.
std::u32string Reorder(std::u32string text) {
    const std::size_t n = text.size();
    std::vector<Kind> kind(n);
    for (std::size_t i = 0; i < n; ++i) kind[i] = Classify(text[i]);

    // Numbers: a separator between two digits, signs and units next to digits.
    for (std::size_t i = 1; i + 1 < n; ++i)
        if ((kind[i] == Kind::ES || kind[i] == Kind::CS) && kind[i - 1] == Kind::EN && kind[i + 1] == Kind::EN) kind[i] = Kind::EN;
    for (std::size_t i = 0; i < n;) {
        if (kind[i] != Kind::ET) {
            ++i;
            continue;
        }
        std::size_t end = i;
        while (end < n && kind[end] == Kind::ET) ++end;
        const bool afterNumber  = i > 0 && kind[i - 1] == Kind::EN;
        const bool beforeNumber = end < n && kind[end] == Kind::EN;
        for (std::size_t k = i; k < end; ++k) kind[k] = afterNumber || beforeNumber ? Kind::EN : Kind::ON;
        i = end;
    }
    for (auto& k : kind)
        if (k == Kind::ES || k == Kind::CS || k == Kind::ET) k = Kind::ON;

    // A number after a Latin letter belongs to the Latin run.
    Kind lastStrong = Kind::R;   // the line starts right to left
    for (std::size_t i = 0; i < n; ++i) {
        if (kind[i] == Kind::L || kind[i] == Kind::R) lastStrong = kind[i];
        else if (kind[i] == Kind::EN && lastStrong == Kind::L) kind[i] = Kind::L;
    }

    // Neutrals take the direction of both neighbours when it is the same, else the base one.
    const auto influence = [](Kind k) { return k == Kind::L ? Kind::L : Kind::R; };   // numbers count as right to left
    std::vector<unsigned char> level(n);
    for (std::size_t i = 0; i < n;) {
        if (kind[i] != Kind::ON) {
            level[i] = kind[i] == Kind::R ? 1 : 2;
            ++i;
            continue;
        }
        std::size_t end = i;
        while (end < n && kind[end] == Kind::ON) ++end;
        const Kind before = i > 0 ? influence(kind[i - 1]) : Kind::R;
        const Kind after  = end < n ? influence(kind[end]) : Kind::R;
        const unsigned char lv = before == after && before == Kind::L ? 2 : 1;
        for (std::size_t k = i; k < end; ++k) level[k] = lv;
        i = end;
    }

    for (std::size_t i = 0; i < n; ++i)
        if (level[i] == 1) text[i] = Mirror(text[i]);

    // Reverse runs from the highest level down to level 1.
    for (unsigned char lv = 2; lv >= 1; --lv) {
        for (std::size_t i = 0; i < n;) {
            if (level[i] < lv) {
                ++i;
                continue;
            }
            std::size_t end = i;
            while (end < n && level[end] >= lv) ++end;
            std::reverse(text.begin() + static_cast<std::ptrdiff_t>(i), text.begin() + static_cast<std::ptrdiff_t>(end));
            std::reverse(level.begin() + static_cast<std::ptrdiff_t>(i), level.begin() + static_cast<std::ptrdiff_t>(end));
            i = end;
        }
    }
    return text;
}

} // namespace

bool HasArabic(std::string_view utf8) {
    // U+0600..06FF is D8 80..DB BF; U+0750..077F is DD 90..DD BF; U+FB50..FEFF is EF AD..EF BF.
    for (std::size_t i = 0; i < utf8.size(); ++i) {
        const unsigned char b = static_cast<unsigned char>(utf8[i]);
        if (b >= 0xD8 && b <= 0xDB) return true;
        if (b == 0xDD && i + 1 < utf8.size() && static_cast<unsigned char>(utf8[i + 1]) >= 0x90) return true;
        if (b == 0xEF && i + 1 < utf8.size()) {
            const unsigned char c = static_cast<unsigned char>(utf8[i + 1]);
            if (c >= 0xAD && c <= 0xBB) return true;
        }
    }
    return false;
}

std::string ToVisual(std::string_view line) {
    if (!HasArabic(line)) return std::string(line);
    return Encode(Reorder(ShapeLetters(Decode(line))));
}

} // namespace bz::arabic
