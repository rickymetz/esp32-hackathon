#!/usr/bin/env python3
"""Check the docs against the code they claim to describe.

A cold read-the-docs pass found that most documentation bugs here were not
prose problems -- they were facts that had been true once. The font list, the
symbol roster and the worked example are all copies of something that lives in
the source, so they can drift silently. These three checks catch that class
mechanically.

    tools/check_docs.py        # exit 1 on any drift
"""
import pathlib, re, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CONTRACT = ROOT / "docs/APP_CONTRACT.md"
FONT_C = ROOT / "launcher/components/lua_module_lvgl/src/lua_lvgl_font.c"
SYM_C = ROOT / "launcher/components/lua_module_lvgl/src/lua_module_lvgl.c"
COUNTER = ROOT / "apps/counter.lua"

fails = []


def check_worked_example():
    """The contract embeds apps/counter.lua verbatim."""
    doc = CONTRACT.read_text()
    start = doc.index("## Worked example")
    fence = doc.index("```lua", start)
    close = doc.index("```", fence + 6)
    embedded = doc[fence + len("```lua\n"):close].rstrip("\n")
    actual = COUNTER.read_text().rstrip("\n")
    if embedded != actual:
        fails.append("APP_CONTRACT 'Worked example' no longer matches apps/counter.lua")


def check_fonts():
    """Every compiled face must be listed, and nothing extra."""
    sizes = {int(m) for m in re.findall(r"\{(\d+),\s*&lv_font_lexend_\d+\}",
                                        FONT_C.read_text())}
    doc = CONTRACT.read_text()
    listed = set()
    for line in doc.splitlines():
        if line.startswith("| ") and "|" in line[2:]:
            cell = line.split("|")[1].strip()
            if re.fullmatch(r"[\d, ]+", cell):
                listed |= {int(x) for x in re.findall(r"\d+", cell)}
    missing = sizes - listed
    extra = listed - sizes
    if missing:
        fails.append(f"fonts compiled but undocumented: {sorted(missing)}")
    if extra:
        fails.append(f"fonts documented but not compiled: {sorted(extra)}")


def check_symbols():
    """lvgl.symbol.* names in the contract must all exist in C."""
    real = set(re.findall(r'LUA_LVGL_SYMBOL\("([a-z0-9_]+)"', SYM_C.read_text()))
    doc = CONTRACT.read_text()
    # Scope to the roster table only -- prose elsewhere contains things like
    # `.lua` that look exactly like a symbol name.
    start = doc.index("The complete set")
    end = doc.index("\n\n", doc.index("|", start))
    table = doc[start:end]
    claimed = set(re.findall(r"`\.([a-z0-9_]+)`", table))
    bogus = claimed - real
    if bogus:
        fails.append(f"lvgl.symbol names documented but absent from C: {sorted(bogus)}")
    undocumented = real - claimed
    if undocumented:
        fails.append(f"lvgl.symbol names in C but undocumented: {sorted(undocumented)}")


check_worked_example()
check_fonts()
check_symbols()

if fails:
    for f in fails:
        print("DRIFT:", f)
    sys.exit(1)
print("docs match the code")
