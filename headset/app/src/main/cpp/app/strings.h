// UI strings for the headset, in the language the user picked (General > Language).
// English is the source of truth: every other language only translates what it has,
// and anything missing falls back to English. The glyph atlas in
// assets/ui/text.png must contain every non-ASCII character used here —
// tools/assetbaker reads the charset from this file's directory when it bakes.
#pragma once

#include <initializer_list>
#include <string>
#include <utility>

namespace rv {

// Activates a language. `choice` is "auto" or a code like "en"/"zh-CN"; "auto"
// resolves against `systemLocale` (a BCP 47 tag from the platform). Unknown
// choices fall back to English.
void SetLanguage(const std::string& choice, const std::string& systemLocale = "en");

// The resolved language code, e.g. "zh-CN".
const std::string& Language();

// The languages the UI can show, as {code, native name} pairs — "English",
// "简体中文" — so the picker reads the same no matter the active language.
struct LanguageChoice {
    const char* code;
    const char* name;
};
const LanguageChoice* LanguageChoices(size_t& count);

// Looks a key up in the active language and returns it in UTF-8. An unknown key
// returns the key itself, so a typo is visible instead of silently blank.
const char* Tr(const char* key);

// Tr with "{name}" placeholders replaced: Trf("computers.connectingTo",
// {{"name", server}}). Values go in as they are; callers escape what needs it.
std::string Trf(const char* key, std::initializer_list<std::pair<const char*, std::string>> params = {});

}  // namespace rv
