#!/usr/bin/env python3
"""ChronoStretch's parameters, in one table. Writes params.json (the toolkit's list, VST order) and src/pdefs.h (the
engine's keys, names, ranges, defaults and kinds). Edit this file, not its outputs.
Run from ports/chrono:  python3 tools/gen_params.py   (tools/gen.ps1 runs it in Docker)

0.2 is a cue sampler: a sound is analysed (BPM, key, its hits), Find Samples places cues, pads 1-32 play them in time and
in key. The pad controls (p_*) edit the selected pad (sel_pad); every pad's settings live in the engine and its state.
Kinds: cont (min/max), int (whole numbers), opt (option list), trig (momentary), text (readout), vis (a picture the
engine moves; never saved). Continuous controls with a mapped range run 0..1 and the engine formats them."""
import json
import os

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

P = []


def cont(key, name, lo, hi, d, unit=None, dyn=False, **kw):
    p = {"key": key, "name": name, "min": float(lo), "max": float(hi), "default": float(d), "kind": "cont"}
    if unit:
        p["unit"] = unit
    if dyn:
        p["dynamic_display"] = True
    p.update(kw)
    P.append(p)


def integer(key, name, lo, hi, d, dyn=True, **kw):
    p = {"key": key, "name": name, "min": lo, "max": hi, "default": d, "display": "int", "kind": "int"}
    if dyn:
        p["dynamic_display"] = True
    p.update(kw)
    P.append(p)


def opt(key, name, options, d=0, **kw):
    p = {"key": key, "name": name, "options": options, "default": d, "kind": "opt"}
    p.update(kw)
    P.append(p)


def trig(key, name, **kw):
    p = {"key": key, "name": name, "momentary": True, "kind": "trig"}
    p.update(kw)
    P.append(p)


def text(key, name, **kw):
    p = {"key": key, "name": name, "type": "readout", "display": "string", "kind": "text"}
    p.update(kw)
    P.append(p)


def row(key, name):   # a list row: its text from the engine, a tap sends 1 ("<key>_on" lights it)
    P.append({"key": key, "name": name, "display": "string", "kind": "row"})


def drag(key, name):   # a drag strip: the engine reads each move from 0.5 and puts it back to 0.5 (never saved)
    P.append({"key": key, "name": name, "min": 0.0, "max": 1.0, "default": 0.5, "kind": "drag"})


def vis(key, name, levels):
    P.append({"key": key, "name": name, "options": [str(i) for i in range(levels)], "default": 0, "kind": "vis"})


QT = 3                                   # Q-Link events per option on the switches (wrapper qlink_ticks: no racing through a list)
KEYS12 = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
LEN_OPTS = ["NEXT CUE", "1/4", "1/2", "1 BEAT", "2 BEATS", "1 BAR", "2 BARS", "4 BARS", "TO END"]
LAYER = ["GLOBAL", "ON", "OFF", "SOLO"]

# ---- the sound and the global controls (PLAY's Q-Links first) ----
opt("sync", "Sync", ["OFF", "ON"], 1, qlink_ticks=QT)                         # stretch to MPC's tempo
opt("key", "Key", ["ORIGINAL"] + KEYS12, 0, qlink_ticks=QT)    # move the sound's key to this one (nearest way)
integer("pitch", "Pitch", -12, 12, 0)                          # semitones on top, tempo untouched
opt("tempo_mult", "Time", ["HALF", "NORMAL", "DOUBLE"], 1, qlink_ticks=QT)    # sync at half or double time
opt("drums", "Drums", ["ON", "OFF", "SOLO"], 0, qlink_ticks=QT)           # OFF takes the drums out, SOLO keeps only them
opt("vocals", "Vocals", ["ON", "OFF", "SOLO"], 0, qlink_ticks=QT)   # unused (vocal removal dropped in 1.0.1): kept so projects load
opt("quality", "Quality", ["TIGHT", "BALANCED", "SMOOTH"], 1, qlink_ticks=QT) # the stretch's window: 1024, 2048, 4096
cont("transient", "Transient", 0, 1, 0.3, dyn=True)            # the hit at 1x before the stretch takes over
cont("volume", "Volume", -24, 6, 0, "dB")
opt("keyboard", "Keyboard", ["OFF", "ON"], 0, refresh_display=True, qlink_ticks=QT)   # the selected pad's cue across the keys
opt("bank", "Bank", ["A", "B", "C", "D"], 0, refresh_display=True, qlink_ticks=QT)   # which 16 pads the screen shows
integer("sel_pad", "Pad", 0, 63, 0, refresh_display=True)      # the pad the p_* controls edit
trig("sel_pad_prev", "Previous Pad", step_of="sel_pad", step_delta=-1)
trig("sel_pad_next", "Next Pad", step_of="sel_pad", step_delta=1)
# the grid on PLAY: a pad of the bank. The skin lays options out top-down, pads go bottom-up (pad 1 bottom left):
# option o is pad slot (3 - o // 4) * 4 + o % 4 (its own inverse), labelled with that pad's number
opt("pad_tap", "Pad Grid", [str((3 - o // 4) * 4 + o % 4 + 1) for o in range(16)], 12, refresh_display=True, qlink_ticks=QT)
opt("follow", "Select On Hit", ["OFF", "ON"], 1, qlink_ticks=QT)              # a played pad becomes the selected one
opt("play_mode", "Play Mode", ["ONE SHOT", "CONTINUE"], 0, qlink_ticks=QT)  # ONE SHOT: the chop; CONTINUE: on past it until a pad is hit
opt("cue_count", "Cues", ["16", "32", "48", "64"], 3, qlink_ticks=QT)   # up to 64: banks A-D, chops 1-64
trig("find", "Chop")                                           # chop again by Chop Mode (MANUAL: arm a real-time chop)
trig("clear_cues", "Clear Cues")
opt("chop_mode", "Chop Mode", ["TRANSIENT", "EQUAL", "BPM", "MANUAL"], 0, refresh_display=True, qlink_ticks=QT)
cont("sensitivity", "Sensitivity", 0, 1, 0.5, dyn=True)        # TRANSIENT: higher finds more hits (up to Cues)
opt("chop_div", "Chop Every", ["1/2 BEAT", "1 BEAT", "2 BEATS", "1 BAR", "2 BARS"], 1, qlink_ticks=QT)  # BPM: a chop on every one
cont("zoom", "Zoom", 0, 16, 0, dyn=True)     # the view: the sound / 2^zoom, any fraction of a step (+ and - an eighth: 1.09x), down to a frame a column
trig("zoom_in", "Zoom In", step_of="zoom", step_delta=0.125)
trig("zoom_out", "Zoom Out", step_of="zoom", step_delta=-0.125)
cont("scroll", "Scroll", 0, 1, 0, dyn=True)                    # where the zoomed view sits (it follows the selected chop)
opt("drag_speed", "Drag Speed", ["FINEST", "FINE", "NORMAL", "FAST", "FASTEST"], 2, qlink_ticks=QT)   # how far a tab drag moves its chop
trig("stop", "Stop")                                           # every voice lets go (CONTINUE and HOLD too), a MANUAL run ends
trig("reset_pads", "Reset Pads")                               # back to the built-in pad notes (forgets LEARN PADS)
opt("sound_key", "Sound Key", ["AUTO"] + [k + " MAJ" for k in KEYS12] + [k + " MIN" for k in KEYS12], 0, qlink_ticks=2)  # AUTO: the key found
# ---- the drive browser (BROWSE): rows of the open folder (folders first, then WAVs); a tap opens a folder or loads a WAV ----
text("disp_path", "Folder")
trig("br_up", "Up")
trig("br_home", "Drives")
trig("br_pgup", "Page Up")
trig("br_pgdn", "Page Down")
NROWS = 10
for i in range(NROWS):
    row("br_%d" % (i + 1), "Browse %d" % (i + 1))
# the tab row under the waveform: a drag that starts on a tab slot grabs the chop whose tab is there and moves it
for i in range(36):
    drag("td%02d" % i, "Tab Drag %02d" % i)
integer("sound", "Sound", 0, 159, 0, nudge_pct=5)   # a browsed sound: a step moves through its folder
trig("sound_prev", "Previous Sound", step_of="sound", step_delta=-1)
trig("sound_next", "Next Sound", step_of="sound", step_delta=1)
trig("rescan", "Rescan")
trig("learn_pads", "Learn Pads")                               # hit pads 1-16 in order: the notes they send become pads 1-16
# ---- the selected pad ----
cont("p_pos", "Cue", 0, 1, 0, dyn=True, refresh_display=True)  # where its cue sits in the view (the whole sound at 1X)
cont("p_nudge", "Nudge", -1, 1, 0, dyn=True)                   # fine move of the cue (a Q-Link step: a column of the view, at most 4 ms), springs back
opt("p_len", "Length", LEN_OPTS, 0, qlink_ticks=QT)
cont("p_level", "Level", -24, 6, 0, "dB")
cont("p_filter", "Filter", -1, 1, 0, dyn=True)                 # below 0 low-pass, above 0 high-pass
cont("p_attack", "Attack", 0, 1, 0, dyn=True)
cont("p_release", "Release", 0, 1, 0.15, dyn=True)
opt("p_reverse", "Reverse", ["OFF", "ON"], 0, qlink_ticks=QT)
integer("p_key", "Pad Key", -24, 24, 0)
cont("p_speed", "Pad Speed", -1, 1, 0, dyn=True)               # 25% .. 400% on top of the sync, log
opt("p_mode", "Mode", ["GLOBAL", "ONE SHOT", "CONTINUE", "GATE", "HOLD"], 0, qlink_ticks=QT)   # GLOBAL: Play Mode's
opt("p_choke", "Choke", ["OFF", "1", "2", "3", "4"], 0, qlink_ticks=QT)
opt("p_drums", "Pad Drums", LAYER, 0, qlink_ticks=QT)
opt("p_vocals", "Pad Vocals", LAYER, 0, qlink_ticks=QT)        # unused, as vocals
# ---- readouts ----
text("status", "Status")
text("disp_info", "Sound Info")
text("disp_pad", "Pad Info")
text("disp_play", "Playing")
# ---- the screen's pictures (the engine moves them; art/make_art.py draws them as display-only filmstrip meters) ----
NCOL = 288                     # waveform columns (4 px each)
WAVE_FRAMES = 76               # 0 empty, 1..75: 25 heights x 3 colours (bass-heavy red, mid green, bright blue)
OV_FRAMES = 52                 # over each column: 0 none, 1 played, 2..17 playhead (pad colour), 18..33 chop line (pad colour),
                               # 34..49 chop line over played, 50 the other bank's chop line, 51 that over played
NMARK = 36                     # tab slots under the waveform (8 columns, 32 px each)
COLS_PER_MARK = 8
MK_FRAMES = 1 + 16 * COLS_PER_MARK * 2   # 0 none, else 1 + (pad * 8 + its column in the slot) * 2 + selected
for i in range(NCOL):
    vis("wv%03d" % i, "Wave %03d" % i, WAVE_FRAMES)
for i in range(NCOL):
    vis("ov%03d" % i, "Over %03d" % i, OV_FRAMES)
for i in range(NMARK):
    vis("mk%02d" % i, "Tab %02d" % i, MK_FRAMES)
for i in range(16):
    vis("lp%02d" % i, "Pad Lamp %02d" % i, 12)           # bank * 3 + (0 no cue, 1 its chop, 2 playing): the pad's own number
SB_FRAMES = 25                 # the browser's scroll bar: 0 none (the folder fits), 1..24 the thumb from top to bottom
vis("sb", "Scroll Bar", SB_FRAMES)
# ---- 0.7, appended (saved projects bind by index: new params only ever go at the end) ----
trig("key_down", "Key Down")                                   # the sound's key a semitone down (MAIN's KEY - and +)
trig("key_up", "Key Up")
text("disp_key", "Key Shown")                                  # the sound's key, and the key it is moved to
trig("br_load", "Load")                                        # the highlighted WAV becomes the sound
drag("br_drag", "Browse Scroll")                               # a drag beside the rows scrolls them
text("disp_pads", "Pads")                                      # which notes play the pads, and the last one heard
# ---- 0.8, appended ----
integer("br_cursor", "Browse Cursor", 0, 999, 0, dyn=True, nudge_pct=2)   # the data wheel on BROWSE: the highlighted row (shown as its name)
trig("br_enter", "Browse Enter")                               # the data wheel pushed: open the folder, or load the WAV
integer("sound_bpm", "Sound BPM", 0, 200, 0, nudge_pct=1)      # 0 AUTO (the tempo found), else 40 + n - 1 BPM
cont("bpm_fine", "BPM Fine", -1, 1, 0, dyn=True)               # +-1 BPM on top, in hundredths
opt("snap", "Snap", ["OFF", "ZERO CROSS"], 0, qlink_ticks=QT)  # a moved or added chop lands on the nearest zero crossing (a clean loop start)
trig("chop_add", "Add Chop")                                   # a chop where the sound plays now (else mid-view)
trig("chop_del", "Delete Chop")                                # the selected chop goes
NMARK_NUM_FRAMES = 1 + 64 * COLS_PER_MARK                      # a tab's number: 0 none, else 1 + chop * 8 + its column in the slot
for i in range(NMARK):
    vis("mn%02d" % i, "Tab Number %02d" % i, NMARK_NUM_FRAMES)
# ---- 0.9, appended ----
# the browser's rows as touch strips (over the list): a drag scrolls the rows, the data wheel on a touched row moves the
# highlight (each WAV plays its preview), a double tap (bt) or a push of the wheel opens the folder or loads the WAV
for i in range(NROWS):
    drag("bd%02d" % (i + 1), "Browse Drag %02d" % (i + 1))
for i in range(NROWS):
    trig("bt%02d" % (i + 1), "Browse Open %02d" % (i + 1))
trig("undo", "Undo")                                           # back one step (chops, pad settings, the sound's settings): up to 10
trig("p_reset", "Reset Pitch And Speed")                       # the selected pad's pitch, speed and reverse back to plain
FX = ["OFF", "LOFI", "TELEPHONE", "LOW PASS", "HIGH PASS", "BAND PASS", "VINTAGE", "LOW SHELF", "HIGH SHELF", "NOTCH",
      "SWEEP", "RISER", "ROBOT", "RADIO", "V CURVE", "WARP-SPEED", "TAPE"]
opt("p_fx", "Pad FX", FX, 0, qlink_ticks=QT)                   # the selected pad's effect (a preset)
cont("p_fx_amt", "FX Amount", 0, 1, 0.6, dyn=True)             # how much of it (shelves and V CURVE: the boost)
opt("p_fx_rate", "FX Rate", ["1/4", "1/2", "1 BEAT", "2 BEATS", "1 BAR", "2 BARS", "4 BARS"], 4, qlink_ticks=QT)   # SWEEP's cycle, RISER's rise
cont("p_fx_mix", "FX Mix", 0, 1, 1, dyn=True)                  # dry .. wet
trig("fx_copy", "FX To All Pads")                              # the selected pad's FX on every pad
trig("fx_clear", "Clear All FX")
text("disp_fx", "FX Info")
# ---- 1.0, appended ----
opt("half", "1/2 Speed", ["OFF", "ON"], 0, qlink_ticks=QT)   # the sound at half the project's tempo (for double-time patterns), pitch kept
# ---- 1.0.2, appended: more to shape a pad's FX with ----
cont("p_fx_drive", "FX Drive", 0, 1, 0, dyn=True)              # saturation before the effect
cont("p_fx_tone", "FX Tone", -1, 1, 0, dyn=True)               # darker .. brighter after it (a tilt)
cont("p_fx_width", "FX Width", 0, 1, 0.5, dyn=True)            # mono .. as is .. wide
cont("p_fx_res", "FX Resonance", 0, 1, 0.25, dyn=True)         # the filters' resonance (the filter presets, SWEEP, RISER)
# ---- 1.0.4, appended: the preset list without OFF, and its own switch ----
opt("p_fx_pick", "FX Preset", FX[1:], 0, qlink_ticks=QT)       # the selected pad's preset (picking one turns its FX on)
opt("p_fx_on", "FX On", ["OFF", "ON"], 0, qlink_ticks=QT)      # its FX off (the preset is kept) or on
# ---- 1.0.8, appended ----
trig("chop_rand", "Random Chops")                              # a new set of chops at random hits through the sound (all banks)
# ---- 1.0.15, appended ----
opt("p_layer", "Layer Group", ["OFF", "1", "2", "3", "4"], 0, qlink_ticks=QT)   # the selected pad's layer group: a hit on any pad of a group plays them all
# ---- 1.0.18, appended ----
for i in range(36):   # a double tap on a tab slot: the chop whose tab is there is selected (its pad, CUE, NUDGE, the Q-Links)
    trig("tt%02d" % i, "Tab Tap %02d" % i)
keys = [p["key"] for p in P]
assert len(set(keys)) == len(keys), "duplicate keys"
with open(os.path.join(HERE, "params.json"), "w") as f:
    f.write('{\n "name": "ChronoStretch",\n "params": [\n')
    f.write(",\n".join("  " + json.dumps({k: v for k, v in p.items() if k != "kind"}) for p in P))
    f.write("\n ]\n}\n")

KIND = {"cont": "K_CONT", "int": "K_INT", "opt": "K_OPT", "trig": "K_TRIG", "text": "K_TEXT", "vis": "K_VIS", "row": "K_ROW", "drag": "K_DRAG"}
with open(os.path.join(HERE, "src", "pdefs.h"), "w") as f:
    f.write("/* Written by tools/gen_params.py: do not edit. */\n#pragma once\n")
    f.write("enum { K_CONT, K_INT, K_OPT, K_TRIG, K_TEXT, K_VIS, K_ROW, K_DRAG };\n#define NROWS %d\n" % NROWS)
    f.write("#define NCOL %d\n#define WAVE_FRAMES %d\n#define OV_FRAMES %d\n#define NMARK %d\n#define COLS_PER_MARK %d\n#define MK_FRAMES %d\n"
            % (NCOL, WAVE_FRAMES, OV_FRAMES, NMARK, COLS_PER_MARK, MK_FRAMES))
    f.write("enum {\n" + "".join("    P_%s,\n" % k.upper() for k in keys) + "    NP\n};\n")
    # the pictures (the engine's vis[]): two runs of params, wv000..sb, then (0.8, appended) the tab numbers mn00..
    vk = [i for i, p in enumerate(P) if p["kind"] == "vis"]
    runs = [[vk[0]]]
    for i in vk[1:]:
        if i == runs[-1][-1] + 1:
            runs[-1].append(i)
        else:
            runs.append([i])
    assert len(runs) == 2, "the pictures must be two runs of params"
    f.write("#define SB_FRAMES %d\n#define MN_FRAMES %d\n#define P_VIS0 P_%s\n#define NVIS1 %d\n#define P_VIS2 P_%s\n#define NVIS %d\n"
            % (SB_FRAMES, NMARK_NUM_FRAMES, P[runs[0][0]]["key"].upper(), len(runs[0]), P[runs[1][0]]["key"].upper(), len(vk)))
    f.write("static const char *const KEYS[NP] = {\n" + "".join('    "%s",\n' % k for k in keys) + "};\n")
    f.write("static const char *const NAMES[NP] = {\n" + "".join('    "%s",\n' % p["name"].upper() for p in P) + "};\n")
    lo = [p.get("min", 0) for p in P]
    hi = [p.get("max", len(p["options"]) - 1 if "options" in p else (1 if p["kind"] == "trig" else 0)) for p in P]
    f.write("static const float PMIN[NP] = { %s };\n" % ", ".join("%g" % float(x) for x in lo))
    f.write("static const float PMAX[NP] = { %s };\n" % ", ".join("%g" % float(x) for x in hi))
    f.write("static const float PDEF[NP] = { %s };\n" % ", ".join(repr(float(p.get("default", 0))) + "f" for p in P))
    f.write("static const unsigned char PKIND[NP] = { %s };\n" % ", ".join(KIND[p["kind"]] for p in P))
    for p in P:
        if p["kind"] == "opt":
            f.write("static const char *const OPT_%s[] = { %s };\n" % (p["key"].upper(), ", ".join('"%s"' % o for o in p["options"])))
    f.write("static const char *const *const OPTS[NP] = { %s };\n"
            % ", ".join("OPT_%s" % p["key"].upper() if p["kind"] == "opt" else "0" for p in P))
print("gen_params: %d params" % len(P))
