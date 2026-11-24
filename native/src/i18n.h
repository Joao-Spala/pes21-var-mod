#pragma once

#include <cstdint>

namespace var_mod {

enum class Language : int32_t {
    PtBr    = 0,
    English = 1,
};

namespace i18n {

// Lock-free current UI language (written from config, read by render/sim threads)
void     SetLanguage(Language lang);
Language GetLanguage();

// Translates a Portuguese (PT-BR) UI string. Returns the input unchanged in PT-BR mode or when the
// string has no entry in i18n_strings.inc. Works on literals AND on runtime buffers that hold a
// PT-BR label (incident labels, banners, VAR notes), so stored data follows the selected language.
// Returned pointer is either the input or a static literal: always valid for the program lifetime.
const char* Tr(const char* ptBr);

const char* LanguageCode(Language lang);      // "pt" / "en" (INI)
Language    ParseLanguageCode(const char* s); // accepts pt, pt-br, ptbr, en, english, 0, 1
const char* LanguageDisplayName(Language lang);

} // namespace i18n

using i18n::Tr;

} // namespace var_mod
