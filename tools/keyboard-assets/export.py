"""Exports what RinCast's keyboard needs from BetterFrame Keyboard, using BetterFrame's own code, so
the port looks and types exactly the same: its languages and layouts as data, its icons, its emoji (drawn
from Noto Color Emoji the way it draws them), its two key click sounds, and the characters to bake.

    python export.py <betterframe dir> <out dir> <emoji font>

Run by build.sh, in a container with Pillow; see there.
"""
import json
import os
import shutil
import struct
import sys
import unicodedata

SRC, OUT, EMOJI_FONT = sys.argv[1], sys.argv[2], sys.argv[3]
sys.path.insert(0, SRC)
import overlay as O  # noqa: E402
import languages as L  # noqa: E402
import emoji_data as E  # noqa: E402
import ime as I  # noqa: E402
from PIL import Image, ImageDraw, ImageFont, features  # noqa: E402

os.makedirs(OUT, exist_ok=True)
CHARS = set()


def note(s):
    if isinstance(s, str):
        CHARS.update(s)
    elif isinstance(s, (list, tuple)):
        for x in s:
            note(x)
    elif isinstance(s, dict):
        for k, v in s.items():
            note(k)
            note(v)


# ---- languages: everything but Chinese, which only types through its dictionaries
langs = []
for l in L.LANGS:
    if l["code"].startswith("zh"):
        continue
    d = dict(l)
    d["rows"] = [[list(c) if isinstance(c, tuple) else c for c in r] for r in l["rows"]]
    for k in ("extra", "numrow", "after0", "punct", "mini"):
        if d.get(k) is not None:
            d[k] = [list(p) for p in d[k]]
    if d.get("topleft") is not None:
        d["topleft"] = list(d["topleft"])
    d["pick"] = list(L.pick_label(l))
    d["native"] = L.native_format(l)
    d["reformat"] = L.can_reformat(l)
    langs.append(d)
    note(d)

groups = [[key, name, [x["code"] for x in ls if not x["code"].startswith("zh")]] for key, name, ls in L.groups()]
groups = [g for g in groups if g[2]]

kana = [chr(c) for c in range(0x3041, 0x3097)] + [chr(c) for c in range(0x30A1, 0x30F7)]
data = {
    "langs": langs,
    "groups": groups,
    "formats": {k: [n, [[list(c) if isinstance(c, tuple) else c for c in r] for r in rows]] for k, (n, rows) in L.FORMATS.items()},
    "format_order": L.FORMAT_ORDER,
    "ansi": sorted(O.ANSI),
    "iso_keys": {k: [list(a) if a else None, list(b)] for k, (a, b) in O.ISO_KEYS.items()},
    "top_fill": {k: [list(p) for p in v] for k, v in O.TOP_FILL.items()},
    "kana_rows": L.KANA_ROWS,
    "flick": L.FLICK,
    "dakuten": {c: L.dakuten(c) for c in kana if L.dakuten(c)},
    "small": {c: L.small(c) for c in kana if L.small(c)},
    "flick_mod": {c: L.flick_mod(c) for c in kana if L.flick_mod(c)},
    "romaji": L._KANA,
    "ja_punct": L.JA_PUNCT,
    "ko_shift": L.KO_SHIFT,
    "romaji_hint": O.ROMAJI_HINT,
    "types": L.TYPES,
    "fn": {k: v[0] for k, v in O.FN.items()},
    "themes": {name: {k: list(v) for k, v in dict(O.THEMES["steam"], **t).items()} for name, t in O.THEMES.items()},
    "theme_order": list(O.THEMES),
    "hangul": {"cho": I._CHO, "jung": I._JUNG, "jong": I._JONG,
               "vv": {a + b: v for (a, b), v in I._VV.items()}, "cc": {a + b: v for (a, b), v in I._CC.items()}},
    "emoji": {"cats": [[icon, name, [x[0] for x in items]] for icon, name, items in E.CATS], "toned": E.TONED},
    # Vietnamese: each vowel with each Telex tone (s f r x j), as Python's NFC composes them
    "vi_tones": {v: {t: unicodedata.normalize("NFC", v + m) for t, m in I._VI_TONES.items()}
                 for v in I._VI_VOWELS + I._VI_VOWELS.upper()},
    "vi_vowels": I._VI_VOWELS,
    "defaults": {k: O.DEFAULTS[k] for k in ("pitch_minimal", "pitch_full", "pitch_advanced", "scale", "distance", "drop", "tilt")},
}
note(data["kana_rows"]); note(data["flick"]); note(data["dakuten"]); note(data["small"]); note(data["flick_mod"])
note(data["romaji"]); note(data["ja_punct"]); note(data["ko_shift"]); note(data["romaji_hint"]); note(data["types"])
note(data["fn"]); note(data["iso_keys"]); note(data["top_fill"]); note(data["formats"])
note(list("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~…‹›€£¥₩"))
note(["Esc", "Delete", "Tab", "Caps", "Shift", "Enter", ".?123", "Fn", "Ctrl", "Start", "Alt", "ABC", "#+=", "123",
      "PrtSc", "ScrLk", "Pause", "Num", "Ins", "Home", "PgUp", "Del", "End", "PgDn", "Emoji you use show up here",
      "Hold any emoji to add it to your Favorites", "Added to Favorites", "Removed from Favorites", "Didn't catch that word",
      ":)", ";)", "...", "->", ".com", "abc", "゛゜", "小", "カナ", "゛゜小", "、。?!", "←", "↑", "→", "↓"])
for f in [f"F{n}" for n in range(1, 13)]:
    note(f)

# ---- word lists: the languages swipe typing and the word bar work in
wdir = os.path.join(OUT, "words")
os.makedirs(wdir, exist_ok=True)
for name in sorted(os.listdir(os.path.join(SRC, "words"))):
    shutil.copy(os.path.join(SRC, "words", name), os.path.join(wdir, name))
    if name.endswith(".txt"):
        with open(os.path.join(SRC, "words", name), encoding="utf-8") as f:
            CHARS.update(f.read())
CHARS.discard("\n")
CHARS.discard("\r")

# ---- what Python knows about characters: upper case (one letter only), marks, the letter under an accent
upper, marks, base = {}, [], {}
for c in sorted(CHARS):
    u = c.upper()
    if len(u) == 1 and u != c:
        upper[c] = u
        CHARS.add(u)
for c in sorted(CHARS):
    if unicodedata.category(c) in ("Mn", "Mc"):
        marks.append(c)
    b = unicodedata.normalize("NFD", c)[0]
    if b != c:
        base[c] = b
CHARS.add("◌")
lower = {c: c.lower() for c in sorted(CHARS) if len(c.lower()) == 1 and c.lower() != c}
alpha = "".join(c for c in sorted(CHARS) if c.isalpha())
digit = "".join(c for c in sorted(CHARS) if c.isdigit())
data["chars"] = {"upper": upper, "lower": lower, "marks": marks, "base": base, "alpha": alpha, "digit": digit}
data["emoji_tabs"] = {"fav": O.FAV_TAB}
with open(os.path.join(OUT, "data.json"), "w", encoding="utf-8") as f:
    json.dump(data, f, ensure_ascii=False, separators=(",", ":"))
with open(os.path.join(OUT, "charset.txt"), "w", encoding="utf-8") as f:
    f.write("".join(sorted(c for c in CHARS if ord(c) >= 32)))


# ---- icons, white on clear: the keyboard tints them
def pack(cells, cell, cols, path):
    rows = -(-len(cells) // cols)
    sheet = Image.new("RGBA", (cols * cell, rows * cell), (0, 0, 0, 0))
    out = {}
    for i, (name, im) in enumerate(cells):
        x, y = (i % cols) * cell, (i // cols) * cell
        sheet.alpha_composite(im, (x + (cell - im.width) // 2, y + (cell - im.height) // 2))
        out[name] = [x + (cell - im.width) // 2, y + (cell - im.height) // 2, im.width, im.height]
    sheet.save(path, optimize=True)
    return out, sheet.size


ICON = 192
white = (255, 255, 255, 255)
icons = []
for kind in ("undo", "cut", "copy", "paste", "selall", "lay-minimal", "lay-full", "lay-advanced", "recenter", "mic", "emoji",
             "swipe", "curve", "panel", "close", "left", "right", "up", "down", "backspace", "delete", "sound", "globe",
             "preview", "words", "search", "gear"):
    icons.append((kind, O.icon_image(kind, ICON, white, (0, 0, 0, 0))))
for kind in ("curve", "panel", "sound", "preview", "words"):
    icons.append((kind + "-off", O.icon_image(kind, ICON, white, (0, 0, 0, 0), slash=True)))

# the corner handles and the favorites star, drawn by BetterFrame's renderer on a box this size
r = O.Renderer("")
O.set_theme("steam")
O.PAL["grab"] = white
box = 120
lw = max(4, int(box * 0.16))
m = lw * 2
im = Image.new("RGBA", (2 * (box + 2 * m), box + 2 * m), (0, 0, 0, 0))
r.draw_handles(im, (m, m, m + box, m + box), (box + 3 * m, m, 2 * box + 3 * m, m + box), lw)
icons.append(("resize", im.crop((0, 0, box + 2 * m, box + 2 * m))))
icons.append(("rotate", im.crop((box + 2 * m, 0, 2 * (box + 2 * m), box + 2 * m))))
star = Image.new("RGBA", (ICON, ICON), (0, 0, 0, 0))
r.draw_star(ImageDraw.Draw(star), ICON / 2, ICON / 2, ICON * 0.48)
icons.append(("star", star))
# the letter bubble's point toward its key: as wide as the cell, as tall as half of it (drawn 4x, scaled)
tail = Image.new("RGBA", (ICON * 4, ICON * 2), (0, 0, 0, 0))
ImageDraw.Draw(tail).polygon([(0, 0), (ICON * 4, 0), (ICON * 2, ICON * 2)], fill=white)
icons.append(("tail", tail.resize((ICON, ICON // 2), Image.LANCZOS)))
O.set_theme("steam")
cells, size = pack(icons, ICON + 2 * int(box * 0.16) * 2 + 8, 8, os.path.join(OUT, "icons.png"))
json.dump({"size": size, "icon": ICON, "handle": [box, int(box * 0.16) * 2], "cells": cells},
          open(os.path.join(OUT, "icons.json"), "w"))

# ---- emoji, the way BetterFrame draws them: the colour font at its one size, cropped, then scaled
font = ImageFont.truetype(EMOJI_FONT, 109)
want = []
for icon, name, items in E.CATS:
    want.append(icon)
    for x in items:
        want.append(x[0])
        for t in range(5):
            want.append(E.with_tone(x[0], t))
want += ["⭐", "✋"] + [E.with_tone("✋", t) for t in range(5)]
seen, order = set(), []
for e in want:
    if e not in seen:
        seen.add(e)
        order.append(e)
CELL = 56  # about what an emoji key comes to in the headset; bigger only adds megabytes
cells = []
missing = 0
for e in order:
    im = Image.new("RGBA", (160, 160), (0, 0, 0, 0))
    ImageDraw.Draw(im).text((80, 80), e, font=font, embedded_color=True, anchor="mm")
    bb = im.getbbox()
    if not bb:
        missing += 1
        continue
    im = im.crop(bb)
    sc = CELL / max(im.width, im.height)
    cells.append((e, im.resize((max(1, round(im.width * sc)), max(1, round(im.height * sc))), Image.LANCZOS)))
cols = 64
emo, esize = pack(cells, CELL, cols, os.path.join(OUT, "emoji.png"))
# As a PNG that's 14 MB. The colour as a JPEG (on black, so its edges stay clean) and the shape as a
# grey PNG come to under half; the app puts them back together when it loads them.
sheet = Image.open(os.path.join(OUT, "emoji.png"))
alpha = sheet.getchannel("A")
rgb = Image.new("RGB", sheet.size, (0, 0, 0))
rgb.paste(sheet.convert("RGB"), mask=alpha)
rgb.save(os.path.join(OUT, "emoji-color.jpg"), quality=90, optimize=True)
alpha.save(os.path.join(OUT, "emoji-alpha.png"), optimize=True)
os.remove(os.path.join(OUT, "emoji.png"))
json.dump({"size": esize, "cell": CELL, "emoji": emo}, open(os.path.join(OUT, "emoji.json"), "w", encoding="utf-8"), ensure_ascii=False)

# ---- the key sounds: 16-bit mono at 48 kHz, at BetterFrame's volume
for sound, fn in (("deep", O.make_click_deep), ("light", O.make_click)):
    for kind in ("key", "big"):
        with open(os.path.join(OUT, f"click-{sound}-{kind}.pcm"), "wb") as f:
            f.write(fn(kind, O.DEFAULTS["volume"]))

print("raqm:", features.check("raqm"), "| languages:", len(langs), "| characters:", len(CHARS),
      "| emoji:", len(cells), "(%d not in the font)" % missing, "| icons:", len(icons))
