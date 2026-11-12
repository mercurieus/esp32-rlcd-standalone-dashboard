#!/usr/bin/env python3
"""Measures how wide a string draws, straight from the LVGL font tables.

Layouts here are sized for the widest value a slot can hold, in English and
Traditional Chinese. Guessing that width from a sample string once hid a layout
that never fit, so the number comes from the same glyph advances the firmware
draws with: adv_w / 16 of each glyph in the generated lv_font_*.c files (adv_w
is 12.4 fixed point). Kerning is not applied, so a pair that kerns tight reads a
fraction of a pixel wide; that error is on the safe side. Example: the
spec's "FEELS -24.1°C RAIN 100%" is 173 px drawn and reads 174.8 here. This
figure is the authoritative one for sizing layouts: wide, never narrow.

  python scripts/measure-text.py --font montserrat_14 "-24.4" "100%"
  python scripts/measure-text.py --font montserrat_20 --cjk 20 "更新 12:30"

Prints "<px>  <text>" per argument. A codepoint with no glyph exits non-zero and
names it - LVGL would draw that as an empty box.
"""
import argparse
import re
import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parent.parent
MONTSERRAT_DIR = PROJECT / "managed_components/lvgl__lvgl/src/font"
CJK_DIR = PROJECT / "components/ui/fonts"


class Font:
    """Codepoint -> advance for one lv_font_conv generated source file."""

    def __init__(self, path: Path):
        if not path.is_file():
            raise SystemExit(f"error: font file not found: {path}")
        text = path.read_text(encoding="utf-8")
        self.name = path.name
        self.advances = self._glyph_advances(text)
        self.cmaps = self._cmaps(text)

    @staticmethod
    def _glyph_advances(text: str) -> list:
        block = re.search(r"glyph_dsc\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
        if block is None:
            raise SystemExit("error: no glyph_dsc table found")
        return [int(m) for m in re.findall(r"\.adv_w\s*=\s*(\d+)", block.group(1))]

    @staticmethod
    def _cmaps(text: str) -> list:
        lists = {
            name: [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)", body)]
            for name, body in re.findall(
                r"(unicode_list_\d+)\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
        }
        block = re.search(r"cmaps\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
        if block is None:
            raise SystemExit("error: no cmaps table found")
        cmaps = []
        for entry in re.findall(r"\{(.*?)\}", block.group(1), re.S):
            start = int(re.search(r"\.range_start\s*=\s*(\d+)", entry).group(1))
            length = int(re.search(r"\.range_length\s*=\s*(\d+)", entry).group(1))
            gid = int(re.search(r"\.glyph_id_start\s*=\s*(\d+)", entry).group(1))
            kind = re.search(r"\.type\s*=\s*LV_FONT_FMT_TXT_CMAP_(\w+)", entry).group(1)
            listing = re.search(r"\.unicode_list\s*=\s*(\w+)", entry).group(1)
            cmaps.append((start, length, gid, kind, lists.get(listing)))
        return cmaps

    def glyph_id(self, cp: int):
        for start, length, gid, kind, offsets in self.cmaps:
            if not start <= cp < start + length:
                continue
            if kind == "FORMAT0_TINY":
                return gid + cp - start
            if kind == "SPARSE_TINY":
                # unicode_list holds offsets from range_start, not codepoints.
                try:
                    return gid + offsets.index(cp - start)
                except ValueError:
                    continue
            raise SystemExit(f"error: {self.name}: unsupported cmap type {kind}")
        return None

    def advance(self, cp: int):
        gid = self.glyph_id(cp)
        return None if gid is None else self.advances[gid] / 16


def measure(text: str, fonts: list) -> float:
    total = 0.0
    for ch in text:
        for font in fonts:
            adv = font.advance(ord(ch))
            if adv is not None:
                total += adv
                break
        else:
            names = ", ".join(f.name for f in fonts)
            raise SystemExit(
                f"error: no glyph for U+{ord(ch):04X} {ch!r} in {names}")
    return total


def _texts_after_options(argv: list) -> list:
    """Insert "--" before the first text so a text like "-10.4°C" is not an option."""
    i = 0
    while i < len(argv) and argv[i] in ("--font", "--cjk", "-h", "--help"):
        i += 1 if argv[i] in ("-h", "--help") else 2
    return argv if i >= len(argv) or argv[i] == "--" else argv[:i] + ["--"] + argv[i:]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--font", required=True,
                    choices=["montserrat_14", "montserrat_20"])
    ap.add_argument("--cjk", choices=["14", "20"],
                    help="fall back to rlcd_cjk_<N>.c for codepoints Montserrat lacks")
    ap.add_argument("text", nargs="+")
    args = ap.parse_args(_texts_after_options(sys.argv[1:]))

    fonts = [Font(MONTSERRAT_DIR / f"lv_font_{args.font}.c")]
    if args.cjk:
        fonts.append(Font(CJK_DIR / f"rlcd_cjk_{args.cjk}.c"))
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    for text in args.text:
        print(f"{measure(text, fonts):.1f}  {text}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
