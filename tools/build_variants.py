"""Compile the watch firmware without needing the watch plugged in.
  python tools/build_variants.py radio       -> full build (WiFi app, Discord, AI Chatbot ...)
  python tools/build_variants.py noradio     -> the same source with RESTWISE_NO_RADIO switched on
  python tools/build_variants.py both
Each variant is compiled from a clean temporary copy of the sketch (sketch.yaml there has no port, so COM3 is not needed),
and the images are copied to OUT_DIR (default: D:\\NCSC-HealthTracker\\Firmware-Builds)."""
import os, re, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.abspath(os.path.join(HERE, ".."))
CLI = r"C:\Users\Ashut\Arduino CLI\arduino-cli.exe"
FQBN = "esp32:esp32:esp32c6:CDCOnBoot=cdc,PartitionScheme=no_ota"
WORK = os.path.join(tempfile.gettempdir(), "restwise_build")
OUT = os.environ.get("RESTWISE_OUT", r"D:\NCSC-HealthTracker\Firmware-Builds")
SKETCH_FILES = ["Restwise-watch.ino", "build_config.h", "block_screen.h", "block_icons.h", "ai_config.h", "discord_config.h", "discord_roots.h"]


def build(variant):
    name = {"radio": "with-radios", "noradio": "no-radio"}[variant]
    root = os.path.join(WORK, variant)
    sk = os.path.join(root, "Restwise-watch")
    shutil.rmtree(sk, ignore_errors=True)
    os.makedirs(sk)
    for f in SKETCH_FILES:
        p = os.path.join(SRC, f)
        if os.path.exists(p):
            shutil.copy(p, sk)
    open(os.path.join(sk, "sketch.yaml"), "w").write("default_fqbn: %s\n" % FQBN)
    if variant == "noradio":
        cfg = os.path.join(sk, "build_config.h")
        t = open(cfg, encoding="utf-8").read()
        assert "// #define RESTWISE_NO_RADIO" in t
        open(cfg, "w", encoding="utf-8").write(t.replace("// #define RESTWISE_NO_RADIO", "#define RESTWISE_NO_RADIO"))
    bdir = os.path.join(root, "build")
    odir = os.path.join(root, "out")
    shutil.rmtree(odir, ignore_errors=True)
    os.makedirs(bdir, exist_ok=True)
    # the ESP32 core's post-build step copies partitions.csv into <sketch>\build\<fqbn>, which must exist
    os.makedirs(os.path.join(sk, "build", FQBN.split(":CDC")[0].replace(":", ".")), exist_ok=True)
    print(f"== compiling {name} ...", flush=True)
    r = subprocess.run([CLI, "compile", "--fqbn", FQBN, "--build-path", bdir, "--output-dir", odir, sk], capture_output=True, text=True)
    log = r.stdout + r.stderr
    open(os.path.join(root, "compile.log"), "w", encoding="utf-8").write(log)
    if r.returncode != 0:
        errs = [l for l in log.splitlines() if re.search(r"error|undefined reference|fatal", l, re.I)]
        print(f"   FAILED ({len(errs)} error lines). First ones:")
        for l in errs[:25]:
            print("   ", l[:260])
        return False
    sizes = [l for l in log.splitlines() if "Sketch uses" in l or "Global variables" in l]
    for l in sizes:
        print("   ", l.strip())
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(odir):
        if f.endswith(".bin"):
            dest = f.replace("Restwise-watch.ino", "Restwise-watch_" + name)
            shutil.copy(os.path.join(odir, f), os.path.join(OUT, dest))
    print("   images:", ", ".join(sorted(f for f in os.listdir(OUT) if name in f)))
    return True


if __name__ == "__main__":
    which = sys.argv[1] if len(sys.argv) > 1 else "both"
    ok = True
    for v in (["radio", "noradio"] if which == "both" else [which]):
        ok = build(v) and ok
    sys.exit(0 if ok else 1)
