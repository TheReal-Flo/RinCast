#!/bin/sh
# Regenerates headset/app/src/main/assets/keyboard from BetterFrame Keyboard 1.0.0 (MIT), the Steam
# Frame keyboard RinCast's keyboard is a port of. Needs Docker, curl and sha256sum; run from the
# repository root:
#
#   sh tools/keyboard-assets/build.sh
#
# The keyboard's text atlas is baked at the end with the PC build's assetbaker, so build that first
# (cmake --build build/pc --config Release --target assetbaker).
set -eu
cd "$(dirname "$0")/../.."
work=build/keyboard-assets
out=headset/app/src/main/assets/keyboard
mkdir -p "$work/fonts"

fetch() {  # url file sha256
    if [ ! -f "$2" ] || ! echo "$3  $2" | sha256sum -c - >/dev/null 2>&1; then
        curl -sSLf -o "$2" "$1"
        echo "$3  $2" | sha256sum -c -
    fi
}
fetch https://github.com/hardwaresteam/BetterFrameKeyboard/releases/download/v1.0.0/betterframe-keyboard-1.0.0.zip \
    "$work/betterframe-keyboard-1.0.0.zip" 26e0f4dbeab7e1fce72a444f3fa8bfd4ff3e77419e152a5378489c6bc2f85a5d
fetch https://github.com/googlefonts/noto-emoji/raw/v2.047/fonts/NotoColorEmoji.ttf \
    "$work/fonts/NotoColorEmoji.ttf" 39ee3c587e10e89669b9ff32703261d10d5f9c4dd5ad147b6b5a1c5200591817
fetch https://github.com/notofonts/notofonts.github.io/raw/main/fonts/NotoSans/hinted/ttf/NotoSans-Medium.ttf \
    "$work/fonts/NotoSans-Medium.ttf" 635d93d1131d791f2576de90b3bb0f7cdf61929906e8420a61b5f7f8e76420bb
fetch https://github.com/notofonts/noto-cjk/raw/Sans2.004/Sans/SubsetOTF/KR/NotoSansKR-Medium.otf     "$work/fonts/NotoSansKR-Medium.otf" b46988ef13e8bac08f3933af686eaf770972994f9b6d335be0184d60169b5431
rm -rf "$work/src"
mkdir -p "$work/src"
(cd "$work/src" && unzip -q ../betterframe-keyboard-1.0.0.zip)
cp "$work"/src/betterframe-keyboard/fonts/NotoSans*.ttf "$work/fonts/"

docker build -q -t rincast-keyboard-assets tools/keyboard-assets >/dev/null
rm -rf "$out"
mkdir -p "$out"
MSYS_NO_PATHCONV=1 docker run --rm -v "$PWD:/repo" -w /repo rincast-keyboard-assets \
    python tools/keyboard-assets/export.py "$work/src/betterframe-keyboard" "$out" "$work/fonts/NotoColorEmoji.ttf"
cp "$work/src/betterframe-keyboard/LICENSE" "$out/LICENSE-BetterFrame.txt"
cp "$work/src/betterframe-keyboard/words/NOTICE.txt" "$out/words/NOTICE.txt"

# The keys' text: Noto Sans Medium as on the Frame, its script fonts for Arabic, Hebrew, Devanagari and
# Thai, Noto Sans SC for kana and Noto Sans KR for Hangul.
baker=build/pc/bin/Release/assetbaker.exe
[ -x "$baker" ] || baker=build/pc/bin/assetbaker
f="$work/fonts"
"$baker" --face "$out" "$out/charset.txt" "$f/NotoSans-Medium.ttf" "$f/NotoSansArabic.ttf" "$f/NotoSansHebrew.ttf"     "$f/NotoSansDevanagari.ttf" "$f/NotoSansThai.ttf" third_party/fonts/NotoSansSC-Medium.ttf "$f/NotoSansKR-Medium.otf"
rm "$out/charset.txt"
