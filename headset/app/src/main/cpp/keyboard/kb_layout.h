// The keyboard's languages and layouts: a port of BetterFrame Keyboard's (MIT), the Steam Frame
// keyboard (github.com/hardwaresteam/BetterFrameKeyboard). Its data comes from assets/keyboard/
// data.json, which tools/keyboard-assets exports from BetterFrame's own code; the functions below
// are its layout functions, line for line, so every language gets exactly the same keys.
#pragma once

#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace rv {
class Assets;
}

namespace rv::kb {

using Pair = std::array<std::string, 2>;  // a key's character and its character with Shift

// One key: what BetterFrame's K() makes. Which of ch / sym / ins it has says what it types.
struct Key {
    std::string label;
    float w = 1;
    std::optional<std::string> ch, ins;     // a letter (follows Shift, Caps, accents) / exactly this text
    std::optional<Pair> sym;                // a key with a Shift character
    std::string act, icon, tip, np, nplabel, sample, semo;
    std::optional<std::string> pick;
    std::vector<std::string> flick;         // a flick key's five kana
    bool mod = false, go = false, center = false, spacer = false, tall = false, altkey = false, emoji = false,
         bar = false, on = false, off = false, rec = false, wide = false, fitlabel = false, ghost = false, fav = false,
         isFlick = false;
};
using Row = std::vector<Key>;

// A letter row's item: a letter, or a (key, with Shift) pair (punctuation among the letters, Dvorak).
struct RowItem {
    std::string c;
    std::optional<Pair> pair;
    const std::string& First() const { return pair ? (*pair)[0] : c; }
};
using LetterRow = std::vector<RowItem>;

struct Lang {
    std::string code, name, currency, space, ime, labels, fmtname, native;
    Pair pick;
    std::vector<LetterRow> rows;
    std::vector<std::map<std::string, std::string>> layers;  // the accent key's sets
    std::vector<std::string> layerSample;                     // each set's first letter (shown on the accent key)
    std::vector<Pair> extra;
    std::optional<std::vector<Pair>> numrow, after0, punct, mini;
    std::optional<Pair> topleft;
    std::map<std::string, std::string> shift, caseMap;
    bool romaji = false, kana = false, flick = false, rtl = false, reformat = false;
};

// What the drawing and the layouts need to know about the keyboard right now (BetterFrame's st).
struct State {
    std::string layout = "minimal", page = "abc", lang = "en-US", fmt = "default", bottomrow = "default", barpos = "top";
    bool shift = false, caps = false, mic = false, ctrl = false, altm = false, fn = false, meta = false, numlock = true;
    bool swipe = false, words = false, curved = true, panel = true, esearch = false;
    int alt = -1;
    int ecat = 1, epage = 0, etone = -1;
    std::string lgroup;  // the language picker: a language with more than one keyboard, open
    int lpage = 0;
    std::vector<std::string> erecent, efav;
    std::set<std::string> locks;
};

class Data {
   public:
    bool Load(const Assets& assets);
    static Data& Get();

    const std::vector<Lang>& Langs() const { return m_langs; }
    const Lang& Language(const std::string& code) const;
    bool HasLang(const std::string& code) const { return m_byCode.count(code) > 0; }
    // [(key, name, [codes])] in the picker's order
    struct Group {
        std::string key, name;
        std::vector<std::string> codes;
    };
    const std::vector<Group>& Groups() const { return m_groups; }
    const std::vector<std::string>& FormatOrder() const { return m_formatOrder; }
    std::string FormatName(const std::string& fmt) const;

    // Characters, as Python sees them
    std::string Upper(const std::string& c, const Lang& lang) const;      // LANG.upper
    std::string Shifted(const std::string& c, const Lang& lang) const;    // LANG.shifted
    std::string UpperWord(const std::string& w, const Lang& lang) const;  // every letter
    std::string LowerWord(const std::string& w) const;
    std::string Show(const std::string& c) const;                          // LANG.show
    std::string Types(const std::string& c) const;                         // LANG.TYPES
    std::string Base(const std::string& c) const;                          // NFD's first character
    bool IsAlpha(const std::string& c) const { return m_alpha.count(c) > 0; }  // str.isalpha
    bool IsDigit(const std::string& c) const { return m_digit.count(c) > 0; }  // str.isdigit

    // Layouts
    std::vector<LetterRow> LetterRows(const Lang& lang, const std::string& fmt) const;
    std::vector<Row> RowsFor(const std::string& layout, const std::string& page, const std::string& langCode,
                             const std::string& fmt, const State& st) const;
    float UnitsFor(const std::string& layout, const std::string& langCode, const std::string& fmt, const State& st) const;
    Row BarItems(const State& st) const;
    int EmojiPages() const { return m_emojiPages; }
    static constexpr int kLangCols = 6, kLangRows = 4;  // the language picker: 24 languages a page
    bool CanReformat(const std::string& code) const { return Language(code).reformat; }

    // Japanese, Korean
    const std::vector<std::string>& KanaRows() const { return m_kanaRows; }
    std::string Dakuten(const std::string& c) const { return Find(m_dakuten, c); }
    std::string Small(const std::string& c) const { return Find(m_small, c); }
    std::string FlickMod(const std::string& c) const { return Find(m_flickMod, c); }
    const std::map<std::string, std::string>& Romaji() const { return m_romaji; }
    std::string JaPunct(const std::string& c) const {
        const std::string s = Find(m_jaPunct, c);
        return s.empty() ? c : s;
    }
    std::string RomajiHint(const std::string& c) const { return Find(m_romajiHint, c); }
    std::string FnName(const std::string& act) const { return Find(m_fn, act); }
    bool HasFn(const std::string& act) const { return m_fn.count(act) > 0; }
    struct Hangul {
        std::vector<std::string> cho, jung, jong;
        std::map<std::string, std::string> vv, cc;
    };
    const Hangul& HangulTables() const { return m_hangul; }
    // Vietnamese: a vowel with a Telex tone key (s f r x j); empty if it can't take one
    std::string ViTone(const std::string& vowel, const std::string& tone) const;
    const std::string& ViVowels() const { return m_viVowels; }

    // Colours
    using Rgba = std::array<int, 4>;
    const std::map<std::string, Rgba>& Theme(const std::string& name) const;
    const std::vector<std::string>& ThemeOrder() const { return m_themeOrder; }

    // Emoji
    struct EmojiCat {
        std::string icon, name;
        std::vector<std::string> items;
    };
    const std::vector<EmojiCat>& EmojiCats() const { return m_emojiCats; }
    std::string WithTone(const std::string& e, int tone) const;
    static constexpr int kFavTab = 98;

    // BetterFrame's sizes and where it opens
    std::array<float, 2> pitchMinimal{0.084f, 0.064f}, pitchFull{0.072f, 0.062f}, pitchAdvanced{0.066f, 0.058f};
    float scale = 0.6f, distance = 0.72f, drop = 0.68f, tilt = 15;

   private:
    static std::string Find(const std::map<std::string, std::string>& m, const std::string& k) {
        auto it = m.find(k);
        return it == m.end() ? std::string() : it->second;
    }
    std::vector<Row> LetterRowsLayout(const std::string& layout, const Lang& lang, const std::string& fmt,
                                      const std::string& bottomrow) const;
    std::vector<Row> TopRows(const std::optional<std::vector<Pair>>& num, bool euro, const Lang* lang) const;
    Row BottomFull(const std::string& bottomrow) const;
    bool IsIso(const Lang& lang) const;
    std::vector<Row> IsoRows(const Lang& lang, const std::vector<LetterRow>& r, const Row& alt, const Row& extra) const;
    std::vector<Row> AdvancedRows(const Lang& lang, const std::string& fmt, const std::string& bottomrow) const;
    std::vector<Row> KanaRowsLayout() const;
    std::vector<Row> FlickRows() const;
    std::vector<Row> EmojiRows(const std::string& layout, float U, const State& st) const;

    std::vector<Lang> m_langs;
    std::map<std::string, size_t> m_byCode;
    std::vector<Group> m_groups;
    std::map<std::string, std::pair<std::string, std::vector<LetterRow>>> m_formats;
    std::vector<std::string> m_formatOrder;
    std::set<std::string> m_ansi;
    std::map<std::string, std::pair<std::optional<Pair>, Pair>> m_isoKeys;
    std::map<std::string, std::vector<Pair>> m_topFill;
    std::vector<std::string> m_kanaRows;
    std::vector<std::vector<std::optional<std::string>>> m_flick;
    std::map<std::string, std::string> m_dakuten, m_small, m_flickMod, m_romaji, m_jaPunct, m_koShift, m_romajiHint,
        m_types, m_fn, m_upper, m_lower, m_base;
    std::set<std::string> m_marks, m_alpha, m_digit;
    Hangul m_hangul;
    std::map<std::string, std::map<std::string, std::string>> m_viTones;
    std::string m_viVowels;
    std::map<std::string, std::map<std::string, Rgba>> m_themes;
    std::vector<std::string> m_themeOrder;
    std::vector<EmojiCat> m_emojiCats;
    std::map<std::string, std::vector<std::string>> m_toned;
    mutable int m_emojiPages = 1;
};

// UTF-8 helpers: a string as its characters (code points), and how many there are.
std::vector<std::string> Chars(const std::string& s);
size_t Length(const std::string& s);

}  // namespace rv::kb
