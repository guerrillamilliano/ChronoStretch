/* ChronoStretch 0.7: a cue sampler for MPC OS on the shared wrapper (wrapper/engine.h, an instrument), on its own stretch
 * engine. A sound (one of eight synthesized built-ins, or a WAV from <plugin>/User or a folder listed in User/paths.txt) is
 * loaded on a worker thread and analysed there: its BPM and beat grid (an onset envelope's autocorrelation), its key
 * (a chromagram against major and minor key profiles), its hits and a waveform at five levels of detail. Chopping puts up
 * to 32 cues on it: TRANSIENT (its hits, as many as Sensitivity lets through), EQUAL (16 or 32 even slices), BPM (one per
 * half beat .. two bars on its beat grid) or MANUAL (the first pad hit plays the sound from the start, each hit after it
 * drops the next chop where it is heard). A chop is dragged by its tab, at any zoom. A BROWSE page lists the drives under
 * /media (and the built-in and User sounds); a tap on a WAV plays a preview of it (the worker loads its first PREVIEW_SEC),
 * a second tap or LOAD makes it the sound (by path, kept in the state). Once a sound came from a folder, the sound arrows
 * step through that folder's WAVs, each played from its start as it arrives.
 * Pads (MPC's scale notes from 72 or plain notes from 36, bank-relative, or MPC's own as learned) play the cues: in time with
 * MPC's tempo (Sync, at half, normal or double time) and moved to a chosen key (the nearest way), tempo and pitch
 * independent, through the 0.1 phase vocoder (identity phase locking, PFFFT, slot-scheduled hops, a cached analysis);
 * the hit plays first through a clean resampler (Transient). Each pad has a level, a filter (low-pass below 0, high-pass
 * above), attack and release, reverse, a key shift, a speed, a mode (one shot, continue, gate, hold), a choke group, a
 * length (to the next cue, in beats or bars, or to the end) and its own drum setting. Keyboard plays the
 * selected pad's cue chromatically (C3 = as is). The p_* controls edit the selected pad.
 * Drums: after the analysis the worker separates the sound's drums (harmonic/percussive separation with median filters on
 * a 2048-point STFT, soft masks) into a layer of its own; FULL, NO DRUMS (the sound minus the layer) and DRUMS ONLY are a
 * gain per layer as a voice reads the sound. (The vocals params are left from a plan dropped in 1.0.1: unused.)
 * Every pad's settings and the cues are in the state ("pN=...;cues=...;"), with the sound by name. */
#define _GNU_SOURCE
#include <ctype.h>
#include <stddef.h>
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "engine.h"
#include "plugin_dir.h"
#include "pdefs.h"
#include "pffft/pffft.h"

#define SR 44100.0f
#define PI 3.14159265f
#define TWO_PI 6.28318531f
#define NV 6               /* voices */
#define MAX_PV 4           /* of them in the vocoder at once (start_pv): the Live II's CPU budget (docs/BENCH.md; 5 sat at the 35% line) */
#define NMAX 4096          /* largest FFT */
#define MMAX (NMAX / 2)
#define NSIZES 5           /* 256 .. 4096 */
#define FIFO (2 * NMAX)    /* a voice's output ring, power of two */
#define NPADS 64          /* four banks of 16 */
#define GRID_SLOT(o) ((3 - (o) / 4) * 4 + (o) % 4)   /* the PLAY grid's option o <-> pad slot (bottom-up pads), its own inverse */
#define MAX_SOUNDS 160
#define MAX_FILE_SEC 360   /* longest sound loaded (six minutes) */
#define MAX_HITS 1024      /* hits kept for the stretch (render_voice) */
#define NBUILTIN 8
#define ON_HOP 512         /* the onset envelope's hop */
#define HP_N 2048          /* the drum separation's STFT */
#define HP_HOP 512
#define HP_MED_T 31        /* its median filters: across time, frames (about 360 ms, longer than a kick) */
#define HP_MED_F 17        /* and across frequency, bins (about 370 Hz) */
#define KEY_MIN_TONAL 0.10 /* the share of a sound's energy in clear spectral peaks below which it has no key (set by test/keytest) */
#define LEARN_WAIT_S 20    /* LEARN PADS gives up after this long without a pad hit */
#define DRAG_GAP_BLOCKS 140   /* a tab drag paused this long (about 0.4 s) grabs again: a new gesture */
#define WHOP 32            /* the waveform's finest peaks */
#define WLOD 5             /* its levels: 32, 128, 512, 2048, 8192 frames a peak */
#define MIN_VIEW NCOL      /* the deepest zoom: a frame a waveform column */
#define PREVIEW_SEC 30     /* a WAV previewed in the browser: its first half minute */
#define BR_ROWS_PER_DRAG 12.0f   /* the browser's scroll strip: rows moved by a drag over the whole strip */
#define BUILTIN_DIR "@"    /* the browser's own folder of the built-in and User sounds */
#define BR_ROWS_PER_ROWDRAG 8.0f   /* a drag on a browser row: rows per unit of drag (a drag sends ~0.047 an event; content follows the finger) */
#define UNDO_N 10
#define ILOG_N 1024
#define FX_LAYERS 4       /* a pad's FX chain: its preset, then up to 3 more layered after it */
#define TRIG_TWICE_BLOCKS 150   /* a button's second 1 within this many blocks (~0.43 s) is the same press (set_value) */

static const int GR_N[6] = { 256, 512, 512, 1024, 2048, 4096 };
static const int GR_OV[6] = { 2, 2, 4, 4, 4, 4 };
static const int QUALITY_GI[3] = { 3, 4, 5 };   /* TIGHT 1024, BALANCED 2048, SMOOTH 4096 */
static const char *const BUILTIN[NBUILTIN] = { "Break 90", "Soul Keys", "Horn Stab", "Vocal Aah", "String Swell", "Choir Ooh",
                                               "Music Box", "Bass Pluck" };
static const int BUILTIN_SYN[NBUILTIN] = { 5, 2, 0, 1, 3, 4, 6, 7 };   /* synth_builtin's own numbering */
static const char *const KEYN[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

/* ---- sounds ---- */
typedef struct {
    int root, lo, hi, nch, len;
    int16_t *data;                    /* interleaved, nch channels, 44.1 kHz */
    char file[64];
} zone_t;
typedef struct {
    char name[64];
    int builtin;
    zone_t z;                         /* the sound */
    /* the analysis (worker, before the hand-over) */
    float bpm; double beat0;          /* tempo (0 = none found) and the first beat (frames) */
    int key;                          /* 0..11 major, 12..23 minor (root = key % 12); -1 none */
    float key_conf, key_tonal;        /* how well the best key fits its profiles (correlation); how tonal the sound is */
    int key_named, bpm_named;         /* the key and tempo came from the file's name (name_hints) */
    long hits[MAX_HITS]; int nhits;   /* its hits, for the stretch (render_voice plays each one clean) */
    long auto_cue[64]; int auto_msg;  /* the chops the worker made for it, for render to take as it takes the sound */
    float *onset; int non;            /* the onset envelope, one value per ON_HOP frames, 0..1 */
    float *wave[WLOD][3]; int nwave[WLOD]; float wtop;   /* the waveform's low, mid and high peaks per WHOP << 2l frames at
                                      * level l (the screen; any zoom reads the coarsest level that still has detail), the loudest */
    /* the drum layer (worker, after the hand-over): drums_ready is set once it is complete */
    int16_t *drums; volatile int drums_ready, drums_pct;
    float sec, mb;
} inst_t;
typedef struct { char name[64], path[512]; int builtin; } entry_t;

/* ---- FFT ---- */
typedef struct {
    int N, M;
    PFFFT_Setup *setup;
    float *in, *out, *work;           /* aligned scratch, N each */
    float *hann, *sine;               /* periodic windows, N */
} fft_t;

/* ---- pads ---- */
typedef struct {
    float level, filter, attack, release, speed;
    int reverse, key, mode, choke, len, drums, vocals;   /* mode: 0 GLOBAL (Play Mode), 1 ONE SHOT, 2 CONTINUE, 3 GATE, 4 HOLD */
    int fx, fx_rate; float fx_amt, fx_mix;   /* the pad's effect (FX tab: a preset, how much, SWEEP/RISER's time, dry/wet) */
    float fx_drive, fx_tone, fx_width, fx_res;   /* and around it: saturation, tilt, stereo width, the filters' resonance */
    int fx_keep;                      /* the preset its FX switch turns back on (fx 0: off) */
    int layer;                        /* its layer group (0 none): a hit on any pad of a group plays them all together */
    int fx_lay[FX_LAYERS - 1];        /* presets layered after its preset, in the order added (0 none); they play while fx is on */
} pad_t;
static const pad_t PAD_DEFAULT = { 0, 0, 0, 0.15f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0.6f, 1, 0, 0, 0.5f, 0.25f, 1, 0, { 0, 0, 0 } };
enum { FX_OFF, FX_LOFI, FX_PHONE, FX_LP, FX_HP, FX_BP, FX_VINTAGE, FX_LSHELF, FX_HSHELF, FX_NOTCH, FX_SWEEP, FX_RISER, FX_ROBOT,
       FX_RADIO, FX_VCURVE, FX_WARP, FX_TAPE };
#define WARP_MAX (3 * 44100)   /* WARP-SPEED's longest cycle, frames (its buffer per voice and channel) */
#define COMB_MAX 600      /* ROBOT's comb, frames (down to 74 Hz) */
typedef struct { float b0, b1, b2, a1, a2; } bq_t;   /* a biquad's coefficients (RBJ), its state per channel in the voice */
typedef struct {   /* one preset of a voice's FX chain: two biquads, LOFI's hold, ROBOT's and TAPE's delay line, WARP-SPEED's place */
    int fx; bq_t fq[2]; float fz[2][2][2]; float hold[2], hold_ph;
    float comb[2][COMB_MAX]; int comb_n, comb_w;
    float *wb; long w_n, w_t;          /* WARP-SPEED: its buffer (chrono_t's warp), the cycle and where in it */
    float wow_ph, flut_ph;            /* TAPE: its wow and flutter, as phases (0..1) */
} fxu_t;

/* ---- voices ---- */
typedef struct {
    /* note_on clears everything up to fifo */
    int active, note, vel, held, released, done_fade, pad, rev, recv;   /* recv: MANUAL chopping's run through the sound */
    unsigned hit;                     /* the hit that started it: a layer group's pads, one hit, leave each other alone */
    const zone_t *z; const int16_t *dl;   /* the sound and its drum layer (or NULL) */
    float gm, gd;                     /* layer gains: the sound, the drum layer */
    long r0, r1;                      /* the region, frames */
    double start;                     /* where it started */
    /* raw */
    int raw_on; double rpos;
    /* the beat: where the sound is due (start + time x rate), where the current clean part began, the next hit, its fade in */
    double tpos, seg; int nh; float xr;
    /* phase vocoder */
    int pv_on, pv_wait, gi, first, stag, fr, fn; double ppos;   /* stag: its slot; pv_wait: starting at it (start_pv) */
    float xf;                         /* raw -> vocoder crossfade */
    int c_ok, c_env, an_ready; long c_a;   /* the last analysis (at frame c_a) is cached; c_env: its envelope too; an_ready: it is
                                      * the frame the next hop synthesizes */
    int frozen, end_frozen; float frz_k; double frz_pos; unsigned pos_gen;   /* 0.1's freeze: never set in 0.2 */
    /* amplitude and filter */
    float env, att_step, rel_s, vel_g, gain;
    float flt_s[2][2]; int flt_mode;  /* filter state; 0 off, 1 low-pass, 2 high-pass */
    float flt_g;
    /* the pad's FX (fx_frame): its chain of presets (u, nu of them), SWEEP/RISER's clock (frames since the hit) */
    int fx, fx_set, fx_on, fx_rate; float fx_amt, fx_mix, fx_beat, fx_res, fx_q, fx_drive, fx_tone, fx_width, tone_k, tone_s[2];
    int lay_set[FX_LAYERS - 1], nu; long fx_t;   /* lay_set: the pad's layers it was started with */
    fxu_t u[FX_LAYERS];
    unsigned age;
    /* the vocoder's buffers (reset by start_pv) */
    float fifo[2][FIFO];
    float ola[2][NMAX];
    float phs[MMAX + 1], ms[2][MMAX + 1];
    /* the cached analysis: magnitudes (L, R, the mid squared), the mid's phase, each bin's true frequency, the envelope (log2) */
    float magL[MMAX + 1], magR[MMAX + 1], magM[MMAX + 1], phA[MMAX + 1], wtr[MMAX + 1], lenv[MMAX + 1];
} voice_t;

typedef struct {
    float val[NP];
    char sname[64];
    int last_touched;
    uint64_t changed[(NP + 63) / 64];
    long vw0, vwl; unsigned vw_blk; double pos_acc;   /* pos_acc: CUE's place, to the fraction of a frame */   /* the view the waveform was last drawn for, and when (display_rev) */
    unsigned short vis[NVIS]; unsigned disp_rev;   /* the screen's pictures as last shown (display_rev) */
    unsigned char wcache[NCOL]; const inst_t *wc_in; long wc_v0, wc_vl;   /* the waveform columns of that view of that sound */
    /* cues (frames, -1 none) and pads; cue_name: the sound they belong to (a state can arrive before its sound) */
    long cue[NPADS]; char cue_name[64];
    /* MANUAL chopping: 1 armed (the next pad hit starts the sound), 2 running (each hit drops chop rec_n); rec_v its voice */
    int rec, rec_n, rec_v, chop_msg, chop_same; long chop_msg_frames, nudged;   /* nudged: frames NUDGE moved the chop, this turn */
    /* a tab drag: the slot it started on, the chop it holds, the block of its last move */
    int drag_slot, drag_pad; unsigned drag_blk; double drag_pos;   /* drag_pos: where the drag has the chop, to a fraction of a frame */
    long learn_frames;                /* LEARN PADS gives up after a quiet spell */
    signed char padmap_before[128];   /* the pads as they were when LEARN PADS began (a cancel puts them back) */
    /* the drive browser: the open folder ("" = the drives, BUILTIN_DIR the sound list), its entries (folders first; li: a
     * sound list entry), the first row shown, the highlighted entry (br_sel: previewed, LOAD loads it) and the scroll strip's
     * part row; the worker lists a folder (br_req); a WAV picked loads by path (req = -2, req_path), browsed: the sound came
     * from it (cur_path). fold_step: the sound arrows on a browsed sound, the next WAV of its folder (the worker finds it) */
    char br_dir[512]; struct br_ent { char name[96]; int dir, li; } *br; int br_n, br_top, br_req, br_scanning, br_sel;
    int br_tops[24], br_depth, br_back_top;   /* the page each folder was on when one inside it opened (BACK goes back to it) */
    short br_hist[128]; unsigned br_coast_blk; char br_rst[NROWS];   /* br_rst: a row strip handed back to the middle (browse_row) */   /* br_top over the last 128 blocks; the last block a coast moved it */
    float br_pos, br_vel, br_carry, br_g_rows; int br_g_on; unsigned br_g_blk, br_drag_blk, aud_due;   /* a drag and its coast (br_coast); a preview waiting for the wheel to rest */
    float br_acc; int br_busy; char br_back[96];   /* br_back: BACK came out of this folder (it is highlighted) */        /* br_busy: the browser thread is loading a preview or finding a step's WAV */
    char req_path[512], req_name[64], cur_path[512]; int browsed, load_fail, fold_step;
    /* the preview: a zone played as it is from its start (render). aud_own: one the worker loaded for it (aud_pending until
     * render takes it, aud_retired until the worker frees it); else the sound's own (aud_on_load: the next sound to arrive) */
    zone_t *aud_own, *aud_pending, *aud_retired; const zone_t *aud_z; double aud_pos; float aud_env;
    int aud_req, aud_on_load, aud_stop; char aud_path[512];
    pad_t pad[NPADS];
    int hold_on[NPADS];               /* HOLD pads that are latched */
    /* which pad a note plays (pad_for_note): 0..31 a pad, 32..47 that pad of the bank, -1 none; learned or built in;
     * hit_note/hit_pad: the last note heard and where it went (the SOUND page shows it) */
    signed char padmap[128]; int learn, save_pads, unmapped_note, learned, hit_note, hit_pad; long unmapped_frames;
    int abs_banks;                    /* the built-in map heard a bank B note of its own: MPC's banks send their own notes */
    float bpm_host;
    int last_note, last_vel, last_pad, nactive;
    long trig_frames;
    float bend;
    unsigned rng, age, hit_id;
    int fxpick_prev, fxpick_lay[FX_LAYERS - 1], fxpick_pad, fx_msg; unsigned fxpick_blk, fx_msg_blk;   /* the FX page: its last pick (the preset before it, the pad, when); what a double tap did */
    fft_t fft[NSIZES];
    /* a hop's scratch (one voice at a time) */
    float fa[2][NMAX] __attribute__((aligned(16))), fb[NMAX] __attribute__((aligned(16)));
    float XL[2 * MMAX + 4] __attribute__((aligned(16))), XR[2 * MMAX + 4] __attribute__((aligned(16))),
          XB[2 * MMAX + 4] __attribute__((aligned(16))), Y[2 * MMAX + 4] __attribute__((aligned(16)));
    float tmp[MMAX + 1];
    float oL[MMAX + 1], oR[MMAX + 1], oM[MMAX + 1], oph[MMAX + 1], ow[MMAX + 1];
    int peaks[MMAX + 1];
    voice_t v[NV];
    /* sound list and the worker */
    entry_t *list; int nlist;
    char user_dir[512]; int builtins;
    inst_t *cur;
    pthread_t th, bth; pthread_mutex_t mu; pthread_cond_t cv, bcv;   /* the worker and the browser thread, one lock */
    int quit, req, req_gen, done_gen, loading, rescan_req, find_req;
    inst_t *pending, *retired, *drums_job;   /* drums_job: a sound whose drums the worker separates next (the first one) */
    unsigned blk, rtg_blk;            /* blocks rendered: the vocoder slots (render_voice); the last a voice played a hit clean again in */
    float lim_env, lim_g;
    float *warp;                      /* WARP-SPEED's buffers: NV voices x 2 channels x WARP_MAX */
    /* UNDO: up to UNDO_N steps back (a ring), each the chops, the pads and the values as they were before an edit; one
     * gesture (the same control moved without a pause) is one step */
    struct undo { long cue[NPADS]; pad_t pad[NPADS]; float val[NP]; } undo[UNDO_N]; int undo_n, undo_at, undo_kind; unsigned undo_blk;
    int br_wrow;                      /* the browser row strip the data wheel last turned (-1 none) */
    float sp_x[NP], sp_mag[NP]; unsigned sp_blk[NP], trg_blk[NP]; char sp_rst[NP];   /* sp_rst: a strip handed back 0.5 (strip_step) */   /* trg_blk: when each button was last pressed (0 never) */   /* spring_step, strip_step: the last value each was sent, its last step's size, when */
    /* the input log (User/input_log.txt, the browser thread writes it): what MPC sends, to tune touch and wheel by */
    struct ilog { unsigned blk; short i; float x; } ilog[ILOG_N]; volatile unsigned ilog_n; unsigned ilog_saved, ilog_blk;
    long st_sec; unsigned st_blk;     /* User/status.txt: when it was last written, and render's block count then */
} chrono_t;

static inline float frand(chrono_t *e) { e->rng ^= e->rng << 13; e->rng ^= e->rng >> 17; e->rng ^= e->rng << 5; return (float)(e->rng & 0xffffff) / 8388608.0f - 1.0f; }
static inline void mark_changed(chrono_t *e, int i) { __atomic_or_fetch(&e->changed[i >> 6], 1ull << (i & 63), __ATOMIC_RELAXED); }
static int opt(const chrono_t *e, int p) { return (int)lrintf(e->val[p]); }
/* the sound's tempo as played: SOUND BPM's (40 + n - 1) or the one found (0 none), plus BPM FINE (+-1 BPM) */
static float snd_bpm(const chrono_t *e, const inst_t *in) {
    int s = opt(e, P_SOUND_BPM);
    float b = s > 0 ? 39.0f + s : in ? in->bpm : 0;
    return b > 0 ? b + e->val[P_BPM_FINE] : 0;
}
/* a picture param's place in vis[]: two runs of params (wv000..sb, then the tab numbers) */
static inline int vis_slot(int i) { return i < P_VIS2 ? i - P_VIS0 : i < P_VIS3 ? NVIS1 + i - P_VIS2 : NVIS2 + i - P_VIS3; }
static inline float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static inline float fast_tanh(float x) { if (x > 3) return 1; if (x < -3) return -1; float x2 = x * x; return x * (27 + x2) / (27 + 9 * x2); }
static inline float db2lin(float d) { return powf(10.0f, d / 20.0f); }
static inline float fatan2(float y, float x) {   /* |error| < 1e-5 rad */
    float ax = fabsf(x), ay = fabsf(y), mx = ax > ay ? ax : ay, mn = ax > ay ? ay : ax;
    if (mx < 1e-30f) return 0;
    float a = mn / mx, s = a * a;
    float r = (((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s) * a + a;
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0) r = 3.14159274f - r;
    return y < 0 ? -r : r;
}
static inline float ffloor(float x) { int i = (int)x; return (float)(i - (x < (float)i)); }   /* floorf without the libm call (armv7 has no rounding instruction) */
static inline float flog2(float x) {   /* about 0.01 off */
    union { float f; uint32_t i; } u = { x };
    float ex = (float)(int)((u.i >> 23) & 255) - 128.0f;
    u.i = (u.i & 0x007fffff) | 0x3f800000;
    return ex + (-0.34484843f * u.f + 2.02466578f) * u.f - 0.67487759f;
}
static inline float fexp2(float x) {
    if (x < -60) return 0;
    float fi = ffloor(x), f = x - fi;
    union { float f; uint32_t i; } u; u.i = (uint32_t)((int)fi + 127) << 23;
    return u.f * (1 + f * (0.6960656f + f * (0.2244200f + f * 0.0794902f)));
}
static inline float wrap_pi(float p) { return p - TWO_PI * ffloor((p + PI) * (1.0f / TWO_PI)); }
static inline float sin_half(float x) {   /* sin on [-pi/2, pi/2], |error| < 2e-7 */
    float x2 = x * x;
    return x * (1 + x2 * (-0.16666667f + x2 * (0.0083333310f + x2 * (-0.00019840874f + x2 * 2.7525562e-6f))));
}
static inline void fast_sincos(float p, float *s, float *c) {   /* p in [-pi, pi] (wrap_pi's range) */
    float a = p > 1.57079637f ? 3.14159274f - p : p < -1.57079637f ? -3.14159274f - p : p;
    float q = p + 1.57079637f; if (q > 3.14159274f) q -= 6.28318548f;
    q = q > 1.57079637f ? 3.14159274f - q : q < -1.57079637f ? -3.14159274f - q : q;
    *s = sin_half(a); *c = sin_half(q);
}
static float note_hz(float n) { return 440.0f * powf(2.0f, (n - 69.0f) / 12.0f); }

/* the mapped knobs */
static float transient_s(float x) { return 0.3f * x * x; }                        /* 0 .. 300 ms */
static float attack_s(float x) { return 0.0005f * powf(8000.0f, x); }            /* 0.5 ms .. 4 s */
static float release_s(float x) { return 0.01f * powf(800.0f, x); }              /* 10 ms .. 8 s */

/* ---- FFT: PFFFT's ordered real transforms (src/pffft), through aligned scratch so callers' buffers need no alignment ---- */
static int fft_init(fft_t *t, int N) {
    memset(t, 0, sizeof *t);
    t->N = N; t->M = N / 2;
    t->setup = pffft_new_setup(N, PFFFT_REAL);
    t->in = pffft_aligned_malloc(sizeof(float) * N); t->out = pffft_aligned_malloc(sizeof(float) * N);
    t->work = pffft_aligned_malloc(sizeof(float) * N);
    t->hann = malloc(sizeof(float) * N); t->sine = malloc(sizeof(float) * N);
    if (!t->setup || !t->in || !t->out || !t->work || !t->hann || !t->sine) return 0;
    for (int n = 0; n < N; n++) { t->hann[n] = (float)(0.5 - 0.5 * cos(2 * M_PI * n / N)); t->sine[n] = (float)sin(M_PI * (n + 0.5) / N); }
    return 1;
}
static void fft_free(fft_t *t) {
    if (t->setup) pffft_destroy_setup(t->setup);
    pffft_aligned_free(t->in); pffft_aligned_free(t->out); pffft_aligned_free(t->work);
    free(t->hann); free(t->sine);
}

/* x[N] -> X[0..M] (interleaved re/im, 2M + 2 floats, the DC and Nyquist bins with zero imaginary parts) */
#define ALIGNED(p) (((uintptr_t)(p) & 15) == 0)
static void rfft(const fft_t *t, const float *x, float *X) {
    const float *in = x;
    if (!ALIGNED(x)) { memcpy(t->in, x, sizeof(float) * t->N); in = t->in; }
    float *out = ALIGNED(X) ? X : t->out;
    pffft_transform_ordered(t->setup, in, out, t->work, PFFFT_FORWARD);
    if (out != X) memcpy(X, out, sizeof(float) * t->N);
    X[t->N] = X[1]; X[t->N + 1] = 0; X[1] = 0;   /* PFFFT packs Nyquist into bin 0's imaginary part */
}

/* X[0..M] -> x[N] times N (the caller scales: the vocoder folds it into its overlap-add); X is scratch (bin 0 repacked) */
static void irfft_n(const fft_t *t, float *X, float *x) {
    X[1] = X[t->N];
    const float *in = X;
    if (!ALIGNED(X)) { memcpy(t->in, X, sizeof(float) * t->N); in = t->in; }
    float *out = ALIGNED(x) ? x : t->out;
    pffft_transform_ordered(t->setup, in, out, t->work, PFFFT_BACKWARD);
    if (out != x) memcpy(x, out, sizeof(float) * t->N);
}

/* X[0..M] -> x[N], so that irfft(rfft(x)) = x */
static void irfft(const fft_t *t, float *X, float *x) {
    irfft_n(t, X, x);
    float g = 1.0f / t->N;
    for (int n = 0; n < t->N; n++) x[n] *= g;
}

static int size_index(int N) { int i = 0; while ((256 << i) < N) i++; return i; }

/* ---- WAV loading (to 44.1 kHz int16, the first two channels) ---- */
static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

static int load_wav(const char *path, zone_t *z, int max_sec) {   /* at most its first max_sec seconds */
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char h[12];
    int ok = 0, fmt = 0, ch = 0, bits = 0, have_fmt = 0;
    unsigned rate = 0;
    long data_off = -1; unsigned data_len = 0;
    float *tmp = NULL;
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) goto out;
    for (;;) {
        unsigned char c[8];
        if (fread(c, 1, 8, f) != 8) break;
        unsigned sz = rd32(c + 4);
        long next = ftell(f) + sz + (sz & 1);
        if (!memcmp(c, "fmt ", 4) && sz >= 16) {
            unsigned char b[40] = {0};
            if (fread(b, 1, sz < 40 ? sz : 40, f) < 16) goto out;
            fmt = rd16(b); ch = rd16(b + 2); rate = rd32(b + 4); bits = rd16(b + 14);
            if (fmt == 0xFFFE && sz >= 26) fmt = rd16(b + 24);
            have_fmt = 1;
        } else if (!memcmp(c, "data", 4)) { data_off = ftell(f); data_len = sz; }
        if (fseek(f, next, SEEK_SET)) break;
    }
    if (!have_fmt || data_off < 0 || ch < 1 || rate < 4000 || rate > 192000) goto out;
    if (!((fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && bits == 32))) goto out;
    int bps = bits / 8, nch = ch >= 2 ? 2 : 1;
    long frames = data_len / (unsigned)(bps * ch);
    if (frames > (long)rate * max_sec) frames = (long)rate * max_sec;
    if (frames < 16) goto out;
    tmp = malloc((size_t)frames * nch * sizeof(float));
    unsigned char *raw = malloc((size_t)4096 * bps * ch);
    if (!tmp || !raw) { free(raw); goto out; }
    fseek(f, data_off, SEEK_SET);
    long done = 0;
    while (done < frames) {
        long n = frames - done > 4096 ? 4096 : frames - done;
        long got = (long)fread(raw, (size_t)(bps * ch), (size_t)n, f);
        if (got <= 0) break;
        for (long i = 0; i < got; i++)
            for (int c = 0; c < nch; c++) {
                const unsigned char *s = raw + (i * ch + c) * bps;
                float x;
                if (fmt == 3) { float fl; memcpy(&fl, s, 4); x = fl; }
                else if (bits == 8) x = (s[0] - 128) / 128.0f;
                else if (bits == 16) x = (int16_t)rd16(s) / 32768.0f;
                else if (bits == 24) x = (float)((int32_t)((unsigned)s[0] << 8 | (unsigned)s[1] << 16 | (unsigned)s[2] << 24) >> 8) / 8388608.0f;
                else x = (float)(int32_t)rd32(s) / 2147483648.0f;
                tmp[(done + i) * nch + c] = x == x ? clampf(x, -1, 1) : 0;
            }
        done += got;
    }
    free(raw);
    if (done < 16) goto out;
    /* to 44.1 kHz: cubic interpolation (a gentle low-pass first when going down a lot) */
    double step = rate / (double)SR;
    long outn = (long)(done / step);
    if (outn < 16) goto out;
    z->data = malloc((size_t)outn * nch * sizeof(int16_t));
    if (!z->data) goto out;
    if (step > 1.3) {   /* two one-pole passes at the new Nyquist */
        float a = 1 - expf(-2 * PI * 0.45f * SR / rate);
        for (int c = 0; c < nch; c++) for (int pass = 0; pass < 2; pass++) {
            float y = 0;
            for (long i = 0; i < done; i++) { y += a * (tmp[i * nch + c] - y); tmp[i * nch + c] = y; }
        }
    }
    for (long i = 0; i < outn; i++) {
        double p = i * step; long k = (long)p; float fr = (float)(p - k);
        for (int c = 0; c < nch; c++) {
            float y0 = tmp[(k > 0 ? k - 1 : 0) * nch + c], y1 = tmp[k * nch + c];
            float y2 = tmp[(k + 1 < done ? k + 1 : done - 1) * nch + c], y3 = tmp[(k + 2 < done ? k + 2 : done - 1) * nch + c];
            float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2 * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
            float y = ((c3 * fr + c2) * fr + c1) * fr + y1;
            z->data[i * nch + c] = (int16_t)lrintf(clampf(y, -1, 1) * 32767.0f);
        }
    }
    z->nch = nch; z->len = (int)outn;
    ok = 1;
out:
    free(tmp);
    fclose(f);
    return ok;
}


/* ---- built-in sounds, synthesized ---- */
static inline float blep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1; }
    if (t > 1 - dt) { t = (t - 1) / dt; return t * t + t + t + 1; }
    return 0;
}
static inline float saw(float *ph, float inc) {
    float y = 2 * *ph - 1 - blep(*ph, inc);
    *ph += inc; if (*ph >= 1) *ph -= 1;
    return y;
}
typedef struct { float s1, s2; } svf_t;
static inline float svf(svf_t *f, float hz, float q, float x, int mode) {   /* TPT state variable: 0 lp, 1 bp, 2 hp */
    float g = tanf(PI * clampf(hz, 10, 20000) / SR), k = 1.0f / q;
    float hp = (x - (k + g) * f->s1 - f->s2) / (1 + g * (g + k));
    float bp = g * hp + f->s1; f->s1 = g * hp + bp;
    float lp = g * bp + f->s2; f->s2 = g * bp + lp;
    return mode == 0 ? lp : mode == 1 ? bp : hp;
}
typedef struct { float y1, y2, a1, a2, g; } res_t;   /* a two-pole resonator (a formant) */
static void res_set(res_t *r, float hz, float bw, float gain_db) {
    float rr = expf(-PI * bw / SR);
    r->y1 = r->y2 = 0;
    r->a1 = 2 * rr * cosf(2 * PI * hz / SR); r->a2 = -rr * rr; r->g = (1 - rr) * db2lin(gain_db) * 2.0f;
}
static inline float res_run(res_t *r, float x) { float y = r->g * x + r->a1 * r->y1 + r->a2 * r->y2; r->y2 = r->y1; r->y1 = y; return y; }
static unsigned brng = 12345;
static inline float bnoise(void) { brng ^= brng << 13; brng ^= brng >> 17; brng ^= brng << 5; return (float)(brng & 0xffffff) / 8388608.0f - 1.0f; }

/* a sung voice: a Rosenberg glottal pulse (differentiated), vibrato, breath, through five formants */
static void sing(float *out, long n, float f0, float t_on, const float *F, const float *B, const float *G, float vib_rate,
                 float level, float pan, int nch) {
    res_t r[5];
    for (int i = 0; i < 5; i++) res_set(&r[i], F[i], B[i], G[i]);
    float ph = bnoise() * 0.5f + 0.5f, last = 0, vph = bnoise();
    long t0 = (long)(t_on * SR);
    for (long i = t0; i < n; i++) {
        float t = (i - t0) / SR;
        float vib = 0.35f * fminf(1, fmaxf(0, (t - 0.35f) / 0.5f)) * sinf(2 * PI * (vib_rate * t + vph));
        float f = f0 * powf(2.0f, (vib + 0.03f * bnoise()) / 12.0f);
        ph += f / SR; if (ph >= 1) ph -= 1;
        float g = ph < 0.4f ? 0.5f * (1 - cosf(PI * ph / 0.4f)) : ph < 0.56f ? cosf(PI * (ph - 0.4f) / 0.32f) : 0;
        float x = (g - last) * 8.0f + 0.04f * bnoise(); last = g;
        float y = 0;
        for (int k = 0; k < 5; k++) y += res_run(&r[k], x);
        float env = fminf(1, t / 0.08f) * level;
        if (nch == 2) { out[2 * i] += y * env * (1 - pan); out[2 * i + 1] += y * env * (1 + pan); }
        else out[i] += y * env;
    }
}

static int synth_builtin(int idx, zone_t *z) {
    static const float SEC[NBUILTIN] = { 1.6f, 2.3f, 2.8f, 3.6f, 3.2f, 5.3333333f, 3.4f, 1.6f };
    static const int ROOT[NBUILTIN] = { 60, 60, 60, 60, 60, 60, 60, 48 };
    long n = (long)(SEC[idx] * SR);
    int nch = idx == 1 ? 1 : 2;
    float *b = calloc((size_t)n * nch, sizeof(float));
    if (!b) return 0;
    brng = 0x2545F491u + (unsigned)idx * 977u;
    switch (idx) {
    case 0: {   /* Horn Stab: C minor 7, two saws a note, a brass filter envelope, a scoop into pitch */
        static const int NT[4] = { 60, 63, 67, 70 };
        for (int k = 0; k < 4; k++) for (int d = 0; d < 2; d++) {
            float ph = bnoise() * 0.5f + 0.5f, det = d ? 0.07f : -0.07f, pan = (k - 1.5f) * 0.25f + (d ? 0.1f : -0.1f);
            svf_t f = {0, 0};
            for (long i = 0; i < n; i++) {
                float t = i / SR;
                float scoop = -0.5f * expf(-t * 40), hz = note_hz(NT[k] + det + scoop);
                float x = saw(&ph, hz / SR);
                float fe = (1 - expf(-t * 60)) * expf(-t * 2.8f);
                float y = svf(&f, 500 + 5200 * fe, 0.9f, x, 0);
                float a = fminf(1, t / 0.025f) * (t < 0.15f ? 1 : 0.55f + 0.45f * expf(-(t - 0.15f) * 6)) * (t > 0.75f ? expf(-(t - 0.75f) * 5) : 1);
                b[2 * i] += 0.11f * y * a * (1 - pan); b[2 * i + 1] += 0.11f * y * a * (1 + pan);
            }
        }
        break;
    }
    case 1: {   /* Vocal Aah, C3 */
        static const float F[5] = { 700, 1220, 2600, 3300, 3750 }, B[5] = { 80, 90, 120, 150, 200 }, G[5] = { 0, -5, -12, -16, -22 };
        sing(b, n, note_hz(60), 0, F, B, G, 5.2f, 0.25f, 0, 1);
        for (long i = 0; i < n; i++) { float t = i / SR; if (t > 1.9f) b[i] *= expf(-(t - 1.9f) * 9); }
        break;
    }
    case 2: {   /* Soul Keys: an FM electric piano, C minor 9, rolled */
        static const int NT[5] = { 48, 63, 67, 70, 74 };
        for (int k = 0; k < 5; k++) {
            float f = note_hz(NT[k]), pan = (k - 2) * 0.18f; long t0 = (long)(k * 0.014f * SR);
            for (long i = t0; i < n; i++) {
                float t = (i - t0) / SR, w = 2 * PI * f * t;
                float I = 2.0f * expf(-t * 3.5f) + 0.35f;
                float y = sinf(w + I * sinf(w)) + 0.18f * sinf(w * 14.0f) * expf(-t * 40);
                float a = expf(-t * 1.0f) * (1 - expf(-t * 400)) * (NT[k] < 55 ? 0.8f : 1.0f);
                float trem = 1 + 0.25f * sinf(2 * PI * 4.6f * t);
                b[2 * i] += 0.12f * y * a * (1 - pan) * trem; b[2 * i + 1] += 0.12f * y * a * (1 + pan) * (2 - trem);
            }
        }
        break;
    }
    case 3: {   /* String Swell: ensemble saws, C minor, swelling in */
        static const int NT[4] = { 48, 55, 63, 67 };
        for (int k = 0; k < 4; k++) for (int d = 0; d < 5; d++) {
            float ph = bnoise() * 0.5f + 0.5f, det = (d - 2) * 0.06f + 0.02f * bnoise(), pan = bnoise() * 0.7f, vr = 4.6f + bnoise();
            svf_t f = {0, 0};
            for (long i = 0; i < n; i++) {
                float t = i / SR, hz = note_hz(NT[k] + det + 0.08f * sinf(2 * PI * vr * t));
                float y = svf(&f, 2600, 0.6f, saw(&ph, hz / SR), 0);
                float a = (t < 1.3f ? powf(t / 1.3f, 1.6f) : 1) * (t > 2.9f ? expf(-(t - 2.9f) * 6) : 1);
                b[2 * i] += 0.035f * y * a * (1 - pan); b[2 * i + 1] += 0.035f * y * a * (1 + pan);
            }
        }
        break;
    }
    case 4: {   /* Choir Ooh: three parts, two singers each */
        static const float F[5] = { 320, 800, 2240, 3000, 3600 }, B[5] = { 60, 90, 130, 160, 200 }, G[5] = { 0, -4, -18, -22, -28 };
        static const int NT[3] = { 55, 60, 63 };
        for (int k = 0; k < 3; k++) for (int d = 0; d < 2; d++)
            sing(b, n, note_hz(NT[k] + (d ? 0.08f : -0.06f)), 0.03f * (k + d), F, B, G, 4.8f + 0.7f * d + 0.3f * k, 0.09f,
                 (k - 1) * 0.4f + (d ? 0.2f : -0.2f), 2);
        for (long i = 0; i < n; i++) { float t = i / SR, a = (t < 0.4f ? t / 0.4f : 1) * (t > 2.6f ? expf(-(t - 2.6f) * 7) : 1); b[2 * i] *= a; b[2 * i + 1] *= a; }
        break;
    }
    case 5: {   /* Break 90: two bars of boom-bap, swung 16ths */
        static const char *const K = "x.....x...x.....x.....x..x.x....", *const S = "....x.......x.......x.......x..x",
                          *const H = "x.x.x.x.x.x.x.x.x.x.x.x.x.x.xox.";
        float step = 60.0f / 90.0f / 4.0f;
        for (int s = 0; s < 32; s++) {
            long t0 = (long)((s * step + (s & 1 ? 0.028f : 0)) * SR);
            if (K[s] == 'x') for (long i = t0; i < n && i < t0 + (long)(0.45f * SR); i++) {
                float t = (i - t0) / SR, ph = 2 * PI * (45 * t + 90 / 35.0f * (1 - expf(-t * 35)));
                float y = sinf(ph) * expf(-t * 6.5f) + (t < 0.003f ? 0.6f * bnoise() : 0);
                y = fast_tanh(1.6f * y) * 0.55f;
                b[2 * i] += y; b[2 * i + 1] += y;
            }
            if (S[s] == 'x') { svf_t f = {0, 0}; float lv = s == 31 ? 0.35f : 1.0f;
                for (long i = t0; i < n && i < t0 + (long)(0.35f * SR); i++) {
                    float t = (i - t0) / SR;
                    float y = 0.35f * sinf(2 * PI * 185 * t) * expf(-t * 28) + 0.55f * svf(&f, 1900, 0.7f, bnoise(), 1) * expf(-t * 13);
                    b[2 * i] += 0.5f * lv * y; b[2 * i + 1] += 0.5f * lv * y;
                }
            }
            if (H[s] != '.') { svf_t f = {0, 0}; float dk = H[s] == 'o' ? 11 : 55, lv = (s & 2) ? 0.10f : 0.14f;
                for (long i = t0; i < n && i < t0 + (long)(0.4f * SR); i++) {
                    float t = (i - t0) / SR, y = svf(&f, 8000, 0.8f, bnoise(), 2) * expf(-t * dk) * lv;
                    b[2 * i] += 0.8f * y; b[2 * i + 1] += 1.2f * y;
                }
            }
        }
        for (long i = 0; i < 2 * n; i++) b[i] = fast_tanh(b[i]);
        break;
    }
    case 6: {   /* Music Box: a phrase of tines */
        static const int NT[10] = { 72, 75, 79, 84, 82, 79, 75, 77, 74, 72 };
        for (int k = 0; k < 10; k++) {
            float f = note_hz(NT[k]), pan = bnoise() * 0.4f; long t0 = (long)(k * 0.28f * SR);
            for (long i = t0; i < n; i++) {
                float t = (i - t0) / SR, w = 2 * PI * f * t;
                float y = sinf(w) * expf(-t * 1.6f) + 0.3f * sinf(w * 2.76f) * expf(-t * 5) + 0.1f * sinf(w * 5.4f) * expf(-t * 11);
                y *= 1 - expf(-t * 900);
                b[2 * i] += 0.14f * y * (1 - pan); b[2 * i + 1] += 0.14f * y * (1 + pan);
            }
        }
        break;
    }
    case 7: {   /* Bass Pluck, C2 */
        float ph = 0, ph2 = 0, f = note_hz(48); svf_t fl = {0, 0};
        for (long i = 0; i < n; i++) {
            float t = i / SR;
            float x = saw(&ph, f / SR) * 0.6f + (ph2 < 0.5f ? 0.5f : -0.5f);
            ph2 += f * 0.5f / SR; if (ph2 >= 1) ph2 -= 1;
            float y = svf(&fl, 180 + 2600 * expf(-t * 11), 1.1f, x, 0);
            float a = expf(-t * 1.7f) * (1 - expf(-t * 500)) * 0.5f;
            b[2 * i] += y * a; b[2 * i + 1] += y * a;
        }
        break;
    }
    }
    float pk = 1e-6f;
    for (long i = 0; i < n * nch; i++) pk = fmaxf(pk, fabsf(b[i]));
    float g = 0.89f / pk;
    z->data = malloc((size_t)n * nch * sizeof(int16_t));
    if (!z->data) { free(b); return 0; }
    for (long i = 0; i < n * nch; i++) z->data[i] = (int16_t)lrintf(clampf(b[i] * g, -1, 1) * 32767.0f);
    free(b);
    z->nch = nch; z->len = (int)n; z->root = ROOT[idx];
    snprintf(z->file, sizeof z->file, "%s", BUILTIN[idx]);
    return 1;
}

/* ---- sound analysis at load ---- */
static inline float zsample(const zone_t *z, long i, int c) {
    if (i < 0 || i >= z->len) return 0;
    return z->data[i * z->nch + (z->nch == 2 ? c : 0)] * (1.0f / 32768.0f);
}
static inline float zmid(const zone_t *z, long i) { return z->nch == 2 ? 0.5f * (zsample(z, i, 0) + zsample(z, i, 1)) : zsample(z, i, 0); }

/* onset candidates of zone 0: a rise of the high-passed energy over its recent average, in 256-frame steps */

/* ---- analysis at load (the worker) ---- */
/* the onset envelope: half-wave rectified log-spectral flux of the mid, a 1024 FFT every ON_HOP frames, scaled to 0..1;
 * value t describes the frames around t * ON_HOP + 512 */
static void analyse_onsets(inst_t *in, fft_t *t) {
    const zone_t *z = &in->z;
    int n = z->len / ON_HOP + 1, M = t->M;
    in->onset = calloc((size_t)n, sizeof(float));
    float *x = malloc(sizeof(float) * t->N), *X = malloc(sizeof(float) * (t->N + 2)), *prev = calloc((size_t)M + 1, sizeof(float));
    if (!in->onset || !x || !X || !prev) { free(x); free(X); free(prev); free(in->onset); in->onset = NULL; return; }
    float top = 1e-9f;
    for (int f = 0; f < n; f++) {
        long s = (long)f * ON_HOP;
        for (int i = 0; i < t->N; i++) x[i] = zmid(z, s + i) * t->hann[i];
        rfft(t, x, X);
        float flux = 0;
        for (int k = 2; k < M; k++) {   /* above ~40 Hz */
            float l = logf(1 + 100 * hypotf(X[2 * k], X[2 * k + 1]));
            if (l > prev[k]) flux += l - prev[k];
            prev[k] = l;
        }
        in->onset[f] = flux;   /* the first frame too: a sound that starts on a hit starts with one */
        if (in->onset[f] > top) top = in->onset[f];
    }
    for (int f = 0; f < n; f++) in->onset[f] /= top;
    in->non = n;
    free(x); free(X); free(prev);
}

/* tempo: the onset envelope's autocorrelation over 60..200 BPM, each lag scored with its double and its four-fold (the bar), weighted towards
 * 100 BPM; then the beat grid's phase. No clear peak: no tempo (bpm 0). */
static void analyse_tempo(inst_t *in) {
    in->bpm = 0; in->beat0 = 0;
    int n = in->non;
    if (n < 200) return;
    const float *o = in->onset;
    double mean = 0; for (int i = 0; i < n; i++) mean += o[i]; mean /= n;
    enum { LMAX = 400 };   /* up to four beats at 60 BPM */
    double r[LMAX + 2] = {0}, r0 = 0;
    for (int i = 0; i < n; i++) r0 += (o[i] - mean) * (o[i] - mean);
    for (int l = 10; l <= LMAX; l++) { double a = 0; for (int i = 0; i + l < n; i++) a += (o[i] - mean) * (o[i + l] - mean); r[l] = a / r0; }
    int best = 0; double bs = -1e9;
    for (int l = 18; l <= 90; l++) {   /* 60*SR / (ON_HOP * l): about 287 .. 57 BPM, kept to 60..200 below */
        double bpm = 60.0 * SR / (ON_HOP * (double)l);
        if (bpm < 60 || bpm > 200) continue;
        double w = exp(-0.5 * pow(log2(bpm / 100.0) / 0.9, 2));
        /* a beat lines up at two beats and at the bar too; the lag is fractional, so each multiple is looked for close by */
        double r2 = -1, r4 = -1;
        for (int d = -2; d <= 2; d++) r2 = fmax(r2, r[2 * l + d]);
        for (int d = -4; d <= 4 && 4 * l + d <= LMAX; d++) r4 = fmax(r4, r[4 * l + d]);
        double s = (r[l] + r2 + 0.5 * r4) * w;
        if (s > bs) { bs = s; best = l; }
    }
    if (!best || r[best] < 0.06) return;   /* nothing rhythmic */
    /* sampling tempos sit around 70..140: above 145, a lag twice as long that also correlates is the beat (eighth-note
     * hats would otherwise double it) */
    while (60.0 * SR / (ON_HOP * (double)best) > 145 && 2 * best + 2 <= LMAX) {
        int b2 = 2 * best - 2;
        for (int l = 2 * best - 1; l <= 2 * best + 2; l++) if (r[l] > r[b2]) b2 = l;
        if (r[b2] < 0.4 * r[best]) break;
        best = b2;
    }
    double y0 = r[best - 1], y1 = r[best], y2 = r[best + 1], d = y0 - 2 * y1 + y2;
    double lag = best + (fabs(d) > 1e-12 ? 0.5 * (y0 - y2) / d : 0);
    in->bpm = (float)(60.0 * SR / (ON_HOP * lag));
    /* phase: the offset whose beats collect the most onset */
    double bp = 0; int steps = (int)(lag * 4);
    for (int s = 0; s < steps; s++) {
        double ph = s * lag / steps, sum = 0;
        for (double p = ph; p < n; p += lag) sum += o[(int)p];
        if (sum > bp) { bp = sum; in->beat0 = ph * ON_HOP + 512; }
    }
}

/* key: a chromagram (8192-point frames, 65 Hz .. 5 kHz, spectral peaks at their interpolated frequency) against two pairs of
 * major and minor profiles (Krumhansl-Kessler, and Albrecht-Shanahan's from pop and jazz), the best of the 24 summed
 * correlations. Whether the sound has a key at all is how tonal it is (the share of its energy in clear peaks): a drum loop
 * has little, music plenty, however loosely it fits a profile */
static void analyse_key(inst_t *in) {
    static const double KK[2][12] = { { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 },
                                      { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 } };
    static const double AS[2][12] = { { .238, .006, .111, .006, .137, .094, .016, .214, .009, .080, .008, .081 },
                                      { .220, .006, .104, .123, .019, .103, .012, .214, .062, .022, .061, .052 } };
    enum { FN = 8192, K0 = 12, K1 = 929 };   /* 65 Hz .. 5 kHz */
    if (in->key_named) return;   /* the file's name says it */
    in->key_conf = 0; in->key_tonal = 0;   /* in->key is written once, at the end: render() reads it */
    fft_t t;
    if (!fft_init(&t, FN)) { fft_free(&t); return; }
    const zone_t *z = &in->z;
    const int16_t *dr = in->drums;   /* once separated, the drums are taken out first: they have no key */
    int nch = z->nch;
    float *x = malloc(sizeof(float) * FN), *X = malloc(sizeof(float) * (FN + 2));
    double *mg = malloc(sizeof(double) * (K1 + 2));
    double chroma[12] = {0}, wsum = 0, lsum = 1e-12;
    int frames = 0;
    long hop = z->len > 1000L * FN ? z->len / 1000 : FN / 2;
    for (long s = 0; x && X && mg && s + FN <= z->len; s += hop, frames++) {
        for (int i = 0; i < FN; i++) {
            float v = zmid(z, s + i);
            if (dr) { long k = (s + i) * nch; v -= nch == 2 ? (dr[k] + dr[k + 1]) * (0.5f / 32768.0f) : dr[k] / 32768.0f; }
            x[i] = v * t.hann[i];
        }
        rfft(&t, x, X);
        double c[12] = {0}, tot = 1e-9, peaks = 0, all = 1e-12;
        for (int k = K0 - 1; k <= K1 + 1; k++) { mg[k - K0 + 1] = hypot(X[2 * k], X[2 * k + 1]); all += mg[k - K0 + 1] * mg[k - K0 + 1]; }
        for (int k = K0 + 1; k < K1; k++) {   /* the peaks, the low ones (the bass, the roots) weighing more */
            double m0 = mg[k - K0], m1 = mg[k - K0 + 1], m2 = mg[k - K0 + 2];
            if (!(m1 > m0 && m1 >= m2 && m1 > 2 * fmax(mg[k - K0 - 1 < 0 ? 0 : k - K0 - 1], mg[k - K0 + 3]))) continue;
            double den = m0 - 2 * m1 + m2, dk = fabs(den) > 1e-12 ? 0.5 * (m0 - m2) / den : 0;
            double f = (k + dk) * SR / FN, w = sqrt(m1) / (1 + f / 800);
            int pc = ((int)lround(12 * log2(f / 16.3516)) % 12 + 12) % 12;
            c[pc] += w; tot += w; peaks += m1 * m1;
        }
        /* a frame counts by how tonal it is (its energy in clear peaks) and how loud */
        double fw = (peaks / all) * sqrt(all);
        for (int i = 0; i < 12; i++) chroma[i] += fw * c[i] / tot;
        wsum += fw; lsum += sqrt(all);
    }
    free(x); free(X); free(mg); fft_free(&t);
    if (frames < 2 || wsum <= 0) { in->key = -1; return; }
    double best = -4;
    int bk = -1;
    for (int mode = 0; mode < 2; mode++)
        for (int root = 0; root < 12; root++) {
            double sum = 0;
            for (int set = 0; set < 2; set++) {
                const double *p = set ? AS[mode] : KK[mode];
                double ma = 0, mb = 0, sab = 0, saa = 0, sbb = 0;
                for (int i = 0; i < 12; i++) { ma += chroma[(root + i) % 12]; mb += p[i]; }
                ma /= 12; mb /= 12;
                for (int i = 0; i < 12; i++) { double a = chroma[(root + i) % 12] - ma, b = p[i] - mb; sab += a * b; saa += a * a; sbb += b * b; }
                sum += sab / sqrt(saa * sbb + 1e-12);
            }
            if (sum > best) { best = sum; bk = mode * 12 + root; }
        }
    in->key_conf = (float)(best / 2);
    in->key_tonal = (float)(wsum / lsum);
    in->key = in->key_tonal >= KEY_MIN_TONAL ? bk : -1;   /* below it the sound is noise and hits (a drum loop): NO KEY */
}

/* the waveform for the screen: low (below ~200 Hz), mid and high (above ~2.5 kHz) peaks per WHOP frames, then each level
 * the peaks of four of the level below */
static void analyse_wave(inst_t *in) {
    const zone_t *z = &in->z;
    for (int l = 0; l < WLOD; l++) {
        int n = (int)(z->len / (WHOP << 2 * l)) + 1;
        for (int b = 0; b < 3; b++) if (!(in->wave[l][b] = calloc((size_t)n, sizeof(float)))) return;
        in->nwave[l] = n;
    }
    float al = 1 - expf(-2 * PI * 200 / SR), ah = 1 - expf(-2 * PI * 2500 / SR), lp = 0, lp2 = 0;
    for (long i = 0; i < z->len; i++) {
        float x = zmid(z, i);
        lp += al * (x - lp); lp2 += ah * (x - lp2);
        float lo = lp, hi = x - lp2, mid = x - lo - hi;
        long w = i / WHOP;
        if (fabsf(lo) > in->wave[0][0][w]) in->wave[0][0][w] = fabsf(lo);
        if (fabsf(mid) > in->wave[0][1][w]) in->wave[0][1][w] = fabsf(mid);
        if (fabsf(hi) > in->wave[0][2][w]) in->wave[0][2][w] = fabsf(hi);
    }
    for (int l = 1; l < WLOD; l++)
        for (int b = 0; b < 3; b++)
            for (int w = 0; w < in->nwave[l - 1]; w++) if (in->wave[l - 1][b][w] > in->wave[l][b][w >> 2]) in->wave[l][b][w >> 2] = in->wave[l - 1][b][w];
    in->wtop = 1e-9f;
    for (int w = 0; w < in->nwave[0]; w++) in->wtop = fmaxf(in->wtop, in->wave[0][0][w] + in->wave[0][1][w] + in->wave[0][2][w]);
}

/* ---- the drum layer: harmonic/percussive separation (median filters, soft masks), streamed frame by frame ---- */
static float median(float *a, int n) {   /* quickselect; a is scrambled */
    int lo = 0, hi = n - 1, k = n / 2;
    while (lo < hi) {
        float p = a[(lo + hi) / 2]; int i = lo, j = hi;
        while (i <= j) { while (a[i] < p) i++; while (a[j] > p) j--; if (i <= j) { float t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; } }
        if (k <= j) hi = j; else if (k >= i) lo = i; else break;
    }
    return a[k];
}

/* fills in->drums; stops early (and returns 0) when *abort_gen moves from gen */
static int separate_drums(inst_t *in, const volatile int *abort_gen, int gen) {
    const zone_t *z = &in->z;
    int N = HP_N, M = N / 2, H = HP_HOP, Q = HP_MED_T, C = Q / 2, CF = HP_MED_F / 2, nch = z->nch, kick_bins = (int)(300.0f * N / SR);
    fft_t t;
    if (!fft_init(&t, N)) { fft_free(&t); return 0; }
    in->drums = calloc((size_t)z->len * nch, sizeof(int16_t));
    float *spec = malloc(sizeof(float) * (size_t)Q * 2 * (N + 2));   /* a ring of Q frames' spectra, per channel */
    float *mag = malloc(sizeof(float) * (size_t)Q * (M + 1));       /* and their mid magnitudes */
    float *x = malloc(sizeof(float) * N), *Y = malloc(sizeof(float) * (N + 2)), *ola = calloc((size_t)2 * N, sizeof(float));
    float *col = malloc(sizeof(float) * Q), *row = malloc(sizeof(float) * Q), *mask = malloc(sizeof(float) * (M + 1));
    int ok = in->drums && spec && mag && x && Y && ola && col && row && mask;
    long frames = z->len / H + N / H + C + 1;   /* frame f starts at (f - N/H + 1) * H, so the sound's start is covered */
    for (long f = 0; ok && f < frames; f++) {
        if (*abort_gen != gen) { ok = 0; break; }
        long s = (f - N / H + 1) * H;
        int slot = (int)(f % Q);
        for (int c = 0; c < nch; c++) {
            for (int i = 0; i < N; i++) x[i] = zsample(z, s + i, c) * t.hann[i];
            rfft(&t, x, spec + ((size_t)slot * 2 + c) * (N + 2));
        }
        const float *L = spec + (size_t)slot * 2 * (N + 2), *R = nch == 2 ? L + (N + 2) : L;
        for (int k = 0; k <= M; k++) { float re = L[2 * k] + R[2 * k], im = L[2 * k + 1] + R[2 * k + 1]; mag[(size_t)slot * (M + 1) + k] = sqrtf(re * re + im * im); }
        long fc = f - C;   /* the centre frame, once Q are in */
        if (fc < 0) continue;
        int cs = (int)(fc % Q);
        const float *cm = mag + (size_t)cs * (M + 1);
        for (int k = 0; k <= M; k++) {
            for (int q = 0; q < Q; q++) col[q] = mag[(size_t)q * (M + 1) + k];   /* across time: the harmonic part */
            float h = median(col, Q);
            int n = 0;
            for (int j = k - CF; j <= k + CF; j++) row[n++] = j < 0 || j > M ? 0 : cm[j];   /* across frequency: the percussive */
            float p = median(row, n);
            /* below ~300 Hz a kick is a short tone, not a broadband click: there, energy above the sustained level is
             * percussive too */
            if (k <= kick_bins && cm[k] - h > p) p = cm[k] - h;
            mask[k] = p * p / (p * p + h * h + 1e-12f);
        }
        long cstart = (fc - N / H + 1) * H;
        for (int c = 0; c < nch; c++) {
            const float *S = spec + ((size_t)cs * 2 + c) * (N + 2);
            for (int k = 0; k <= M; k++) { Y[2 * k] = S[2 * k] * mask[k]; Y[2 * k + 1] = S[2 * k + 1] * mask[k]; }
            irfft(&t, Y, x);
            float *o = ola + (size_t)c * N;
            for (int i = 0; i < N; i++) o[i] += x[i] * t.hann[i] * (1.0f / 1.5f);
            for (int i = 0; i < H; i++) {
                long p = cstart + i;
                if (p >= 0 && p < z->len) in->drums[p * nch + c] = (int16_t)lrintf(clampf(o[i], -1, 1) * 32767.0f);
            }
            memmove(o, o + H, sizeof(float) * (N - H));
            memset(o + N - H, 0, sizeof(float) * H);
        }
        in->drums_pct = (int)(100 * f / frames);
    }
    free(spec); free(mag); free(x); free(Y); free(ola); free(col); free(row); free(mask);
    fft_free(&t);
    if (ok) { analyse_key(in); __atomic_store_n(&in->drums_ready, 1, __ATOMIC_RELEASE); }   /* the key again, without the drums */
    return ok;
}

static void free_inst(inst_t *in) {
    if (!in) return;
    free(in->z.data); free(in->drums); free(in->onset);
    for (int l = 0; l < WLOD; l++) for (int b = 0; b < 3; b++) free(in->wave[l][b]);
    free(in);
}

static int chop_transients(const inst_t *in, float sens, long *out, int n);

/* what a file's name says (sample packs name their loops "... 85 BPM ...", sometimes "... C# min ..."): the tempo, taken over
 * the one found (which is often a little off, or twice or half it), and the key, taken outright */
static void name_hints(inst_t *in, const char *name) {
    char low[256];
    int n = 0;
    for (; name[n] && n < (int)sizeof low - 1; n++) low[n] = (char)tolower((unsigned char)name[n]);
    low[n] = 0;
    for (const char *p = strstr(low, "bpm"); p; p = strstr(p + 3, "bpm")) {
        const char *q = p, *d;
        while (q > low && (q[-1] == ' ' || q[-1] == '_' || q[-1] == '-')) q--;
        for (d = q; d > low && isdigit((unsigned char)d[-1]); d--) {}
        int b = q - d >= 2 && q - d <= 3 ? atoi(d) : 0;
        if (b >= 40 && b <= 250) { in->bpm = (float)b; in->bpm_named = 1; break; }
    }
    static const int PC[7] = { 9, 11, 0, 2, 4, 5, 7 };   /* a .. g */
    for (int i = 0; i < n; i++) {
        if (low[i] < 'a' || low[i] > 'g' || (i && isalnum((unsigned char)low[i - 1]))) continue;
        int j = i + 1, pc = PC[low[i] - 'a'];
        if (low[j] == '#') { pc++; j++; } else if (low[j] == 'b' && !isalpha((unsigned char)low[j + 1])) { pc--; j++; }
        else if (low[j] == 'b' && (low[j + 1] == ' ' || low[j + 1] == '_' || low[j + 1] == 'm')) { pc--; j++; }
        if (low[j] == ' ' || low[j] == '_' || low[j] == '-') j++;
        int minor = -1;
        if (!strncmp(low + j, "minor", 5)) { minor = 1; j += 5; }
        else if (!strncmp(low + j, "major", 5)) { minor = 0; j += 5; }
        else if (!strncmp(low + j, "min", 3)) { minor = 1; j += 3; }
        else if (!strncmp(low + j, "maj", 3)) { minor = 0; j += 3; }
        else if (low[j] == 'm' && j == i + 1 + (name[i + 1] == '#' || name[i + 1] == 'b') && isupper((unsigned char)name[i])) { minor = 1; j++; }   /* "Am", "C#m" */
        if (minor < 0 || isalpha((unsigned char)low[j])) continue;
        in->key = minor * 12 + (pc + 12) % 12; in->key_named = 1; in->key_conf = 1; in->key_tonal = 1;
        break;
    }
}

static inst_t *load_entry(const entry_t *en) {
    inst_t *in = calloc(1, sizeof(inst_t));
    if (!in) return NULL;
    snprintf(in->name, sizeof in->name, "%s", en->name);
    in->builtin = en->builtin;
    int ok = en->builtin >= 0 ? synth_builtin(BUILTIN_SYN[en->builtin], &in->z) : load_wav(en->path, &in->z, MAX_FILE_SEC);
    if (!ok) { free(in); return NULL; }
    in->z.root = 60; in->z.lo = 0; in->z.hi = 127;
    in->sec = in->z.len / SR;
    in->mb = in->z.len * in->z.nch * 2 * 2 / 1048576.0f;   /* with its drum layer */
    fft_t t;
    if (fft_init(&t, 1024)) analyse_onsets(in, &t);
    fft_free(&t);
    if (in->onset) analyse_tempo(in);
    analyse_key(in);
    analyse_wave(in);
    if (en->builtin < 0) name_hints(in, en->name);
    in->nhits = chop_transients(in, 0.75f, in->hits, MAX_HITS);   /* where the stretch plays the hits clean (render_voice) */
    return in;
}

/* ---- chopping: cues by Chop Mode ---- */
typedef struct { long pos; float score; } cand_t;
static int cand_cmp(const void *a, const void *b) { float x = ((const cand_t *)a)->score, y = ((const cand_t *)b)->score; return x < y ? 1 : x > y ? -1 : 0; }
static int long_cmp(const void *a, const void *b) { long x = *(const long *)a, y = *(const long *)b; return x < y ? -1 : x > y; }

/* a hit to the frame: in 32-frame blocks of the mid's slope (its first difference: a hat over a ringing kick still stands
 * out) around an onset frame, the block that rises most over the two before it, less a block in front (so the chop starts
 * just before the attack, not on it) */
static long refine_hit(const zone_t *z, long centre) {
    enum { B = 32, NB = 48 };
    float en[NB];
    long s0 = centre - NB / 2 * B;
    for (int b = 0; b < NB; b++) {
        float a = 0;
        for (int i = 0; i < B; i++) {
            long k = s0 + b * B + i;
            float x = k >= 1 && k < z->len ? zmid(z, k) - zmid(z, k - 1) : 0;
            a += x * x;
        }
        en[b] = a;
    }
    int best = -1; float bs = 0;
    for (int b = 2; b < NB; b++) { float r = en[b] - fmaxf(en[b - 1], en[b - 2]); if (r > bs) { bs = r; best = b; } }
    long p = best < 0 ? centre - 512 : s0 + (long)best * B - B;
    return p < 0 ? 0 : p >= z->len ? z->len - 1 : p;
}

/* TRANSIENT: the onset envelope's peaks that stand above their surroundings by more than Sensitivity's threshold, at least
 * 60 ms apart; more than n: the strongest n. Sensitivity 0 lets through only the biggest hits, 1 nearly every one. */
static int chop_transients(const inst_t *in, float sens, long *out, int n) {
    if (!in->onset || in->non < 8) return 0;
    const float *o = in->onset;
    int cap = 4096, nc = 0, gap = (int)(0.06f * SR / ON_HOP + 0.5f);
    cand_t *c = malloc(sizeof(cand_t) * cap);
    if (!c) return 0;
    for (int f = 0; f < in->non && nc < cap; f++) {
        int top = 1;
        for (int j = f - 3; j <= f + 3 && top; j++) if (j != f && j >= 0 && j < in->non && (o[j] > o[f] || (o[j] == o[f] && j < f))) top = 0;
        if (!top) continue;
        float m = 0; int k = 0;   /* the surroundings: the mean over a quarter second either side */
        for (int j = f - 10; j <= f + 10; j++) if (j >= 0 && j < in->non && j != f) { m += o[j]; k++; }
        float d = o[f] - (k ? m / k : 0);
        if (d > 0.01f) c[nc++] = (cand_t){ f, d };
    }
    qsort(c, (size_t)nc, sizeof(cand_t), cand_cmp);
    /* the threshold, a share of the strongest hit's lift: 80% at Sensitivity 0 (the biggest hits), 2% at 1 (nearly all) */
    float thr = nc ? c[0].score * (0.78f * (1 - sens) * (1 - sens) + 0.02f) : 0;
    while (nc > 0 && c[nc - 1].score < thr) nc--;
    int np = 0;
    for (int i = 0; i < nc && np < n; i++) {   /* strongest first, none within the gap of a stronger one */
        int clash = 0;
        for (int j = 0; j < np && !clash; j++) clash = labs(out[j] - c[i].pos) < gap;
        if (!clash) out[np++] = c[i].pos;
    }
    free(c);
    for (int i = 0; i < np; i++) out[i] = refine_hit(&in->z, out[i] * ON_HOP + ON_HOP);
    qsort(out, (size_t)np, sizeof(long), long_cmp);
    /* a chop at least 70 ms long, and on something audible: the 40 ms after it reach 3% of the sound's loudest (a hit found
     * in a quiet lead-in or a tail would be a pad that seems to play nothing) */
    int k = 0;
    for (int i = 0; i < np; i++) {
        float pk = 0;
        if (in->nwave[0])
            for (long w = out[i] / WHOP, w1 = (out[i] + (long)(0.04f * SR)) / WHOP; w <= w1 && w < in->nwave[0]; w++)
                pk = fmaxf(pk, in->wave[0][0][w] + in->wave[0][1][w] + in->wave[0][2][w]);
        if (in->nwave[0] && pk < 0.03f * in->wtop) continue;
        if (k && out[i] - out[k - 1] < (long)(0.07f * SR)) continue;
        out[k++] = out[i];
    }
    return k;
}

static const float CHOP_BEATS[5] = { 0.5f, 1, 2, 4, 8 };
/* the chops Chop Mode puts on a sound (MANUAL: TRANSIENT, until a run drops chops), into cue[] (-1 none); its message (1 no
 * tempo, 5 few hits) or 0. bpm: the tempo to chop BPM by (a new sound's own, before SOUND BPM is set for it). Writes nothing
 * else, so the worker runs it for a new sound (render must not: a long sound takes longer than a block) */
static int chop_into(const chrono_t *e, const inst_t *in, float bpm, long *cue) {
    for (int i = 0; i < NPADS; i++) cue[i] = -1;
    long len = in->z.len, out[NPADS];
    int n = 16 * (1 + opt(e, P_CUE_COUNT)), np = 0, mode = opt(e, P_CHOP_MODE), msg = 0;
    if (len < 4096) { cue[0] = 0; return 0; }
    if (mode == 1) for (np = 0; np < n; np++) out[np] = len * np / n;
    else if (mode == 2) {   /* on the beat grid, from its first line in the sound */
        double step = CHOP_BEATS[opt(e, P_CHOP_DIV)] * 60.0 * SR / (bpm > 0 ? bpm : 120.0f);
        double p = bpm > 0 ? fmod(in->beat0, step) : 0;
        for (; np < n && p < len - 1024; p += step) out[np++] = (long)p;
        if (bpm <= 0) msg = 1;
    } else {
        np = chop_transients(in, e->val[P_SENSITIVITY], out, n);
        if (np < 4) msg = 5;
    }
    if (np == 0) out[np++] = 0;
    for (int i = 0; i < np; i++) cue[i] = out[i];
    return msg;
}
static void chop_now(chrono_t *e, const inst_t *in) {
    int msg = chop_into(e, in, snd_bpm(e, in), e->cue);
    snprintf(e->cue_name, sizeof e->cue_name, "%s", in->name);
    if (msg) { e->chop_msg = msg; e->chop_msg_frames = (long)((msg == 1 ? 4 : 5) * SR); }
}

/* ---- sound list ---- */
static int has_wav_ext(const char *n) { size_t l = strlen(n); return l > 4 && !strcasecmp(n + l - 4, ".wav"); }
static int entry_cmp(const void *a, const void *b) { return strcasecmp(((const entry_t *)a)->path, ((const entry_t *)b)->path); }

static int scan_dir(const char *dir, entry_t *out, int n, int max) {
    DIR *d = opendir(dir);
    if (!d) return n;
    int first = n;
    struct dirent *de;
    while ((de = readdir(d)) && n < max) {
        if (de->d_name[0] == '.' || !has_wav_ext(de->d_name)) continue;
        entry_t *en = &out[n];
        snprintf(en->path, sizeof en->path, "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(en->path, &st) || S_ISDIR(st.st_mode)) continue;
        char base[60];
        snprintf(base, sizeof base, "%s", de->d_name);
        char *dot = strrchr(base, '.'); if (dot) *dot = 0;
        snprintf(en->name, sizeof en->name, "User - %s", base);
        en->builtin = -1;
        n++;
    }
    closedir(d);
    qsort(out + first, (size_t)(n - first), sizeof(entry_t), entry_cmp);
    return n;
}

static int build_list(const char *user_dir, entry_t *out, int max, int builtins) {   /* builtins: the tests' synthesized sounds */
    int n = 0;
    for (int i = 0; i < NBUILTIN && builtins; i++) {
        snprintf(out[n].name, sizeof out[n].name, "Built-in - %s", BUILTIN[i]);
        out[n].path[0] = 0; out[n].builtin = i; n++;
    }
    n = scan_dir(user_dir, out, n, max);
    char p[600], line[512];
    snprintf(p, sizeof p, "%s/paths.txt", user_dir);   /* more folders, one per line */
    FILE *f = fopen(p, "r");
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '/') n = scan_dir(line, out, n, max);
    }
    if (f) fclose(f);
    return n;
}

static const entry_t *selected(const chrono_t *e) {
    if (!e->nlist) return NULL;
    int i = (int)lrintf(e->val[P_SOUND]);
    return &e->list[i < 0 ? 0 : i % e->nlist];
}
static int find_entry(const chrono_t *e, const char *name) {
    for (int i = 0; i < e->nlist; i++) if (!strcmp(e->list[i].name, name)) return i;
    return -1;
}
static void request_load(chrono_t *e) {   /* with e->mu held */
    const entry_t *en = selected(e);
    e->req = en ? (int)(en - e->list) : -1;
    if (en) snprintf(e->sname, sizeof e->sname, "%s", en->name);
    e->browsed = 0; e->load_fail = 0;
    e->req_gen++;
    pthread_cond_signal(&e->cv);
}
/* a WAV picked in the browser (or a browsed sound a state names), by its path; with e->mu held */
static void request_path(chrono_t *e, const char *path) {
    const char *b = strrchr(path, '/');
    snprintf(e->req_name, sizeof e->req_name, "%s", b ? b + 1 : path);
    char *dot = strrchr(e->req_name, '.'); if (dot) *dot = 0;
    snprintf(e->req_path, sizeof e->req_path, "%s", path);
    snprintf(e->cur_path, sizeof e->cur_path, "%s", path);
    snprintf(e->sname, sizeof e->sname, "%s", e->req_name);
    e->req = -2; e->browsed = 1; e->load_fail = 0;
    /* the sound arrows now step through the folder (set_value): the list index parks mid-range, so a step either way is a
     * change the host sends (one at an end would be clamped away) */
    if (e->val[P_SOUND] != 80) { e->val[P_SOUND] = 80; mark_changed(e, P_SOUND); }
    e->req_gen++;
    pthread_cond_signal(&e->cv);
}

/* ---- the drive browser ---- */
static int br_cmp(const void *a, const void *b) {
    const struct br_ent *x = a, *y = b;
    return x->dir != y->dir ? y->dir - x->dir : strcasecmp(x->name, y->name);
}
/* a drive's name on screen: MPC's own words for its mounts */
static const char *drive_label(const char *mount) {
    if (!strcmp(mount, "az01-internal-sd")) return "INTERNAL";
    return mount;
}
/* the entries of folder dir ("" = the drives under /media, less the system's own, after the sound list's own folder),
 * folders first, then WAVs; the browser thread */
static int list_folder(const char *dir, struct br_ent **out, int with_list) {
    int n = 0, cap = 256;
    struct br_ent *v = malloc(sizeof *v * cap);
    if (!v) return 0;
    DIR *d = opendir(dir[0] ? dir : "/media");
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (!dir[0] && (!strcmp(de->d_name, "acvs-content") || !strcmp(de->d_name, "az01-internal"))) continue;
        char p[700];
        snprintf(p, sizeof p, "%s/%s", dir[0] ? dir : "/media", de->d_name);
        struct stat st;
        if (stat(p, &st)) continue;
        int isdir = S_ISDIR(st.st_mode);
        if (!isdir && (!dir[0] || !has_wav_ext(de->d_name))) continue;
        if (n + 1 >= cap) { struct br_ent *g = realloc(v, sizeof *v * cap * 2); if (!g) break; v = g; cap *= 2; }
        snprintf(v[n].name, sizeof v[n].name, "%s", de->d_name);
        v[n].dir = isdir; v[n].li = -1;
        n++;
    }
    if (d) closedir(d);
    qsort(v, (size_t)n, sizeof *v, br_cmp);
    if (!dir[0] && with_list) {   /* first among the drives: the sound list's own folder (li -2), when it has any */
        memmove(v + 1, v, sizeof *v * n);
        snprintf(v[0].name, sizeof v[0].name, "USER SOUNDS");
        v[0].dir = 1; v[0].li = -2; n++;
    }
    *out = v;
    return n;
}
/* the sound list as a browser folder (its entries load by index); with e->mu held */
static int list_sounds(const chrono_t *e, struct br_ent **out) {
    struct br_ent *v = malloc(sizeof *v * (e->nlist + 1));
    if (!v) return 0;
    for (int i = 0; i < e->nlist; i++) { snprintf(v[i].name, sizeof v[i].name, "%s", e->list[i].name); v[i].dir = 0; v[i].li = i; }
    *out = v;
    return e->nlist;
}
static void free_zone(zone_t *z) { if (z) { free(z->data); free(z); } }
/* the browser's highlighted row kept on screen, as MPC's own browser does: the list moves only as far as it must (the wheel past
 * the last row scrolls one row); a row far off (BACK's) comes up mid-list */
static void br_show(chrono_t *e, int idx) {
    int max = e->br_n > NROWS ? e->br_n - NROWS : 0, top = e->br_top;
    if (idx < top - 1 || idx > top + NROWS) top = idx - NROWS / 2;
    else if (idx < top) top = idx;
    else if (idx >= top + NROWS) top = idx - NROWS + 1;
    e->br_top = top < 0 ? 0 : top > max ? max : top;
    e->br_pos = (float)e->br_top; e->br_vel = 0;
}

/* the browser's own thread: folder listings, previews, the sound arrows' steps through a folder (the worker can be busy
 * separating a sound's drums for a good while) and freeing the previews render let go of */
static void *browser(void *arg) {
    chrono_t *e = arg;
    pthread_mutex_lock(&e->mu);
    while (!e->quit) {
        if (e->aud_retired) { zone_t *z = e->aud_retired; e->aud_retired = NULL; pthread_mutex_unlock(&e->mu); free_zone(z); pthread_mutex_lock(&e->mu); continue; }
        if (e->br_req) {   /* the open folder */
            e->br_req = 0; e->br_scanning = 1;
            char dir[512]; snprintf(dir, sizeof dir, "%s", e->br_dir);
            struct br_ent *v = NULL;
            int n;
            if (!strcmp(dir, BUILTIN_DIR)) n = list_sounds(e, &v);
            else { int wl = e->nlist > 0; pthread_mutex_unlock(&e->mu); n = list_folder(dir, &v, wl); pthread_mutex_lock(&e->mu); }
            if (!strcmp(dir, e->br_dir)) {
                free(e->br); e->br = v; e->br_n = n; e->br_sel = -1; e->br_acc = 0; e->br_vel = 0; e->br_g_on = 0; e->br_coast_blk = e->blk - 1000;
                if (e->br_top >= n) e->br_top = 0;
                if (e->br_back_top > 0) {   /* BACK: the page it was on */
                    int max = n > NROWS ? n - NROWS : 0;
                    e->br_top = e->br_back_top > max ? max : e->br_back_top;
                }
                e->br_back_top = -1;
                for (int i = 0; e->br_back[0] && i < n; i++)   /* BACK: the folder it came out of highlighted */
                    if (!strcmp(v[i].name, e->br_back) || (v[i].li == -2 && !strcmp(e->br_back, BUILTIN_DIR))) { e->br_sel = i; br_show(e, i); break; }
                e->br_back[0] = 0;
            } else free(v);
            e->br_scanning = 0;
            continue;
        }
        if (e->aud_req) {   /* a WAV's preview: its first PREVIEW_SEC, handed to render (aud_pending) */
            e->aud_req = 0; e->br_busy = 1;
            char p[512]; snprintf(p, sizeof p, "%s", e->aud_path);
            pthread_mutex_unlock(&e->mu);
            zone_t *z = calloc(1, sizeof *z);
            if (z && !load_wav(p, z, PREVIEW_SEC)) { free(z); z = NULL; }
            pthread_mutex_lock(&e->mu);
            if (z && !e->aud_req && !strcmp(p, e->aud_path)) { free_zone(e->aud_pending); e->aud_pending = z; }
            else free_zone(z);   /* another was asked for meanwhile */
            e->br_busy = 0;
            continue;
        }
        if (e->fold_step) {   /* the next (or previous) WAV of the browsed sound's folder, round its ends */
            int step = e->fold_step; e->fold_step = 0;
            char dir[512], name[128];
            snprintf(dir, sizeof dir, "%s", e->cur_path);
            char *sl = strrchr(dir, '/');
            if (!e->browsed || !sl) continue;
            snprintf(name, sizeof name, "%s", sl + 1); *sl = 0;
            e->br_busy = 1;
            pthread_mutex_unlock(&e->mu);
            struct br_ent *v = NULL;
            int n = list_folder(dir, &v, 0), at = -1, nw = 0;
            for (int i = 0; i < n; i++) if (!v[i].dir) { nw++; if (!strcmp(v[i].name, name)) at = i; }
            pthread_mutex_lock(&e->mu);
            e->br_busy = 0;
            if (nw && at >= 0) {
                int i = at;
                do i = (i + step + n) % n; while (v[i].dir);
                char p[700];
                snprintf(p, sizeof p, "%s/%s", dir, v[i].name);
                request_path(e, p);
                e->aud_on_load = 1;
                if (!strcmp(e->br_dir, dir) && i < e->br_n && !strcmp(e->br[i].name, v[i].name)) { e->br_sel = i; br_show(e, i); }
                mark_changed(e, P_SOUND);
            }
            free(v);
            continue;
        }
        {
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec - e->st_sec >= 2) {
                char p[600], line[900];
                unsigned b = e->blk;
                snprintf(line, sizeof line, "t=%ld blocks=%u (+%u in %ld s) sets=%u dir=%s rows=%d top=%d sel=%d req=%d scanning=%d busy=%d "
                         "sound=%s loading=%d pending=%d preview=%d\n", (long)now.tv_sec, b, b - e->st_blk, (long)(now.tv_sec - e->st_sec),
                         e->ilog_n, e->br_dir[0] ? e->br_dir : "(drives)", e->br_n, e->br_top, e->br_sel, e->br_req, e->br_scanning, e->br_busy,
                         e->cur ? e->cur->name : "(none)", e->loading, e->pending != NULL, e->aud_z != NULL);
                e->st_sec = now.tv_sec; e->st_blk = b;
                snprintf(p, sizeof p, "%s/status.txt", e->user_dir);
                pthread_mutex_unlock(&e->mu);
                FILE *f = fopen(p, "w");
                if (f) { fputs(line, f); fclose(f); }
                pthread_mutex_lock(&e->mu);
                continue;
            }
        }
        unsigned ln = __atomic_load_n(&e->ilog_n, __ATOMIC_ACQUIRE);
        struct timespec lt; clock_gettime(CLOCK_MONOTONIC, &lt);
        if (ln != e->ilog_saved && lt.tv_sec - (long)e->ilog_blk >= 2) {   /* the input log, at most every 2 s, with what came in */
            struct ilog snap[ILOG_N];
            unsigned from = ln > ILOG_N ? ln - ILOG_N : 0;
            for (unsigned k = from; k < ln; k++) snap[k - from] = e->ilog[k % ILOG_N];
            e->ilog_saved = ln; e->ilog_blk = (unsigned)lt.tv_sec;
            pthread_mutex_unlock(&e->mu);
            char p[600];
            snprintf(p, sizeof p, "%s/input_log.txt", e->user_dir);
            FILE *f = fopen(p, "w");
            if (f) {
                fprintf(f, "# ChronoStretch: the last %u controls MPC set (block of 128 frames, key, value), to tune touch and wheel by\n", ln - from);
                for (unsigned k = 0; k < ln - from; k++) {
                    char cc[16]; int si = snap[k].i;
                    if (si <= -100) snprintf(cc, sizeof cc, "cc%d", -100 - si);
                    fprintf(f, "%u\t%s\t%.5f\n", snap[k].blk, si >= 0 ? KEYS[si] : si == -1 ? "transport" : cc, snap[k].x);
                }
                fclose(f);
            }
            pthread_mutex_lock(&e->mu);
            continue;
        }
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&e->bcv, &e->mu, &ts);
    }
    pthread_mutex_unlock(&e->mu);
    return NULL;
}

static void save_padmap(const chrono_t *e);

static void *worker(void *arg) {
    chrono_t *e = arg;
    pthread_mutex_lock(&e->mu);
    while (!e->quit) {
        if (e->retired) { inst_t *r = e->retired; e->retired = NULL; pthread_mutex_unlock(&e->mu); free_inst(r); pthread_mutex_lock(&e->mu); continue; }
        if (e->save_pads) {   /* 1: the learned map to User/pads.txt; 2: forget it */
            int how = e->save_pads; e->save_pads = 0;
            pthread_mutex_unlock(&e->mu);
            if (how == 2) { char p[600]; snprintf(p, sizeof p, "%s/pads.txt", e->user_dir); remove(p); } else save_padmap(e);
            pthread_mutex_lock(&e->mu);
            continue;
        }
        if (e->drums_job) {   /* the first sound's drums (create() loaded it itself) */
            inst_t *in = e->drums_job; e->drums_job = NULL;
            int gen = e->req_gen;
            pthread_mutex_unlock(&e->mu);
            separate_drums(in, &e->req_gen, gen);
            pthread_mutex_lock(&e->mu);
            continue;
        }
        if (e->rescan_req) {
            e->rescan_req = 0;
            entry_t *nl = calloc(MAX_SOUNDS, sizeof(entry_t));
            if (nl) {
                pthread_mutex_unlock(&e->mu);
                int n = build_list(e->user_dir, nl, MAX_SOUNDS, e->builtins);
                pthread_mutex_lock(&e->mu);
                free(e->list); e->list = nl; e->nlist = n;
                char keep[64]; snprintf(keep, sizeof keep, "%s", e->sname);
                int i = find_entry(e, keep);
                if (i >= 0 && i != (int)lrintf(e->val[P_SOUND])) { e->val[P_SOUND] = (float)i; mark_changed(e, P_SOUND); }
                if (i < 0 && !e->browsed) request_load(e);   /* a browsed sound is not on the list: it stays */
                if (!strcmp(e->br_dir, BUILTIN_DIR)) { e->br_req = 1; pthread_cond_signal(&e->bcv); }
            }
            continue;
        }
        if (e->req_gen != e->done_gen && !e->pending) {
            int gen = e->req_gen;   /* debounce: load only where a sweep stops */
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 120000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            if (gen > 1) pthread_cond_timedwait(&e->cv, &e->mu, &ts);
            if (e->quit) break;
            if (gen != e->req_gen) continue;
            int idx = e->req;
            entry_t en;
            memset(&en, 0, sizeof en);
            if (idx >= 0 && idx < e->nlist) en = e->list[idx];
            else if (idx == -2) { snprintf(en.name, sizeof en.name, "%s", e->req_name); snprintf(en.path, sizeof en.path, "%s", e->req_path); en.builtin = -1; }
            e->loading = 1;
            pthread_mutex_unlock(&e->mu);
            inst_t *in = idx >= 0 || idx == -2 ? load_entry(&en) : NULL;
            if (in) in->auto_msg = chop_into(e, in, in->bpm, in->auto_cue);   /* its chops here, unlocked: not in render, not holding mu */
            pthread_mutex_lock(&e->mu);
            e->loading = 0;
            e->done_gen = gen;
            if (!in && idx == -2 && gen == e->req_gen) { e->load_fail = 1; e->aud_on_load = 0; }
            if (in) {
                e->pending = in;
                /* then its drums, here, while render() already plays it (it frees nothing: only this thread retires) */
                pthread_mutex_unlock(&e->mu);
                separate_drums(in, &e->req_gen, gen);
                pthread_mutex_lock(&e->mu);
            }
            continue;
        }
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&e->cv, &e->mu, &ts);
    }
    pthread_mutex_unlock(&e->mu);
    return NULL;
}

/* ---- the phase vocoder (from 0.1) ---- */
static void voice_fft_reset(voice_t *v) {
    memset(v->ola, 0, sizeof v->ola);
    v->fr = 0; v->fn = 0; v->first = 1; v->c_ok = 0; v->c_env = 0; v->an_ready = 0;
}

static int hop_slots(int gi) { int s = GR_N[gi] / GR_OV[gi] / 128; return s < 1 ? 1 : s > 64 ? 64 : s; }   /* blocks a hop lasts */

/* the vocoder takes over from raw playback at its slot block (render_voice: so voices frozen together don't all start in
 * one block); with nothing else sounding it starts at once */
static void start_pv_now(chrono_t *e, voice_t *v, double at) {
    (void)e;
    v->pv_wait = 0;
    v->pv_on = 1; v->ppos = at; v->xf = 0;
    voice_fft_reset(v);
}

static void start_pv(chrono_t *e, voice_t *v, double at) {
    v->gi = QUALITY_GI[opt(e, P_QUALITY)];
    /* its slot: which of the hop's blocks it synthesizes in (render_voice), the one with the fewest voices of this size,
     * tried in the order 0, 2, 4 .. 1, 3 .. so the busy blocks alternate with quiet ones */
    int slots = hop_slots(v->gi), cnt[64] = {0};
    for (int i = 0; i < NV; i++)
        if (&e->v[i] != v && e->v[i].active && (e->v[i].pv_on || e->v[i].pv_wait) && e->v[i].gi == v->gi) cnt[e->v[i].stag % 64]++;
    int best = 0, bc = 1 << 30;
    for (int i = 0; i < slots; i++) { int s = (2 * i) % slots + (2 * i >= slots && slots > 1); if (cnt[s] < bc) { bc = cnt[s]; best = s; } }
    v->stag = best;
    {   /* at most MAX_PV voices in the vocoder, and 4 of the largest grain (its hop halves cost twice): the oldest lets go fast */
        int big = GR_N[v->gi] >= 4096, n = 0, nb = 0; voice_t *old = NULL, *oldb = NULL;
        for (int i = 0; i < NV; i++) {
            voice_t *u = &e->v[i];
            if (u == v || !u->active || u->done_fade || !(u->pv_on || u->pv_wait)) continue;
            n++;
            if (!old || u->age < old->age) old = u;
            if (GR_N[u->gi] >= 4096) { nb++; if (!oldb || u->age < oldb->age) oldb = u; }
        }
        if (big && nb >= 4 && oldb) { oldb->done_fade = 1; n--; if (old == oldb) old = NULL; }
        if (n >= MAX_PV) {
            if (!old) for (int i = 0; i < NV; i++) {
                voice_t *u = &e->v[i];
                if (u != v && u->active && !u->done_fade && (u->pv_on || u->pv_wait) && (!old || u->age < old->age)) old = u;
            }
            if (old) old->done_fade = 1;
        }
    }
    if (v->raw_on && !v->pv_on) v->pv_wait = 1;
    else start_pv_now(e, v, at);
}


typedef struct {
    float rho;          /* pitch ratio */
    float S;            /* full stretch */
    float tr, cexp, ramp, phi;   /* transient (frames), curve exponent, ramp fraction, formant ratio */
    int classic, end_action, loop;
    float scatter, blur_tau;
    int disp;           /* this voice feeds the screen */
} hopp_t;

/* the stretch profile's playback rate at source position p (frames per frame) */
static float profile_rate(const voice_t *v, const hopp_t *hp, double p) {
    double d = p - v->start;
    if (d < hp->tr) return 1.0f;
    double span = (v->r1 - v->start - hp->tr) * hp->ramp;
    float u = span > 1 ? (float)((d - hp->tr) / span) : 1.0f;
    u = clampf(u, 0, 1);
    return 1.0f / powf(hp->S, powf(u, hp->cexp));
}


/* ---- reading the sound: the layers (the sound, its drum layer) mixed by the voice's gains, forwards or reversed ---- */
static inline float vsample(const voice_t *v, long i, int c) {   /* i: the region's own (forward) frame */
    if (i < v->r0 || i >= v->r1) return 0;
    if (v->rev) i = v->r0 + v->r1 - 1 - i;
    const zone_t *z = v->z;
    long k = i * z->nch + (z->nch == 2 ? c : 0);
    float s = v->gm * z->data[k];
    if (v->dl) s += v->gd * v->dl[k];
    return s * (1.0f / 32768.0f);
}

static void read_frame(const voice_t *v, long a, int N, const float *win, float *L, float *R, float *mid, int loop) {
    (void)loop;
    const zone_t *z = v->z;
    int st = z->nch == 2;
    if (!v->rev && !v->dl && v->gm == 1.0f && a >= v->r0 && a + N <= v->r1) {   /* the usual case: the sound itself */
        const int16_t *s = z->data + a * z->nch;
        const float k = 1.0f / 32768.0f, k2 = 2.0f / 32768.0f;   /* one loop per case, so each vectorizes */
        if (mid && st) for (int n = 0; n < N; n++) mid[n] = (s[2 * n] + s[2 * n + 1]) * k * win[n];
        else if (mid) for (int n = 0; n < N; n++) mid[n] = s[n] * k2 * win[n];
        else if (st) for (int n = 0; n < N; n++) { L[n] = s[2 * n] * k * win[n]; R[n] = s[2 * n + 1] * k * win[n]; }
        else for (int n = 0; n < N; n++) L[n] = R[n] = s[n] * k * win[n];
        return;
    }
    for (int n = 0; n < N; n++) {
        float l = vsample(v, a + n, 0), r = st ? vsample(v, a + n, 1) : l, w = win[n];
        if (mid) mid[n] = (l + r) * w;
        else { L[n] = l * w; R[n] = r * w; }
    }
}

static void voice_env(chrono_t *e, voice_t *v, int N, int M) {
    const float *magM = v->magM;   /* squared magnitudes */
    float *lenv = v->lenv;
    int h = (int)ceilf(130.0f / (SR / N));
    if (h < 1) h = 1;
    float mmax = 1e-12f;
    for (int k = 0; k <= M; k++) if (magM[k] > mmax) mmax = magM[k];
    /* the max of each block of 2h bins (one pass), joined by straight lines between the blocks' centres */
    int B = 2 * h, nb = M / B + 1;
    float *bm = e->tmp;
    for (int b = 0; b < nb; b++) {
        float m = 0; int hi = (b + 1) * B > M + 1 ? M + 1 : (b + 1) * B;
        for (int k = b * B; k < hi; k++) if (magM[k] > m) m = magM[k];
        bm[b] = 0.5f * flog2(m + mmax * 1e-12f + 1e-24f);   /* the log of the max: one log a block */
    }
    float binv = 1.0f / B;
    for (int k = 0; k <= M; k++) {
        float x = (k + 0.5f) * binv - 0.5f;
        int b0 = x < 0 ? 0 : (int)x;
        if (x < 0) x = 0;
        lenv[k] = b0 >= nb - 1 ? bm[nb - 1] : bm[b0] + (bm[b0 + 1] - bm[b0]) * (x - b0);
    }
    float ac = expf(-1.0f / h), s = lenv[0];
    for (int k = 0; k <= M; k++) { s = ac * s + (1 - ac) * lenv[k]; lenv[k] = s; }
    for (int k = M; k >= 0; k--) { s = ac * s + (1 - ac) * lenv[k]; lenv[k] = s; }
    v->c_env = 1;
}

static int wants_formants(const hopp_t *hp) { return fabsf(hp->phi - 1) > 0.001f || (!hp->classic && fabsf(hp->rho - 1) > 0.0005f); }

/* A hop is two halves, so a voice's work can fall in two blocks (render_voice): the analysis of the frame at ppos (and its
 * envelope when wanted), into the voice's cache; then hop_synth. */
static void hop_analyze(chrono_t *e, voice_t *v, const hopp_t *hp) {
    const fft_t *t = &e->fft[size_index(GR_N[v->gi])];
    int N = t->N, M = t->M, ov = GR_OV[v->gi], Hs = N / ov;
    const float *wa = ov == 2 ? t->sine : t->hann;
    long a = (long)floor(v->ppos);
    int mono = v->z->nch == 1;
    float *magL = v->magL, *magR = mono ? v->magL : v->magR, *magM = v->magM;
    /* analysis. A frozen playhead (the same frame again) reuses the last one. Else L and R (or the mono channel) at a, and
     * each bin's true frequency from the mid's phase change since the last frame (or, after a jump, a frame one hop back) */
    if (!(v->c_ok && a == v->c_a)) {
        read_frame(v, a, N, wa, e->fa[0], e->fa[1], NULL, hp->loop);
        rfft(t, e->fa[0], e->XL);
        if (!mono) rfft(t, e->fa[1], e->XR);
        long ha = v->c_ok ? a - v->c_a : 0;
        int reuse = ha > 0 && ha <= N / 2;
        if (!reuse) { ha = Hs; read_frame(v, a - Hs, N, wa, NULL, NULL, e->fb, hp->loop); rfft(t, e->fb, e->XB); }
        float hinv = 1.0f / ha;
        for (int k = 0; k <= M; k++) {
            float lr = e->XL[2 * k], li = e->XL[2 * k + 1], mr = lr, mi = li, l2 = lr * lr + li * li;
            magL[k] = sqrtf(l2);
            if (!mono) {
                float rr = e->XR[2 * k], ri = e->XR[2 * k + 1];
                mr += rr; mi += ri;
                magR[k] = sqrtf(rr * rr + ri * ri);
                magM[k] = mr * mr + mi * mi;   /* squared: it only picks peaks and shapes the envelope */
            } else magM[k] = l2;
            float pa = fatan2(mi, mr), pb = reuse ? v->phA[k] : fatan2(e->XB[2 * k + 1], e->XB[2 * k]);
            float om = TWO_PI * k / N;
            v->phA[k] = pa;
            v->wtr[k] = om + wrap_pi(pa - pb - om * ha) * hinv;   /* the bin's true frequency, rad per frame */
        }
        v->c_ok = 1; v->c_a = a; v->c_env = 0;
    }
    /* the spectral envelope, when the formants need placing or the screen wants it */
    if ((wants_formants(hp) || hp->disp) && !v->c_env) voice_env(e, v, N, M);
    v->an_ready = 1;
}

/* the second half: the moved spectrum, phase locking, synthesis and overlap-add, then the playhead moves on */
static void hop(chrono_t *e, voice_t *v, const hopp_t *hp) {
    if (!v->an_ready) hop_analyze(e, v, hp);
    const fft_t *t = &e->fft[size_index(GR_N[v->gi])];
    int N = t->N, M = t->M, ov = GR_OV[v->gi], Hs = N / ov;
    const float *wa = ov == 2 ? t->sine : t->hann;
    float gain = (ov == 2 ? 1.0f : 1.0f / (0.375f * ov)) / N;   /* the overlap-add normalisation and the inverse FFT's 1/N */
    int mono = v->z->nch == 1;
    float *magL = v->magL, *magR = mono ? v->magL : v->magR, *magM = v->magM, *lenv = v->lenv;
    int fmt = wants_formants(hp);
    if ((fmt || hp->disp) && !v->c_env) voice_env(e, v, N, M);   /* a knob moved since the analysis */
    /* the formant target: an output bin j wants the envelope at j / q, the moved spectrum brings it from j / rho */
    float q = hp->classic ? hp->rho * hp->phi : hp->phi;
    float rinv = 1.0f / hp->rho, qinv = 1.0f / q;
    float omax = 0;
    for (int j = 0; j <= M; j++) {
        float s = j * rinv;
        int k0 = (int)s;
        if (k0 >= M) { e->oL[j] = e->oR[j] = e->oM[j] = 0; e->ow[j] = TWO_PI * j / N; e->oph[j] = 0; continue; }
        float fr = s - k0;
        float mL = magL[k0] + (magL[k0 + 1] - magL[k0]) * fr;
        float mR = magR[k0] + (magR[k0 + 1] - magR[k0]) * fr;
        float mM = magM[k0] + (magM[k0 + 1] - magM[k0]) * fr;
        if (fmt) {
            float sq = j * qinv; int q0 = (int)sq;
            float et = q0 >= M ? lenv[M] - 4 : lenv[q0] + (lenv[q0 + 1] - lenv[q0]) * (sq - q0);
            float es = lenv[k0] + (lenv[k0 + 1] - lenv[k0]) * fr;
            float c = fexp2(clampf(et - es, -7, 5));
            mL *= c; mR *= c; mM *= c * c;
        }
        int kn = fr < 0.5f ? k0 : k0 + 1;
        e->oL[j] = mL; e->oR[j] = mR; e->oM[j] = mM;
        e->ow[j] = v->wtr[kn] * hp->rho;
        e->oph[j] = v->phA[kn];
        if (mM > omax) omax = mM;
    }
    /* phase: identity locking around the peaks of the moved spectrum */
    if (v->first) {
        for (int j = 0; j <= M; j++) v->phs[j] = e->oph[j];
    } else {
        int np = 0;
        float floor_m = omax * 1e-8f;   /* -80 dB, on squared magnitudes */
        for (int j = 2; j <= M - 2; j++) {
            float m = e->oM[j];
            if (m > floor_m && m > e->oM[j - 1] && m >= e->oM[j + 1] && m > e->oM[j - 2] && m >= e->oM[j + 2]) e->peaks[np++] = j;
        }
        if (!np) for (int j = 0; j <= M; j++) v->phs[j] = wrap_pi(v->phs[j] + e->ow[j] * Hs);
        else {
            for (int i = 0; i < np; i++) { int p = e->peaks[i]; e->tmp[i] = wrap_pi(v->phs[p] + e->ow[p] * Hs); }
            int pi = 0;
            for (int j = 0; j <= M; j++) {
                while (pi + 1 < np && j > (e->peaks[pi] + e->peaks[pi + 1]) / 2) pi++;
                int p = e->peaks[pi];
                v->phs[j] = j == p ? e->tmp[pi] : wrap_pi(e->tmp[pi] + e->oph[j] - e->oph[p]);
            }
        }
    }
    if (hp->scatter > 0) for (int j = 1; j < M; j++) v->phs[j] = wrap_pi(v->phs[j] + hp->scatter * PI * frand(e));
    /* magnitudes, blurred in time */
    float b = hp->blur_tau > 0 ? expf(-Hs / (hp->blur_tau * SR)) : 0;
    if (v->first) b = 0;
    int nc = mono ? 1 : 2;
    for (int j = 0; j <= M; j++) {
        v->ms[0][j] = b * v->ms[0][j] + (1 - b) * e->oL[j];
        if (!mono) v->ms[1][j] = b * v->ms[1][j] + (1 - b) * e->oR[j];
    }
    if (mono) memcpy(v->ms[1], v->ms[0], sizeof(float) * (M + 1));
    /* synthesis, each channel (the phases' cos and sin once, into oph and ow, which are done with); mono: one, copied */
    for (int j = 0; j <= M; j++) fast_sincos(v->phs[j], &e->ow[j], &e->oph[j]);
    for (int c = 0; c < nc; c++) {
        for (int j = 0; j <= M; j++) { e->Y[2 * j] = v->ms[c][j] * e->oph[j]; e->Y[2 * j + 1] = v->ms[c][j] * e->ow[j]; }
        e->Y[1] = 0; e->Y[2 * M + 1] = 0;
        irfft_n(t, e->Y, e->fa[c]);
        float *o = v->ola[c];
        for (int n = 0; n < N; n++) o[n] += e->fa[c][n] * wa[n] * gain;
        /* the finished part into the ring, the rest moves up */
        for (int n = 0; n < Hs; n++) v->fifo[c][(v->fr + v->fn + n) & (FIFO - 1)] = o[n];
        memmove(o, o + Hs, sizeof(float) * (N - Hs));
        memset(o + N - Hs, 0, sizeof(float) * Hs);
    }
    if (mono) for (int n = 0; n < Hs; n++) { int k = (v->fr + v->fn + n) & (FIFO - 1); v->fifo[1][k] = v->fifo[0][k]; }
    v->fn += Hs;
    v->first = 0;
    /* advance at the pad's rate (the hit at 1x first, profile_rate) */
    float rate = profile_rate(v, hp, v->ppos + N / 2);
    v->ppos += Hs * rate;
    v->an_ready = 0;   /* the next frame is analyzed afresh (or found in the cache) */
}

/* ---- pads and voices ---- */
static inline void raw_read(const voice_t *v, double pos, float *l, float *r) {
    long i = (long)floor(pos); float fr = (float)(pos - i);
    float y[2][4];
    for (int k = 0; k < 4; k++) { y[0][k] = vsample(v, i - 1 + k, 0); y[1][k] = v->z->nch == 2 ? vsample(v, i - 1 + k, 1) : y[0][k]; }
    for (int c = 0; c < 2; c++) {
        float c1 = 0.5f * (y[c][2] - y[c][0]), c2 = y[c][0] - 2.5f * y[c][1] + 2 * y[c][2] - 0.5f * y[c][3];
        float c3 = 0.5f * (y[c][3] - y[c][0]) + 1.5f * (y[c][1] - y[c][2]);
        float o = ((c3 * fr + c2) * fr + c1) * fr + y[c][1];
        if (c) *r = o; else *l = o;
    }
}

static float speed_ratio(float x) { return powf(4.0f, x); }                      /* 25% .. 400% */
static float lp_hz(float x) { return 20000.0f * powf(150.0f / 20000.0f, -x); }  /* x -1..0: 150 Hz .. open */
static float hp_hz(float x) { return 20.0f * powf(6000.0f / 20.0f, x); }         /* x 0..1: 20 Hz .. 6 kHz */

/* the region a pad plays: from its cue to the next cue, a length in beats or bars, or the end */
static void pad_region(const chrono_t *e, const inst_t *in, int p, long *r0, long *r1) {
    long len = in->z.len, s = e->cue[p];
    *r0 = s;
    int L = e->pad[p].len;
    if (L == 0) {   /* NEXT CUE */
        long nx = len;
        for (int i = 0; i < NPADS; i++) if (e->cue[i] > s && e->cue[i] < nx) nx = e->cue[i];
        *r1 = nx;
    } else if (L == 8) *r1 = len;   /* TO END */
    else {
        static const float BEATS[8] = { 0, 0.25f, 0.5f, 1, 2, 4, 8, 16 };
        double beat = snd_bpm(e, in) > 0 ? 60.0 * SR / snd_bpm(e, in) : 0.5 * SR;
        *r1 = s + (long)(BEATS[L] * beat);
        if (*r1 > len) *r1 = len;
    }
    if (*r1 - *r0 < 64) *r1 = *r0 + 64 > len ? len : *r0 + 64;
}

/* a drum setting: the pad's own, or the global one */
static int layer_mode(int pad_mode, int global) { return pad_mode == 0 ? global : pad_mode - 1; }   /* 0 full, 1 off, 2 only */

/* how a pad plays: 1 ONE SHOT (its chop), 2 CONTINUE (on past the chop until a pad is hit), 3 GATE, 4 HOLD */
static int pad_mode(const chrono_t *e, int p) { int m = e->pad[p].mode; return m ? m : 1 + opt(e, P_PLAY_MODE); }

/* the sound's key: SOUND KEY when set, else the one found (-1 none) */
static int sound_key(const chrono_t *e, const inst_t *in) { int s = opt(e, P_SOUND_KEY); return s ? s - 1 : in ? in->key : -1; }

/* the semitones the sound moves to reach the chosen key, the nearest way (-5 .. +6) */
static int key_shift(const chrono_t *e, const inst_t *in) {
    int k = opt(e, P_KEY), sk = sound_key(e, in);
    if (!k || sk < 0) return 0;
    int d = ((k - 1) - sk % 12 + 12) % 12;
    return d > 6 ? d - 12 : d;
}

static float sync_rate(const chrono_t *e, const inst_t *in) {   /* the sound's playback rate for MPC's tempo, 1 without; HALF SPEED halves it */
    static const float MULT[3] = { 0.5f, 1, 2 };
    float h = opt(e, P_HALF) ? 0.5f : 1;
    if (!opt(e, P_SYNC) || !in || snd_bpm(e, in) <= 0 || e->bpm_host <= 0) return h;
    return h * e->bpm_host * MULT[opt(e, P_TEMPO_MULT)] / snd_bpm(e, in);
}

static void voice_release(voice_t *v) { v->released = 1; }

/* ---- the view: the part of the sound the waveform shows (Zoom: the sound / 2^zoom, any fraction; Scroll: where it sits) ---- */
static void view_of(const chrono_t *e, const inst_t *in, long *v0, long *vl) {
    long len = in->z.len, l = (long)(len * exp2(-(double)clampf(e->val[P_ZOOM], 0, 16)));
    if (l < MIN_VIEW) l = MIN_VIEW;
    if (l > len) l = len;
    *vl = l > 0 ? l : 1;
    *v0 = (long)(e->val[P_SCROLL] * (double)(len - l));
}
/* the selected pad's cue as a place in the view (p_pos, which the drag area moves), told to the host */
static void show_pos(chrono_t *e) {
    const inst_t *in = e->cur;
    int sp = (int)lrintf(e->val[P_SEL_PAD]);
    long v0, vl;
    if (!in || e->cue[sp] < 0) e->val[P_P_POS] = 0;
    else { view_of(e, in, &v0, &vl); e->val[P_P_POS] = clampf((float)((double)(e->cue[sp] - v0) / vl), 0, 1); }
    mark_changed(e, P_P_POS);
}
/* the view centred on frame f (as near as the sound's ends allow) */
static void centre_view(chrono_t *e, long f) {
    const inst_t *in = e->cur;
    if (!in) return;
    long v0, vl;
    view_of(e, in, &v0, &vl);
    float s = in->z.len > vl ? clampf((float)((double)(f - vl / 2) / (in->z.len - vl)), 0, 1) : 0;
    if (s != e->val[P_SCROLL]) { e->val[P_SCROLL] = s; mark_changed(e, P_SCROLL); }
    show_pos(e);
}
/* frame f in view, else the view moves to it */
static void keep_in_view(chrono_t *e, long f) {
    const inst_t *in = e->cur;
    long v0, vl;
    if (!in || f < 0) return;
    view_of(e, in, &v0, &vl);
    if (f < v0 || f >= v0 + vl) centre_view(e, f);
}

static void select_pad(chrono_t *e, int p);

/* the built-in pad map: MPC pads in a C-major scale pad mode (pad 1 = 72, as a Live II sends by default: 72, 74, 76, 77 ..),
 * plus plain chromatic pads from C1 (36..). The first 16 notes of each are pads 1-16 of the bank on screen (BANK), always;
 * the 16 notes after them (an MPC that goes on up for its bank B) are bank B's pads (D's with BANK on C or D) */
static void default_padmap(chrono_t *e) {
    static const int DEG[7] = { 0, 2, 4, 5, 7, 9, 11 };
    memset(e->padmap, -1, sizeof e->padmap);
    for (int n = 36; n <= 67; n++) e->padmap[n] = (signed char)(n < 52 ? 32 + n - 36 : n - 36);
    for (int i = 0; i < 32; i++) { int n = 72 + 12 * (i / 7) + DEG[i % 7]; if (n < 128) e->padmap[n] = (signed char)(i < 16 ? 32 + i : i); }
    e->learned = 0; e->abs_banks = 0;
}

/* a learned map from <User>/pads.txt ("note slot" lines, slot 0..15 = that pad of the bank) */
static void load_padmap(chrono_t *e) {
    char p[600], line[64];
    snprintf(p, sizeof p, "%s/pads.txt", e->user_dir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    int n, s, any = 0;
    signed char m[128];
    memset(m, -1, sizeof m);
    while (fgets(line, sizeof line, f)) if (sscanf(line, "%d %d", &n, &s) == 2 && n >= 0 && n < 128 && s >= 0 && s < 16) { m[n] = (signed char)(32 + s); any = 1; }
    fclose(f);
    if (any) { memcpy(e->padmap, m, sizeof m); e->learned = 1; }
}

static void save_padmap(const chrono_t *e) {   /* the worker */
    char p[600];
    snprintf(p, sizeof p, "%s/pads.txt", e->user_dir);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "# ChronoStretch's pads: the note MPC sends for each pad of a bank (LEARN PADS writes this)\n");
    for (int n = 0; n < 128; n++) if (e->padmap[n] >= 32) fprintf(f, "%d %d\n", n, e->padmap[n] - 32);
    fclose(f);
}

static int pad_for_note(const chrono_t *e, int note) {
    if (opt(e, P_KEYMAP) == 1) return note >= 36 && note < 36 + NPADS ? note - 36 : -1;   /* MIDI KEYS ALL 64: a keyboard over all banks */
    int m = note >= 0 && note < 128 ? e->padmap[note] : -1;
    if (m < 0) return -1;
    int bank = opt(e, P_BANK), pair = bank >= 2 ? 32 : 0;   /* C and D: A and B 32 chops on */
    if (m < 32) return m + pair;
    return m - 32 + 16 * bank;   /* always the bank BANK shows (1.0.11: a remembered "MPC sends its own banks" sent bank B's
                                  * and D's pads back to A and C for good, once one stray note had set it) */
}

static double heard_pos(const voice_t *v);
static void fx_start(chrono_t *e, voice_t *v, const pad_t *pd, int fresh);
static void play_pad(chrono_t *e, const inst_t *in, int note, int vel, int p, int kb, int recrun, unsigned hit, int lead);
static void note_on(chrono_t *e, int note, int vel) {
    const inst_t *in = e->cur;
    if (!in) return;
    int kb = opt(e, P_KEYBOARD), p;
    if (e->learn > 0) {   /* LEARN PADS: this note is pad e->learn of the bank */
        if (e->learn == 1) memset(e->padmap, -1, sizeof e->padmap);
        if (e->padmap[note] >= 0) return;   /* the same pad twice: wait for the next one */
        e->padmap[note] = (signed char)(32 + e->learn - 1);
        e->learn_frames = (long)(LEARN_WAIT_S * SR);
        if (++e->learn > 16) { e->learn = 0; e->save_pads = 1; e->learned = 1; }
    }
    e->aud_stop = 1;   /* a pad hit ends a preview */
    if (kb) p = (int)lrintf(e->val[P_SEL_PAD]);
    else if ((p = pad_for_note(e, note)) < 0) { e->unmapped_note = note; e->unmapped_frames = (long)(4 * SR); e->hit_note = note; e->hit_pad = -1; if (e->chop_msg >= 6) e->chop_msg_frames = 0; return; }
    if (!kb) {
        e->hit_note = note; e->hit_pad = p;
    }
    if (p < 0 || p >= NPADS) return;
    int recrun = 0;
    if (e->rec == 2) {   /* MANUAL, running: any pad drops the next chop where the sound is heard now */
        const voice_t *rv = &e->v[e->rec_v];
        int max = 16 * (1 + opt(e, P_CUE_COUNT));
        if (rv->active && rv->recv && e->rec_n < max) {
            long f = (long)heard_pos(rv);
            if (f > e->cue[e->rec_n - 1] + 256 && f < in->z.len - 256) {
                e->cue[e->rec_n] = f;
                select_pad(e, e->rec_n);
                keep_in_view(e, f);
                if (++e->rec_n >= max) { e->rec = 0; voice_release(&e->v[e->rec_v]); e->chop_msg = 2; e->chop_msg_frames = (long)(4 * SR); }
            }
        }
        return;
    }
    if (e->rec == 1) {   /* MANUAL, armed: this hit plays the sound from the start, chop 1 at the start */
        for (int k = 0; k < NPADS; k++) e->cue[k] = -1;
        snprintf(e->cue_name, sizeof e->cue_name, "%s", in->name);
        e->cue[0] = 0; e->rec_n = 1; p = 0; recrun = 1; kb = 0;
    }
    if (e->cue[p] < 0) { e->unmapped_note = -1 - p; e->unmapped_frames = (long)(3 * SR); if (e->chop_msg >= 6) e->chop_msg_frames = 0; return; }   /* a pad with no chop: say so */
    unsigned hit = ++e->hit_id;
    play_pad(e, in, note, vel, p, kb, recrun, hit, 1);
    if (!kb && !recrun && e->pad[p].layer)   /* LAYER GROUP: the other pads of its group play with it */
        for (int q = 0; q < NPADS; q++)
            if (q != p && e->pad[q].layer == e->pad[p].layer && e->cue[q] >= 0) play_pad(e, in, note, vel, q, 0, 0, hit, 0);
}

/* a pad's chop starting (note_on). lead: the pad hit, not one of its layer group */
static void play_pad(chrono_t *e, const inst_t *in, int note, int vel, int p, int kb, int recrun, unsigned hit, int lead) {
    const pad_t *pd = recrun ? &PAD_DEFAULT : &e->pad[p];
    int mode = recrun ? 2 : pad_mode(e, p);
    if (!kb && mode == 4) {   /* HOLD: a second hit lets go */
        if (e->hold_on[p]) {
            e->hold_on[p] = 0;
            for (int i = 0; i < NV; i++) if (e->v[i].active && e->v[i].pad == p) voice_release(&e->v[i]);
            return;
        }
        e->hold_on[p] = 1;
    }
    for (int i = 0; i < NV; i++) {   /* the same pad again (not in Keyboard), or its choke group: they let go fast */
        voice_t *u = &e->v[i];
        if (!u->active || u->done_fade || u->hit == hit) continue;   /* this hit's own layer pads stay */
        if ((!kb && u->pad == p) || (kb && u->note == note) || (pd->choke && u->pad != p && e->pad[u->pad].choke == pd->choke)
            || (!kb && pad_mode(e, u->pad) == 2)) u->done_fade = 1;   /* any hit ends a CONTINUE */
    }
    voice_t *v = NULL;
    for (int i = 0; i < NV && !v; i++) if (!e->v[i].active) v = &e->v[i];
    if (!v) {
        unsigned best = 0xffffffffu;
        for (int i = 0; i < NV; i++) if ((e->v[i].released || e->v[i].done_fade) && e->v[i].age < best) { best = e->v[i].age; v = &e->v[i]; }
        if (!v) for (int i = 0; i < NV; i++) if (e->v[i].age < best) { best = e->v[i].age; v = &e->v[i]; }
    }
    memset(v, 0, offsetof(voice_t, fifo));
    v->active = 1; v->held = 1; v->note = note; v->vel = vel; v->pad = p; v->age = ++e->age; v->hit = hit;
    v->z = &in->z; v->rev = pd->reverse;
    pad_region(e, in, p, &v->r0, &v->r1);
    if (mode == 2) {   /* CONTINUE: on past the chop (forwards to the end, or reversed back to the start) */
        if (v->rev) v->r0 = 0; else v->r1 = in->z.len;
    }
    if (recrun) { v->recv = 1; e->rec = 2; e->rec_v = (int)(v - e->v); centre_view(e, 0); }
    /* the drum layer, once separated */
    int dm = layer_mode(pd->drums, opt(e, P_DRUMS));
    v->gm = 1; v->gd = 0; v->dl = NULL;
    if (dm && __atomic_load_n(&in->drums_ready, __ATOMIC_ACQUIRE)) { v->dl = in->drums; if (dm == 1) v->gd = -1; else { v->gm = 0; v->gd = 1; } }
    v->start = v->r0; v->raw_on = 1; v->rpos = v->r0; v->frz_k = 1;
    v->tpos = v->seg = v->r0; v->xr = 1;
    v->vel_g = 0.35f + 0.65f * powf(vel / 127.0f, 1.2f);
    v->gain = db2lin(pd->level);
    v->env = 0;   /* always a ramp in, at least 1 ms: a chop starts without a click wherever it is */
    v->att_step = 1.0f / (fmaxf(0.001f, attack_s(pd->attack)) * SR);
    v->rel_s = release_s(pd->release);
    fx_start(e, v, pd, 1);
    v->flt_mode = pd->filter < -0.02f ? 1 : pd->filter > 0.02f ? 2 : 0;
    v->flt_g = v->flt_mode ? tanf(PI * clampf(v->flt_mode == 1 ? lp_hz(pd->filter) : hp_hz(pd->filter), 10, 19000) / SR) : 0;
    if (e->val[P_TRANSIENT] < 0.001f) { v->raw_on = 0; start_pv(e, v, v->start); v->xf = 1; }   /* no transient: the vocoder at once */
    if (!lead) return;
    e->last_note = note; e->last_vel = vel; e->last_pad = p;
    e->trig_frames = (long)(0.15f * SR);
    if (!kb && opt(e, P_FOLLOW) && p != (int)lrintf(e->val[P_SEL_PAD])) select_pad(e, p);
}

static void note_off(chrono_t *e, int note) {
    int kb = opt(e, P_KEYBOARD);
    for (int i = 0; i < NV; i++) {
        voice_t *v = &e->v[i];
        if (!v->active || v->note != note || !v->held || v->recv) continue;
        v->held = 0;
        if (kb || pad_mode(e, v->pad) == 3) voice_release(v);   /* GATE (and Keyboard) let go; ONE SHOT and CONTINUE play on, HOLD waits */
    }
}

static void ilog_add(chrono_t *e, int i, float x);
static void midi(void *inst, const uint8_t *msg, int len) {
    chrono_t *e = inst;
    if (len < 2) return;
    int st = msg[0] & 0xf0, d1 = msg[1] & 0x7f, d2 = len > 2 ? msg[2] & 0x7f : 0;
    if (st == 0x90 && d2) note_on(e, d1, d2);
    else if (st == 0x80 || st == 0x90) note_off(e, d1);
    else if (st == 0xE0) e->bend = ((d2 << 7 | d1) - 8192) / 8192.0f;
    else if (st == 0xB0 && (d1 == 120 || d1 == 123)) {   /* all sound off, all notes off (MPC's STOP may send them): the log learns */
        ilog_add(e, -100 - d1, (float)d2);
        for (int i = 0; i < NV; i++) if (e->v[i].active) { if (d1 == 120) e->v[i].active = 0; else voice_release(&e->v[i]); }
        memset(e->hold_on, 0, sizeof e->hold_on);
        e->aud_stop = 1;   /* the preview too */
    }
}

/* ---- the pads' FX: presets made of two biquads (RBJ) and a little more, after the pad's filter ---- */
enum { BQ_LP, BQ_HP, BQ_BP, BQ_NOTCH, BQ_LSHELF, BQ_HSHELF, BQ_THRU };
static void bq_set(bq_t *q, int type, float hz, float Q, float gain_db) {
    float w = TWO_PI * clampf(hz, 10, 0.45f * SR) / SR, c = cosf(w), s = sinf(w), al = s / (2 * Q), A = powf(10, gain_db / 40);
    float b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0, sa = 2 * sqrtf(A) * al;
    switch (type) {
    case BQ_LP: b0 = (1 - c) / 2; b1 = 1 - c; b2 = b0; a0 = 1 + al; a1 = -2 * c; a2 = 1 - al; break;
    case BQ_HP: b0 = (1 + c) / 2; b1 = -(1 + c); b2 = b0; a0 = 1 + al; a1 = -2 * c; a2 = 1 - al; break;
    case BQ_BP: b0 = al; b1 = 0; b2 = -al; a0 = 1 + al; a1 = -2 * c; a2 = 1 - al; break;
    case BQ_NOTCH: b0 = 1; b1 = -2 * c; b2 = 1; a0 = 1 + al; a1 = -2 * c; a2 = 1 - al; break;
    case BQ_LSHELF:
        b0 = A * ((A + 1) - (A - 1) * c + sa); b1 = 2 * A * ((A - 1) - (A + 1) * c); b2 = A * ((A + 1) - (A - 1) * c - sa);
        a0 = (A + 1) + (A - 1) * c + sa; a1 = -2 * ((A - 1) + (A + 1) * c); a2 = (A + 1) + (A - 1) * c - sa; break;
    case BQ_HSHELF:
        b0 = A * ((A + 1) + (A - 1) * c + sa); b1 = -2 * A * ((A - 1) + (A + 1) * c); b2 = A * ((A + 1) + (A - 1) * c - sa);
        a0 = (A + 1) - (A - 1) * c + sa; a1 = 2 * ((A - 1) - (A + 1) * c); a2 = (A + 1) - (A - 1) * c - sa; break;
    default: break;
    }
    q->b0 = b0 / a0; q->b1 = b1 / a0; q->b2 = b2 / a0; q->a1 = a1 / a0; q->a2 = a2 / a0;
}
static inline float bq_run(const bq_t *q, float *z, float x) {   /* transposed direct form II */
    float y = q->b0 * x + z[0];
    z[0] = q->b1 * x - q->a1 * y + z[1];
    z[1] = q->b2 * x - q->a2 * y;
    return y;
}
static const float FX_BEATS[7] = { 0.25f, 0.5f, 1, 2, 4, 8, 16 };

/* SWEEP (a resonant low-pass up and down, a triangle a cycle) and RISER (a resonant high-pass up over its time from the hit):
 * their filter where they are now, every 32 frames */
static void fx_tick(voice_t *v, fxu_t *u) {
    float t = v->fx_t / v->fx_beat, a = v->fx_amt;
    if (u->fx == FX_SWEEP) {
        float ph = t - ffloor(t), tri = 1 - fabsf(2 * ph - 1), lo = 12000.0f * powf(80.0f / 12000.0f, 0.3f + 0.7f * a);
        bq_set(&u->fq[0], BQ_LP, lo * powf(12000.0f / lo, tri), 3.5f * v->fx_q, 0);
    } else {
        float w = fminf(1, t), top = 1500 + 8500 * a;
        bq_set(&u->fq[0], BQ_HP, 30 * powf(top / 30, w), 2.2f * v->fx_q, 0);
    }
}

/* a voice's FX from its pad: the preset's filters (RESONANCE scales their Q), SWEEP's cycle and RISER's rise (both inside the
 * chop as it plays at this tempo, so a short chop hears all of it), WARP-SPEED's cycle, and DRIVE, TONE and WIDTH around them.
 * fresh: the voice starts (else the pad's FX changed while it plays, and the sweeps and WARP-SPEED keep their place) */
static void fx_start(chrono_t *e, voice_t *v, const pad_t *pd, int fresh) {
    float a = pd->fx_amt, q = 0.5f + 2 * pd->fx_res, tempo = e->bpm_host > 0 ? e->bpm_host : snd_bpm(e, e->cur) > 0 ? snd_bpm(e, e->cur) : 120;
    v->fx = v->fx_set = pd->fx; v->fx_amt = a;
    for (int k = 0; k < FX_LAYERS - 1; k++) v->lay_set[k] = pd->fx_lay[k];
    v->fx_mix = pd->fx_mix; v->fx_rate = pd->fx_rate; v->fx_res = pd->fx_res; v->fx_q = q;
    v->fx_drive = pd->fx_drive; v->fx_tone = pd->fx_tone; v->fx_width = pd->fx_width;
    int chain[FX_LAYERS], n = 0;   /* the preset, then its layers (none while the pad's FX are off) */
    if (pd->fx) {
        chain[n++] = pd->fx;
        for (int k = 0; k < FX_LAYERS - 1; k++) if (pd->fx_lay[k] > 0 && pd->fx_lay[k] != pd->fx) chain[n++] = pd->fx_lay[k];
    }
    v->nu = n;
    v->fx_on = n || pd->fx_drive > 0.001f || fabsf(pd->fx_tone) > 0.001f || fabsf(pd->fx_width - 0.5f) > 0.001f;
    v->tone_k = 1 - expf(-TWO_PI * 800 / SR);
    float beat = FX_BEATS[pd->fx_rate < 0 || pd->fx_rate > 6 ? 4 : pd->fx_rate] * 60.0f * SR / tempo;
    int swept = 0;
    for (int k = 0; k < n; k++) swept |= chain[k] == FX_SWEEP || chain[k] == FX_RISER;
    if (swept) {
        float out = (v->r1 - v->r0) / fmaxf(0.01f, sync_rate(e, e->cur) * speed_ratio(pd->speed));
        if (beat > out) beat = out;
    }
    v->fx_beat = beat < 64 ? 64 : beat;
    if (fresh) v->fx_t = 0;
    int warped = 0;
    for (int k = 0; k < n; k++) {
        fxu_t *u = &v->u[k];
        if (u->fx != chain[k]) { memset(u, 0, sizeof *u); }   /* a preset new in this place starts from silence */
        u->fx = chain[k];
        bq_set(&u->fq[0], BQ_THRU, 1000, 0.7f, 0); bq_set(&u->fq[1], BQ_THRU, 1000, 0.7f, 0);
        switch (u->fx) {
        case FX_LOFI: bq_set(&u->fq[0], BQ_LP, 2500 + 9000 * (1 - a), 0.7f * q, 0); break;
        case FX_PHONE: bq_set(&u->fq[0], BQ_HP, 300 + 300 * a, 0.8f * q, 0); bq_set(&u->fq[1], BQ_LP, 3400 - 1000 * a, 0.8f * q, 0); break;
        case FX_LP: bq_set(&u->fq[0], BQ_LP, 20000.0f * powf(150.0f / 20000.0f, a), 1.0f * q, 0); break;
        case FX_HP: bq_set(&u->fq[0], BQ_HP, 20.0f * powf(300.0f, a), 0.9f * q, 0); break;
        case FX_BP: bq_set(&u->fq[0], BQ_BP, 200.0f * powf(25.0f, a), 2.0f * q, 0); break;
        case FX_VINTAGE: bq_set(&u->fq[0], BQ_HP, 70, 0.7f, 0); bq_set(&u->fq[1], BQ_LP, 7000 - 3500 * a, 0.7f * q, 0); break;
        case FX_LSHELF: bq_set(&u->fq[0], BQ_LSHELF, 180, 0.707f, 15 * a); break;   /* a boost, up to +15 dB */
        case FX_HSHELF: bq_set(&u->fq[0], BQ_HSHELF, 4000, 0.707f, 15 * a); break;
        case FX_NOTCH: {   /* two wide notches an octave apart: a hollow, phased gap */
            float f = 150.0f * powf(30.0f, a);
            bq_set(&u->fq[0], BQ_NOTCH, f, 0.6f * q, 0); bq_set(&u->fq[1], BQ_NOTCH, 2 * f, 0.6f * q, 0);
            break;
        }
        case FX_SWEEP: case FX_RISER: fx_tick(v, u); break;
        case FX_ROBOT: u->comb_n = (int)(SR / (75 + 225 * a)); if (u->comb_n > COMB_MAX) u->comb_n = COMB_MAX; if (u->comb_w >= u->comb_n) u->comb_w = 0; break;
        case FX_RADIO: bq_set(&u->fq[0], BQ_HP, 600 + 300 * a, 1.2f * q, 0); bq_set(&u->fq[1], BQ_LP, 2600 - 900 * a, 1.2f * q, 0); break;
        case FX_VCURVE: bq_set(&u->fq[0], BQ_LSHELF, 120, 0.707f, 15 * a); bq_set(&u->fq[1], BQ_HSHELF, 6000, 0.707f, 15 * a); break;
        case FX_TAPE:   /* a head bump low down, the top rolling off as the tape wears (Amount) */
            bq_set(&u->fq[0], BQ_LSHELF, 90, 0.707f, 2 + 3 * a); bq_set(&u->fq[1], BQ_LP, 16000 - 10000 * a, 0.7f * q, 0);
            u->comb_n = COMB_MAX; if (u->comb_w >= COMB_MAX) u->comb_w = 0;
            break;
        case FX_WARP:
            if (!e->warp || warped++) { u->fx = FX_OFF; break; }   /* one WARP-SPEED buffer a voice */
            u->wb = e->warp + (size_t)(v - e->v) * 2 * WARP_MAX;
            u->w_n = v->fx_beat > WARP_MAX ? WARP_MAX : (long)v->fx_beat;
            if (fresh || u->w_t >= u->w_n) u->w_t = 0;
            break;
        }
    }
}

/* the pad's FX changed while its voice plays: take them up at once */
static void fx_live(chrono_t *e, voice_t *v, const pad_t *pd) {
    if (pd->fx != v->fx_set || memcmp(pd->fx_lay, v->lay_set, sizeof v->lay_set) || pd->fx_amt != v->fx_amt || pd->fx_mix != v->fx_mix || pd->fx_rate != v->fx_rate || pd->fx_res != v->fx_res
        || pd->fx_drive != v->fx_drive || pd->fx_tone != v->fx_tone || pd->fx_width != v->fx_width) fx_start(e, v, pd, 0);
}

/* one preset of the chain on a frame (x: both channels, in and out) */
static inline void fx_unit(voice_t *v, fxu_t *u, float *x) {
    float a = v->fx_amt;
    if (u->fx == FX_LOFI) {   /* held at a lower rate (22 .. 6 kHz) and fewer bits (12 .. 6), then dulled */
        u->hold_ph += (22050 - 16000 * a) / SR;
        if (u->hold_ph >= 1) {
            u->hold_ph -= 1;
            float lv = powf(2, 12 - 6 * a);
            for (int c = 0; c < 2; c++) u->hold[c] = lrintf(x[c] * lv) / lv;
        }
    }
    if (u->fx == FX_WARP) {   /* WARP-SPEED: each cycle (from the hit) plays its own first half at half speed, an octave down, as a
                               * tape slowed to half and caught up again at the next: half time, in time. Amount: the seams' fade */
        long t = u->w_t, k = t >> 1;
        float fade = (0.002f + 0.03f * a) * SR, g = fminf(1, fminf((t + 1) / fade, (u->w_n - t) / fade)), fr = (t & 1) * 0.5f;
        for (int c = 0; c < 2; c++) {
            float *b = u->wb + (size_t)c * WARP_MAX;
            b[t] = x[c];
            x[c] = (b[k] + (b[k + 1 <= t ? k + 1 : k] - b[k]) * fr) * g;
        }
        if (++u->w_t >= u->w_n) u->w_t = 0;
    } else if (u->fx == FX_TAPE) {   /* TAPE: wow (0.6 Hz) and flutter (6.5 Hz) as a delay that sways, then saturation, the
                                      * head bump and the rolled-off top. Amount: how worn (deeper wow, more drive, less top) */
        float wow = sin_half(PI * (fabsf(2 * u->wow_ph - 1) - 0.5f)), flut = sin_half(PI * (fabsf(2 * u->flut_ph - 1) - 0.5f));
        float dl = 60 + (6 + 40 * a) * wow + (0.3f + 1.2f * a) * flut;   /* about 1 ms: the chop stays on time */
        if ((u->wow_ph += 0.6f / SR) >= 1) u->wow_ph -= 1;
        if ((u->flut_ph += 6.5f / SR) >= 1) u->flut_ph -= 1;
        float rp = u->comb_w - dl; if (rp < 0) rp += COMB_MAX;
        int k0 = (int)rp, k1 = k0 + 1 >= COMB_MAX ? 0 : k0 + 1; float fr = rp - k0;
        for (int c = 0; c < 2; c++) {
            float *b = u->comb[c];
            b[u->comb_w] = x[c];
            float y = b[k0] + (b[k1] - b[k0]) * fr, g = 1 + 2.5f * a;
            y = fast_tanh(g * y) / fast_tanh(g);
            x[c] = bq_run(&u->fq[1], u->fz[1][c], bq_run(&u->fq[0], u->fz[0][c], y));
        }
        if (++u->comb_w >= COMB_MAX) u->comb_w = 0;
    } else if (u->fx) {
        if ((u->fx == FX_SWEEP || u->fx == FX_RISER) && !(v->fx_t & 31)) fx_tick(v, u);
        for (int c = 0; c < 2; c++) {
            float d = x[c], y = d;
            switch (v->fx) {
            case FX_LOFI: y = bq_run(&u->fq[0], u->fz[0][c], u->hold[c]); break;
            case FX_PHONE: y = fast_tanh(4.0f * bq_run(&u->fq[1], u->fz[1][c], bq_run(&u->fq[0], u->fz[0][c], d))) * 0.6f; break;
            case FX_VINTAGE: y = bq_run(&u->fq[1], u->fz[1][c], bq_run(&u->fq[0], u->fz[0][c], fast_tanh(d * (1 + 3 * a)) / (1 + a))); break;
            case FX_RADIO: y = fast_tanh(2.2f * bq_run(&u->fq[1], u->fz[1][c], bq_run(&u->fq[0], u->fz[0][c], d))) * 0.7f; break;
            case FX_VCURVE: y = bq_run(&u->fq[1], u->fz[1][c], bq_run(&u->fq[0], u->fz[0][c], d)) * 0.6f; break;
            case FX_ROBOT: {   /* a short feedback comb on a pitch: metallic, voice-like */
                float *b = u->comb[c];
                y = d + 0.82f * b[u->comb_w];
                b[u->comb_w] = y;
                y *= 0.35f;
                break;
            }
            case FX_BP: y = 3.0f * bq_run(&u->fq[0], u->fz[0][c], d); break;   /* a narrow band: made up to about the level it came in at */
            case FX_NOTCH: y = bq_run(&u->fq[1], u->fz[1][c], bq_run(&u->fq[0], u->fz[0][c], d)); break;
            default: y = bq_run(&u->fq[0], u->fz[0][c], d); break;
            }
            x[c] = y;
        }
        if (u->fx == FX_ROBOT && ++u->comb_w >= u->comb_n) u->comb_w = 0;
    }
}

/* one frame through the voice's FX: DRIVE, its chain of presets in order, TONE, WIDTH, then MIX against the dry frame */
static inline void fx_frame(voice_t *v, float *l, float *r) {
    float dry[2] = { *l, *r }, x[2] = { *l, *r };
    if (v->fx_drive > 0.001f) {   /* a soft clip, about level for the same loudness */
        float g = 1 + 8 * v->fx_drive, n = 1.0f / sqrtf(g);
        for (int c = 0; c < 2; c++) x[c] = fast_tanh(g * x[c]) * n;
    }
    for (int k = 0; k < v->nu; k++) fx_unit(v, &v->u[k], x);
    if (v->nu) v->fx_t++;
    if (fabsf(v->fx_tone) > 0.001f)   /* a tilt about 800 Hz: the lows down and the highs up (brighter), or the other way */
        for (int c = 0; c < 2; c++) {
            v->tone_s[c] += v->tone_k * (x[c] - v->tone_s[c]);
            float lo = v->tone_s[c], hi = x[c] - lo, t = v->fx_tone;
            x[c] = lo * (1 - 0.6f * t) + hi * (1 + 1.2f * t);
        }
    if (fabsf(v->fx_width - 0.5f) > 0.001f) {   /* mid and side: 0 mono, 0.5 as is, 1 the side doubled */
        float m = 0.5f * (x[0] + x[1]), sd = (x[0] - x[1]) * v->fx_width;
        x[0] = m + sd; x[1] = m - sd;
    }
    *l = dry[0] + v->fx_mix * (x[0] - dry[0]);
    *r = dry[1] + v->fx_mix * (x[1] - dry[1]);
}

static void render_voice(chrono_t *e, voice_t *v, float *L, float *R, int frames) {
    const inst_t *in = e->cur;
    const pad_t *pd = v->recv ? &PAD_DEFAULT : &e->pad[v->pad];
    fx_live(e, v, pd);
    float semis = key_shift(e, in) + e->val[P_PITCH] + pd->key + 2.0f * e->bend
                + (opt(e, P_KEYBOARD) ? v->note - 60 : 0);
    float rate = sync_rate(e, in) * speed_ratio(pd->speed);
    hopp_t hp;
    memset(&hp, 0, sizeof hp);
    hp.rho = powf(2.0f, semis / 12.0f);
    hp.S = 1.0f / rate;
    hp.tr = transient_s(e->val[P_TRANSIENT]) * SR;
    hp.cexp = 1; hp.ramp = 0; hp.phi = 1; hp.classic = 1;
    int N = GR_N[v->gi];
    /* a hit inside a long stretched play (CONTINUE, a long chop): once the beat (tpos) reaches it, it plays clean again through
     * the resampler, and the vocoder takes over again from where the beat is then, so every hit stays crisp and in time */
    if (v->pv_on && !v->raw_on && !v->rev && fabsf(rate - hp.rho) > 0.002f) {
        while (v->nh < in->nhits && in->hits[v->nh] < v->tpos - 256) v->nh++;
        long h = v->nh < in->nhits ? in->hits[v->nh] : -1;
        if (h >= 0 && v->tpos >= h && h < v->r1 - N && h - v->seg >= hp.tr + N && e->rtg_blk != e->blk) {   /* one voice a block */
            v->raw_on = 1; v->rpos = h; v->seg = h; v->xr = 0; v->nh++; e->rtg_blk = e->blk;
        }
    }
    /* the raw path hands over to the vocoder past the transient, once time and pitch part ways: the first time from where the
     * resampler got to (no seam), after a hit from where the beat is (in time) */
    double at = v->seg > v->start ? v->tpos : v->rpos;
    if (v->raw_on && !v->pv_on && !v->pv_wait && fabsf(rate - hp.rho) > 0.002f && v->rpos - v->seg >= hp.tr) start_pv(e, v, at);
    float rel_k = expf(-6.9f / ((v->done_fade ? 0.012f : v->rel_s) * SR));
    float xf_step = 1.0f / N;
    if (v->pv_wait && (!v->raw_on || (int)(e->blk % hop_slots(v->gi)) == v->stag)) start_pv_now(e, v, at);
    float pv[2][128];
    if (v->pv_on) {
        /* synthesize in its slot block, enough to last until its slot comes round again (a new voice: until then), and
         * analyze the next frame in the block after, so a hop's two halves and the voices' hops spread over the blocks */
        int slots = hop_slots(v->gi), until = (v->stag - (int)(e->blk % slots) + slots) % slots;
        int need = frames * (until > 0 ? until : slots), synth = v->fn < need;
        while (v->fn < need) hop(e, v, &hp);
        if (!synth && !v->an_ready && until == slots - 1) hop_analyze(e, v, &hp);
        for (int i = 0; i < frames; i++) { int k = (v->fr + i) & (FIFO - 1); pv[0][i] = v->fifo[0][k]; pv[1][i] = v->fifo[1][k]; }
        v->fr = (v->fr + frames) & (FIFO - 1); v->fn -= frames;
    }
    int ended = 0;
    float g0 = v->gain * v->vel_g;
    for (int i = 0; i < frames; i++) {
        float l = 0, r = 0;
        if (v->raw_on) {
            raw_read(v, v->rpos, &l, &r);
            v->rpos += hp.rho;
            if (v->rpos >= v->r1 - 1) {
                if (v->pv_wait) v->raw_on = 0;   /* the vocoder starts next block */
                else if (!v->pv_on) { v->raw_on = 0; ended = 1; }
            }
        }
        if (v->pv_on) {
            if (v->raw_on && v->xr < 1) {   /* a hit again: the resampler in over 64 frames, the vocoder out, then off */
                float k = v->xr; l = pv[0][i] * (1 - k) + l * k; r = pv[1][i] * (1 - k) + r * k;
                if ((v->xr += 1.0f / 64) >= 1) { v->xr = 1; v->pv_on = 0; }
            }
            else if (v->raw_on) { float k = v->xf; l = l * (1 - k) + pv[0][i] * k; r = r * (1 - k) + pv[1][i] * k; v->xf += xf_step; if (v->xf >= 1) { v->xf = 1; v->raw_on = 0; } }
            else { l = pv[0][i]; r = pv[1][i]; }
        }
        v->tpos += rate;
        if (v->flt_mode) {   /* the pad's filter: a 12 dB state-variable low- or high-pass */
            float g = v->flt_g, k = 1.414f, x[2] = { l, r };
            for (int c = 0; c < 2; c++) {
                float *s = v->flt_s[c];
                float hpv = (x[c] - (k + g) * s[0] - s[1]) / (1 + g * (g + k));
                float bp = g * hpv + s[0]; s[0] = g * hpv + bp;
                float lp = g * bp + s[1]; s[1] = g * bp + lp;
                x[c] = v->flt_mode == 1 ? lp : hpv;
            }
            l = x[0]; r = x[1];
        }
        if (v->fx_on) fx_frame(v, &l, &r);   /* the pad's FX */
        if (v->released || v->done_fade) v->env *= rel_k;
        else if (v->env < 1) { v->env += v->att_step; if (v->env > 1) v->env = 1; }
        float g = v->env * g0;
        L[i] += l * g; R[i] += r * g;
    }
    if (v->pv_on && v->ppos >= v->r1 + N && !v->raw_on) ended = 1;
    if (ended) v->active = 0;
    if ((v->released || v->done_fade) && v->env < 0.0002f) v->active = 0;
    if (!v->active && pad_mode(e, v->pad) == 4) e->hold_on[v->pad] = 0;
}

/* ---- engine ---- */
static void *create(const char *data_dir) {
    (void)data_dir;
    chrono_t *e = NULL;   /* 16-byte aligned for the FFT buffers in it */
    if (posix_memalign((void **)&e, 64, sizeof(chrono_t))) return NULL;
    memset(e, 0, sizeof(chrono_t));
    for (int s = 0; s < NSIZES; s++) if (!fft_init(&e->fft[s], 256 << s)) goto fail;
    for (int i = 0; i < NP; i++) e->val[i] = PDEF[i];
    for (int i = 0; i < NPADS; i++) { e->pad[i] = PAD_DEFAULT; e->cue[i] = -1; }
    e->rng = 0x9E3779B9u ^ (unsigned)(uintptr_t)e; e->last_touched = -1; e->last_pad = -1;
    e->drag_slot = -1; e->drag_pad = -1; e->br_req = 1; e->br_sel = -1;   /* the browser opens on the drives */
    e->hit_note = -1; e->hit_pad = -1;
    for (int i = 0; i < NP; i++) if (PKIND[i] == K_DRAG) e->sp_x[i] = 0.5f;   /* where MPC starts a drag strip (strip_step) */
    e->br_wrow = -1; e->undo_kind = -1; e->br_back_top = -1; e->br_coast_blk = (unsigned)-1000;
    e->lim_g = 1; e->bpm_host = 0;
    char dir[512];
    if (!mpc_plugin_dir(dir, sizeof dir)) snprintf(dir, sizeof dir, ".");
    snprintf(e->user_dir, sizeof e->user_dir, "%s/User", dir);
    e->list = calloc(MAX_SOUNDS, sizeof(entry_t));
    if (!e->list) goto fail;
    e->warp = calloc((size_t)NV * 2 * WARP_MAX, sizeof(float));   /* none: WARP-SPEED plays dry */
    e->builtins = getenv("CHRONO_BUILTINS") != NULL;   /* the plugin opens with no sound, ready for one; the tests use built-ins */
    e->nlist = build_list(e->user_dir, e->list, MAX_SOUNDS, e->builtins);
    default_padmap(e); load_padmap(e);
    pthread_mutex_init(&e->mu, NULL);
    pthread_cond_init(&e->cv, NULL);
    pthread_cond_init(&e->bcv, NULL);
    pthread_mutex_lock(&e->mu);
    if (e->builtins) {   /* the tests: the first built-in now, so it plays at once; the worker separates its drums */
        request_load(e);
        e->cur = load_entry(selected(e));
        if (e->cur) { e->done_gen = e->req_gen; e->drums_job = e->cur; chop_now(e, e->cur); }
    }
    pthread_mutex_unlock(&e->mu);
    if (pthread_create(&e->th, NULL, worker, e)) e->th = 0;
    if (pthread_create(&e->bth, NULL, browser, e)) e->bth = 0;
    return e;
fail:
    for (int s = 0; s < NSIZES; s++) fft_free(&e->fft[s]);
    free(e->list); free(e->warp);
    free(e);
    return NULL;
}

static void destroy(void *inst) {
    chrono_t *e = inst;
    pthread_mutex_lock(&e->mu);
    e->quit = 1; e->req_gen++;   /* stops a drum separation */
    pthread_cond_signal(&e->cv);
    pthread_cond_signal(&e->bcv);
    pthread_mutex_unlock(&e->mu);
    if (e->th) pthread_join(e->th, NULL);
    if (e->bth) pthread_join(e->bth, NULL);
    free_inst(e->cur); if (e->pending != e->cur) free_inst(e->pending); free_inst(e->retired);
    free_zone(e->aud_own); free_zone(e->aud_pending); free_zone(e->aud_retired);
    for (int s = 0; s < NSIZES; s++) fft_free(&e->fft[s]);
    free(e->list); free(e->br); free(e->warp);
    pthread_mutex_destroy(&e->mu);
    pthread_cond_destroy(&e->cv);
    pthread_cond_destroy(&e->bcv);
    free(e);
}

static int key_index(const char *k) {   /* from just past the last match: a state lists its keys in order */
    static int last = -1;   /* only a hint, so threads may share it */
    int from = __atomic_load_n(&last, __ATOMIC_RELAXED);
    for (int n = 0; n < NP; n++) { int i = (from + 1 + n) % NP; if (!strcmp(KEYS[i], k)) { __atomic_store_n(&last, i, __ATOMIC_RELAXED); return i; } }
    return -1;
}
static int saved(int i) { return (PKIND[i] == K_CONT || PKIND[i] == K_INT || PKIND[i] == K_OPT) && strncmp(KEYS[i], "p_", 2) && i != P_SOUND; }

/* the selected pad's settings into the p_* controls, which the host is told about */
static void select_pad(chrono_t *e, int p) {
    e->val[P_SEL_PAD] = (float)p;
    if (opt(e, P_BANK) != p / 16) { e->val[P_BANK] = (float)(p / 16); mark_changed(e, P_BANK); }
    const pad_t *pd = &e->pad[p];
    keep_in_view(e, e->cue[p]);   /* a chop picked off screen: the view goes to it */
    show_pos(e);
    e->val[P_P_NUDGE] = 0; e->val[P_P_LEN] = (float)pd->len; e->val[P_P_LEVEL] = pd->level; e->val[P_P_FILTER] = pd->filter;
    e->val[P_P_ATTACK] = pd->attack; e->val[P_P_RELEASE] = pd->release; e->val[P_P_REVERSE] = (float)pd->reverse;
    e->val[P_P_KEY] = (float)pd->key; e->val[P_P_SPEED] = pd->speed; e->val[P_P_MODE] = (float)pd->mode;
    e->val[P_P_CHOKE] = (float)pd->choke; e->val[P_P_DRUMS] = (float)pd->drums;
    e->val[P_P_LAYER] = (float)pd->layer; mark_changed(e, P_P_LAYER);
    e->val[P_P_VOCALS] = (float)pd->vocals;
    e->val[P_PAD_TAP] = (float)GRID_SLOT(p % 16);
    mark_changed(e, P_SEL_PAD); mark_changed(e, P_PAD_TAP);
    for (int i = P_P_POS; i <= P_P_VOCALS; i++) mark_changed(e, i);
    e->val[P_P_FX_PICK] = (float)((pd->fx ? pd->fx : pd->fx_keep > 0 ? pd->fx_keep : 1) - 1); e->val[P_P_FX_ON] = pd->fx ? 1.0f : 0.0f;
    mark_changed(e, P_P_FX_PICK); mark_changed(e, P_P_FX_ON);
    e->val[P_P_FX] = (float)pd->fx; e->val[P_P_FX_AMT] = pd->fx_amt; e->val[P_P_FX_RATE] = (float)pd->fx_rate; e->val[P_P_FX_MIX] = pd->fx_mix;
    for (int i = P_P_FX; i <= P_P_FX_MIX; i++) mark_changed(e, i);
    e->val[P_P_FX_DRIVE] = pd->fx_drive; e->val[P_P_FX_TONE] = pd->fx_tone; e->val[P_P_FX_WIDTH] = pd->fx_width; e->val[P_P_FX_RES] = pd->fx_res;
    for (int i = P_P_FX_DRIVE; i <= P_P_FX_RES; i++) mark_changed(e, i);
}

/* pads stay in the order of their chops: after a cue moves past another, the cues (with their pads' settings) are put back
 * in time order, cueless pads last, and the selection follows the moved cue */
static void sort_pads(chrono_t *e, int moved) {
    int idx[NPADS], n = 0;
    for (int i = 0; i < NPADS; i++) if (e->cue[i] >= 0) idx[n++] = i;
    for (int i = 1; i < n; i++) { int k = idx[i], j = i; while (j > 0 && e->cue[idx[j - 1]] > e->cue[k]) { idx[j] = idx[j - 1]; j--; } idx[j] = k; }
    for (int i = 0; i < NPADS; i++) if (e->cue[i] < 0) idx[n++] = i;
    int same = 1;
    for (int i = 0; i < NPADS; i++) same &= idx[i] == i;
    if (same) return;
    long cue[NPADS]; pad_t pad[NPADS]; int hold[NPADS], where = moved;
    for (int i = 0; i < NPADS; i++) { cue[i] = e->cue[idx[i]]; pad[i] = e->pad[idx[i]]; hold[i] = e->hold_on[idx[i]]; if (idx[i] == moved) where = i; }
    memcpy(e->cue, cue, sizeof cue); memcpy(e->pad, pad, sizeof pad); memcpy(e->hold_on, hold, sizeof hold);
    for (int v = 0; v < NV; v++) if (e->v[v].active) for (int i = 0; i < NPADS; i++) if (idx[i] == e->v[v].pad) { e->v[v].pad = i; break; }
    select_pad(e, where);
}

/* MANUAL chopping ends (CHOP again, or the sound ran out): the run lets go, the chops dropped so far stay */
static void stop_rec(chrono_t *e) {
    if (e->rec == 2) {
        voice_t *v = &e->v[e->rec_v];
        if (v->active && v->recv) voice_release(v);
        e->chop_msg = 2; e->chop_msg_frames = (long)(4 * SR);
    }
    e->rec = 0;
}

/* the bank's tabs under the view: the pad whose tab each slot shows (-1 none; two in one slot: the lower pad) and the
 * column in the slot its chop is on (the tab's point, right over its line) */
static void tab_slots(const chrono_t *e, const inst_t *in, int *pad, int *off) {
    long v0, vl;
    view_of(e, in, &v0, &vl);
    int bank = opt(e, P_BANK);
    for (int k = 0; k < NMARK; k++) { pad[k] = -1; off[k] = 0; }
    for (int s = 15; s >= 0; s--) {
        long q = e->cue[bank * 16 + s];
        if (q < v0 || q >= v0 + vl) continue;
        int c = (int)((double)(q - v0) * NCOL / vl);
        pad[c / COLS_PER_MARK] = bank * 16 + s; off[c / COLS_PER_MARK] = c % COLS_PER_MARK;
    }
}

/* Nudge's reach (seconds at full turn). A Q-Link event moves the knob 1/64 of it (1/128 of its range, from the middle it
 * springs back to), so a step moves the chop one waveform column of the view, and at most 4 ms */
static float nudge_s(const chrono_t *e) {
    long v0, vl;
    if (!e->cur) return 0.256f;
    view_of(e, e->cur, &v0, &vl);
    return 64 * fmaxf(1.0f / SR, fminf(0.010f, (float)vl / NCOL / SR));
}

/* LEARN PADS called off (a second tap, or no pad hit for a while): the pads as they were before it */
static void learn_cancel(chrono_t *e) {
    e->learn = 0;
    memcpy(e->padmap, e->padmap_before, sizeof e->padmap);
    e->chop_msg = 4; e->chop_msg_frames = (long)(3 * SR);
}

/* a move of a control that springs back to its middle (the drag strips, NUDGE): MPC sends the value it last read back plus a
 * step, but sends a burst before it reads the middle back, each adding on to its own last value (0.51, 0.52, 0.53 for three
 * wheel clicks; 0.547, 0.595, 0.642 for a drag, NOTES 2026-10-27). So a value further out the same way, soon after the last,
 * is a step from that last one, not from the middle */
static float spring_step(chrono_t *e, int i, float x, float mid) {
    float last = e->sp_x[i], d = x - mid;
    int more = e->blk - e->sp_blk[i] <= 16 && (last - mid) * d > 0 && fabsf(d) > fabsf(last - mid) + 1e-6f;
    e->sp_x[i] = x; e->sp_blk[i] = e->blk;
    return more ? x - last : d;
}

/* a drag strip's move (the tab strips, the browser's rows). While a finger is on a strip MPC keeps its own value for it and
 * sends that plus each move; it does not take a value the engine reports until the page is drawn again (input log
 * 2026-10-28: 0.521 0.565 .. 0.934 1.000 1.000 1.000 within one drag). So the engine follows MPC's value (val = what it sent,
 * which is also what it reads back) and a move is the step from the last value sent. A jump of more than 0.3 is MPC having
 * read the value afresh. Pinned at an end, each further event is one more step that way, the size of the last real one */
static float strip_step(chrono_t *e, int i, float x) {
    if (e->sp_rst[i]) {   /* handed back the middle: MPC may have taken it, or still be on its own value */
        if (fabsf(x - 0.5f) < fabsf(x - e->sp_x[i])) e->sp_x[i] = 0.5f;
        e->sp_rst[i] = 0;
    }
    float last = e->sp_x[i], d;
    int pinned = x == last && (x >= 0.999f || x <= 0.001f);
    if (pinned) d = (x > 0.5f ? 1 : -1) * (e->sp_mag[i] > 0 ? e->sp_mag[i] : 0.04f);
    else {
        d = x - last;
        if (fabsf(d) > 1e-6f) e->sp_mag[i] = fminf(fabsf(d), 0.1f);
    }
    e->sp_x[i] = x; e->sp_blk[i] = e->blk;
    e->val[i] = x;
    return d;
}

/* a place for a chop: SNAP ZERO CROSS moves it to the nearest sign change of the sound within 5 ms (a clean loop start) */
static long snap_cue(const chrono_t *e, const inst_t *in, long f) {
    f = f < 0 ? 0 : f > in->z.len - 64 ? in->z.len - 64 : f;
    if (!opt(e, P_SNAP)) return f;
    long reach = (long)(0.005f * SR);
    for (long d = 0; d <= reach; d++)
        for (int s = -1; s <= 1; s += 2) {
            long k = f + s * d;
            if (k >= 1 && k < in->z.len && (zmid(&in->z, k - 1) <= 0) != (zmid(&in->z, k) <= 0)) return k;
        }
    return f;
}

/* a drag on tab slot k: its first move (or the first after a pause) grabs the chop whose tab is there (or reaches into it from
 * the slot before); each move after shifts it by the move times Drag Speed's share of the view, kept to the fraction of a frame
 * (drag_pos), so the slowest drag at the deepest zoom still moves it a frame at a time. MPC sends a drag as the value it last
 * read plus a step, so the strip is put back to the middle after every move and never runs out of travel. */
/* a double tap on tab slot k: the chop whose tab is there (or reaches into it from the slot before) is selected, as a drag
 * on it would grab it, so CUE, NUDGE and the Q-Links move it */
static void tab_tap(chrono_t *e, int k) {
    const inst_t *in = e->cur;
    if (!in) return;
    int pad[NMARK], off[NMARK];
    tab_slots(e, in, pad, off);
    int p = pad[k] >= 0 ? pad[k] : k > 0 && pad[k - 1] >= 0 ? pad[k - 1] : -1;
    if (p >= 0) select_pad(e, p);
}

static void tab_drag(chrono_t *e, int k, float x) {
    static const float GAIN[5] = { 0.01f, 0.05f, 0.22f, 0.5f, 1.0f };
    const inst_t *in = e->cur;
    float d = strip_step(e, P_TD00 + k, x);
    if (!in || fabsf(d) < 1e-6f) return;
    if (k != e->drag_slot || e->drag_pad < 0 || e->blk - e->drag_blk > DRAG_GAP_BLOCKS) {
        int pad[NMARK], off[NMARK];
        tab_slots(e, in, pad, off);
        e->drag_slot = k;
        e->drag_pad = pad[k] >= 0 ? pad[k] : k > 0 && pad[k - 1] >= 0 ? pad[k - 1] : -1;
        if (e->drag_pad >= 0) { select_pad(e, e->drag_pad); e->drag_pos = (double)e->cue[e->drag_pad]; }
    }
    e->drag_blk = e->blk;
    int p = e->drag_pad;
    if (p < 0) return;
    long v0, vl;
    view_of(e, in, &v0, &vl);
    e->drag_pos = fmin(fmax(e->drag_pos + (double)d * GAIN[opt(e, P_DRAG_SPEED)] * vl, 0), (double)(in->z.len - 64));
    e->cue[p] = snap_cue(e, in, lround(e->drag_pos));
    sort_pads(e, p);   /* past a neighbour: the pads renumber, the selection follows the chop */
    e->drag_pad = (int)lrintf(e->val[P_SEL_PAD]);
    keep_in_view(e, e->cue[e->drag_pad]);
    show_pos(e);
}

/* the browser's entry idx: a folder opens; a WAV highlights and plays its preview, or loads when it was already highlighted
 * (load: LOAD, which always loads); an entry of the sound list loads and plays from its start. With e->mu held */
static void browse_pick(chrono_t *e, int idx, int load) {
    if (e->br_scanning || idx < 0 || idx >= e->br_n) return;
    const struct br_ent *b = &e->br[idx];
    char p[700];
    snprintf(p, sizeof p, "%s/%s", e->br_dir[0] ? e->br_dir : "/media", b->name);
    if (b->dir) {
        if (e->br_depth < 24) e->br_tops[e->br_depth] = e->br_top;
        e->br_depth++;
        snprintf(e->br_dir, sizeof e->br_dir, "%s", b->li == -2 ? BUILTIN_DIR : p);
        e->br_top = 0; e->br_sel = -1; e->br_acc = 0; e->br_req = 1; e->load_fail = 0;
    } else if (b->li >= 0) {
        e->br_sel = idx; e->val[P_SOUND] = (float)b->li; mark_changed(e, P_SOUND);
        request_load(e); e->aud_on_load = 1;
    } else if (load || idx == e->br_sel) {
        e->br_sel = idx; request_path(e, p); mark_changed(e, P_SOUND);
        e->aud_on_load = 0;   /* its preview plays on */
    } else {
        e->br_sel = idx; snprintf(e->aud_path, sizeof e->aud_path, "%s", p); e->aud_req = 1;
    }
    pthread_cond_signal(&e->bcv);
}

/* the browser's scroll strip: MPC sends a drag as the value it read back plus a step, so the strip goes back to the middle
 * after each move; the bar follows the finger (down shows later rows), whole rows at a time, the rest kept for the next move */
static void browse_drag(chrono_t *e, float x) {
    e->val[P_BR_DRAG] = 0.5f;
    mark_changed(e, P_BR_DRAG);
    float d = spring_step(e, P_BR_DRAG, x, 0.5f);
    if (fabsf(d) < 1e-6f) return;
    pthread_mutex_lock(&e->mu);
    e->br_acc -= d * BR_ROWS_PER_DRAG;
    int n = (int)e->br_acc;
    if (n) {
        int max = e->br_n > NROWS ? e->br_n - NROWS : 0, top = e->br_top + n;
        e->br_acc -= (float)n;
        e->br_top = top < 0 ? 0 : top > max ? max : top;
    }
    pthread_mutex_unlock(&e->mu);
}

/* the key the sound plays in (its key moved by KEY and PITCH), -1 none */
static int moved_key(const chrono_t *e, const inst_t *in) {
    int sk = sound_key(e, in);
    if (sk < 0) return -1;
    int r = ((sk % 12 + key_shift(e, in) + (int)lrintf(e->val[P_PITCH])) % 12 + 12) % 12;
    return r + (sk >= 12 ? 12 : 0);
}

/* the browser's highlight to entry c (kept on screen); a WAV plays its preview. With e->mu held */
static void cursor_to(chrono_t *e, int c) {
    if (e->br_scanning || e->br_n <= 0) return;
    c = c < 0 ? 0 : c >= e->br_n ? e->br_n - 1 : c;
    if (c == e->br_sel) return;
    e->br_sel = c; br_show(e, c);
    const struct br_ent *b = &e->br[c];
    if (!b->dir && b->li < 0) {
        snprintf(e->aud_path, sizeof e->aud_path, "%s/%s", e->br_dir[0] ? e->br_dir : "/media", b->name);
        e->aud_due = e->blk + 60; if (!e->aud_due) e->aud_due = 1;   /* ~170 ms after the last click (br_coast) */
    } else e->aud_due = 0;
}

/* the list's top to (fractional) row f, kept in range; with e->mu held */
static void br_scroll_to(chrono_t *e, float f) {
    float max = e->br_n > NROWS ? (float)(e->br_n - NROWS) : 0;
    if (f <= 0 || f >= max) e->br_vel = 0;   /* an end stops a coast */
    e->br_pos = f < 0 ? 0 : f > max ? max : f;
    e->br_top = (int)lrintf(e->br_pos);
}
/* each block, with e->mu held: a flick, once the finger is off (no move for 12 blocks, ~35 ms), coasts on, slowing down */
static void br_coast(chrono_t *e) {
    for (int i = P_TD00; i < P_TD00 + NMARK; i++)   /* the tab strips and the rows, once let go: back to the middle */
        if (!e->sp_rst[i] && fabsf(e->val[i] - 0.5f) > 0.15f && e->blk - e->sp_blk[i] > 35) { e->val[i] = 0.5f; mark_changed(e, i); e->sp_rst[i] = 1; }
    for (int i = P_BD01; i < P_BD01 + NROWS; i++)
        if (!e->sp_rst[i] && fabsf(e->val[i] - 0.5f) > 0.15f && e->blk - e->sp_blk[i] > 35) { e->val[i] = 0.5f; mark_changed(e, i); e->sp_rst[i] = 1; }
    if (e->br_g_on && e->blk - e->br_drag_blk > 12) {
        e->br_g_on = 0;
        float v = e->br_g_rows / (float)(e->br_drag_blk - e->br_g_blk + 6) + 0.5f * e->br_carry;   /* rows a block */
        e->br_vel = fabsf(v) > 0.03f ? fminf(fmaxf(v, -2), 2) : 0;
    }
    e->br_hist[e->blk & 127] = (short)e->br_top;   /* where the list was, a block at a time (a double tap's row) */
    if (!e->br_g_on && e->br_vel != 0) {
        e->br_coast_blk = e->blk;
        br_scroll_to(e, e->br_pos + e->br_vel);
        e->br_vel *= 0.99f;
        if (fabsf(e->br_vel) < 0.02f) e->br_vel = 0;
    }
    if (e->aud_due && (int)(e->blk - e->aud_due) >= 0) {   /* the wheel at rest on a WAV: its preview */
        e->aud_due = 0; e->aud_req = 1; pthread_cond_signal(&e->bcv);
    }
}

/* a browser row strip moved. MPC sends a touch drag, a data wheel click and a Q-Link step alike, as the value it read back plus
 * a step: a drag moves at least ~1/40 of the strip an event, a wheel click 0.01 and a Q-Link step 1/128. A click or step moves
 * the highlight a row (clockwise down; the first from the row touched), a drag scrolls the rows with the finger */
static void browse_row(chrono_t *e, int row, float x) {
    int i = P_BD01 + row;
    /* the wheel near an end of the strip: the engine hands MPC the middle (0.5) again, which MPC takes between bursts of
     * clicks (input log 2026-10-28: at 1.000 further clicks that way sent nothing, the wheel seemed stuck). Until it has, a
     * click may come from either value */
    if (e->br_rst[row] && fabsf(x - 0.5f) < fabsf(x - e->sp_x[i]) && fabsf(fabsf(x - e->sp_x[i]) - 0.01f) >= 0.0007f) e->sp_x[i] = 0.5f;   /* nearer the middle: MPC took it */
    if (e->br_rst[row] && fabsf(x - 0.5f) < 0.2f) e->br_rst[row] = 0;
    float d = strip_step(e, i, x);
    if (fabsf(d) < 1e-6f) return;
    pthread_mutex_lock(&e->mu);
    float ad = fabsf(d);
    if (fabsf(ad - 0.01f) < 0.0007f || fabsf(ad - 1 / 128.0f) < 0.0004f) {   /* a wheel click (0.01) or a Q-Link step (1/128) */
        int seen = e->br_sel >= e->br_top && e->br_sel < e->br_top + NROWS;
        int c = seen || (row == e->br_wrow && e->br_sel >= 0) ? e->br_sel + (d > 0 ? 1 : -1) : e->br_top + row;
        e->br_wrow = row;
        cursor_to(e, c);
        if (x > 0.85f || x < 0.15f) { e->val[i] = 0.5f; mark_changed(e, i); e->br_rst[row] = 1; }   /* back to the middle */
    } else {
        float rows = d * BR_ROWS_PER_ROWDRAG;   /* the finger up shows later rows */
        if (!e->br_g_on) {   /* a new touch: from where the list is; a flick the same way as one still coasting adds to it */
            e->br_g_on = 1; e->br_g_blk = e->blk; e->br_g_rows = 0;
            e->br_carry = e->br_vel * rows > 0 ? e->br_vel : 0;
            e->br_vel = 0; e->br_pos = (float)e->br_top;
        }
        e->br_g_rows += rows; e->br_drag_blk = e->blk;
        br_scroll_to(e, e->br_pos + rows);
    }
    pthread_mutex_unlock(&e->mu);
}

/* RANDOM CHOPS: all 64 pads chopped at places picked at random: the sound's hits (any strength) first, then, for a sound
 * with fewer, places anywhere (on a zero crossing when SNAP is on); at least 70 ms apart, in time order through the banks */
static void random_chops(chrono_t *e, const inst_t *in) {
    long len = in->z.len, hit[256], got[NPADS], gap = (long)(0.07f * SR);
    if (len < 4096) return;
    int nh = chop_transients(in, 1.0f, hit, 256), ng = 0;
    for (int i = nh - 1; i > 0; i--) {   /* shuffled */
        int j = (int)((frand(e) * 0.5f + 0.5f) * (i + 1)); j = j > i ? i : j;
        long t = hit[i]; hit[i] = hit[j]; hit[j] = t;
    }
    for (int t = 0; ng < NPADS && t < nh + 4000; t++) {
        long f = t < nh ? hit[t] : snap_cue(e, in, (long)((frand(e) * 0.5f + 0.5f) * (len - 4096)));
        int clash = 0;
        for (int k = 0; k < ng && !clash; k++) clash = labs(got[k] - f) < gap;
        if (!clash) got[ng++] = f;
    }
    qsort(got, (size_t)ng, sizeof(long), long_cmp);
    for (int k = 0; k < NPADS; k++) e->cue[k] = k < ng ? got[k] : -1;
    snprintf(e->cue_name, sizeof e->cue_name, "%s", in->name);
}

/* UNDO: what an edit can be taken back from (chops, pad settings, the sound's settings; not the view, the bank or the browser) */
static int undoable(int i, float x) {
    if (i >= P_TD00 && i < P_TD00 + NMARK) return 1;
    if (i >= P_FL00 && i < P_FL00 + 16) return 1;
    if (i == P_FIND || i == P_CHOP_RAND || i == P_CLEAR_CUES || i == P_CHOP_ADD || i == P_CHOP_DEL || i == P_P_RESET || i == P_FX_COPY || i == P_FX_CLEAR
        || i == P_KEY_UP || i == P_KEY_DOWN) return x > 0.5f;
    if (i == P_ZOOM || i == P_SCROLL || i == P_BANK || i == P_SEL_PAD || i == P_PAD_TAP || i == P_DRAG_SPEED || i == P_BR_CURSOR) return 0;
    return (PKIND[i] == K_CONT || PKIND[i] == K_INT || PKIND[i] == K_OPT) && i != P_SOUND;
}
/* the state before an edit, as a step (one per gesture: the same control again within a moment adds none) */
static void undo_push(chrono_t *e, int kind) {
    int same = kind == e->undo_kind && PKIND[kind] != K_TRIG && e->blk - e->undo_blk < DRAG_GAP_BLOCKS && e->undo_n > 0;   /* a press is a step of its own */
    e->undo_kind = kind; e->undo_blk = e->blk;
    if (same) return;
    struct undo *u = &e->undo[e->undo_at];
    memcpy(u->cue, e->cue, sizeof u->cue); memcpy(u->pad, e->pad, sizeof u->pad); memcpy(u->val, e->val, sizeof u->val);
    e->undo_at = (e->undo_at + 1) % UNDO_N;
    if (e->undo_n < UNDO_N) e->undo_n++;
}
static void undo_pop(chrono_t *e) {
    if (!e->undo_n) { e->chop_msg = 11; e->chop_msg_frames = (long)(3 * SR); return; }
    e->undo_at = (e->undo_at + UNDO_N - 1) % UNDO_N; e->undo_n--;
    const struct undo *u = &e->undo[e->undo_at];
    memcpy(e->cue, u->cue, sizeof e->cue); memcpy(e->pad, u->pad, sizeof e->pad);
    for (int i = 0; i < NP; i++)
        if (undoable(i, 0) && i != P_P_POS && i != P_P_NUDGE && e->val[i] != u->val[i] && strncmp(KEYS[i], "p_", 2)) { e->val[i] = u->val[i]; mark_changed(e, i); }
    e->undo_kind = -1;
    int sp = (int)lrintf(e->val[P_SEL_PAD]);
    select_pad(e, sp);   /* the pad's own controls from its restored settings */
    e->chop_msg = 10; e->chop_msg_frames = (long)(3 * SR); e->unmapped_frames = 0;
}

/* the data wheel on a BROWSE button that has the focus (it was the last thing touched, e.g. BACK): MPC turns the button's own
 * value, from its last, in 0.01 a click (input log 2026-10-27). Those small values move the highlight; a press is 1 */
static int browse_button_wheel(chrono_t *e, int i, float x) {
    if (!(i == P_BR_UP || i == P_BR_HOME || i == P_BR_LOAD || i == P_BR_PGUP || i == P_BR_PGDN || i == P_BR_ENTER
          || (i >= P_BT01 && i < P_BT01 + NROWS))) return 0;
    float last = e->sp_x[i];
    unsigned blk0 = e->sp_blk[i];
    int recent = e->blk - blk0 < 400;
    e->sp_x[i] = x; e->sp_blk[i] = e->blk;
    if (x > 0.5f) return last > 0.5f && recent && e->blk - blk0 < 60;   /* a press; MPC can send one twice ~110 ms apart (input log 2026-10-28: br_up 1, 39 blocks, 1): once */
    if (x < 0.005f && (last > 0.5f || !recent)) return 1;   /* its release (0), or a 0 out of nowhere */
    int fresh = x < last && x < 0.025f && e->blk - blk0 > 15;   /* a new burst after a pause, counted from 0 again */
    float d = x - (recent && last < 0.5f && !fresh ? last : 0);
    (void)d;   /* the wheel turning a button: nothing (1.0.10: the user wants the wheel on the list only) */
    return 1;
}

/* a double tap on FX preset f: its two taps picked it, so the pad's preset goes back to what it was; then f is layered after
 * it, or taken off if it was layered. A pad with no FX takes f as its preset */
static void fx_layer_tap(chrono_t *e, int f) {
    int sp = (int)lrintf(e->val[P_SEL_PAD]);
    pad_t *pd = &e->pad[sp];
    if (e->fxpick_pad == sp && e->blk - e->fxpick_blk < 520) {   /* its taps picked f (~1.5 s: a slow double tap took 0.9 s): undone */
        pd->fx = e->fxpick_prev; memcpy(pd->fx_lay, e->fxpick_lay, sizeof pd->fx_lay);
        e->fxpick_blk = e->blk - 1000;   /* used */
    }
    int was = pd->fx;
    if (!was) { pd->fx = pd->fx_keep = f; select_pad(e, sp); e->fx_msg = 16; e->fx_msg_blk = e->blk; return; }
    pd->fx_keep = was;
    int k, at = -1;
    for (k = 0; k < FX_LAYERS - 1; k++) if (pd->fx_lay[k] == f) break;
    if (k < FX_LAYERS - 1) {   /* layered: off, the rest close up */
        for (; k < FX_LAYERS - 2; k++) pd->fx_lay[k] = pd->fx_lay[k + 1];
        pd->fx_lay[FX_LAYERS - 2] = 0;
        e->fx_msg = 17;
    } else if (f == was) e->fx_msg = 18;   /* the pad's own preset */
    else {
        for (k = 0; k < FX_LAYERS - 1 && at < 0; k++) if (!pd->fx_lay[k]) at = k;
        if (at < 0) e->fx_msg = 19;   /* full */
        else { pd->fx_lay[at] = f; e->fx_msg = 16; }
    }
    e->fx_msg_blk = e->blk;
    select_pad(e, sp);
}

/* STOP: everything lets go at once (fading out), the preview too; a MANUAL run ends */
static void stop_all(chrono_t *e) {
    if (e->rec) stop_rec(e);
    for (int k = 0; k < NV; k++) if (e->v[k].active) e->v[k].done_fade = 1;
    memset(e->hold_on, 0, sizeof e->hold_on);
    e->aud_stop = 1;
}

static void set_value(chrono_t *e, int i, float x) {
    if (PKIND[i] == K_TEXT || PKIND[i] == K_VIS || i == P_SOUND_PREV || i == P_SOUND_NEXT || i == P_SEL_PAD_PREV || i == P_SEL_PAD_NEXT) return;
    /* a double tap on a row and the data wheel's push reach the engine as a switch MPC flips (1, then 0 the next time: input
     * log 2026-10-28); the 0 was taken for a release, so every other one did nothing. Either way is a press */
    if (((i >= P_BT01 && i < P_BT01 + NROWS) || i == P_BR_ENTER) && x < 0.005f && e->sp_x[i] > 0.5f && e->blk - e->sp_blk[i] > 40) x = 1;
    if (i >= P_FL00 && i < P_FL00 + 16) {   /* a double tap on an FX preset (a switch MPC flips: 1, or 0 the next time) */
        int press = x > 0.5f || (e->sp_x[i] > 0.5f && e->blk - e->sp_blk[i] > 40);
        e->sp_x[i] = x > 0.5f ? 1 : 0; e->sp_blk[i] = e->blk;
        if (press) fx_layer_tap(e, i - P_FL00 + 1);
        e->val[i] = 0; return;
    }
    if (i >= P_TT00 && i < P_TT00 + NMARK) {   /* a double tap on a tab slot (a switch MPC flips: 1, or 0 the next time) */
        int press = x > 0.5f || (e->sp_x[i] > 0.5f && e->blk - e->sp_blk[i] > 40);
        e->sp_x[i] = x > 0.5f ? 1 : 0; e->sp_blk[i] = e->blk;
        if (press) tab_tap(e, i - P_TT00);
        e->val[i] = 0; return;
    }
    if (PKIND[i] == K_TRIG && x > 0.5f && !(i >= P_BT01 && i < P_BT01 + NROWS) && !(i >= P_TT00 && i < P_TT00 + NMARK) && i != P_BR_ENTER) {
        if (e->trg_blk[i] && e->blk - e->trg_blk[i] < TRIG_TWICE_BLOCKS) { e->val[i] = 0; return; }   /* the same press again */
        e->trg_blk[i] = e->blk ? e->blk : 1;
    }
    if (browse_button_wheel(e, i, x)) return;
    if (PKIND[i] != K_TRIG && PKIND[i] != K_ROW && PKIND[i] != K_DRAG) {   /* the same value again (MPC sends back what the engine reported): nothing to do */
        float c = clampf(x, PMIN[i], PMAX[i]);
        if (PKIND[i] == K_INT || PKIND[i] == K_OPT) c = roundf(c);
        if (c == e->val[i]) return;
    }
    if (undoable(i, x) && e->cur) undo_push(e, i);
    if (i == P_BR_DRAG) { browse_drag(e, clampf(x, 0, 1)); return; }
    if (i >= P_BD01 && i < P_BD01 + NROWS) { browse_row(e, i - P_BD01, clampf(x, 0, 1)); return; }
    if (i >= P_BT01 && i < P_BT01 + NROWS) {   /* a double tap on a row: the folder opens, the WAV (or sound) loads */
        if (x > 0.5f) {
            pthread_mutex_lock(&e->mu);
            int top = e->blk - e->br_coast_blk < 128 ? e->br_hist[(e->blk - 100) & 127] : e->br_top;   /* coasting: where it was */
            e->br_wrow = -1; e->br_vel = 0; e->br_g_on = 0;
            if (top != e->br_top) br_scroll_to(e, (float)top);
            browse_pick(e, top + i - P_BT01, 1);
            pthread_mutex_unlock(&e->mu);
        }
        e->val[i] = 0; return;
    }
    if (PKIND[i] == K_DRAG) { tab_drag(e, i - P_TD00, clampf(x, 0, 1)); return; }
    if (PKIND[i] == K_ROW) { if (x > 0.5f) { pthread_mutex_lock(&e->mu); browse_pick(e, e->br_top + i - P_BR_1, 0); pthread_mutex_unlock(&e->mu); } return; }
    x = clampf(x, PMIN[i], PMAX[i]);
    if (PKIND[i] == K_INT || PKIND[i] == K_OPT) x = roundf(x);
    float old = e->val[i];
    e->val[i] = x;
    int sp = (int)lrintf(e->val[P_SEL_PAD]);
    pad_t *pd = &e->pad[sp];
    const inst_t *in = e->cur;
    switch (i) {
    case P_RESCAN: if (x > 0.5f) { pthread_mutex_lock(&e->mu); e->rescan_req = 1; pthread_cond_signal(&e->cv); pthread_mutex_unlock(&e->mu); } e->val[i] = 0; return;
    case P_LEARN_PADS:   /* a second tap while learning cancels it */
        if (x > 0.5f) {
            if (e->learn) learn_cancel(e);
            else { memcpy(e->padmap_before, e->padmap, sizeof e->padmap); e->learn = 1; e->learn_frames = (long)(LEARN_WAIT_S * SR); }
        }
        e->val[i] = 0; return;
    case P_FIND:   /* CHOP: by Chop Mode (the line under the waveform says how many); MANUAL arms a run, or ends the one going */
        if (x > 0.5f && in) {
            if (e->rec) stop_rec(e);
            else if (opt(e, P_CHOP_MODE) == 3) e->rec = 1;
            else {
                e->chop_msg_frames = 0;
                long was[NPADS]; memcpy(was, e->cue, sizeof was);
                chop_now(e, in); select_pad(e, sp);
                e->chop_same = !memcmp(was, e->cue, sizeof was);
                if (e->chop_msg_frames <= 0) { e->chop_msg = 6; e->chop_msg_frames = (long)(3 * SR); }   /* unless it warned */
                e->unmapped_frames = 0;   /* the newest message shows */
            }
        }
        e->val[i] = 0; return;
    case P_CHOP_RAND:
        if (x > 0.5f && in) {
            if (e->rec) stop_rec(e);
            random_chops(e, in); select_pad(e, sp);
            e->chop_msg = 15; e->chop_msg_frames = (long)(3 * SR); e->unmapped_frames = 0;
        }
        e->val[i] = 0; return;
    case P_CHOP_ADD:   /* a chop where the sound plays now (else mid-view), on the first free pad, then the pads in time order */
        if (x > 0.5f && in) {
            int k = -1;
            for (int j = 0; j < NPADS && k < 0; j++) if (e->cue[j] < 0) k = j;
            if (k < 0) { e->chop_msg = 8; e->chop_msg_frames = (long)(3 * SR); }
            else {
                const voice_t *nw = NULL;
                for (int j = 0; j < NV; j++) if (e->v[j].active && !e->v[j].done_fade && (!nw || e->v[j].age > nw->age)) nw = &e->v[j];
                long v0, vl, f;
                view_of(e, in, &v0, &vl);
                f = snap_cue(e, in, nw ? (long)heard_pos(nw) : v0 + vl / 2);
                if (strcmp(e->cue_name, in->name)) snprintf(e->cue_name, sizeof e->cue_name, "%s", in->name);
                e->cue[k] = f; e->pad[k] = PAD_DEFAULT;
                sort_pads(e, k);
                for (int j = 0; j < NPADS; j++) if (e->cue[j] == f) { select_pad(e, j); break; }
                e->chop_msg = 7; e->chop_msg_frames = (long)(3 * SR); e->unmapped_frames = 0;
            }
        }
        e->val[i] = 0; return;
    case P_CHOP_DEL:   /* the selected chop goes; the chops after it move down a pad */
        if (x > 0.5f && e->cue[sp] >= 0) {
            e->cue[sp] = -1; e->pad[sp] = PAD_DEFAULT; e->hold_on[sp] = 0;
            for (int j = 0; j < NV; j++) if (e->v[j].active && e->v[j].pad == sp) e->v[j].done_fade = 1;
            sort_pads(e, sp);
            int last = -1;
            for (int j = 0; j < NPADS; j++) if (e->cue[j] >= 0) last = j;
            select_pad(e, sp <= last ? sp : last >= 0 ? last : 0);
            e->chop_msg = 9; e->chop_msg_frames = (long)(3 * SR); e->unmapped_frames = 0;
        }
        e->val[i] = 0; return;
    case P_BR_CURSOR:   /* a step of the browser's cursor (the data wheel, a Q-Link): the highlighted row a step on */
        pthread_mutex_lock(&e->mu);
        cursor_to(e, e->br_sel < 0 ? 0 : (int)x);   /* nothing highlighted yet: the first row */
        if (e->br_sel >= 0) e->val[i] = (float)e->br_sel;
        pthread_mutex_unlock(&e->mu);
        mark_changed(e, i);
        return;
    case P_UNDO: if (x > 0.5f) undo_pop(e); e->val[i] = 0; return;
    case P_P_RESET:   /* the selected pad's pitch, speed and reverse back to plain */
        if (x > 0.5f) { pd->key = 0; pd->speed = 0; pd->reverse = 0; select_pad(e, sp); }
        e->val[i] = 0; return;
    case P_P_FX: pd->fx = (int)x; if (pd->fx) pd->fx_keep = pd->fx; select_pad(e, sp); return;
    case P_P_FX_PICK: {   /* picking a preset makes it the pad's own and turns the FX on. The chain it changed is kept a moment,
                           * for a double tap (whose first tap is a pick) to put back. A layered preset picked swaps places
                           * with the pad's own, so no preset drops out of the chain (input log 2026-10-30) */
        int f = (int)x + 1;
        if (f != pd->fx || e->fxpick_pad != sp) {   /* a pick that changes it (a double tap's second does not) */
            e->fxpick_prev = pd->fx; memcpy(e->fxpick_lay, pd->fx_lay, sizeof e->fxpick_lay);
        }
        e->fxpick_blk = e->blk; e->fxpick_pad = sp;
        for (int k = 0; k < FX_LAYERS - 1; k++) if (pd->fx_lay[k] == f) pd->fx_lay[k] = pd->fx;
        pd->fx = pd->fx_keep = f; select_pad(e, sp); return;
    }
    case P_P_FX_ON: pd->fx = x > 0.5f ? (pd->fx_keep > 0 ? pd->fx_keep : 1) : 0; select_pad(e, sp); return;
    case P_P_FX_AMT: pd->fx_amt = x; return;
    case P_P_FX_RATE: pd->fx_rate = (int)x; return;
    case P_P_FX_MIX: pd->fx_mix = x; return;
    case P_P_FX_DRIVE: pd->fx_drive = x; return;
    case P_P_FX_TONE: pd->fx_tone = x; return;
    case P_P_FX_WIDTH: pd->fx_width = x; return;
    case P_P_FX_RES: pd->fx_res = x; return;
    case P_FX_COPY: case P_FX_CLEAR:   /* the selected pad's FX on every pad, or none anywhere */
        if (x > 0.5f) {
            for (int k = 0; k < NPADS; k++) {
                pad_t *q = &e->pad[k];
                const pad_t *f = i == P_FX_COPY ? pd : &PAD_DEFAULT;
                q->fx = f->fx; q->fx_keep = f->fx_keep; q->fx_amt = f->fx_amt; q->fx_rate = f->fx_rate; q->fx_mix = f->fx_mix;
                q->fx_drive = f->fx_drive; q->fx_tone = f->fx_tone; q->fx_width = f->fx_width; q->fx_res = f->fx_res;
                memcpy(q->fx_lay, f->fx_lay, sizeof q->fx_lay);
            }
            select_pad(e, sp);
            e->chop_msg = i == P_FX_COPY ? 12 : 13; e->chop_msg_frames = (long)(3 * SR); e->unmapped_frames = 0;
        }
        e->val[i] = 0; return;
    case P_BR_ENTER:   /* the data wheel pushed: the highlighted folder opens, its WAV (or sound) loads */
        if (x > 0.5f) { pthread_mutex_lock(&e->mu); browse_pick(e, e->br_sel, 1); pthread_mutex_unlock(&e->mu); }
        e->val[i] = 0; return;
    case P_CHOP_MODE: case P_SENSITIVITY: case P_CUE_COUNT: case P_CHOP_DIV:   /* chop again as they move */
        if (x == old || !in) return;
        if (e->rec) stop_rec(e);
        if (opt(e, P_CHOP_MODE) == 3) { if (i == P_CHOP_MODE) e->rec = 1; return; }
        chop_now(e, in); select_pad(e, sp); return;
    case P_CLEAR_CUES: if (x > 0.5f) { if (e->rec) stop_rec(e); for (int k = 0; k < NPADS; k++) e->cue[k] = -1; select_pad(e, sp); } e->val[i] = 0; return;
    case P_ZOOM: if (x != old && in) { long v0, vl; view_of(e, in, &v0, &vl); centre_view(e, e->cue[sp] >= 0 ? e->cue[sp] : v0 + vl / 2); } return;
    case P_SCROLL: show_pos(e); return;
    case P_STOP: if (x > 0.5f) stop_all(e); e->val[i] = 0; return;   /* everything lets go at once, the preview too */
    case P_KEY_DOWN: case P_KEY_UP:   /* MAIN's KEY - and +: a semitone, tempo untouched (Pitch, which the key shown follows) */
        if (x > 0.5f) {
            float pt = clampf(e->val[P_PITCH] + (i == P_KEY_UP ? 1 : -1), PMIN[P_PITCH], PMAX[P_PITCH]);
            if (pt != e->val[P_PITCH]) { e->val[P_PITCH] = pt; mark_changed(e, P_PITCH); }
        }
        e->val[i] = 0; return;
    case P_BR_LOAD: if (x > 0.5f) { pthread_mutex_lock(&e->mu); browse_pick(e, e->br_sel, 1); pthread_mutex_unlock(&e->mu); } e->val[i] = 0; return;
    case P_RESET_PADS:
        if (x > 0.5f) {
            default_padmap(e); e->learn = 0;
            pthread_mutex_lock(&e->mu); e->save_pads = 2; pthread_cond_signal(&e->cv); pthread_mutex_unlock(&e->mu);
            e->chop_msg = 3; e->chop_msg_frames = (long)(3 * SR);
        }
        e->val[i] = 0; return;
    case P_BR_HOME: case P_BR_UP: case P_BR_PGUP: case P_BR_PGDN:
        if (x > 0.5f) {
            pthread_mutex_lock(&e->mu);
            e->br_vel = 0; e->br_g_on = 0; e->br_coast_blk = e->blk - 1000;   /* a coast stops */
            int max = e->br_n > NROWS ? e->br_n - NROWS : 0;
            if (i == P_BR_PGUP) e->br_top = e->br_top >= NROWS ? e->br_top - NROWS : 0;
            else if (i == P_BR_PGDN) e->br_top = e->br_top + NROWS < max ? e->br_top + NROWS : max;
            else {   /* Drives, or up a folder (a drive's top goes back to the drives) */
                char *sl = strrchr(e->br_dir, '/');
                snprintf(e->br_back, sizeof e->br_back, "%s", i == P_BR_HOME ? "" : sl ? sl + 1 : e->br_dir);   /* BACK lights it */
                if (i == P_BR_HOME || !sl || sl - e->br_dir <= 6 /* "/media" */) e->br_dir[0] = 0;
                else *sl = 0;
                e->br_back_top = -1;
                if (i == P_BR_HOME) e->br_depth = 0;
                else if (e->br_depth > 0 && --e->br_depth < 24) e->br_back_top = e->br_tops[e->br_depth];
                e->br_top = 0; e->br_sel = -1; e->br_acc = 0; e->br_req = 1; e->load_fail = 0;
                pthread_cond_signal(&e->bcv);
            }
            pthread_mutex_unlock(&e->mu);
        }
        e->val[i] = 0; return;
    case P_SOUND:   /* a step (the arrows, a Q-Link): the next sound, played from its start as it arrives; a browsed sound's
                     * next is the next WAV of its folder (the browser thread finds it), the list index stays parked */
        if (x != old) {
            pthread_mutex_lock(&e->mu);
            if (e->browsed) { e->fold_step = x > old ? 1 : -1; e->val[i] = old; mark_changed(e, i); pthread_cond_signal(&e->bcv); }
            else { request_load(e); e->aud_on_load = 1; }
            pthread_mutex_unlock(&e->mu);
        }
        return;
    case P_SEL_PAD: if (x != old) select_pad(e, (int)x); return;
    case P_BANK: if (x != old) { int np = (sp % 16) + 16 * (int)x; select_pad(e, np); } return;
    case P_PAD_TAP: select_pad(e, GRID_SLOT((int)x) + 16 * opt(e, P_BANK)); return;   /* a tap on the grid: that pad of the bank */
    case P_P_POS: if (in) {   /* a place in the view (the CUE knob: finer as the view closes in) */
            long v0, vl;
            view_of(e, in, &v0, &vl);
            float d = x - old;
            if (e->cue[sp] < 0 || fabsf(d) > 0.2f) e->pos_acc = v0 + (double)x * vl;
            else {
                if (fabs(e->pos_acc - e->cue[sp]) >= 1) e->pos_acc = (double)e->cue[sp];   /* moved by something else since */
                e->pos_acc += (double)d * vl * (fabsf(d) > 1.5f / 128 ? 1.0 : 0.45);   /* a fast spin (several clicks an event): at full */
            }
            e->pos_acc = fmin(fmax(e->pos_acc, 0), (double)(in->z.len - 64));
            e->cue[sp] = lround(e->pos_acc);
            sort_pads(e, sp);
            show_pos(e);   /* where it is now, for the next click to start from */
        }
        return;
    case P_P_NUDGE:   /* a fine move, then back to the middle */
        if (e->chop_msg != 14 || e->chop_msg_frames <= 0) e->nudged = 0;   /* a new turn counts from 0 */
        if (in && e->cue[sp] >= 0 && fabsf(x) > 0.001f) {
            float st = spring_step(e, i, x, 0);
            long c0 = e->cue[sp], c = c0 + (long)lrintf(st * nudge_s(e) * SR);
            e->cue[sp] = c < 0 ? 0 : c > in->z.len - 64 ? in->z.len - 64 : c;
            e->nudged += e->cue[sp] - c0; e->chop_msg = 14; e->chop_msg_frames = (long)(2 * SR); e->unmapped_frames = 0;
            keep_in_view(e, e->cue[sp]); show_pos(e);
            sort_pads(e, sp);
        }
        e->val[i] = 0; mark_changed(e, i); return;
    case P_P_LEN: pd->len = (int)x; return;
    case P_P_LEVEL: pd->level = x; return;
    case P_P_FILTER: pd->filter = x; return;
    case P_P_ATTACK: pd->attack = x; return;
    case P_P_RELEASE: pd->release = x; return;
    case P_P_REVERSE: pd->reverse = (int)x; return;
    case P_P_KEY: pd->key = (int)x; return;
    case P_P_SPEED: pd->speed = x; return;
    case P_P_MODE: pd->mode = (int)x; if (pad_mode(e, sp) != 4) e->hold_on[sp] = 0; return;
    case P_P_CHOKE: pd->choke = (int)x; return;
    case P_P_LAYER: pd->layer = (int)x; return;
    case P_P_DRUMS: pd->drums = (int)x; return;
    case P_P_VOCALS: pd->vocals = (int)x; return;
    }
}

/* the state: "key=value;" for the globals, "cues=a,b,..;", "pN=..;" for the pads that differ from the default,
 * "cname=..;sname=..;" */
static void apply_state(chrono_t *e, const char *val) {
    char *buf = strdup(val), want[64] = "", wpath[512] = "";
    if (!buf) return;
    char *save = NULL;
    for (char *t = strtok_r(buf, ";", &save); t; t = strtok_r(NULL, ";", &save)) {
        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = 0;
        const char *v = eq + 1;
        if (!strcmp(t, "sname")) { snprintf(want, sizeof want, "%s", v); continue; }
        if (!strcmp(t, "spath")) { snprintf(wpath, sizeof wpath, "%s", v); continue; }   /* a sound picked in the browser */
        if (!strcmp(t, "cname")) { snprintf(e->cue_name, sizeof e->cue_name, "%s", v); continue; }
        if (!strcmp(t, "roll")) continue;   /* 0.4 Find Samples' shuffle */
        if (!strcmp(t, "cues")) {
            for (int k = 0; k < NPADS; k++) e->cue[k] = -1;
            char *s2 = NULL; int k = 0;
            for (char *c = strtok_r((char *)v, ",", &s2); c && k < NPADS; c = strtok_r(NULL, ",", &s2)) e->cue[k++] = strtol(c, NULL, 10);
            continue;
        }
        if (t[0] == 'p' && isdigit((unsigned char)t[1])) {
            int p = atoi(t + 1);
            if (p < 0 || p >= NPADS) continue;
            pad_t *pd = &e->pad[p];
            int x[8] = {0}, rate = 4, layer = 0, lay[3] = { 0, 0, 0 };
            float amt = 0.6f, mix = 1, f4[4] = { 0, 0, 0.5f, 0.25f };
            *pd = PAD_DEFAULT;
            int n = sscanf(v, "%f,%f,%f,%f,%f,%d,%d,%d,%d,%d,%d,%d,%d,%f,%d,%f,%f,%f,%f,%f,%d,%d,%d,%d", &pd->level, &pd->filter, &pd->attack, &pd->release, &pd->speed,
                           &x[0], &x[1], &x[2], &x[3], &x[4], &x[5], &x[6], &x[7], &amt, &rate, &mix, &f4[0], &f4[1], &f4[2], &f4[3], &layer, &lay[0], &lay[1], &lay[2]);
            pd->reverse = x[0]; pd->key = x[1]; pd->choke = x[3];
            if (n == 13) { pd->mode = x[2] == 1 ? 3 : x[2] == 2 ? 4 : 0; pd->len = x[5]; pd->drums = x[6]; pd->vocals = x[7]; }   /* 0.2 (with a colour) */
            else { pd->mode = x[2]; pd->len = x[4]; pd->drums = x[5]; pd->vocals = x[6]; }
            if (n >= 16) { pd->fx = x[7]; pd->fx_amt = amt; pd->fx_rate = rate; pd->fx_mix = mix; if (pd->fx) pd->fx_keep = pd->fx; }   /* 0.9: its FX */
            if (n >= 20) { pd->fx_drive = f4[0]; pd->fx_tone = f4[1]; pd->fx_width = f4[2]; pd->fx_res = f4[3]; }   /* 1.0.2 */
            if (n >= 21) pd->layer = layer < 0 || layer > 4 ? 0 : layer;   /* 1.0.15: its layer group */
            for (int k = 0; k < 3 && n >= 22 + k; k++) pd->fx_lay[k] = lay[k] < 0 || lay[k] > FX_TAPE ? 0 : lay[k];   /* 1.0.22: its FX layers */
            continue;
        }
        int i = key_index(t);
        if (i >= 0 && saved(i)) e->val[i] = clampf((float)atof(v), PMIN[i], PMAX[i]);
    }
    free(buf);
    struct stat st;
    if (wpath[0] && !stat(wpath, &st)) {
        pthread_mutex_lock(&e->mu);
        if (!e->browsed || strcmp(e->cur_path, wpath)) request_path(e, wpath);
        pthread_mutex_unlock(&e->mu);
    } else if (want[0]) {
        pthread_mutex_lock(&e->mu);
        int k = find_entry(e, want);
        if (k >= 0 && (k != (int)lrintf(e->val[P_SOUND]) || strcmp(e->sname, want))) { e->val[P_SOUND] = (float)k; request_load(e); }
        snprintf(e->sname, sizeof e->sname, "%s", want);
        pthread_mutex_unlock(&e->mu);
    }
    for (int i = 0; i < NP; i++) if (saved(i)) mark_changed(e, i);
    select_pad(e, (int)lrintf(e->val[P_SEL_PAD]));
    e->last_touched = -1;
}

/* an entry of the input log: a param's index, or -1 MPC's transport, -100 - n a CC n */
static void ilog_add(chrono_t *e, int i, float x) {
    unsigned k = e->ilog_n % ILOG_N;
    e->ilog[k].blk = e->blk; e->ilog[k].i = (short)i; e->ilog[k].x = x;
    __atomic_store_n(&e->ilog_n, e->ilog_n + 1, __ATOMIC_RELEASE);
}

static void set_param(void *inst, const char *key, const char *val) {
    chrono_t *e = inst;
    if (!strcmp(key, "transport")) {   /* HAS_TRANSPORT: MPC's sequence plays (1) or stops (0): its STOP stops us too */
        ilog_add(e, -1, (float)atof(val));
        if (atoi(val) == 0) stop_all(e);
        return;
    }
    if (!strcmp(key, "lfo_bpm")) { float b = (float)atof(val); if (b > 20 && b < 400) e->bpm_host = b; return; }
    if (!strcmp(key, "state")) { apply_state(e, val); return; }
    int i = key_index(key);
    if (i < 0) return;
    if (PKIND[i] != K_TEXT && PKIND[i] != K_VIS) ilog_add(e, i, (float)atof(val));   /* the input log */
    set_value(e, i, (float)atof(val));
    if (PKIND[i] != K_TRIG && PKIND[i] != K_TEXT && PKIND[i] != K_ROW && PKIND[i] != K_DRAG) e->last_touched = i;
}

static int fmt_time(char *buf, int len, float s) { return s < 1.0f ? snprintf(buf, len, "%.0f ms", s * 1000) : snprintf(buf, len, "%.2f s", s); }
static int fmt_clock(char *buf, int len, float s) { int m = (int)(s / 60); return snprintf(buf, len, "%d:%06.3f", m, s - 60 * m); }
static void pad_name(int p, char *buf, int len) { snprintf(buf, (size_t)len, "%c%d", "ABCD"[p / 16 & 3], p % 16 + 1); }
static void key_name(int k, char *buf, int len) { if (k < 0) snprintf(buf, (size_t)len, "NO KEY"); else snprintf(buf, (size_t)len, "%s %s", KEYN[k % 12], k < 12 ? "MAJ" : "MIN"); }

static int display(chrono_t *e, int i, char *buf, int len) {
    float x = e->val[i];
    const inst_t *in = e->cur;
    int sp = (int)lrintf(e->val[P_SEL_PAD]);
    switch (i) {
    case P_TRANSIENT: return x < 0.001f ? snprintf(buf, len, "OFF") : snprintf(buf, len, "%.0f ms", transient_s(x) * 1000);
    case P_PITCH: case P_P_KEY: return snprintf(buf, len, "%+d st", (int)lrintf(x));
    case P_ZOOM: { float m = exp2f(x); return snprintf(buf, len, m < 10 ? "%.2fX" : m < 100 ? "%.1fX" : "%.0fX", m); }
    case P_VOLUME: case P_P_LEVEL: return snprintf(buf, len, "%+.1f dB", x);
    case P_SEL_PAD: { char n[8]; pad_name((int)lrintf(x), n, sizeof n); return snprintf(buf, len, "%s", n); }
    case P_P_POS: { if (!in || e->cue[sp] < 0) return snprintf(buf, len, "NO CUE"); return fmt_clock(buf, len, e->cue[sp] / SR); }
    case P_P_NUDGE: return snprintf(buf, len, "%+.1f ms", x * nudge_s(e) * 1000);
    case P_P_FILTER: return fabsf(x) <= 0.02f ? snprintf(buf, len, "OFF") : x < 0 ? snprintf(buf, len, "LP %.0f Hz", lp_hz(x)) : snprintf(buf, len, "HP %.0f Hz", hp_hz(x));
    case P_P_ATTACK: return fmt_time(buf, len, attack_s(x));
    case P_P_RELEASE: return fmt_time(buf, len, release_s(x));
    case P_P_SPEED: return snprintf(buf, len, "%.0f%%", speed_ratio(x) * 100);
    case P_SOUND_BPM: return x < 0.5f ? (in && in->bpm > 0 ? snprintf(buf, len, "AUTO %.2f", in->bpm) : snprintf(buf, len, "AUTO"))
                                      : snprintf(buf, len, "%d BPM", 39 + (int)lrintf(x));
    case P_BPM_FINE: return snprintf(buf, len, "%+.2f BPM", x);
    case P_P_FX_AMT: { int f = e->pad[sp].fx; return f == FX_LSHELF || f == FX_HSHELF ? snprintf(buf, len, "%+.1f dB", (2 * x - 1) * 12) : f == FX_VCURVE ? snprintf(buf, len, "+%.1f dB", 12 * x) : snprintf(buf, len, "%.0f%%", x * 100); }
    case P_P_FX_MIX: return snprintf(buf, len, "%.0f%%", x * 100);
    case P_P_FX_TONE: return fabsf(x) < 0.005f ? snprintf(buf, len, "FLAT") : snprintf(buf, len, "%s %.0f%%", x < 0 ? "DARK" : "BRIGHT", fabsf(x) * 100);
    case P_P_FX_WIDTH: return x < 0.005f ? snprintf(buf, len, "MONO") : snprintf(buf, len, "%.0f%%", x * 200);
    case P_BR_CURSOR: {   /* the highlighted row's name */
        pthread_mutex_lock(&e->mu);
        int n, c = e->br_sel;
        if (e->br_scanning || c < 0 || c >= e->br_n) n = e->cur ? snprintf(buf, len, "%s   (TOUCH A ROW, TURN THE WHEEL)", e->sname) : snprintf(buf, len, "TOUCH A ROW, TURN THE DATA WHEEL; DOUBLE-TAP OR PUSH TO OPEN OR LOAD");
        else n = snprintf(buf, len, "%s%s   (PUSH: %s)", e->br[c].name, e->br[c].dir ? "  /" : "", e->br[c].dir ? "OPEN" : "LOAD");
        pthread_mutex_unlock(&e->mu);
        return n;
    }
    case P_SOUND: {
        pthread_mutex_lock(&e->mu);
        const entry_t *en = selected(e);
        int n = e->browsed ? snprintf(buf, len, "Drive - %s", e->sname) : snprintf(buf, len, "%s", en ? en->name : "LOAD A SAMPLE ON THE BROWSE PAGE");
        pthread_mutex_unlock(&e->mu);
        return n;
    }
    default:
        if (PKIND[i] == K_OPT && OPTS[i]) return snprintf(buf, len, "%s", OPTS[i][opt(e, i)]);
        if (PKIND[i] == K_INT) return snprintf(buf, len, "%d", (int)lrintf(x));
        return snprintf(buf, len, "%.0f%%", x * 100);
    }
}

/* ---- the screen's pictures ---- */
/* where in the sound a voice is, as heard */
static double heard_pos(const voice_t *v) {
    double p = v->pv_on && (v->xf > 0.5f || !v->raw_on) ? v->ppos + GR_N[v->gi] / 2 : v->rpos;
    return v->rev ? v->r0 + v->r1 - 1 - p : p;
}

/* a waveform column over frames [a, b): 0 empty, else 1 + 3 * (height 0..24) + colour (0 bass-heavy red, 1 mid green,
 * 2 bright blue), the height a little compressed (power 0.7) so quiet passages still read without flattening the hits.
 * Read from the coarsest level with at least two peaks in the column; a column under WHOP frames (the deepest zooms) is
 * measured on the sound itself, coloured by the peaks around it. */
static unsigned char wave_frame(const inst_t *in, long a, long b) {
    if (b - a < WHOP) {
        float pk = 0;
        for (long k = a; k < (b > a ? b : a + 1); k++) pk = fmaxf(pk, fabsf(zmid(&in->z, k)));
        long w = a / WHOP < in->nwave[0] ? a / WHOP : in->nwave[0] - 1;
        float lo = in->wave[0][0][w], mid = in->wave[0][1][w], hi = in->wave[0][2][w], br = (mid + 2 * hi) / (lo + mid + hi + 1e-9f);
        int h = (int)lrintf(powf(pk / in->wtop, 0.7f) * 25);
        if (h < 1) return pk > in->wtop * 0.003f ? 1 : 0;
        if (h > 25) h = 25;
        return (unsigned char)(1 + 3 * (h - 1) + (br < 0.55f ? 0 : br < 1.0f ? 1 : 2));
    }
    int l = 0;
    while (l + 1 < WLOD && (WHOP << 2 * (l + 1)) * 2 <= b - a) l++;
    long hop = WHOP << 2 * l, wa = a / hop, wb = (b + hop - 1) / hop;
    if (wb > in->nwave[l]) wb = in->nwave[l];
    float lo = 0, mid = 0, hi = 0;
    const float *L = in->wave[l][0], *M = in->wave[l][1], *H = in->wave[l][2];
    for (long w = wa; w < wb; w++) { lo = fmaxf(lo, L[w]); mid = fmaxf(mid, M[w]); hi = fmaxf(hi, H[w]); }
    float pk = lo + mid + hi;
    int h = (int)lrintf(powf(pk / in->wtop, 0.7f) * 25);
    if (h < 1) return pk > in->wtop * 0.003f ? 1 : 0;
    if (h > 25) h = 25;
    float br = (mid + 2 * hi) / (pk + 1e-9f);
    int col = br < 0.55f ? 0 : br < 1.0f ? 1 : 2;
    return (unsigned char)(1 + 3 * (h - 1) + col);
}

/* the pictures: the pad lamps; the view's waveform; over each column the played part (the newest voice's chop to where it is
 * heard), its playhead in its pad's colour and the chop lines (the bank's in their pads' colours, the other bank's grey);
 * under them each of the bank's chops a numbered tab whose point is on its line */
static void compute_visuals(chrono_t *e, unsigned short *nv) {
    memset(nv, 0, NVIS * sizeof *nv);
    const inst_t *in = e->cur;
    int bank = opt(e, P_BANK), sp = (int)lrintf(e->val[P_SEL_PAD]);
    const voice_t *nw = NULL;
    for (int i = 0; i < NV; i++) if (e->v[i].active && (!nw || e->v[i].age > nw->age)) nw = &e->v[i];
    /* the pads (each its own colour): 0 no cue, 1 its chop, 2 playing */
    for (int s = 0; s < 16; s++) {
        int p = bank * 16 + s, on = 0;
        for (int i = 0; i < NV; i++) on |= e->v[i].active && e->v[i].pad == p && !e->v[i].released && !e->v[i].done_fade;
        nv[P_LP00 + s - P_VIS0] = (unsigned char)(3 * bank + (e->cue[p] < 0 ? 0 : on ? 2 : 1));   /* its own number per bank */
    }
    {   /* the FX page's presets: the selected pad's own lit, the ones layered after it lit softer (none while its FX are off) */
        const pad_t *d = &e->pad[sp];
        for (int s = 0; s < 16; s++) {
            int f = s + 1, lay = 0;
            for (int k = 0; k < FX_LAYERS - 1; k++) lay |= d->fx_lay[k] == f;
            nv[NVIS2 + s] = (unsigned short)(!d->fx ? 0 : d->fx == f ? 1 : lay ? 2 : 0);
        }
    }
    if (!in || !in->nwave[WLOD - 1]) return;
    long v0, vl;
    view_of(e, in, &v0, &vl);
    if (e->wc_in != in || e->wc_v0 != v0 || e->wc_vl != vl) {   /* the waveform only changes with the view */
        for (int c = 0; c < NCOL; c++) e->wcache[c] = wave_frame(in, v0 + vl * c / NCOL, v0 + vl * (c + 1) / NCOL);
        e->wc_in = in; e->wc_v0 = v0; e->wc_vl = vl;
    }
    for (int c = 0; c < NCOL; c++) nv[c] = e->wcache[c];
    unsigned short *ov = nv + (P_OV000 - P_VIS0); unsigned char pl[NCOL] = {0};
#define COL(f) ((int)((double)((f) - v0) * NCOL / vl))
#define IN_VIEW(f) ((f) >= v0 && (f) < v0 + vl)
    double head = nw ? heard_pos(nw) : 0;
    if (nw) {
        double a = nw->rev ? head : nw->start, b = nw->rev ? nw->r0 + nw->r1 - 1 - nw->start : head;
        long fa = (long)fmax(a, (double)v0), fb = (long)fmin(b, (double)(v0 + vl - 1));
        for (int c = fa <= fb ? COL(fa) : 1, c1 = fa <= fb ? COL(fb) : 0; c <= c1; c++) pl[c] = ov[c] = 1;
    }
    for (int p = 0; p < NPADS; p++)   /* the other banks' chops: faded, in their pads' colours */
        if (p / 16 != bank && IN_VIEW(e->cue[p])) { int c = COL(e->cue[p]); ov[c] = (unsigned short)(52 + p % 16); }
    for (int s = 15; s >= 0; s--) {   /* the bank's (two in one column: the lower pad shows) */
        long q = e->cue[bank * 16 + s];
        if (IN_VIEW(q)) { int c = COL(q); ov[c] = (unsigned char)((pl[c] ? 34 : 18) + s); }
    }
    if (nw && IN_VIEW((long)head)) ov[COL((long)head)] = (unsigned char)(2 + nw->pad % 16);
#undef COL
#undef IN_VIEW
    int pad[NMARK], off[NMARK];
    tab_slots(e, in, pad, off);
    for (int k = 0; k < NMARK; k++)
        if (pad[k] >= 0) {
            nv[P_MK00 + k - P_VIS0] = (unsigned short)(1 + ((pad[k] % 16) * COLS_PER_MARK + off[k]) * 2 + (pad[k] == sp));
            nv[NVIS1 + k] = (unsigned short)(1 + pad[k] * COLS_PER_MARK + off[k]);   /* its number, 1..64 through the banks */
        }
    /* the browser's scroll bar: its thumb from the first row to the last page (none while the folder fits) */
    int bn = e->br_n, bt = e->br_top;
    if (bn > NROWS) nv[P_SB - P_VIS0] = (unsigned char)(1 + lrintf((SB_FRAMES - 2) * fminf(1, (float)bt / (bn - NROWS))));
}

static int get_param(void *inst, const char *key, char *buf, int len) {
    chrono_t *e = inst;
    const inst_t *in = e->cur;
    if (!strcmp(key, "display_rev")) {   /* HAS_DISPLAY_REV: the pictures the host should show */
        unsigned short nv[NVIS];
        long v0 = 0, vl = 0;
        if (in) view_of(e, in, &v0, &vl);
        if ((v0 != e->vw0 || vl != e->vwl) && e->blk - e->vw_blk < 30) return snprintf(buf, len, "%u", e->disp_rev);
        if (v0 != e->vw0 || vl != e->vwl) { e->vw0 = v0; e->vwl = vl; e->vw_blk = e->blk; }
        compute_visuals(e, nv);
        if (memcmp(nv, e->vis, sizeof nv)) { memcpy(e->vis, nv, sizeof nv); e->disp_rev++; }
        return snprintf(buf, len, "%u", e->disp_rev);
    }
    if (!strcmp(key, "state")) {
        int n = 0;
        for (int i = 0; i < NP && n < len; i++) if (saved(i)) n += snprintf(buf + n, (size_t)(len - n), "%s=%g;", KEYS[i], e->val[i]);
        if (n < len) n += snprintf(buf + n, (size_t)(len - n), "cues=");
        for (int k = 0; k < NPADS && n < len; k++) n += snprintf(buf + n, (size_t)(len - n), "%ld%s", e->cue[k], k < NPADS - 1 ? "," : ";");
        for (int p = 0; p < NPADS && n < len; p++) {
            const pad_t *d = &e->pad[p];
            if (!memcmp(d, &PAD_DEFAULT, sizeof *d)) continue;
            n += snprintf(buf + n, (size_t)(len - n), "p%d=%g,%g,%g,%g,%g,%d,%d,%d,%d,%d,%d,%d,%d,%g,%d,%g,%g,%g,%g,%g,%d,%d,%d,%d;", p, d->level, d->filter, d->attack,
                          d->release, d->speed, d->reverse, d->key, d->mode, d->choke, d->len, d->drums, d->vocals, d->fx, d->fx_amt, d->fx_rate, d->fx_mix, d->fx_drive, d->fx_tone, d->fx_width, d->fx_res, d->layer, d->fx_lay[0], d->fx_lay[1], d->fx_lay[2]);
        }
        pthread_mutex_lock(&e->mu);
        if (n < len) n += snprintf(buf + n, (size_t)(len - n), "cname=%s;sname=%s;", e->cue_name, e->sname);
        if (n < len && e->browsed) n += snprintf(buf + n, (size_t)(len - n), "spath=%s;", e->cur_path);
        pthread_mutex_unlock(&e->mu);
        return n < len ? n : 0;
    }
    if (!strcmp(key, "disp_info")) {   /* the sound: name, tempo (and where Sync takes it), key (and where Key takes it), drums */
        if (!in) return snprintf(buf, len, "NO SOUND YET:  OPEN BROWSE, PICK A SAMPLE AND LOAD IT");
        const char *nm = strstr(in->name, " - ") ? strstr(in->name, " - ") + 3 : in->name;
        char k[40], tempo[48], dr[24];
        int sk = sound_key(e, in), mk = moved_key(e, in);
        key_name(sk, k, sizeof k);
        if (mk != sk) { char b[16]; key_name(mk, b, sizeof b); snprintf(k + strlen(k), sizeof k - strlen(k), " > %s", b); }
        float r = sync_rate(e, in);
        if (snd_bpm(e, in) <= 0) snprintf(tempo, sizeof tempo, "NO TEMPO");
        else if (opt(e, P_SYNC) && e->bpm_host <= 0) snprintf(tempo, sizeof tempo, "%.2f BPM (NO MPC TEMPO YET)", snd_bpm(e, in));
        else if (r != 1) snprintf(tempo, sizeof tempo, "%.2f > %.2f BPM", snd_bpm(e, in), snd_bpm(e, in) * r);
        else snprintf(tempo, sizeof tempo, "%.2f BPM", snd_bpm(e, in));
        if (__atomic_load_n(&in->drums_ready, __ATOMIC_ACQUIRE)) snprintf(dr, sizeof dr, "DRUMS READY");
        else snprintf(dr, sizeof dr, "DRUMS ...");   /* no running percentage: each change of a polled text makes MPC rebuild its whole parameter screen (NOTES 2026-10-27) */
        return snprintf(buf, len, "%s     %s     %s     %s", nm, tempo, k, dr);
    }
    if (!strcmp(key, "disp_pad")) {   /* the selected pad (or Learn Pads, or a note no pad has) */
        if (e->learn > 0) return snprintf(buf, len, "LEARN PADS:  HIT PAD %d OF 16 IN ORDER  (TAP LEARN PADS AGAIN TO CANCEL)", e->learn);
        if (e->rec == 1) return snprintf(buf, len, "MANUAL CHOP:  HIT A PAD TO START THE SOUND");
        if (e->rec == 2) return snprintf(buf, len, "MANUAL CHOP:  HIT A PAD AT EACH CHOP  (%d PLACED, CHOP TO END)", e->rec_n);
        if (e->chop_msg_frames > 0 && e->chop_msg == 3) return snprintf(buf, len, "PADS BACK TO MPC'S OWN NOTES");
        if (e->chop_msg_frames > 0 && e->chop_msg == 4) return snprintf(buf, len, "LEARN PADS CANCELLED:  PADS AS BEFORE");
        if (e->chop_msg_frames > 0 && e->chop_msg == 5) {
            int n = 0;
            for (int k = 0; k < NPADS; k++) n += e->cue[k] >= 0;
            return snprintf(buf, len, "ONLY %d HIT%s FOUND:  TURN UP SENSITIVITY, OR TRY EQUAL OR BPM", n, n == 1 ? "" : "S");
        }
        if (e->chop_msg_frames > 0 && e->chop_msg == 1) return snprintf(buf, len, "NO TEMPO FOUND:  CHOPPED AT 120 BPM");
        if (e->chop_msg_frames > 0 && e->chop_msg == 2) {
            int n = 0;
            for (int k = 0; k < NPADS; k++) n += e->cue[k] >= 0;
            return snprintf(buf, len, "MANUAL CHOP DONE:  %d CHOPS", n);
        }
        if (e->unmapped_frames > 0 && e->unmapped_note >= 0) return snprintf(buf, len, "NOTE %d IS ON NO PAD:  TAP LEARN PADS (SOUND PAGE)", e->unmapped_note);
        if (e->unmapped_frames > 0) { char pn[8]; pad_name(-1 - e->unmapped_note, pn, sizeof pn); return snprintf(buf, len, "PAD %s HAS NO CHOP:  TAP RANDOM CHOPS, OR ADD ONE", pn); }
        if (e->chop_msg_frames > 0 && e->chop_msg >= 6) {   /* CHOP, ADD, DELETE: what they did */
            int n = 0;
            for (int k = 0; k < NPADS; k++) n += e->cue[k] >= 0;
            char pn[8]; pad_name((int)lrintf(e->val[P_SEL_PAD]), pn, sizeof pn);
            if (e->chop_msg == 6) return snprintf(buf, len, "CHOPPED BY %s:  %d CHOPS%s", OPT_CHOP_MODE[opt(e, P_CHOP_MODE)], n,
                                                  e->chop_same ? "  (THE SAME: MOVE SENSITIVITY OR CUES)" : "");
            if (e->chop_msg == 14) return snprintf(buf, len, "NUDGED PAD %s %+.1f MS  (NOW %.3f S)", pn, e->nudged * 1000 / SR, e->cue[(int)lrintf(e->val[P_SEL_PAD])] / SR);
            if (e->chop_msg == 15) return snprintf(buf, len, "RANDOM CHOPS:  %d CHOPS THROUGH BANKS A-D  (PRESS AGAIN FOR NEW ONES)", n);
            if (e->chop_msg == 7) return snprintf(buf, len, "CHOP ADDED:  NOW PAD %s, %d CHOPS", pn, n);
            if (e->chop_msg == 8) return snprintf(buf, len, "ALL 64 PADS HAVE A CHOP:  DELETE ONE FIRST");
            if (e->chop_msg == 10) return snprintf(buf, len, "UNDONE:  %d MORE STEP%s TO GO BACK", e->undo_n, e->undo_n == 1 ? "" : "S");
            if (e->chop_msg == 11) return snprintf(buf, len, "NOTHING TO UNDO");
            if (e->chop_msg == 12) return snprintf(buf, len, "PAD %s'S FX ON ALL 64 PADS", pn);
            if (e->chop_msg == 13) return snprintf(buf, len, "FX CLEARED ON ALL PADS");
            return snprintf(buf, len, "CHOP DELETED:  %d CHOPS", n);
        }
        if (opt(e, P_BANK) > 0) {   /* bank B, C or D with nothing on it: why */
            int bank = opt(e, P_BANK), nb = 0, n = 0;
            for (int k = 0; k < NPADS; k++) { n += e->cue[k] >= 0; nb += k / 16 == bank && e->cue[k] >= 0; }
            if (!nb && 16 * (1 + opt(e, P_CUE_COUNT)) <= 16 * bank)
                return snprintf(buf, len, "BANK %c IS EMPTY:  SET CUES TO %d FOR CHOPS %d-%d", "ABCD"[bank], 16 * (bank + 1), 16 * bank + 1, 16 * bank + 16);
            if (!nb && opt(e, P_CHOP_MODE) == 0) return snprintf(buf, len, "BANK %c IS EMPTY:  ONLY %d HITS FOUND, TURN UP SENSITIVITY", "ABCD"[bank], n);
            if (!nb) return snprintf(buf, len, "BANK %c IS EMPTY:  ONLY %d CHOPS", "ABCD"[bank], n);
        }
        int p = (int)lrintf(e->val[P_SEL_PAD]);
        char n[8], c[24];
        pad_name(p, n, sizeof n);
        if (e->cue[p] < 0) return snprintf(buf, len, "PAD %s   NO CHOP: TAP CHOP", n);
        fmt_clock(c, sizeof c, e->cue[p] / SR);
        const pad_t *d = &e->pad[p];
        char ks[16] = "";
        if (d->key) snprintf(ks, sizeof ks, "   %+d st", d->key);
        return snprintf(buf, len, "PAD %s   %s   %s%s%s", n, c, OPT_P_MODE[pad_mode(e, p)], ks, d->reverse ? "   REVERSE" : "");
    }
    if (!strcmp(key, "disp_play")) {   /* the sync and key moves, and what plays */
        char s[48], k[48] = "", a[16], b[16];
        float r = sync_rate(e, in);
        if (!opt(e, P_SYNC)) snprintf(s, sizeof s, "SYNC OFF");
        else if (!in || snd_bpm(e, in) <= 0) snprintf(s, sizeof s, "SYNC: NO TEMPO FOUND");
        else if (e->bpm_host <= 0) snprintf(s, sizeof s, "SYNC: WAITING FOR MPC TEMPO");
        else snprintf(s, sizeof s, "SYNC %.2f > %.1f BPM (%.0f%%)", snd_bpm(e, in), snd_bpm(e, in) * r, r * 100);
        int sk = sound_key(e, in), mk = moved_key(e, in);
        if (in && mk != sk) {
            key_name(sk, a, sizeof a); key_name(mk, b, sizeof b);
            snprintf(k, sizeof k, "   KEY %s > %s (%+d)", a, b, key_shift(e, in) + (int)lrintf(e->val[P_PITCH]));
        }
        char pl[24] = "";
        if (e->nactive) { char n[8]; pad_name(e->last_pad, n, sizeof n); snprintf(pl, sizeof pl, "   PLAYING %s", n); }
        return snprintf(buf, len, "%s%s%s", s, k, pl);
    }
    if (!strcmp(key, "status")) {
        pthread_mutex_lock(&e->mu);
        int loading = e->loading || e->req_gen != e->done_gen || e->pending;
        char ln[64]; snprintf(ln, sizeof ln, "%s", e->sname);
        pthread_mutex_unlock(&e->mu);
        if (loading) return snprintf(buf, len, "LOADING  %s", ln);
        if (e->last_touched >= 0) { char d[48]; display(e, e->last_touched, d, sizeof d); return snprintf(buf, len, "%s  %s", NAMES[e->last_touched], d); }
        return snprintf(buf, len, "%s", ln);
    }
    if (!strcmp(key, "disp_key")) {   /* MAIN's KEY: the sound's key, and the key it plays in once moved */
        char a[16], b[16];
        int sk = sound_key(e, in), mk = moved_key(e, in), pt = (int)lrintf(e->val[P_PITCH]);
        if (!in) return snprintf(buf, len, "-");
        if (sk < 0) return pt ? snprintf(buf, len, "NO KEY %+d", pt) : snprintf(buf, len, "NO KEY");
        key_name(sk, a, sizeof a);
        if (mk == sk) return snprintf(buf, len, "%s", a);
        key_name(mk, b, sizeof b);
        return snprintf(buf, len, "%s > %s", a, b);
    }
    if (!strcmp(key, "disp_fx")) {   /* the FX tab: the selected pad and its effect */
        int p = (int)lrintf(e->val[P_SEL_PAD]);
        const pad_t *d = &e->pad[p];
        char pn[8], am[24]; pad_name(p, pn, sizeof pn); display(e, P_P_FX_AMT, am, sizeof am);
        if (!d->fx) return snprintf(buf, len, "PAD %s:  NO FX   (PICK ONE, THEN HIT THE PAD)", pn);
        char ch[96]; int m = snprintf(ch, sizeof ch, "%s", OPT_P_FX[d->fx]);
        for (int k = 0; k < FX_LAYERS - 1; k++) if (d->fx_lay[k] > 0 && m < (int)sizeof ch) m += snprintf(ch + m, sizeof ch - (size_t)m, " + %s", OPT_P_FX[d->fx_lay[k]]);
        if (e->fx_msg && e->blk - e->fx_msg_blk < 1000)   /* ~3 s */
            return snprintf(buf, len, "PAD %s:  %s   %s", pn, ch, e->fx_msg == 17 ? "(LAYER TAKEN OFF)" : e->fx_msg == 18 ? "(ITS OWN PRESET: TAP ANOTHER TO CHANGE IT)"
                            : e->fx_msg == 19 ? "(4 FX ALREADY: DOUBLE-TAP ONE TO TAKE IT OFF)" : "(DOUBLE-TAP A PRESET TO LAYER IT OR TAKE IT OFF)");
        return snprintf(buf, len, "PAD %s:  %s   AMOUNT %s   MIX %.0f%%%s%s", pn, ch, am, d->fx_mix * 100,
                        d->fx == FX_SWEEP || d->fx == FX_RISER || d->fx == FX_WARP ? "   EVERY " : "", d->fx == FX_SWEEP || d->fx == FX_RISER || d->fx == FX_WARP ? OPT_P_FX_RATE[d->fx_rate] : "");
    }
    if (!strcmp(key, "disp_pads")) {   /* which notes play the pads (not the last one heard: a text that changes with each hit makes MPC rebuild its screen) */
        return snprintf(buf, len, "PADS: %s", e->learned ? "LEARNED NOTES  (RESET PADS: MPC'S OWN)" : "MPC NOTES (72.. OR 36..)");
    }
    if (!strncmp(key, "br_", 3) && isdigit((unsigned char)key[3])) {   /* a browser row: its name ("<key>_on": highlighted) */
        int r = atoi(key + 3) - 1, on = strstr(key, "_on") != NULL, n = 0;
        pthread_mutex_lock(&e->mu);
        int idx = e->br_top + r;
        if (r >= 0 && r < NROWS && !e->br_scanning && idx < e->br_n) {
            const struct br_ent *b = &e->br[idx];
            if (on) {   /* the highlighted entry (previewed, or the sound loaded), else the loaded WAV */
                char p[700];
                snprintf(p, sizeof p, "%s/%s", e->br_dir[0] ? e->br_dir : "/media", b->name);
                n = snprintf(buf, len, "%d", e->br_sel >= 0 ? idx == e->br_sel : !b->dir && e->browsed && !strcmp(p, e->cur_path));
            } else if (b->dir) n = snprintf(buf, len, "%s  /", e->br_dir[0] ? b->name : drive_label(b->name));
            else n = snprintf(buf, len, "%s", b->name);
        } else n = snprintf(buf, len, on ? "0" : "");
        pthread_mutex_unlock(&e->mu);
        return n;
    }
    if (!strcmp(key, "disp_path")) {   /* where the browser is, which rows show, or why a WAV did not load */
        pthread_mutex_lock(&e->mu);
        int n;
        if (e->load_fail) n = snprintf(buf, len, "COULD NOT LOAD %s:  WAV FILES UP TO SIX MINUTES ONLY", e->req_name);
        else if (e->br_scanning) n = snprintf(buf, len, "OPENING ...");
        else if (!e->br_dir[0]) n = snprintf(buf, len, "DRIVES:  TAP ONE TO OPEN IT");
        else {
            const char *d = e->br_dir, *rest = d;
            char top[64] = "";
            if (!strcmp(d, BUILTIN_DIR)) { snprintf(top, sizeof top, "USER SOUNDS"); rest = ""; }
            else if (!strncmp(d, "/media/", 7)) {   /* "/media/az01-internal-sd/Samples" -> "INTERNAL/Samples" */
                const char *m = d + 7, *sl = strchr(m, '/');
                char mount[64];
                snprintf(mount, sizeof mount, "%.*s", (int)(sl ? sl - m : (long)strlen(m)), m);
                snprintf(top, sizeof top, "%s", drive_label(mount));
                rest = sl ? sl : "";
            }
            n = snprintf(buf, len, "%s%s", top, rest);
            if (e->br_n == 0) n += snprintf(buf + n, (size_t)(len > n ? len - n : 0), "   (NO FOLDERS OR WAV FILES)");
            else n += snprintf(buf + n, (size_t)(len > n ? len - n : 0), "   (%d-%d OF %d)", e->br_top + 1,
                               e->br_top + NROWS < e->br_n ? e->br_top + NROWS : e->br_n, e->br_n);
            if (e->br_sel >= 0 && e->br_sel < e->br_n && !e->br[e->br_sel].dir && e->br[e->br_sel].li < 0
                && !(e->browsed && strstr(e->cur_path, e->br[e->br_sel].name)))
                n += snprintf(buf + n, (size_t)(len > n ? len - n : 0), "   TAP IT AGAIN OR LOAD TO USE IT");
        }
        pthread_mutex_unlock(&e->mu);
        return n;
    }
    size_t kl = strlen(key);
    if (kl > 8 && !strcmp(key + kl - 8, "_display")) {
        char k[32];
        snprintf(k, sizeof k, "%.*s", (int)(kl - 8), key);
        int i = key_index(k);
        return i >= 0 ? display(e, i, buf, len) : 0;
    }
    if (!strcmp(key, "br_cursor")) return snprintf(buf, len, "%d", e->br_sel < 0 ? 0 : e->br_sel);   /* the data wheel steps from the row lit */
    int i = key_index(key);
    if (i < 0) return 0;
    if (PKIND[i] == K_VIS) return snprintf(buf, len, "%d", e->vis[vis_slot(i)]);   /* as of the last display_rev */
    return snprintf(buf, len, "%g", e->val[i]);
}

int mpc_engine_changed(void *inst, char *key, int len) {   /* one moved parameter per call, lowest index first */
    chrono_t *e = inst;
    for (int w = 0; w < (NP + 63) / 64; w++) {
        uint64_t c = __atomic_load_n(&e->changed[w], __ATOMIC_RELAXED);
        if (!c) continue;
        int b = __builtin_ctzll(c);
        __atomic_and_fetch(&e->changed[w], ~(1ull << b), __ATOMIC_RELAXED);
        snprintf(key, (size_t)len, "%s", KEYS[w * 64 + b]);
        return 1;
    }
    return 0;
}

/* ---- render ---- */
static void render(void *inst, int16_t *out_lr, int frames) {
    chrono_t *e = inst;
    if (!pthread_mutex_trylock(&e->mu)) {   /* pick up a freshly loaded sound, or a preview */
        br_coast(e);
        if (e->pending && !e->retired) {
            for (int i = 0; i < NV; i++) e->v[i].active = 0;
            memset(e->hold_on, 0, sizeof e->hold_on);
            e->rec = 0;
            if (e->aud_z == &e->cur->z) e->aud_z = NULL;   /* the old sound's preview stops before it goes */
            e->retired = e->cur;
            e->cur = e->pending; e->pending = NULL;
            /* its cues: kept when they were saved for this sound, else chopped */
            if (strcmp(e->cue_name, e->cur->name)) {   /* a new sound starts fresh: new chops, pads back to their defaults */
                for (int p = 0; p < NPADS; p++) e->pad[p] = PAD_DEFAULT;
                e->val[P_SOUND_BPM] = 0; e->val[P_BPM_FINE] = 0; mark_changed(e, P_SOUND_BPM); mark_changed(e, P_BPM_FINE);   /* its own tempo */
                e->undo_n = 0;   /* a new sound: nothing to go back to */
                memcpy(e->cue, e->cur->auto_cue, sizeof e->cue);   /* chopped by the worker */
                snprintf(e->cue_name, sizeof e->cue_name, "%s", e->cur->name);
                if (e->cur->auto_msg) { e->chop_msg = e->cur->auto_msg; e->chop_msg_frames = (long)(5 * SR); }
            }
            select_pad(e, (int)lrintf(e->val[P_SEL_PAD]));
            if (e->aud_on_load) { e->aud_on_load = 0; e->aud_z = &e->cur->z; e->aud_pos = 0; e->aud_env = 0; e->aud_stop = 0; }
            pthread_cond_signal(&e->cv);
        }
        if (e->aud_pending && !e->aud_retired) {   /* a preview the browser thread loaded: the one before goes back to it */
            e->aud_retired = e->aud_own;
            e->aud_own = e->aud_pending; e->aud_pending = NULL;
            e->aud_z = e->aud_own; e->aud_pos = 0; e->aud_env = 0; e->aud_stop = 0;
            pthread_cond_signal(&e->bcv);
        }
        pthread_mutex_unlock(&e->mu);
    }
    e->blk++;
    float L[128], R[128];
    memset(L, 0, sizeof L); memset(R, 0, sizeof R);
    int na = 0;
    for (int i = 0; i < NV; i++) if (e->v[i].active) { render_voice(e, &e->v[i], L, R, frames); na += e->v[i].active; }
    e->nactive = na;
    if (e->aud_z) {   /* the preview, as it is: in over 4 ms, out over the last 10 ms or when stopped */
        const zone_t *z = e->aud_z;
        float fin = 1.0f / (0.004f * SR), fout = expf(-1.0f / (0.008f * SR));
        for (int i = 0; i < frames; i++) {
            long k = (long)e->aud_pos;
            if (k >= z->len || (e->aud_stop && e->aud_env < 0.0005f)) { e->aud_z = NULL; break; }
            if (e->aud_stop) e->aud_env *= fout;
            else if (e->aud_env < 1) e->aud_env = fminf(1, e->aud_env + fin);
            float g = fminf(e->aud_env, (z->len - k) / (0.01f * SR));
            float l = zsample(z, k, 0), r = z->nch == 2 ? zsample(z, k, 1) : l;
            L[i] += l * g; R[i] += r * g;
            e->aud_pos += 1;
        }
    }
    if (!e->aud_z) e->aud_stop = 0;
    if (e->rec == 2) {   /* MANUAL: the run is over once the sound ran out; while it runs, a zoomed view pages after the playhead */
        const voice_t *rv = &e->v[e->rec_v];
        if (!rv->active || !rv->recv) stop_rec(e);
        else if (!(e->blk & 15)) keep_in_view(e, (long)heard_pos(rv));
    }
    if (e->chop_msg_frames > 0) e->chop_msg_frames -= frames;
    if (e->learn > 0 && (e->learn_frames -= frames) <= 0) learn_cancel(e);
    if (e->trig_frames > 0) e->trig_frames -= frames;
    if (e->unmapped_frames > 0) e->unmapped_frames -= frames;
    float vol = db2lin(e->val[P_VOLUME]), lim_rel = expf(-1.0f / (0.12f * SR));
    for (int i = 0; i < frames; i++) {
        float l = L[i] * vol, r = R[i] * vol;
        float pk = fmaxf(fabsf(l), fabsf(r));
        e->lim_env = pk > e->lim_env ? pk : lim_rel * e->lim_env + (1 - lim_rel) * pk;
        float tg = e->lim_env > 0.89f ? 0.89f / e->lim_env : 1.0f;
        e->lim_g += (tg < e->lim_g ? 0.5f : 0.0005f) * (tg - e->lim_g);
        l = fast_tanh(l * e->lim_g * 1.05f); r = fast_tanh(r * e->lim_g * 1.05f);
        out_lr[2 * i] = (int16_t)lrintf(clampf(l, -1, 1) * 32767.0f);
        out_lr[2 * i + 1] = (int16_t)lrintf(clampf(r, -1, 1) * 32767.0f);
    }
}

static const mpc_engine_t ENGINE = { create, destroy, midi, set_param, get_param, render, NULL, NULL };
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
