#include "kb_layout.h"

#include <algorithm>
#include <numeric>

#include "nlohmann/json.hpp"
#include "platform/assets.h"
#include "rc/platform.h"

namespace rv::kb {

using json = nlohmann::ordered_json;  // (the accent sets' order matters: the key shows the next one's first letter)

std::vector<std::string> Chars(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char b = static_cast<unsigned char>(s[i]);
        const size_t n = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 1;
        out.push_back(s.substr(i, n));
        i += n;
    }
    return out;
}

size_t Length(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

namespace {

// BetterFrame's K() and its little helpers
Key K(const std::string& label, float w = 1) {
    Key k;
    k.label = label;
    k.w = w;
    return k;
}
Key Mod(const std::string& label, float w, const std::string& act, bool center = false) {
    Key k = K(label, w);
    k.mod = true;
    k.act = act;
    k.center = center;
    return k;
}
Key Icon(float w, const std::string& act, const std::string& icon) {
    Key k = Mod("", w, act);
    k.icon = icon;
    return k;
}
Key Spacer(float w) {
    Key k = K("", w);
    k.spacer = true;
    return k;
}
Key Space(float w) {
    Key k = K("", w);
    k.act = "space";
    return k;
}
Key Enter(float w, bool tall = false, bool center = false) {
    Key k = K("Enter", w);
    k.go = true;
    k.act = "enter";
    k.tall = tall;
    k.center = center;
    return k;
}
Key Letter(const std::string& c) {
    Key k = K(c);
    k.ch = c;
    return k;
}
Key Sym(const Pair& p, float w = 1) {
    Key k = K(p[0], w);
    k.sym = p;
    return k;
}
Row Syms(const std::vector<Pair>& pairs, float w = 1) {
    Row r;
    for (const Pair& p : pairs) r.push_back(Sym(p, w));
    return r;
}
Row Ins(const std::vector<std::string>& list, float w) {
    Row r;
    for (const std::string& c : list) {
        Key k = K(c, w);
        k.ins = c;
        r.push_back(k);
    }
    return r;
}
Row Digits(float w = 1) {
    return Ins({"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"}, w);
}
Row LKeys(const LetterRow& row) {
    Row r;
    for (const RowItem& c : row) r.push_back(c.pair ? Sym(*c.pair) : Letter(c.c));
    return r;
}
float Sum(const Row& r) {
    float s = 0;
    for (const Key& k : r) s += k.w;
    return s;
}
Row operator+(Row a, const Row& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// Makes a row exactly U key widths wide: stretches Space / Enter / Back / Shift, or shrinks every key.
Row& Fit(Row& row, float U) {
    const float w = Sum(row);
    if (w < U - 1e-6f) {
        Key* flex = nullptr;
        for (Key& k : row)
            if (k.act == "space") { flex = &k; break; }
        if (!flex)
            for (auto it = row.rbegin(); it != row.rend(); ++it)
                if (it->act == "enter" || it->act == "shift" || it->act == "back" || it->act == "del") { flex = &*it; break; }
        if (!flex)
            for (Key& k : row)
                if (k.act == "tab" || k.act == "caps") { flex = &k; break; }
        if (flex) flex->w += U - w;  // (rows of plain letters just stay centred)
    } else if (w > U + 1e-6f) {
        for (Key& k : row) k.w *= U / w;
    }
    return row;
}

std::optional<Pair> ReadPair(const json& j) {
    if (!j.is_array() || j.size() != 2) return std::nullopt;
    return Pair{j[0].get<std::string>(), j[1].get<std::string>()};
}
std::optional<std::vector<Pair>> ReadPairs(const json& j) {
    if (!j.is_array()) return std::nullopt;
    std::vector<Pair> out;
    for (const auto& p : j) out.push_back(*ReadPair(p));
    return out;
}
LetterRow ReadRow(const json& j) {
    LetterRow r;
    if (j.is_string()) {
        for (const std::string& c : Chars(j.get<std::string>())) r.push_back({c, std::nullopt});
    } else {
        for (const auto& c : j) {
            if (c.is_string()) r.push_back({c.get<std::string>(), std::nullopt});
            else r.push_back({"", ReadPair(c)});
        }
    }
    return r;
}
std::map<std::string, std::string> ReadMap(const json& j) {
    std::map<std::string, std::string> m;
    if (j.is_object())
        for (auto& [k, v] : j.items())
            if (v.is_string()) m[k] = v.get<std::string>();
    return m;
}
bool HasLatin(const LetterRow& r) {
    for (const RowItem& c : r)
        if (!c.pair && c.c.size() == 1 && c.c[0] >= 'a' && c.c[0] <= 'z') return true;
    return false;
}
bool RowHasPair(const LetterRow& r, const Pair& p) {
    for (const RowItem& c : r)
        if (c.pair && *c.pair == p) return true;
    return false;
}

}  // namespace

Data& Data::Get() {
    static Data d;
    return d;
}

bool Data::Load(const Assets& assets) {
    if (!m_langs.empty()) return true;  // (once)
    try {
        const json d = json::parse(assets.ReadText("keyboard/data.json"));
        for (const auto& l : d["langs"]) {
            Lang L;
            L.code = l["code"];
            L.name = l["name"];
            L.currency = l["currency"];
            L.space = l.value("space", "");
            L.ime = l["ime"].is_string() ? l["ime"].get<std::string>() : "";
            L.labels = l["labels"].is_string() ? l["labels"].get<std::string>() : "";
            L.fmtname = l["fmtname"].is_string() ? l["fmtname"].get<std::string>() : "";
            L.native = l["native"];
            L.pick = *ReadPair(l["pick"]);
            for (const auto& r : l["rows"]) L.rows.push_back(ReadRow(r));
            for (const auto& layer : l["layers"]) {
                L.layers.push_back(ReadMap(layer));
                L.layerSample.push_back(layer.empty() ? "" : layer.begin().value().get<std::string>());
            }
            if (auto e = ReadPairs(l["extra"])) L.extra = *e;
            L.numrow = ReadPairs(l["numrow"]);
            L.after0 = ReadPairs(l["after0"]);
            L.punct = ReadPairs(l["punct"]);
            L.mini = ReadPairs(l["mini"]);
            L.topleft = ReadPair(l["topleft"]);
            L.shift = ReadMap(l["shift"]);
            L.caseMap = ReadMap(l["case"]);
            L.romaji = l.value("romaji", false);
            L.kana = l.value("kana", false);
            L.flick = l.value("flick", false);
            L.rtl = l.value("rtl", false);
            L.reformat = l.value("reformat", false);
            m_byCode[L.code] = m_langs.size();
            m_langs.push_back(std::move(L));
        }
        for (const auto& g : d["groups"]) m_groups.push_back({g[0], g[1], g[2].get<std::vector<std::string>>()});
        for (auto& [k, v] : d["formats"].items()) {
            std::vector<LetterRow> rows;
            for (const auto& r : v[1]) rows.push_back(ReadRow(r));
            m_formats[k] = {v[0].get<std::string>(), rows};
        }
        m_formatOrder = d["format_order"].get<std::vector<std::string>>();
        for (const auto& c : d["ansi"]) m_ansi.insert(c.get<std::string>());
        for (auto& [k, v] : d["iso_keys"].items()) m_isoKeys[k] = {ReadPair(v[0]), *ReadPair(v[1])};
        for (auto& [k, v] : d["top_fill"].items()) m_topFill[k] = *ReadPairs(v);
        m_kanaRows = d["kana_rows"].get<std::vector<std::string>>();
        for (const auto& r : d["flick"]) {
            std::vector<std::optional<std::string>> row;
            for (const auto& f : r) row.push_back(f.is_string() ? std::optional<std::string>(f.get<std::string>()) : std::nullopt);
            m_flick.push_back(row);
        }
        m_dakuten = ReadMap(d["dakuten"]);
        m_small = ReadMap(d["small"]);
        m_flickMod = ReadMap(d["flick_mod"]);
        m_romaji = ReadMap(d["romaji"]);
        m_jaPunct = ReadMap(d["ja_punct"]);
        m_koShift = ReadMap(d["ko_shift"]);
        m_romajiHint = ReadMap(d["romaji_hint"]);
        m_types = ReadMap(d["types"]);
        m_fn = ReadMap(d["fn"]);
        const auto& h = d["hangul"];
        m_hangul.cho = Chars(h["cho"].get<std::string>());
        m_hangul.jung = Chars(h["jung"].get<std::string>());
        m_hangul.jong = h["jong"].get<std::vector<std::string>>();
        m_hangul.vv = ReadMap(h["vv"]);
        m_hangul.cc = ReadMap(h["cc"]);
        for (auto& [v, tones] : d["vi_tones"].items()) m_viTones[v] = ReadMap(tones);
        m_viVowels = d["vi_vowels"];
        for (auto& [name, colors] : d["themes"].items())
            for (auto& [k, v] : colors.items())
                m_themes[name][k] = {v[0], v[1], v[2], v.size() > 3 ? v[3].get<int>() : 255};
        m_themeOrder = d["theme_order"].get<std::vector<std::string>>();
        for (const auto& c : d["emoji"]["cats"]) m_emojiCats.push_back({c[0], c[1], c[2].get<std::vector<std::string>>()});
        for (auto& [k, v] : d["emoji"]["toned"].items()) m_toned[k] = v.get<std::vector<std::string>>();
        const auto& ch = d["chars"];
        m_upper = ReadMap(ch["upper"]);
        m_lower = ReadMap(ch["lower"]);
        m_base = ReadMap(ch["base"]);
        for (const auto& c : ch["marks"]) m_marks.insert(c.get<std::string>());
        for (const std::string& c : Chars(ch["alpha"].get<std::string>())) m_alpha.insert(c);
        for (const std::string& c : Chars(ch["digit"].get<std::string>())) m_digit.insert(c);
        const auto& def = d["defaults"];
        pitchMinimal = {def["pitch_minimal"][0], def["pitch_minimal"][1]};
        pitchFull = {def["pitch_full"][0], def["pitch_full"][1]};
        pitchAdvanced = {def["pitch_advanced"][0], def["pitch_advanced"][1]};
        scale = def["scale"];
        distance = def["distance"];
        drop = def["drop"];
        tilt = def["tilt"];
    } catch (const std::exception& e) {
        RC_LOGE("Keyboard: data.json: %s", e.what());
        return false;
    }
    RC_LOGI("Keyboard: %zu languages", m_langs.size());
    return !m_langs.empty();
}

const Lang& Data::Language(const std::string& code) const {
    static const Lang none;
    if (m_langs.empty()) return none;  // (its data didn't load)
    auto it = m_byCode.find(code);
    return m_langs[it == m_byCode.end() ? 0 : it->second];
}

std::string Data::FormatName(const std::string& fmt) const {
    auto it = m_formats.find(fmt);
    return it == m_formats.end() ? fmt : it->second.first;
}

// ---- characters ----------------------------------------------------------------------

std::string Data::Upper(const std::string& c, const Lang& lang) const {
    if (auto it = lang.caseMap.find(c); it != lang.caseMap.end()) return it->second;
    const auto cs = Chars(c);
    if (cs.size() != 1) return c;  // (Python: never one letter into two, and a word's upper() is longer than 1)
    // Hiragana become katakana
    if (c.size() == 3 && static_cast<unsigned char>(c[0]) == 0xE3) {
        const uint32_t cp = ((c[0] & 0x0F) << 12) | ((c[1] & 0x3F) << 6) | (c[2] & 0x3F);
        if (cp >= 0x3041 && cp <= 0x3096) {
            const uint32_t k = cp + 0x60;
            return {char(0xE0 | (k >> 12)), char(0x80 | ((k >> 6) & 0x3F)), char(0x80 | (k & 0x3F))};
        }
    }
    if (auto it = m_koShift.find(c); it != m_koShift.end()) return it->second;
    if (auto it = m_upper.find(c); it != m_upper.end()) return it->second;
    return c;
}

std::string Data::Shifted(const std::string& c, const Lang& lang) const {
    auto it = lang.shift.find(c);
    return it != lang.shift.end() && !it->second.empty() ? it->second : Upper(c, lang);
}

std::string Data::UpperWord(const std::string& w, const Lang& lang) const {
    std::string out;
    for (const std::string& c : Chars(w)) out += Upper(c, lang);
    return out;
}

std::string Data::LowerWord(const std::string& w) const {
    std::string out;
    for (const std::string& c : Chars(w)) {
        auto it = m_lower.find(c);
        out += it == m_lower.end() ? c : it->second;
    }
    return out;
}

std::string Data::Show(const std::string& c) const {
    return Chars(c).size() == 1 && m_marks.count(c) ? "◌" + c : c;
}

std::string Data::Types(const std::string& c) const {
    auto it = m_types.find(c);
    return it == m_types.end() ? c : it->second;
}

std::string Data::Base(const std::string& c) const {
    auto it = m_base.find(c);
    return it == m_base.end() ? c : it->second;
}

std::string Data::ViTone(const std::string& vowel, const std::string& tone) const {
    auto it = m_viTones.find(vowel);
    return it == m_viTones.end() ? std::string() : Find(it->second, tone);
}

const std::map<std::string, Data::Rgba>& Data::Theme(const std::string& name) const {
    auto it = m_themes.find(name);
    return it == m_themes.end() ? m_themes.at("steam") : it->second;
}

std::string Data::WithTone(const std::string& e, int tone) const {
    if (tone < 0 || tone >= 5) return e;
    std::string plain = e;
    for (size_t p; (p = plain.find("️")) != std::string::npos;) plain.erase(p, 3);
    auto it = m_toned.find(plain);
    return it == m_toned.end() || int(it->second.size()) <= tone ? e : it->second[tone];
}

// ---- layouts (overlay.py, from _letter_rows on) -------------------------------------------

std::vector<LetterRow> Data::LetterRows(const Lang& lang, const std::string& fmt) const {
    std::vector<LetterRow> rows = lang.rows;
    auto f = m_formats.find(fmt);
    if (f != m_formats.end() && lang.reformat) {
        std::vector<LetterRow> out;
        for (int i = 0; i < 3; i++) {
            LetterRow r = f->second.second[i];
            for (const RowItem& c : rows[i]) {
                const bool latin = !c.pair && c.c.size() == 1 && c.c[0] >= 'a' && c.c[0] <= 'z';
                if (!latin && !(!c.pair && c.c == ";")) r.push_back(c);
            }
            out.push_back(r);
        }
        return out;
    }
    return rows;
}

std::vector<Row> Data::KanaRowsLayout() const {
    std::vector<Row> rows;
    for (const std::string& r : m_kanaRows) {
        Row row;
        for (const std::string& c : Chars(r)) row.push_back(Letter(c));
        rows.push_back(row);
    }
    rows[0].push_back(Icon(1, "back", "backspace"));
    rows[1].push_back(Mod("゛゜", 1, "dakuten", true));
    rows[2].push_back(Mod("小", 1, "small", true));
    rows[3].push_back(Enter(1));
    rows[4].push_back(Mod("カナ", 1, "shift", true));
    rows.push_back({Mod(".?123", 2, "p123"), Mod("Tab", 1.2f, "tab"), Space(7.8f)});
    return rows;
}

std::vector<Row> Data::FlickRows() const {
    const float side = 2.0f, mid = 7.0f / 3;
    std::vector<Row> rows;
    for (const auto& r : m_flick) {
        Row row;
        for (const auto& f : r) {
            if (!f) {
                row.push_back(Mod("゛゜小", mid, "flickmod", true));
            } else {
                const auto cs = Chars(*f);
                Key k = K(cs[0] != "、" ? cs[0] : "、。?!", mid);
                k.isFlick = true;
                k.flick = cs;
                k.center = true;
                row.push_back(k);
            }
        }
        rows.push_back(row);
    }
    const Row left = {Mod("カナ", side, "shift", true), Icon(side, "left", "left"), Mod(".?123", side, "p123"),
                      Icon(side, "emoji", "emoji")};
    const Row right = {Icon(side, "back", "backspace"), Icon(side, "right", "right"), Space(side), Enter(side)};
    std::vector<Row> out;
    for (size_t i = 0; i < rows.size() && i < 4; i++) out.push_back(Row{left[i]} + rows[i] + Row{right[i]});
    return out;
}

std::vector<Row> Data::TopRows(const std::optional<std::vector<Pair>>& num, bool euro, const Lang* lang) const {
    const std::vector<Pair> after0 = lang && lang->after0 ? *lang->after0
                                     : euro ? std::vector<Pair>{{"'", "?"}, {"=", "+"}} : std::vector<Pair>{{"-", "_"}, {"=", "+"}};
    const Pair topleft = lang && lang->topleft ? *lang->topleft : Pair{"`", "~"};
    Row f = {Mod("Esc", 1, "esc")};
    for (int n = 1; n <= 12; n++) f.push_back(Mod("F" + std::to_string(n), 1, "f" + std::to_string(n), true));
    f.push_back(Mod("Delete", 2, "del"));
    Row digits = num ? Syms(*num)
                     : Syms({{"1", "!"}, {"2", "@"}, {"3", "#"}, {"4", "$"}, {"5", "%"}, {"6", "^"}, {"7", "&"},
                             {"8", "*"}, {"9", "("}, {"0", ")"}});
    return {f, Syms({topleft}) + digits + Syms(after0) + Row{Icon(2, "back", "backspace")}};
}

Row Data::BottomFull(const std::string& bottomrow) const {
    const Row arrows = {Icon(1.1f, "left", "left"), Icon(1.1f, "up", "up"), Icon(1.1f, "down", "down"),
                        Icon(1.1f, "right", "right")};
    if (bottomrow == "laptop")  // like a laptop: Ctrl in the corner, then Fn, Start, Alt
        return Row{Mod("Ctrl", 1.4f, "ctrl"), Mod("Fn", 1.1f, "fn"), Mod("Start", 1.2f, "start"), Mod("Alt", 1.0f, "alt-mod"),
                   Space(4.9f)} + arrows;
    return Row{Mod(".?123", 1.4f, "p123"), Mod("Fn", 1.0f, "fn"), Mod("Ctrl", 1.1f, "ctrl"), Mod("Start", 1.2f, "start"),
               Mod("Alt", 1.0f, "alt-mod"), Space(4.9f)} + arrows;
}

bool Data::IsIso(const Lang& lang) const { return !m_ansi.count(lang.code) && !lang.kana; }

std::vector<Row> Data::IsoRows(const Lang& lang, const std::vector<LetterRow>& r, const Row& alt, const Row& extra) const {
    std::set<std::string> used;
    for (const auto& row : r)
        for (const RowItem& c : row) used.insert(c.First());
    if (lang.numrow)
        for (const Pair& p : *lang.numrow) used.insert(p[0]);
    if (lang.after0)
        for (const Pair& p : *lang.after0) used.insert(p[0]);
    used.insert(lang.topleft ? (*lang.topleft)[0] : "`");
    std::optional<Pair> iso = Pair{"<", ">"};
    Pair hashk{"\\", "|"};
    if (auto it = m_isoKeys.find(lang.code); it != m_isoKeys.end()) {
        iso = it->second.first;
        hashk = it->second.second;
    }
    const float W = 15.0f, ENTER = 1.25f;
    Row row3 = LKeys(r[0]);
    auto tf = m_topFill.find(lang.code);
    for (const Key& k : Syms(tf != m_topFill.end() ? tf->second : std::vector<Pair>{{"[", "{"}, {"]", "}"}}))
        if (!used.count((*k.sym)[0]) && row3.size() < 12) {
            row3.push_back(k);
            used.insert((*k.sym)[0]);
        }
    Row row4 = LKeys(r[1]);
    const std::vector<Pair> after = r[1].size() > 9 ? std::vector<Pair>{hashk, {";", ":"}, {"'", "\""}}
                                                    : std::vector<Pair>{{";", ":"}, {"'", "\""}, hashk};
    for (const Key& k : Syms(after))
        if (!used.count((*k.sym)[0]) && row4.size() < 12) {
            row4.push_back(k);
            used.insert((*k.sym)[0]);
        }
    // Tab and Caps take up whatever is left, so the tall Enter lines up exactly on both rows
    row3 = Row{Mod("Tab", std::max(1.0f, W - ENTER - float(row3.size())), "tab")} + row3 + Row{Enter(ENTER, true)};
    row4 = Row{Mod("Caps", std::max(1.0f, W - ENTER - float(row4.size())), "caps")} + row4 + Row{Spacer(ENTER)};
    const Row lead = !iso || used.count((*iso)[0]) ? Row{} : Syms({*iso});
    Row row5 = Row{Mod("Shift", 1.25f, "shift")} + lead + LKeys(r[2]) + alt + extra;
    const std::vector<Pair> punct = lang.punct && !lang.punct->empty() ? *lang.punct
                                    : lang.code == "en-GB" ? std::vector<Pair>{{",", "<"}, {".", ">"}, {"/", "?"}}
                                                           : std::vector<Pair>{{",", ";"}, {".", ":"}, {"-", "_"}, {"/", "?"}};
    for (const Key& k : Syms(punct)) {
        const std::string& c = (*k.sym)[0];
        bool inRow = false;
        for (const Key& x : row5)
            if (x.sym && (*x.sym)[0] == c) inRow = true;
        if (!used.count(c) && !inRow && Sum(row5) + 1 + 1.25f <= W + 1e-6f) row5.push_back(k);
    }
    row5.push_back(Mod("Shift", std::max(1.25f, W - Sum(row5)), "shift"));
    return {row3, row4, row5};
}

std::vector<Row> Data::LetterRowsLayout(const std::string& layout, const Lang& lang, const std::string& fmt,
                                        const std::string& bottomrow) const {
    if (lang.flick) return FlickRows();
    if (lang.kana) return KanaRowsLayout();
    const std::vector<LetterRow> r = LetterRows(lang, fmt);
    const LetterRow &r1 = r[0], &r2 = r[1], &r3 = r[2];
    Row alt;
    if (!lang.layers.empty()) {
        Key a = Mod("", 1, "alt");
        a.altkey = true;
        alt.push_back(a);
    }
    const Row extra = Syms(lang.extra);
    std::optional<Row> num;
    if (lang.numrow) num = Syms(*lang.numrow);
    if (layout == "minimal") {
        // like a phone: comma and full stop beside Space, the Shift row's free spots get ? ' -
        std::set<std::string> used;
        for (const auto& row : r)
            for (const RowItem& c : row) used.insert(c.First());
        if (lang.numrow)
            for (const Pair& p : *lang.numrow) used.insert(p[0]);
        Row row2 = LKeys(r1);
        if (r1.size() < 11 && !used.count("'")) row2 = row2 + Syms({{"'", "\""}});
        if (row2.size() > r1.size()) used.insert("'");
        Row tail4 = alt + extra;
        const float room = std::max({11.f, float(row2.size()), float(r2.size()) + (r2.size() <= 9 ? 1.5f + 0.5f : 1.3f)});
        std::vector<Pair> order = {{"?", "!"}, {"'", "\""}, {"-", "_"}};
        const std::string l2 = lang.code.substr(0, 2);
        if (l2 == "fr" || l2 == "it" || l2 == "uk") order = {{"'", "\""}, {"?", "!"}, {"-", "_"}};
        for (const Key& k : Syms(order))
            if (1 + float(r3.size()) + float(tail4.size()) + 1 <= room + 1e-6f && !used.count((*k.sym)[0])) {
                tail4.push_back(k);
                used.insert((*k.sym)[0]);
            }
        Row bottom;
        float sp, tab;
        if (bottomrow == "laptop") {
            bottom = {Mod("Ctrl", 1.1f, "ctrl"), Mod("Fn", 1.0f, "fn"), Mod(".?123", 1.4f, "p123")};
            sp = 4.5f;
            tab = 1.0f;
        } else {
            bottom = {Mod(".?123", 1.6f, "p123")};
            sp = 6.2f;
            tab = 1.2f;
        }
        if (!used.count(",")) bottom = bottom + Syms({{",", ";"}});
        bottom.push_back(Space(sp));
        if (!used.count(".")) bottom = bottom + Syms({{".", ":"}});
        bottom.push_back(Mod("Tab", tab, "tab"));
        if (lang.mini) num = (num ? *num : Digits()) + Syms(*lang.mini);
        Row row3 = r2.size() <= 9 ? Row{Spacer(0.5f)} : Row{};
        row3 = row3 + LKeys(r2) + Row{Enter(r2.size() <= 9 ? 1.5f : 1.3f)};
        return {(num ? *num : Digits()) + Row{Icon(1, "back", "backspace")}, row2, row3,
                Row{Mod("Shift", 1, "shift")} + LKeys(r3) + tail4, bottom};
    }
    if (IsIso(lang)) {
        std::vector<Row> out = TopRows(num ? std::optional<std::vector<Pair>>(*lang.numrow) : std::nullopt,
                                       lang.code != "en-GB", &lang);
        for (Row& row : IsoRows(lang, r, alt, extra)) out.push_back(row);
        out.push_back(BottomFull(bottomrow));
        return out;
    }
    Row row3 = Row{Mod("Tab", 1.5f, "tab")} + LKeys(r1);
    const bool own = lang.punct && lang.punct->empty();  // its own punctuation among the letters
    for (const Key& k : (own ? Row{} : Syms({{"[", "{"}, {"]", "}"}})) + Row{Sym({"\\", "|"}, 1.5f)})
        if (Sum(row3) + k.w <= 15.01f) row3.push_back(k);
    row3[0].w += std::max(0.0f, 15 - Sum(row3));
    Row row4 = Row{Mod("Caps", 1.75f, "caps")} + LKeys(r2);
    for (const Key& k : Syms({{";", ":"}, {"'", "\""}})) {
        if (RowHasPair(r1, *k.sym) || RowHasPair(r3, *k.sym)) continue;  // already among the letters (Dvorak)
        if (Sum(row4) + k.w + 1.75f <= 15.01f) row4.push_back(k);
    }
    row4.push_back(Enter(std::max(1.75f, 15 - Sum(row4))));
    Row row5 = Row{Mod("Shift", 2.25f, "shift")} + LKeys(r3) + alt + extra;
    if (!own)
        for (const Key& k : Syms({{",", "<"}, {".", ">"}, {"/", "?"}}))
            if (!RowHasPair(r3, *k.sym)) row5.push_back(k);
    row5.push_back(Mod("Shift", std::max(1.25f, 15 - Sum(row5)), "shift"));
    if (row5.back().w > row5.front().w) std::swap(row5.back().w, row5.front().w);  // the left Shift gets the bigger key
    std::vector<Row> out = TopRows(num ? std::optional<std::vector<Pair>>(*lang.numrow) : std::nullopt, false, &lang);
    out.push_back(row3);
    out.push_back(row4);
    out.push_back(row5);
    out.push_back(BottomFull(bottomrow));
    return out;
}

std::vector<Row> Data::AdvancedRows(const Lang& lang, const std::string& fmt, const std::string& bottomrow) const {
    if (lang.flick) return FlickRows();
    if (lang.kana) return KanaRowsLayout();
    std::vector<Row> rows = LetterRowsLayout("full", lang, fmt, bottomrow);
    Row fkeys = {Mod("Esc", 1, "esc"), Spacer(1)};
    for (int g = 0; g < 3; g++) {
        for (int n = g * 4 + 1; n <= g * 4 + 4; n++) fkeys.push_back(Mod("F" + std::to_string(n), 1, "f" + std::to_string(n), true));
        if (g < 2) fkeys.push_back(Spacer(0.5f));
    }
    const Row bottom = {Mod("Ctrl", 1.5f, "ctrl"), Mod("Start", 1.25f, "start"), Mod("Alt", 1.25f, "alt-mod"), Space(6.5f),
                        Mod("Alt", 1.25f, "alt-mod"), Mod("Fn", 1.0f, "fn"), Mod("Ctrl", 1.5f, "ctrl")};
    std::vector<Row> main = {fkeys};
    for (size_t i = 1; i < 5 && i < rows.size(); i++) main.push_back(rows[i]);
    main.push_back(bottom);
    float U = 15;
    for (const Row& r : main) U = std::max(U, Sum(r));
    for (Row& r : main) Fit(r, U);
    auto gap = [] { return Spacer(0.5f); };
    auto nav = [](const std::string& label, const std::string& act) { return Mod(label, 1, act, true); };
    auto np = [](const std::string& d, const std::string& navk, const std::string& lab, float w = 1) {
        Key k = K(d, w);
        k.ins = d;
        k.np = navk.empty() ? "-" : navk;  // ("-": a numpad key that does nothing with Num Lock off)
        k.nplabel = lab;
        return k;
    };
    Key plus = K("+", 1);
    plus.ins = "+";
    plus.tall = true;
    auto insKey = [](const std::string& c) {
        Key k = K(c, 1);
        k.ins = c;
        return k;
    };
    Key enter = Enter(1, true, true);
    const std::vector<Row> side = {
        {nav("PrtSc", "prtsc"), nav("ScrLk", "scrlk"), nav("Pause", "pause"), gap(), Spacer(4)},
        {nav("Ins", "ins"), nav("Home", "home"), nav("PgUp", "pgup"), gap(), Mod("Num", 1, "numlock", true), insKey("/"),
         insKey("*"), insKey("-")},
        {nav("Del", "del"), nav("End", "end"), nav("PgDn", "pgdn"), gap(), np("7", "HOME", "Home"), np("8", "UP", "↑"),
         np("9", "PAGEUP", "PgUp"), plus},
        {Spacer(3), gap(), np("4", "LEFT", "←"), np("5", "", ""), np("6", "RIGHT", "→"), Spacer(1)},
        {Spacer(1), Icon(1, "up", "up"), Spacer(1), gap(), np("1", "END", "End"), np("2", "DOWN", "↓"),
         np("3", "PAGEDOWN", "PgDn"), enter},
        {Icon(1, "left", "left"), Icon(1, "down", "down"), Icon(1, "right", "right"), gap(), np("0", "INSERT", "Ins", 2),
         np(".", "DELETE", "Del"), Spacer(1)},
    };
    std::vector<Row> out;
    for (size_t i = 0; i < main.size() && i < side.size(); i++) out.push_back(main[i] + Row{gap()} + side[i]);
    return out;
}

float Data::UnitsFor(const std::string& layout, const std::string& langCode, const std::string& fmt, const State& st) const {
    const Lang& lang = Language(langCode);
    const float base = layout == "minimal" || lang.kana ? 11.f : 15.f;
    const std::vector<Row> rows = layout == "advanced" ? AdvancedRows(lang, fmt, st.bottomrow)
                                                       : LetterRowsLayout(layout, lang, fmt, st.bottomrow);
    float m = base;
    for (const Row& r : rows) m = std::max(m, Sum(r));
    return m;
}

std::vector<Row> Data::EmojiRows(const std::string& layout, float U, const State& st) const {
    int cat = st.ecat;
    const int cols = layout == "minimal" ? 10 : 14;
    const float w = U / cols;
    const std::set<std::string> favset(st.efav.begin(), st.efav.end());
    const int ntabs = int(m_emojiCats.size()) + 1;
    Row tabs;
    for (size_t i = 0; i < m_emojiCats.size(); i++) {
        Key k = K(m_emojiCats[i].icon, U / ntabs);
        k.bar = true;
        k.act = "ecat:" + std::to_string(i);
        k.on = int(i) == cat;
        k.emoji = true;
        k.tip = m_emojiCats[i].name;
        tabs.push_back(k);
    }
    Key fav = K("⭐", U / ntabs);
    fav.bar = true;
    fav.act = "ecat:" + std::to_string(kFavTab);
    fav.on = cat == kFavTab;
    fav.emoji = true;
    fav.tip = "Favorites";
    tabs.insert(tabs.begin() + 1, fav);
    const int tone = st.etone;
    std::vector<std::string> all;
    if (cat == kFavTab) all = st.efav;
    else if (cat == 0) all = st.erecent;
    else if (cat > 0 && cat < int(m_emojiCats.size()))
        for (const std::string& e : m_emojiCats[cat].items) all.push_back(WithTone(e, tone));
    const int nrows = layout == "minimal" ? 3 : 4;  // the same height as the letters
    const int per = cols * nrows;
    const int pages = std::max(1, (int(all.size()) + per - 1) / per);
    const int pg = std::min(st.epage, pages - 1);
    m_emojiPages = pages;
    std::vector<Row> grid;
    for (int r = 0; r < nrows; r++) {
        Row row;
        for (int i = pg * per + r * cols; i < pg * per + (r + 1) * cols && i < int(all.size()); i++) {
            Key k = K(all[i], w);
            k.ins = all[i];
            k.emoji = true;
            k.fav = favset.count(all[i]) > 0;
            row.push_back(k);
        }
        auto message = [&](const char* text) {
            Key k = K(text, U);
            k.bar = true;
            k.act = "noop";
            k.fitlabel = true;
            return Row{k};
        };
        if (row.empty() && r == 0 && cat == 0) row = message("Emoji you use show up here");
        if (row.empty() && r == 0 && cat == kFavTab) row = message("Hold any emoji to add it to your Favorites");
        if (!row.empty() && !row[0].bar)
            while (int(row.size()) < cols) row.push_back(Spacer(w));
        grid.push_back(row.empty() ? Row{Spacer(U)} : row);
    }
    const float s = U / 11;
    Key left = Icon(1.0f * s, pg > 0 ? "epage:-1" : "noop", "left");
    left.off = pg == 0;
    Key right = Icon(1.0f * s, pg < pages - 1 ? "epage:1" : "noop", "right");
    right.off = pg >= pages - 1;
    Key hand = Mod(WithTone("✋", tone), 1.0f * s, "etone");
    hand.emoji = true;
    hand.tip = "Skin tone";
    Row bottom = {Mod("ABC", 1.5f * s, "abc"), left,
                  Mod(std::to_string(pg + 1) + " / " + std::to_string(pages), 1.0f * s, "noop", true), right, hand,
                  Space(4.0f * s), Icon(1.5f * s, "back", "backspace")};
    std::vector<Row> out = {Fit(tabs, U)};
    for (Row& r : grid) out.push_back(Fit(r, U));
    out.push_back(Fit(bottom, U));
    return out;
}

std::vector<Row> Data::RowsFor(const std::string& layoutIn, const std::string& page, const std::string& langCode,
                               const std::string& fmt, const State& st) const {
    std::string layout = layoutIn;
    const Lang& lang = Language(langCode);
    const float U = UnitsFor(layout, langCode, fmt, st);
    if (page == "emoji") return EmojiRows(lang.flick ? "minimal" : layout, U, st);
    auto pickKey = [](const std::string& label, float w, const std::string& act, bool on) {
        Key k = K(label, w);
        k.bar = true;
        k.act = act;
        k.on = on;
        k.pick = "";
        return k;
    };
    if (page == "fmt") {  // the key layout picker
        Row opts;
        for (const std::string& f : m_formatOrder) {
            std::string name, sample;
            std::vector<LetterRow> rows;
            if (f == "default") {
                name = "Default (" + lang.native + ")";
                rows = LetterRows(lang, "default");
            } else {
                auto it = m_formats.find(f);
                if (it == m_formats.end()) continue;
                name = it->second.first;
                rows = it->second.second;
            }
            for (size_t i = 0; i < rows[0].size() && i < 6; i++) {
                if (i) sample += " ";
                sample += UpperWord(rows[0][i].First(), lang);
            }
            Key k = K(name, U / 4);
            k.bar = true;
            k.act = "fmt:" + f;
            k.on = f == fmt;
            k.fitlabel = true;
            k.sample = sample;
            opts.push_back(k);
        }
        const size_t half = std::min<size_t>(4, opts.size());
        Row a(opts.begin(), opts.begin() + half), b(opts.begin() + half, opts.end());
        return {Fit(a, U), Fit(b, U)};
    }
    if (page == "lang") {  // each language once, in its own name, big enough to read
        const std::string cur = lang.code.substr(0, lang.code.find('-'));
        for (const Group& g : m_groups) {
            if (g.key != st.lgroup) continue;
            Row row;  // a language with more than one keyboard: which one
            for (const std::string& code : g.codes) {
                const Lang& l = Language(code);
                row.push_back(pickKey(l.pick[1].empty() ? l.native : l.pick[1], U / std::max<size_t>(3, g.codes.size()),
                                      "lang:" + code, code == lang.code));
            }
            Row back = {pickKey("‹  " + g.name, U / 3, "lback", false)};
            return {Fit(row, U), Fit(back, U)};
        }
        const int per = kLangCols * kLangRows;
        const int pages = std::max(1, (int(m_groups.size()) + per - 1) / per);
        const int pg = std::max(0, std::min(pages - 1, st.lpage));
        std::vector<Row> rows;
        for (int i = pg * per; i < std::min<int>(int(m_groups.size()), (pg + 1) * per); i += kLangCols) {
            Row row;
            for (int j = i; j < std::min<int>(int(m_groups.size()), i + kLangCols); j++) {
                const Group& g = m_groups[j];
                row.push_back(pickKey(g.name + (g.codes.size() > 1 ? "  ›" : ""), U / kLangCols,
                                      g.codes.size() > 1 ? "lgroup:" + g.key : "lang:" + g.codes[0], g.key == cur));
            }
            rows.push_back(row);
        }
        while (int(rows.size()) < kLangRows) rows.push_back({Spacer(U / kLangCols)});
        if (pages > 1) {
            Key l = pickKey("", U / 8, "lpage:-1", false), r = pickKey("", U / 8, "lpage:+1", false);
            l.icon = "left";
            r.icon = "right";
            l.pick.reset();
            r.pick.reset();
            Key n = pickKey(std::to_string(pg + 1) + " / " + std::to_string(pages), U / 6, "noop", false);
            n.pick.reset();
            rows.push_back({l, n, r});
        }
        for (Row& r : rows) Fit(r, U);
        return rows;
    }
    if (page == "abc") {
        std::vector<Row> rows = layout == "advanced" ? AdvancedRows(lang, fmt, st.bottomrow)
                                                     : LetterRowsLayout(layout, lang, fmt, st.bottomrow);
        for (Row& r : rows) Fit(r, U);
        return rows;
    }
    if (lang.kana) layout = "minimal";
    const bool mini = layout == "minimal";
    const float f = mini ? 11.f / 15 : 1.0f, cw = mini ? 1.1f : 1.5f;
    const float s = 15.f / 11 * f;
    const Row bottom = {Mod("ABC", 1.6f * s, "abc"), Mod("Tab", 1.2f * s, "tab"), Space(5.6f * s), Enter(2.6f * s)};
    std::vector<Row> rows;
    if (!mini) rows.push_back(LetterRowsLayout("full", lang, fmt, st.bottomrow)[0]);
    const std::string& cur = lang.currency;
    if (page == "123") {
        rows.push_back(Digits(cw));
        rows.push_back(Ins({"#", "@", cur, "&", "*", "(", ")", "-", "_", "/"}, cw));
        rows.push_back(Ins({"!", "?", ".", ",", "'", "\"", ":", ";", "=", "+"}, cw));
        rows.push_back(Row{Mod("#+=", 1.65f * s, "psym")} + Ins({"%", "^", "~", "`", "|", "\\", "<"}, cw) +
                       Row{Icon(1.65f * s, "back", "backspace")});
    } else {
        const std::string money = cur != "$" ? "$" : "€";
        rows.push_back(Ins({"[", "]", "{", "}", "<", ">", "^", "~", "`", "|"}, cw));
        rows.push_back(Ins({"\\", "/", "=", "+", "-", "_", "*", "&", "%", money}, cw));
        rows.push_back(Ins({"@", "#", "!", "?", ";", ":", "'", "\"", ",", "."}, cw));
        rows.push_back(Row{Mod("123", 1.65f * s, "p123")} + Ins({"(", ")", ":)", ";)", "...", "->", ".com"}, cw) +
                       Row{Icon(1.65f * s, "back", "backspace")});
    }
    rows.push_back(bottom);
    for (Row& r : rows) Fit(r, U);
    return rows;
}

Row Data::BarItems(const State& st) const {
    auto b = [](const std::string& act, const std::string& icon, const std::string& tip, bool wide = false) {
        Key k = K("", 0.95f);
        k.bar = true;
        k.act = act;
        k.icon = icon;
        k.tip = tip;
        k.wide = wide;
        return k;
    };
    Key minimal = b("lay-minimal", "lay-minimal", "Minimal", true), full = b("lay-full", "lay-full", "Standard", true),
        advanced = b("lay-advanced", "lay-advanced", "Full", true);
    minimal.on = st.layout == "minimal";
    full.on = st.layout == "full";
    advanced.on = st.layout == "advanced";
    Key emoji = b("emoji", "emoji", "Emoticon", true);
    emoji.on = st.page == "emoji";
    Key swipe = b("swipe", "swipe", "Swipe typing");
    swipe.on = st.swipe;
    Key curve = b("curve", "curve", "Curved keyboard");
    curve.off = !st.curved;
    Key panel = b("panel", "panel", "Backdrop");
    panel.off = !st.panel;
    // (BetterFrame's voice typing button isn't here: RinCast has no voice typing)
    return {b("undo", "undo", "Undo"), b("cut", "cut", "Cut"), b("copy", "copy", "Copy"), b("paste", "paste", "Paste"),
            b("selall", "selall", "Select all"), Spacer(0.5f), minimal, full, advanced, Spacer(0.5f),
            b("recenter", "recenter", "Recenter"), Spacer(0.5f), emoji, swipe, curve, panel, b("hide", "close", "Close keyboard")};
}

}  // namespace rv::kb
