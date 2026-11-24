#include "i18n.h"

#include <atomic>
#include <cstring>
#include <string_view>
#include <unordered_map>

namespace var_mod {
namespace i18n {

namespace {

std::atomic<int32_t> g_language{ static_cast<int32_t>(Language::PtBr) };

struct Entry {
    const char* pt;
    const char* en;
};

constexpr Entry kTable[] = {
#define I18N(pt, en) { pt, en },
#include "i18n_strings.inc"
#undef I18N
};

const std::unordered_map<std::string_view, const char*>& EnglishMap() {
    // Built once on first English lookup (thread-safe static init), then read-only
    static const std::unordered_map<std::string_view, const char*> map = [] {
        std::unordered_map<std::string_view, const char*> m;
        m.reserve(sizeof(kTable) / sizeof(kTable[0]) * 2);
        for (const Entry& e : kTable) {
            m.emplace(std::string_view(e.pt), e.en);
        }
        return m;
    }();
    return map;
}

} // namespace

void SetLanguage(Language lang) {
    const int32_t v = (lang == Language::English) ? 1 : 0;
    g_language.store(v, std::memory_order_relaxed);
    if (v == 1) {
        (void)EnglishMap(); // build the table off the render hot path
    }
}

Language GetLanguage() {
    return (g_language.load(std::memory_order_relaxed) == 1) ? Language::English : Language::PtBr;
}

const char* Tr(const char* ptBr) {
    if (!ptBr || g_language.load(std::memory_order_relaxed) != 1) return ptBr;
    const auto& map = EnglishMap();
    const auto it = map.find(std::string_view(ptBr));
    return (it != map.end()) ? it->second : ptBr;
}

const char* LanguageCode(Language lang) {
    return (lang == Language::English) ? "en" : "pt";
}

Language ParseLanguageCode(const char* s) {
    if (!s) return Language::PtBr;
    char low[16] = {};
    size_t n = 0;
    for (; s[n] != '\0' && n + 1 < sizeof(low); ++n) {
        const char c = s[n];
        low[n] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    if (std::strcmp(low, "en") == 0 || std::strcmp(low, "english") == 0 ||
        std::strcmp(low, "en-us") == 0 || std::strcmp(low, "1") == 0) {
        return Language::English;
    }
    return Language::PtBr;
}

const char* LanguageDisplayName(Language lang) {
    // Each language is always shown in its own name so a user who cannot read the current one finds it
    return (lang == Language::English) ? "English" : "Português (BR)";
}

} // namespace i18n
} // namespace var_mod
