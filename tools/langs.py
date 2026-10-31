#!/usr/bin/env python3
"""Water Closet language files.

  tools/langs.py            rewrite lang/en_US.json (the master) from the source and data
  tools/langs.py --check    also check every other lang/<locale>.json against the master

The master has one entry for every string the game shows: the tr("key", "English") calls in
src/, the menu labels in data/widgets/, the tips in data/stages/ and the story text. A
translation pack may leave strings out (they stay English), but must not invent keys, and
every string must keep the same format codes (%d, %s ...) in the same order and the same
[Button] names as the English one, because the game fills those in.
"""
import glob, json, os, re, sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
os.chdir(ROOT)

CALL = re.compile(r'\btr\(\s*"([A-Za-z0-9_.]+)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\)')


def unescape(s):
    return json.loads('"' + s + '"')


def master():
    d = {}
    for path in sorted(glob.glob("src/**/*.c", recursive=True)):
        for key, eng in CALL.findall(open(path, encoding="utf-8").read()):
            d[key] = unescape(eng)
    for path in sorted(glob.glob("data/widgets/*.json")):
        for w in json.load(open(path, encoding="utf-8")):
            base = "widget.%s.%s" % (w["groupName"], w["name"])
            d[base + ".text"] = w["text"]
            for i, o in enumerate(w.get("options", [])):
                if not re.fullmatch(r"[0-9 x]+", o):
                    d["%s.opt%d" % (base, i + 1)] = o
    for path in sorted(glob.glob("data/stages/*.json")):
        num = int(os.path.basename(path).split(".")[0])
        for i, tip in enumerate(json.load(open(path, encoding="utf-8")).get("tips", [])):
            d["stage.%03d.tip%d" % (num, i + 1)] = tip
    d["story.text"] = open("data/misc/story.txt", encoding="utf-8").read().rstrip("\n")
    return d


FMT = re.compile(r"%[-+ #0]*\d*(?:\.\d+)?[a-zA-Z%]")
BTN = re.compile(r"\[[A-Za-z0-9 ]+\]")


def check(master_d):
    bad = 0
    for path in sorted(glob.glob("lang/*.json")):
        loc = os.path.basename(path)[:-5]
        if loc == "en_US":
            continue
        pack = json.load(open(path, encoding="utf-8"))
        for k, v in pack.items():
            if k.startswith("_"):
                continue
            if k not in master_d:
                print("%s: unknown key %s" % (loc, k)); bad += 1; continue
            e = master_d[k]
            if FMT.findall(v) != FMT.findall(e):
                print("%s: %s: format codes %s should be %s" % (loc, k, FMT.findall(v), FMT.findall(e))); bad += 1
            if sorted(BTN.findall(v)) != sorted(BTN.findall(e)):
                print("%s: %s: button names %s should be %s" % (loc, k, BTN.findall(v), BTN.findall(e))); bad += 1
        missing = [k for k in master_d if k not in pack]
        print("%s: %d of %d strings translated%s" % (loc, len(master_d) - len(missing), len(master_d), "" if not missing else " (%d left in English)" % len(missing)))
    return bad


if __name__ == "__main__":
    m = master()
    os.makedirs("lang", exist_ok=True)
    with open("lang/en_US.json", "w", encoding="utf-8") as f:
        json.dump(m, f, ensure_ascii=False, indent=2)
        f.write("\n")
    print("lang/en_US.json: %d strings" % len(m))
    if "--check" in sys.argv:
        sys.exit(1 if check(m) else 0)
