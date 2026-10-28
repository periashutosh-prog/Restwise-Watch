"""Proves compare_with_python.py can FAIL: break the Python reference on purpose in three ways and check each one is caught."""
import os, runpy, sys, io, contextlib
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.abspath(os.path.join(HERE, "..", "..", "UI-Screens", "render_engine")))
import block_ui

sabotages = {
    "letter spacing 12 instead of 11": lambda: setattr(block_ui, "BIG_ADV", 12),
    "'Lunch Break' guessed as a meal": lambda: setattr(block_ui, "classify", (lambda orig: (lambda l: "meal" if "lunch" in l.lower() else orig(l)))(block_ui.classify)),
    "night sleep starts at 21:00 instead of 20:00": lambda: setattr(block_ui, "is_night_sleep", lambda s, e: e <= s or s >= 21 * 60 or s < 4 * 60),
}
orig = {k: getattr(block_ui, k) for k in ("BIG_ADV", "classify", "is_night_sleep")}
caught = 0
for name, patch in sabotages.items():
    for k, v in orig.items():
        setattr(block_ui, k, v)
    patch()
    buf = io.StringIO()
    code = 0
    sys.argv = ["compare_with_python.py", "150"]
    with contextlib.redirect_stdout(buf):
        try:
            runpy.run_path(os.path.join(HERE, "compare_with_python.py"), run_name="__main__")
        except SystemExit as e:
            code = e.code if isinstance(e.code, int) else 1
    status = "caught" if code else "NOT CAUGHT"
    caught += bool(code)
    print(f"{status}: {name}")
print(f"{caught} of {len(sabotages)} deliberate mistakes were caught")
sys.exit(0 if caught == len(sabotages) else 1)
