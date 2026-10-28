"""Run the firmware's screen code (block_screen.h) on this PC and compare it with the approved Python renders.
usage: python compare_with_python.py [random_cases]      (needs: pip install ziglang)
Checks, all against UI-Screens/render_engine/block_ui.py:
  1. every review frame, pixel by pixel      2. thousands of random names / times / categories, pixel by pixel
  3. the line-breaking function              4. the label-word icon guess
  5. findCurrent() against an independent minute-by-minute reference."""
import os, random, shutil, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.abspath(os.path.join(HERE, "..", "..", "UI-Screens", "render_engine"))
sys.path.insert(0, ENGINE)
import block_ui as B
from oled_render import Oled, GFX_DIR

N_RANDOM = int(sys.argv[1]) if len(sys.argv) > 1 else 3000
KEYS = ["school", "tuition", "homework", "meal", "break", "sleep", "free", "travel"]
KIND_INDEX = {"study": 0, "meal": 1, "sleep": 2, "free": 3, "travel": 4, "break": 5}

# ---- build the test program -----------------------------------------------------------------------------------------------
shutil.copy(os.path.join(GFX_DIR, "glcdfont.c"), os.path.join(HERE, "glcdfont_copy.c"))
exe = os.path.join(HERE, "test_block_screen.exe")
cmd = [sys.executable, "-m", "ziglang", "c++", "-std=c++17", "-O1", "-Wall", "-I" + HERE, "-o", exe, os.path.join(HERE, "test_block_screen.cpp")]
r = subprocess.run(cmd, capture_output=True, text=True, cwd=HERE)
if r.returncode != 0:
    print(r.stdout, r.stderr)
    sys.exit("BUILD FAILED")
warn = [l for l in (r.stderr or "").splitlines() if "warning" in l]
print("built the test program" + (f" ({len(warn)} warnings)" if warn else " (no warnings)"))
for w in warn[:5]:
    print("  ", w)

proc = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)


def ask(lines, n_out):
    for l in lines:
        proc.stdin.write(l + "\n")
    proc.stdin.flush()
    return [proc.stdout.readline().rstrip("\n") for _ in range(n_out)]


def c_frame(label, start, end, cat, show_time=True, force=False, yoff=0):
    out = ask(["R\t%s\t%d\t%d\t%d\t%d\t%d\t%d" % (label, start, end, cat, int(show_time), int(force), yoff)], 65)
    rows = out[:64]
    wrapped = out[64] == "wrapped=1"
    return rows, wrapped


def py_frame(label, start, end, cat, show_time=True, force=False):
    d = Oled()
    B.draw_block(d, label, start, end, None if cat < 0 else KEYS[cat], show_time, force)
    return ["".join("1" if v else "0" for v in row) for row in d.buf]


fails = []


def check_frame(tag, label, start, end, cat, show_time=True, force=False):
    cr, wrapped = c_frame(label, start, end, cat, show_time, force)
    pr = py_frame(label, start, end, cat, show_time, force)
    if cr != pr or wrapped:
        diff = sum(a != b for ra, rb in zip(cr, pr) for a, b in zip(ra, rb))
        fails.append((tag, label, start, end, cat, show_time, force, diff, wrapped))


# ---- 1. the review set ---------------------------------------------------------------------------------------------------
M = lambda h, m: h * 60 + m
review = [("School", M(8, 40), M(15, 0), 0), ("Tuition", M(17, 0), M(20, 0), 1), ("Homework", M(16, 0), M(17, 0), 2), ("Dinner", M(21, 30), M(22, 0), 3),
          ("Lunch", M(15, 30), M(16, 0), 3), ("Travel Home", M(15, 0), M(15, 30), 7), ("Water Break", M(10, 45), M(10, 46), 4), ("Lunch Break", M(12, 30), M(13, 0), 4),
          ("Free Time", M(16, 0), M(16, 30), 6), ("Sleep", M(22, 0), M(6, 0), 5), ("Chess Club", M(18, 0), M(19, 0), 6), ("Travel to School", M(8, 10), M(8, 40), 7),
          ("Math", M(9, 0), M(9, 40), 0), ("Extra-hard-maths", M(17, 0), M(18, 0), 1), ("Evening Group Study", M(18, 30), M(19, 30), 2),
          ("Extracurricular", M(16, 30), M(17, 30), 6), ("", M(12, 0), M(12, 30), 3), ("Nap", M(14, 0), M(14, 30), 5)]
for lab, s, e, c in review:
    check_frame("review", lab, s, e, c)
check_frame("night, no sleep block", "Sleep", 0, 0, -1, False, True)
print(f"1. review frames: {len(review) + 1} compared")

# ---- 2. random frames ----------------------------------------------------------------------------------------------------
rng = random.Random(2026)
WORDS = ["School", "Tuition", "Homework", "Math", "Maths", "Physics", "Chemistry", "Lab", "Extra-hard-maths", "Evening", "Group", "Study", "Travel", "Home", "Lunch", "Break",
         "Water", "Dinner", "Free", "Time", "Sleep", "Nap", "Chess", "Club", "Extracurricular", "Breakfast", "Rest", "Exam", "Revision", "Bus", "Walk", "Class", "Coaching",
         "A", "I", "Go", "Pre-board", "Self-study", "Well-being", "X-ray", "-a", "a-", "--", "Internationalization", "Supercalifragilistic", "ab-cd-ef-gh-ij", "Snack", "Commute"]


def random_name():
    k = rng.random()
    if k < 0.12:
        return ""
    if k < 0.25:
        return "".join(rng.choice("abcdefghijklmnopqrstuvwxyz-ABCDE ") for _ in range(rng.randint(1, 23))).strip(" ") or "x"
    parts = [rng.choice(WORDS) for _ in range(rng.randint(1, 4))]
    sep = rng.choice([" ", " ", "-", "  "])
    name = sep.join(parts)
    if rng.random() < 0.15:
        name = "  " + name + " "
    return name[:rng.choice([19, 19, 19, 23])]


for i in range(N_RANDOM):
    name = random_name()
    s = rng.randint(0, 1439)
    e = rng.choice([rng.randint(0, 1440), (s + rng.randint(1, 180)) % 1441])
    cat = rng.choice([-1, 0, 1, 2, 3, 4, 5, 6, 7])
    check_frame("random", name, s, e, cat, rng.random() < 0.9, rng.random() < 0.05)
print(f"2. random frames: {N_RANDOM} compared")

# ---- 3. line breaking ----------------------------------------------------------------------------------------------------
nw = 0
for i in range(2000):
    name = random_name().strip()
    for mc, ml, hard in ((7, 2, False), (12, 2, True), (12, 3, True), (6, 2, False)):
        got = ask(["W\t%s\t%d\t%d\t%d" % (name, mc, ml, int(hard))], 1)[0]
        want = B.wrap(name, mc, ml, hard)
        want = "NONE" if (want is None or want == []) else "|".join(want)
        if got != want:
            fails.append(("wrap", name, mc, ml, hard, got, want))
        nw += 1
print(f"3. line breaking: {nw} compared")

# ---- 4. label-word icon guess --------------------------------------------------------------------------------------------
ng = 0
for i in range(2000):
    name = random_name()
    got = int(ask(["G\t%s" % name], 1)[0])
    want = KIND_INDEX[B.classify(name)]
    if got != want:
        fails.append(("guess", name, got, want))
    ng += 1
print(f"4. icon guess from words: {ng} compared")

# ---- 5. which block is running now ---------------------------------------------------------------------------------------
def ref_current(blocks, today, cur):
    """Independent reference: minutes since each candidate block started; the most recently started wins, then the shorter one."""
    best = None
    for i, (days, s, e, cat) in enumerate(blocks):
        if s == e:
            continue
        ago = None
        if s < e:
            if (days >> today) & 1 and s <= cur < e:
                ago = cur - s
        else:
            if (days >> today) & 1 and cur >= s:
                ago = cur - s
            elif (days >> ((today + 6) % 7)) & 1 and cur < e:
                ago = cur + 1440 - s
        if ago is None:
            continue
        length = (e - s) if s < e else (e + 1440 - s)
        key = (ago, length)
        if best is None or key < best[0]:
            best = (key, i)
    return -1 if best is None else best[1]


nf = 0
for t in range(300):
    blocks = []
    for _ in range(rng.randint(0, 14)):
        s = rng.randint(0, 1439)
        e = rng.choice([rng.randint(0, 1440), (s + rng.randint(1, 240)) % 1441])
        blocks.append((rng.randint(0, 127), s, e, rng.choice([0, 1, 2, 3, 4, 5, 6, 7, 255])))
    for _ in range(12):
        today, cur = rng.randint(0, 6), rng.randint(0, 1439)
        lines = ["F\t%d\t%d\t%d" % (today, cur, len(blocks))] + ["%d %d %d %d" % b for b in blocks]
        got = int(ask(lines, 1)[0])
        want = ref_current(blocks, today, cur)
        if got != want:
            fails.append(("findCurrent", today, cur, blocks, got, want))
        nf += 1
print(f"5. which block runs now: {nf} compared")

proc.stdin.close()
proc.wait()
print()
if fails:
    print(f"FAILED: {len(fails)} mismatches. First ones:")
    for f in fails[:8]:
        print("  ", f)
    sys.exit(1)
print("ALL MATCH: the firmware code gives the same frames, line breaks, icon guesses and block lookups as the approved Python version.")
