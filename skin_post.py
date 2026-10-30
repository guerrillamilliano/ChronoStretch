#!/usr/bin/env python3
"""vst.json "skin_post": run by gen_vst.py as `python3 skin_post.py <plugin folder>` on the built skin; fails when a target
is missing. Geometry comes from art/geometry.json (art/make_art.py).

1. The pad lamps stack under the grid. The builder bakes each picture frame opaque (the background redrawn under it), so
   each frame's image is replaced by the transparent PNG it came from (MPC honours PNG alpha), at exactly its box.
2. The screen's meters (waveform columns, their overlays, the tabs): the builder re-draws each filmstrip through the
   browser, which can soften 4 px columns; each meter definition gets the exact strip art/make_art.py drew instead.
3. The tab drag strips: the layout asks for thin sliders td00..td35 (MPC decodes every filmstrip frame each time the
   screen opens, so a big strip would cost memory); each is grown here over its tab slot, pointed at a 2-frame
   near-invisible strip, without its value text, and its own strip deleted. The browser's row strips (bd01..10, the
   vertical sliders) the same, over the rows, with a double tap and the data wheel's push as second and third handles.
4. The pad grid: its buttons become near-invisible cells (the selected pad's ringed), so the
   lamps under them show through, and their option names are hidden."""
import json
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ART = os.path.join(HERE, "art")
STRIP = "chrono_drag.png"
Y = 86   # layout y -> skin y
geo = json.load(open(os.path.join(ART, "geometry.json")))
params = json.load(open(os.path.join(HERE, "params.json")))["params"]
keys = [p["key"] for p in params]
by_index = {keys.index(k): (pat, box) for k, (pat, box) in geo["pictures"].items()}

skins = os.path.join(sys.argv[1], "Plugin Skins")
path = os.path.join(skins, "TUI.json")
tui = json.load(open(path))
local = tui["pageData"]["componentDefinitions"]["localComponentDefinitions"]
placed = [c for d in local if isinstance(d.get("value"), dict) for c in d["value"].get("componentsData", [])]
removed, keep = set(), set()

# 1. pictures
per = {}
for c in placed:
    hs = c["bounds"].get("additionalInvalidatingHandles") or []
    m = re.match(r"IndexedEnabling/(\d+)/\d+/Parameter (\d+)$", hs[0]) if len(hs) == 1 else None
    if c["componentData"]["type"] != "Image" or not m or int(m.group(2)) not in by_index:
        continue
    pattern, (x, y, w, h) = by_index[int(m.group(2))]
    f = pattern % int(m.group(1))
    if not os.path.exists(os.path.join(ART, f)):
        sys.exit("skin_post: art/%s is missing (art/make_art.py)" % f)
    removed.add(c["componentData"]["data"]["image"])
    c["componentData"]["data"]["image"] = f
    c["bounds"]["bounds"] = "%d %d %d %d" % (x, y - Y, w, h)
    keep.add(f)
    per[int(m.group(2))] = per.get(int(m.group(2)), 0) + 1
for i in by_index:
    n = len(params[i]["options"])
    if not per.get(i, 0) or per[i] % n:   # a picture on several pages: each placing has them all
        sys.exit("skin_post: %s: expected %d picture images a page, found %d" % (keys[i], n, per.get(i, 0)))

# 2. the meters' strips
nmeter = 0
for mt in geo["meters"]:
    defs = [d for d in local if re.match(r"shMeter_%dx%d(_.*)?_f%d$" % (mt["w"], mt["h"], mt["frames"]), d["key"])]
    if len(defs) != 1:
        sys.exit("skin_post: expected one meter definition %dx%d of %d frames, found %s" % (mt["w"], mt["h"], mt["frames"], [d["key"] for d in defs]))
    for c in defs[0]["value"]["componentsData"]:
        cd = c["componentData"]
        if cd["type"] == "Knob" and cd["data"].get("knobType") == "FilmStrip":
            removed.add(cd["data"]["filmStrip"])
            cd["data"]["filmStrip"] = "chrono_" + mt["file"]
            nmeter += 1
    shutil.copy(os.path.join(ART, mt["file"]), os.path.join(skins, "chrono_" + mt["file"]))
    keep.add("chrono_" + mt["file"])

# 3. the tab drag strips (the only horizontal sliders, all one shape: one definition placed once per tab slot)
boxes = geo["drags"]["boxes"]
defs = [d for d in local if d["key"].startswith("shSlider_h_")]
defs = [d for d in defs if isinstance(d.get("value"), dict) and any(c["componentData"]["type"] == "Knob" for c in d["value"].get("componentsData", []))]
if len(defs) != 1:
    sys.exit("skin_post: expected one slider definition for the tab drags, found %s" % [d["key"] for d in defs])
d = tdef = defs[0]
w, h = boxes[0][2], boxes[0][3]
kept = []
for c in d["value"]["componentsData"]:
    cd = c["componentData"]
    if cd["type"] == "Label":
        continue
    if cd["type"] == "Knob":
        removed.add(cd["data"]["filmStrip"])
        cd["data"]["filmStrip"] = STRIP
        cd["data"]["numFrames"] = 2
    c["bounds"]["bounds"] = "0 0 %d %d" % (w, h)
    kept.append(c)
d["value"]["componentsData"] = kept
where = sorted((c for c in placed if c["componentData"]["type"] == d["key"]), key=lambda c: int(c["bounds"]["bounds"].split()[0]))
if len(where) != len(boxes):
    sys.exit("skin_post: expected %s placed %d times, found %d" % (d["key"], len(boxes), len(where)))
for c, (x, y, bw, bh) in zip(where, sorted(boxes)):
    c["bounds"]["bounds"] = "%d %d %d %d" % (x, y - Y, bw, bh)

# 3b. the browser's row strips (the only vertical sliders, one shape: one definition placed once per row), grown over the rows
# the same way. A drag on one scrolls, the data wheel turns the one touched; a double tap toggles its row's open trigger
# (bt<row>, momentary) through a second handle, "Open", and a push of the wheel the enter trigger ("Enter")
vg = geo["vdrags"]
vboxes = vg["boxes"]
vdefs = [d for d in local if d["key"].startswith("shSlider_v_") and isinstance(d.get("value"), dict)]
if len(vdefs) != 1:
    sys.exit("skin_post: expected one vertical slider definition (the browser's rows), found %s" % [d["key"] for d in vdefs])
vd = vdefs[0]
kept = []
for c in vd["value"]["componentsData"]:
    cd = c["componentData"]
    if cd["type"] == "Label":
        continue
    if cd["type"] == "Knob":
        removed.add(cd["data"]["filmStrip"])
        cd["data"]["filmStrip"] = STRIP
        cd["data"]["numFrames"] = 2
    c["bounds"]["bounds"] = "0 0 %d %d" % (vboxes[0][2], vboxes[0][3])
    kept.append(c)
vd["value"]["componentsData"] = kept


def act(on, handler, handle, extra=""):
    return {"version": 2, "onAction": on, "handler": handler, "handleName": handle, "additionalData": extra,
            "handle remapping": {"version": 1, "map": []}}


vd["value"]["actions"] = [act("Mouse Down", "Q-Link", "Data"), act("Double Click", "Toggle Switch", "Open"),
                          act("Enter Pressed", "Toggle Switch", "Enter")]
vwhere = sorted((c for c in placed if c["componentData"]["type"] == vd["key"]), key=lambda c: int(c["bounds"]["bounds"].split()[1]))
if len(vwhere) != len(vboxes):
    sys.exit("skin_post: expected %s placed %d times, found %d" % (vd["key"], len(vboxes), len(vwhere)))
ei = keys.index(vg["enter"])
for c, (x, y, bw, bh) in zip(vwhere, sorted(vboxes, key=lambda b: b[1])):
    c["bounds"]["bounds"] = "%d %d %d %d" % (x, y - Y, bw, bh)
    row = keys[int(c["handle remapping"]["map"][0]["value"].split()[1])]   # bd<NN>
    c["handle remapping"]["map"] += [{"key": "Open", "value": "Parameter %d" % keys.index(vg["open"] + row[len(vg["prefix"]):])},
                                     {"key": "Enter", "value": "Parameter %d" % ei}]
# the list's own rows stay as the text and the lit row under the strips, and take no touches (the strips have them)
rowdefs = {c["componentData"]["type"] for c in placed
           if any(m["key"] == "Data" and keys[int(m["value"].split()[1])] in ["br_%d" % (r + 1) for r in range(len(vboxes))]
                  for m in c["handle remapping"]["map"])}
if len(rowdefs) < 1:
    sys.exit("skin_post: the browser's list rows (br_1..) not found")
for d in local:
    if d["key"] in rowdefs:
        d["value"]["ignoreMousePresses"] = True

# 3c. the browser's buttons take no focus: after a tap on one the data wheel stays on the list row touched before (MPC sent
# the wheel to the focused PAGE UP; the user wants it on the list only)
nf = 0
nofocus = {keys.index(k) for k in ("br_load", "br_home", "br_up", "br_pgup", "br_pgdn")}
for c in placed:
    hm = c.get("handle remapping", {}).get("map", [])
    if any(m["key"] == "Data" and m["value"].startswith("Parameter ") and int(m["value"].split()[1]) in nofocus for m in hm):
        c["bounds"]["acceptsHWFocus"] = "No"
        nf += 1
if nf != len(nofocus):
    sys.exit("skin_post: expected the %d browser buttons, found %d" % (len(nofocus), nf))

# 3d. the tab drag strips also take a double tap: it toggles the slot's tap trigger (tt<NN>, momentary) through a second
# handle, "Open", and the engine selects the chop whose tab is there
tdef["value"]["actions"] = [act("Mouse Down", "Q-Link", "Data"), act("Double Click", "Toggle Switch", "Open")]
for c in where:   # the tab strips, as placed (section 3)
    td = keys[int(c["handle remapping"]["map"][0]["value"].split()[1])]   # td<NN>
    c["handle remapping"]["map"].append({"key": "Open", "value": "Parameter %d" % keys.index("tt" + td[2:])})

# 4. the option grids' cells
cells = {}
for g in geo["grids"]:
    n = 0
    for d in local:
        if not d["key"].startswith("shSeg_%s_" % g["key"]):
            continue
        for c in d["value"]["componentsData"]:
            cd = c["componentData"]
            if cd["type"] == "Button":
                removed.update((cd["data"]["onImage"], cd["data"]["offImage"]))
                cd["data"]["onImage"], cd["data"]["offImage"] = g["on"], g["off"]
                n += 1
            elif cd["type"] == "Label" and "textColour" in cd["data"]:   # the option's own name: the art carries it
                cd["data"]["textColour"] = "00000000"
    if n == 0:
        sys.exit("skin_post: no %s cells (shSeg_%s_*)" % (g["key"], g["key"]))
    cells[g["key"]] = n
    keep.update((g["on"], g["off"]))

json.dump(tui, open(path, "w"), indent=1)
for f in removed - keep:
    if os.path.exists(os.path.join(skins, f)):
        os.remove(os.path.join(skins, f))
for f in keep:
    if not f.startswith("chrono_"):
        shutil.copy(os.path.join(ART, f), os.path.join(skins, f))
shutil.copy(os.path.join(ART, "clear.png"), os.path.join(skins, STRIP))
print("skin_post: %d transparent picture images (%d pictures), %d meter strips swapped, %d tab drag strips, cells %s"
      % (sum(per.values()), len(per), nmeter, len(boxes), cells))
