/* Offline checks and audition WAVs for ChronoStretch 1.0 (test/demo.ps1 runs it under ASan/UBSan): the analysis (tempo,
 * key), chopping, the screen and zoom, pads in time and in key, the pad settings, Keyboard, the drum layer, the state, and the time a block
 * takes. Writes build/demo/*.wav. Exit code 0 when every check passes.  build/demo_run [quick] */
#include "../src/chrono.c"
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (c) printf("  ok    "); else { printf("  FAIL  "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void w16(FILE *f, unsigned v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); }
static void w32(FILE *f, unsigned v) { w16(f, v & 0xffff); w16(f, v >> 16); }
static void write_wav(const char *name, const int16_t *s, long frames) {
    char p[256]; snprintf(p, sizeof p, "build/demo/%s.wav", name);
    FILE *f = fopen(p, "wb"); if (!f) return;
    fwrite("RIFF", 1, 4, f); w32(f, 36 + frames * 4); fwrite("WAVEfmt ", 1, 8, f); w32(f, 16); w16(f, 1); w16(f, 2);
    w32(f, 44100); w32(f, 44100 * 4); w16(f, 4); w16(f, 16); fwrite("data", 1, 4, f); w32(f, frames * 4);
    fwrite(s, 4, (size_t)frames, f); fclose(f);
}
/* a set, as a separate gesture: a button pressed again here is a new press, not MPC's second send of the same one */
static void setp(chrono_t *e, const char *k, double v) { char b[32]; snprintf(b, sizeof b, "%g", v); memset(e->trg_blk, 0, sizeof e->trg_blk); set_param(e, k, b); }
static void msg(chrono_t *e, int a, int b, int c) { uint8_t m[3] = { (uint8_t)a, (uint8_t)b, (uint8_t)c }; midi(e, m, 3); }
static void wait_sound(chrono_t *e) {   /* until the selected sound is in and its drums are separated */
    int16_t blk[256];
    for (int i = 0; i < 6000; i++) {
        render(e, blk, 128);
        pthread_mutex_lock(&e->mu);
        int busy = e->loading || e->req_gen != e->done_gen || e->pending || !e->cur || strcmp(e->cur->name, e->sname)
                || !__atomic_load_n(&e->cur->drums_ready, __ATOMIC_ACQUIRE);
        pthread_mutex_unlock(&e->mu);
        if (!busy) return;
        usleep(2000);
    }
    printf("  (sound %s not ready)\n", e->sname);
}
static void pick(chrono_t *e, const char *sound) {
    char n[64]; snprintf(n, sizeof n, "Built-in - %s", sound);
    for (int i = 0; i < e->nlist; i++) if (!strcmp(e->list[i].name, n)) {
        if (!e->cur || strcmp(e->cur->name, n)) { pthread_mutex_lock(&e->mu); e->val[P_SOUND] = (float)i; request_load(e); pthread_mutex_unlock(&e->mu); }
        break;
    }
    wait_sound(e);
}
/* a note: on at block 0, off at `off` (or never), `total` blocks; all sound off after */
static double g_life;   /* how long the last note's voice played, seconds */
static int16_t *play(chrono_t *e, int note, int off, int total) {
    int16_t *out = calloc((size_t)total * 256, sizeof(int16_t));
    g_life = 0;
    for (int b = 0; b < total; b++) {
        if (b == 0) msg(e, 0x90, note, 110);
        if (b == off) msg(e, 0x80, note, 0);
        render(e, out + b * 256, 128);
        for (int i = 0; i < NV; i++) if (e->v[i].active) g_life = (b + 1) * 128 / SR;
    }
    msg(e, 0xB0, 120, 0);
    return out;
}
static double rms(const int16_t *s, long a, long b) {
    double acc = 0; for (long i = a; i < b; i++) { double x = 0.5 * (s[2 * i] + s[2 * i + 1]); acc += x * x; }
    return b > a ? sqrt(acc / (b - a)) / 32768.0 : 0;
}
static double drms(const int16_t *s, long a, long b) {   /* the rms of the slope (first difference) of the mid */
    double acc = 0; for (long i = a + 1; i < b; i++) { double x = 0.5 * (s[2 * i] + s[2 * i + 1]) - 0.5 * (s[2 * i - 2] + s[2 * i - 1]); acc += x * x; }
    return b > a + 1 ? sqrt(acc / (b - a - 1)) / 32768.0 : 0;
}
static long last_loud(const int16_t *s, long n) {   /* the end of the last 10 ms above -45 dB */
    for (long w = n - 441; w > 0; w -= 220) if (rms(s, w, w + 441) > 0.0056) return w + 441;
    return 0;
}
static void wait_browser(chrono_t *e) {   /* until the browser thread has nothing left to do */
    int16_t blk[256];
    for (int i = 0; i < 3000; i++) {
        pthread_mutex_lock(&e->mu);
        int busy = e->br_req || e->br_scanning || e->aud_req || e->fold_step || e->br_busy;
        pthread_mutex_unlock(&e->mu);
        if (!busy) return;
        render(e, blk, 128);
        usleep(1000);
    }
    printf("  (the browser thread is still busy)\n");
}
static double preview_rms(chrono_t *e) {   /* the preview, once it plays: its level over 100 blocks */
    int16_t *out = calloc(100 * 256, sizeof(int16_t));
    for (int i = 0; i < 3000 && !e->aud_z; i++) { render(e, out, 128); usleep(1000); }
    for (int b = 0; b < 100; b++) render(e, out + b * 256, 128);
    double r = rms(out, 0, 100 * 128);
    free(out);
    return r;
}
static double pitch(const int16_t *s, long a, long n) {   /* autocorrelation f0, 60 .. 1000 Hz: the shortest strong period */
    static double c[800]; double best = -1; int bl = 0;
    for (int lag = 44; lag < 735; lag++) {
        double acc = 0, e0 = 0, e1 = 0;
        for (long i = a; i < a + n; i++) { double x = s[2 * i] + s[2 * i + 1], y = s[2 * (i + lag)] + s[2 * (i + lag) + 1]; acc += x * y; e0 += x * x; e1 += y * y; }
        c[lag] = acc / sqrt(e0 * e1 + 1e-9);
        if (c[lag] > best) best = c[lag];
    }
    for (int lag = 45; lag < 734; lag++) if (c[lag] > c[lag - 1] && c[lag] >= c[lag + 1] && c[lag] > 0.5 && c[lag] >= 0.9 * best) { bl = lag; break; }
    if (!bl) return 0;
    double y0 = c[bl - 1], y1 = c[bl], y2 = c[bl + 1], d = y0 - 2 * y1 + y2;
    return 44100.0 / (bl + (fabs(d) > 1e-12 ? 0.5 * (y0 - y2) / d : 0));
}
static double centroid(const int16_t *s, long a) {
    fft_t t; fft_init(&t, 4096);
    float x[4096], X[4098];
    for (int i = 0; i < 4096; i++) x[i] = (s[2 * (a + i)] + s[2 * (a + i) + 1]) * t.hann[i];
    rfft(&t, x, X);
    double num = 0, den = 0;
    for (int k = 1; k <= 2048; k++) { double m = hypot(X[2 * k], X[2 * k + 1]); num += m * k * 44100.0 / 4096; den += m; }
    fft_free(&t);
    return den > 0 ? num / den : 0;
}
/* a sound made here: n frames of fn(t, ch) */
static inst_t *make_inst(const char *name, long n, float (*fn)(long, int)) {
    inst_t *in = calloc(1, sizeof(inst_t));
    snprintf(in->name, sizeof in->name, "%s", name);
    in->builtin = -1; in->z.nch = 2; in->z.len = (int)n; in->z.root = 60; in->z.hi = 127;
    in->z.data = malloc(sizeof(int16_t) * n * 2);
    for (long i = 0; i < n; i++) for (int c = 0; c < 2; c++) in->z.data[2 * i + c] = (int16_t)lrintf(clampf(fn(i, c), -1, 1) * 32767);
    in->sec = n / SR;
    fft_t t; fft_init(&t, 1024); analyse_onsets(in, &t); fft_free(&t);
    analyse_tempo(in); analyse_key(in); analyse_wave(in);
    return in;
}
static void use_inst(chrono_t *e, inst_t *in) {   /* swap a made sound in (the worker idle) */
    pthread_mutex_lock(&e->mu);
    free_inst(e->cur); e->cur = in; snprintf(e->sname, sizeof e->sname, "%s", in->name);
    pthread_mutex_unlock(&e->mu);
    int g = 0; separate_drums(in, &g, 0);
    chop_now(e, in);
    select_pad(e, 0);
}
static float tone(float hz, long i) { return sinf(2 * PI * hz * i / SR); }
static float chord_fn(long i, int c, const int *notes, int k) { float s = 0; for (int j = 0; j < k; j++) s += 0.12f * tone(note_hz(notes[j]), i) + 0.04f * tone(2 * note_hz(notes[j]), i); (void)c; return s; }
static float c_major(long i, int c) { static const int N[6] = { 48, 52, 55, 60, 64, 67 }; return chord_fn(i, c, N, 6); }
static float a_minor(long i, int c) { static const int N[6] = { 45, 52, 57, 60, 64, 69 }; return chord_fn(i, c, N, 6); }
static float keys_part(long i, int c) { static const int N[4] = { 53, 57, 60, 64 }; return chord_fn(i, c, N, 4) * (0.8f + 0.2f * sinf(2 * PI * 0.5f * i / SR)); }   /* a held, swelling chord */
static float drum_part(long i, int c) {   /* 100 BPM kick and hat */
    long beat = (long)(60.0 / 100 * SR); float t = (i % beat) / SR, th = (i % (beat / 2)) / SR;
    unsigned r = (unsigned)i * 2654435761u; float nz = ((r >> 8) & 0xffff) / 32768.0f - 1;
    (void)c;
    return 0.7f * sinf(2 * PI * (50 * t + 90 / 30.0f * (1 - expf(-t * 30)))) * expf(-t * 9) + 0.25f * nz * expf(-th * 60);
}
static float mixed(long i, int c) { return keys_part(i, c) + drum_part(i, c); }
static int16_t *g_brk; static long g_brk_len;   /* Break 90, kept for a long loop of it */
static float long_loop(long i, int c) { return 0.6f * g_brk[(i % g_brk_len) * 2 + c] / 32768.0f + 0.5f * keys_part(i, c); }

int main(int argc, char **argv) {
    int quick = argc > 1 && !strcmp(argv[1], "quick");
    if (system("mkdir -p build/demo")) {}
    printf("ChronoStretch 1.0.7 offline test\n");

    {   /* 1.0: the plugin opens with no sound, ready for one */
        chrono_t *e0 = create(NULL); char buf[128];
        get_param(e0, "disp_info", buf, sizeof buf);
        CHECK(!e0->cur && e0->nlist == 0 && strstr(buf, "LOAD IT"), "a new instance has no sound and says how to load one (%s)", buf);
        destroy(e0);
    }
    setenv("CHRONO_BUILTINS", "1", 1);   /* the tests' own synthesized sounds from here on */
    chrono_t *e = create(NULL);
    wait_sound(e);
    CHECK(e->cur && !strcmp(e->cur->name, "Built-in - Break 90"), "the first sound loads (Break 90), its drums separated");
    {   /* Cues 64 by default (0.8), so banks B, C and D have chops 17 on */
        int nb = 0; for (int i = 16; i < NPADS; i++) nb += e->cue[i] >= 0;
        char buf[128]; setp(e, "bank", 1); get_param(e, "disp_pad", buf, sizeof buf);
        CHECK(opt(e, P_CUE_COUNT) == 3 && nb > 0 && !strstr(buf, "EMPTY"), "Cues 64 by default: bank B has %d chops (%s)", nb, buf);
        setp(e, "bank", 0); setp(e, "cue_count", 0); setp(e, "bank", 1); get_param(e, "disp_pad", buf, sizeof buf);
        CHECK(strstr(buf, "BANK B IS EMPTY:  SET CUES TO 32") != NULL, "Cues 16: bank B says why it is empty (%s)", buf);
        setp(e, "bank", 0); setp(e, "sel_pad", 0);
    }

    /* 1. analysis */
    CHECK(fabsf(e->cur->bpm - 90) < 1.0f, "Break 90's tempo: %.2f BPM", e->cur->bpm);
    g_brk_len = e->cur->z.len; g_brk = malloc(sizeof(int16_t) * g_brk_len * 2); memcpy(g_brk, e->cur->z.data, sizeof(int16_t) * g_brk_len * 2);
    { inst_t *lp = make_inst("Long loop", (long)(60 * SR), long_loop);
      CHECK(fabsf(lp->bpm - 90) < 1.0f, "a minute of the break looped, with keys: %.2f BPM", lp->bpm); free_inst(lp); }
    double period = 60.0 * SR / 90, ph = fmod(e->cur->beat0, period);
    CHECK(ph < 0.03 * SR || period - ph < 0.03 * SR, "beat grid on the kick (phase %.0f ms)", ph / SR * 1000);
    inst_t *ck = make_inst("C major", (long)(4 * SR), c_major), *am = make_inst("A minor", (long)(4 * SR), a_minor);
    CHECK(ck->key == 0, "C major chord: key %s%s", KEYN[ck->key % 12], ck->key >= 12 ? " MIN" : " MAJ");
    CHECK(am->key == 12 + 9, "A minor chord: key %s%s", KEYN[am->key % 12], am->key >= 12 ? " MIN" : " MAJ");
    CHECK(ck->bpm == 0, "a held chord has no tempo (%.1f)", ck->bpm);
    free_inst(ck); free_inst(am);
    pick(e, "Soul Keys");
    char kn[16]; key_name(e->cur->key, kn, sizeof kn);
    CHECK(e->cur->key == 12 || e->cur->key == 3, "Soul Keys (C minor 9): %s", kn);
    pick(e, "Break 90");

    /* 2. chopping: TRANSIENT (on the hits, Sensitivity sets how many), EQUAL, BPM, MANUAL */
    /* Break 90's hits, as synth_builtin makes them: a 16th each where its kick, snare or hat plays, odd 16ths 28 ms late */
    long hits[32]; int nhits = 0;
    { static const char *const K = "x.....x...x.....x.....x..x.x....", *const S = "....x.......x.......x.......x..x",
                        *const H = "x.x.x.x.x.x.x.x.x.x.x.x.x.x.xox.";
      for (int s = 0; s < 32; s++) if (K[s] != '.' || S[s] != '.' || H[s] != '.') hits[nhits++] = (long)((s * 60.0f / 90.0f / 4.0f + (s & 1 ? 0.028f : 0)) * SR); }
    int n = 0, sorted = 1, before_hit = 0, big = 0;
    for (int i = 0; i < NPADS; i++) if (e->cue[i] >= 0) { n++; if (i && e->cue[i] <= e->cue[i - 1]) sorted = 0; }
    for (int i = 0; i < n; i++) {   /* a chop at most 4 ms before its hit, never after it */
        long d = 1 << 30; for (int h = 0; h < nhits; h++) if (labs(hits[h] - e->cue[i]) < labs(d)) d = hits[h] - e->cue[i];
        if (d >= 0 && d <= 0.004 * SR) before_hit++; else printf("        chop %d at %.1f ms is %.1f ms from a hit\n", i + 1, e->cue[i] / SR * 1000, d / SR * 1000);
    }
    CHECK(n == 16 && sorted, "TRANSIENT: %d chops (20 hits, Cues 16), in order", n);
    CHECK(before_hit == n, "%d of %d chops 0 to 4 ms before a hit", before_hit, n);
    setp(e, "sensitivity", 0.1); int nlo = 0; for (int i = 0; i < NPADS; i++) nlo += e->cue[i] >= 0;
    for (int i = 0; i < nlo; i++) { int s = (int)lrint((e->cue[i] / SR) / (60.0 / 90 / 4)); big += s < 32 && ("x...x.x...x.x...x...x.x..x.xx..x"[s] == 'x'); }
    setp(e, "sensitivity", 0.9); int nhi = 0; for (int i = 0; i < NPADS; i++) nhi += e->cue[i] >= 0;
    setp(e, "cue_count", 1); int n32 = 0; for (int i = 0; i < NPADS; i++) n32 += e->cue[i] >= 0;
    setp(e, "cue_count", 0); setp(e, "sensitivity", 0.5);
    CHECK(nlo < nhi && big == nlo, "Sensitivity: %d chops at 10%% (all kicks and snares), %d at 90%%, %d at 90%% with Cues 32", nlo, nhi, n32);
    period = 60.0 * SR / e->cur->bpm;   /* the tempo found, as BPM chops it */
    setp(e, "chop_mode", 1);   /* EQUAL */
    int eq = 1; for (int i = 0; i < 16; i++) eq &= e->cue[i] == e->cur->z.len * i / 16;
    setp(e, "cue_count", 1); n = 0; for (int i = 0; i < NPADS; i++) n += e->cue[i] >= 0;
    CHECK(eq && n == 32, "EQUAL: 16 even slices, 32 when asked (%d)", n);
    setp(e, "cue_count", 0);
    setp(e, "chop_mode", 2);   /* BPM, a chop a beat */
    { int ok = 1; n = 0; for (int i = 0; i < NPADS; i++) n += e->cue[i] >= 0;
      for (int i = 1; i < n; i++) ok &= fabs(e->cue[i] - e->cue[i - 1] - period) < 2;
      double d = fmod(e->cue[0] - e->cur->beat0 + 100 * period, period);
      CHECK(n >= 4 && ok && (d < 2 || period - d < 2), "BPM: %d chops a beat apart, on the beat grid", n);
      setp(e, "chop_div", 3); int nb = 0; for (int i = 0; i < NPADS; i++) nb += e->cue[i] >= 0;
      CHECK(nb >= 2 && fabs(e->cue[1] - e->cue[0] - 4 * period) < 2, "BPM 1 BAR: %d chops a bar apart", nb);
      setp(e, "chop_div", 1); }
    {   /* MANUAL: armed, the first hit plays the sound from the start (chop 1), each hit after drops the next where it is heard */
        int16_t blk[256]; char buf[128];
        setp(e, "chop_mode", 3);
        get_param(e, "disp_pad", buf, sizeof buf);
        CHECK(e->rec == 1 && strstr(buf, "HIT A PAD TO START"), "MANUAL arms: %s", buf);
        msg(e, 0x90, 40, 100);
        for (int i = 0; i < 100; i++) render(e, blk, 128);
        msg(e, 0x90, 45, 100);
        for (int i = 0; i < 100; i++) render(e, blk, 128);
        msg(e, 0x90, 36, 100);
        render(e, blk, 128);
        n = 0; for (int i = 0; i < NPADS; i++) n += e->cue[i] >= 0;
        double t1 = e->cue[1] / SR, t2 = e->cue[2] / SR;
        CHECK(e->rec == 2 && n == 3 && e->cue[0] == 0 && fabs(t1 - 100 * 128 / SR) < 0.06 && fabs(t2 - 200 * 128 / SR) < 0.06,
              "MANUAL: chops at 0, %.3f and %.3f s (hits at %.3f and %.3f s)", t1, t2, 100 * 128 / SR, 200 * 128 / SR);
        setp(e, "find", 1);
        for (int i = 0; i < 200; i++) render(e, blk, 128);
        int on = 0; for (int i = 0; i < NV; i++) on += e->v[i].active;
        get_param(e, "disp_pad", buf, sizeof buf);
        CHECK(e->rec == 0 && on == 0 && e->cue[2] >= 0 && strstr(buf, "3 CHOPS"), "CHOP ends the run, the chops stay: %s", buf);
        msg(e, 0x90, 37, 100); render(e, blk, 128);
        CHECK(e->last_pad == 1, "after it, the pads play the chops");
        msg(e, 0xB0, 120, 0);
    }
    setp(e, "chop_mode", 1);   /* EQUAL from here on: a fixed layout */

    /* 2b. the screen: the waveform, chop lines and tabs on the same columns, the lamps, the playhead, zoom, grab and drag */
    {
        char buf[64]; int16_t blk[256];
        long len = e->cur->z.len;
        get_param(e, "display_rev", buf, sizeof buf);
        int cols = 0, lamps = 0, heights[26] = {0}, tabs = 0, inorder = 1, aligned = 1, last = -1, lines = 0, other = 0;
        for (int c = 0; c < NCOL; c++) { int f = e->vis[c]; cols += f > 0; if (f) heights[(f - 1) / 3 + 1]++; }
        for (int c = 0; c < NCOL; c++) { int f = e->vis[P_OV000 + c - P_VIS0]; lines += f >= 18 && f < 34; other += f != 0 && (f < 18 || f >= 34); }
        for (int k = 0; k < NMARK; k++) {
            int f = e->vis[P_MK00 + k - P_VIS0];
            if (!f) continue;
            int pad = (f - 1) / 2 / COLS_PER_MARK, off = (f - 1) / 2 % COLS_PER_MARK, col = k * COLS_PER_MARK + off;
            tabs++; if (pad < last) inorder = 0; last = pad;
            aligned &= col == (int)((double)e->cue[pad] * NCOL / len) && e->vis[P_OV000 + col - P_VIS0] == 18 + pad;
        }
        for (int s = 0; s < 16; s++) lamps += e->vis[P_LP00 + s - P_VIS0] == 1;
        int nh = 0; for (int h = 1; h <= 25; h++) nh += heights[h] > 0;
        CHECK(cols >= 200 && nh >= 10, "waveform drawn: %d of %d columns, %d different heights", cols, NCOL, nh);
        CHECK(lines == 16 && other == 0, "16 chop lines in their pads' colours, nothing else while nothing plays");
        CHECK(tabs == 16 && inorder && aligned, "16 tabs, pad 1 to 16 left to right, each tab's point on its chop's line");
        CHECK(lamps == 16, "16 pad lamps lit for their chops");
        setp(e, "pad_tap", GRID_SLOT(2));   /* the grid cell of pad A3 */
        CHECK((int)lrintf(e->val[P_SEL_PAD]) == 2, "a tap on the grid selects pad A3");
        get_param(e, "display_rev", buf, sizeof buf);
        { int sel = 0; for (int k = 0; k < NMARK; k++) { int f = e->vis[P_MK00 + k - P_VIS0]; if (f && (f - 1) % 2) sel = (f - 1) / 2 / COLS_PER_MARK + 1; }
          CHECK(sel == 3, "the selected chop's tab is lit (tab %d)", sel); }
        {   /* a drag that starts on tab 10's slot: MPC sends the strip's value (0.5) plus a step each move */
            long c9 = e->cue[9];
            char k[8]; snprintf(k, sizeof k, "td%02d", (int)(e->cue[9] * NCOL / len) / COLS_PER_MARK);
            setp(e, k, 0.52); setp(e, k, 0.54);   /* as MPC sends a drag: its own value on, a step at a time */
            long want = c9 + lround(2 * 0.02 * 0.22 * len);   /* NORMAL: 0.22 of the view a whole strip (about the finger) */
            CHECK((int)lrintf(e->val[P_SEL_PAD]) == 9 && labs(e->cue[9] - want) < 4 && fabsf(e->val[key_index(k)] - 0.54f) < 1e-6f,
                  "dragging tab 10 grabs chop 10 and moves it %.1f ms (2 steps of 2%% at NORMAL), following MPC's own value for the strip",
                  (e->cue[9] - c9) / SR * 1000);
            setp(e, k, 0.52); setp(e, k, 0.50);
            CHECK(labs(e->cue[9] - c9) < 4, "dragged back, it is where it was");
            {   /* 1.0.20: a drag run to the strip's end; let go, the strip is handed back the middle; the next drag (on the
                 * tab where the chop is now) moves on either way MPC sends it (from 0.5, or still from its own end) */
                int16_t bk[256]; long keep[NPADS]; memcpy(keep, e->cue, sizeof keep);
                setp(e, "drag_speed", 0); setp(e, "sel_pad", 9);
                #define TAB_OF(p) ({ long v0_, vl_; view_of(e, e->cur, &v0_, &vl_); snprintf(k, sizeof k, "td%02d", (int)((e->cue[p] - v0_) * NCOL / vl_) / COLS_PER_MARK); key_index(k); })
                int ti = TAB_OF(9);
                for (int s = 0; s < 20; s++) setp(e, k, fmin(1.0, 0.52 + 0.05 * s));
                long c1 = e->cue[9];
                for (int j = 0; j < 60; j++) render(e, bk, 128);
                int rst = e->val[ti] == 0.5f;
                ti = TAB_OF(9);
                setp(e, k, 0.55);   /* MPC took the middle */
                long c2 = e->cue[9];
                for (int j = 0; j < 60; j++) render(e, bk, 128);
                ti = TAB_OF(9);
                setp(e, k, e->sp_x[ti] + 0.05);   /* MPC still on its own value */
                long c3 = e->cue[9];
                CHECK(rst && c1 > c9 && c2 > c1 && c3 > c2, "a drag to the strip's end, let go: the strip is back in the middle and the next drag goes on (%ld %ld %ld)", c1 - c9, c2 - c1, c3 - c2);
                memcpy(e->cue, keep, sizeof keep); setp(e, "drag_speed", 2);
            }
        }
        setp(e, "pad_tap", GRID_SLOT(2));
        msg(e, 0x90, 38, 100);
        for (int i = 0; i < 40; i++) render(e, blk, 128);
        get_param(e, "display_rev", buf, sizeof buf);
        int head = -1, c2 = (int)((double)e->cue[2] * NCOL / len);
        for (int c = 0; c < NCOL; c++) { int f = e->vis[P_OV000 + c - P_VIS0]; if (f >= 2 && f < 18) head = c; }
        CHECK(e->vis[P_LP02 - P_VIS0] == 2 && e->vis[P_OV000 + c2 - P_VIS0] == 34 + 2 && head > c2 && e->vis[P_OV000 + head - P_VIS0] == 2 + 2,
              "playing pad A3 lights its lamp, plays from its line (column %d), its playhead in its colour (column %d)", c2, head);
        msg(e, 0xB0, 120, 0);
        /* zoom: 8X centred on the selected chop; the drag moves it by the finger's share of the view */
        setp(e, "zoom", 3);
        long v0, vl; view_of(e, e->cur, &v0, &vl);
        get_param(e, "display_rev", buf, sizeof buf);
        int cs = (int)((double)(e->cue[2] - v0) * NCOL / vl);
        CHECK(vl == len / 8 && e->cue[2] >= v0 && e->cue[2] < v0 + vl && e->vis[P_OV000 + cs - P_VIS0] == 18 + 2,
              "8X: a view of %.2f s around the selected chop, its line at column %d", vl / SR, cs);
        long before = e->cue[2];
        setp(e, "p_pos", e->val[P_P_POS] + 0.02);
        CHECK(labs(e->cue[2] - before - (long)(0.02 * vl)) < 4, "a drag of 2%% of the waveform moves the chop %.1f ms (2%% of the view)", (e->cue[2] - before) / SR * 1000);
        setp(e, "p_pos", e->val[P_P_POS] - 0.02);
        setp(e, "zoom", 16);
        view_of(e, e->cur, &v0, &vl);
        get_param(e, "display_rev", buf, sizeof buf);
        int nh2 = 0, seen[76] = {0}; for (int c = 0; c < NCOL; c++) if (!seen[e->vis[c]]++) nh2++;
        CHECK(vl == MIN_VIEW && nh2 >= 6, "deepest zoom: %.0f ms across, %d frames a column, %d different columns", vl / SR * 1000, (int)(vl / NCOL), nh2);
        {   /* 1.0.8: zoom in eighths of a step (1.09x), not doubling */
            long a0, al, b0, bl; char zs[32];
            setp(e, "zoom", 3); view_of(e, e->cur, &a0, &al);
            setp(e, "zoom", 3.125); view_of(e, e->cur, &b0, &bl);
            get_param(e, "zoom_display", zs, sizeof zs);
            CHECK(fabs((double)al / bl - 1.0905) < 0.01 && !strcmp(zs, "8.72X"), "ZOOM + is an eighth of a step: the view %.4fx narrower (%s)", (double)al / bl, zs);
        }
        {   /* 1.0.8: RANDOM CHOPS: 64 chops through banks A-D, in time order, apart, a new set each press */
            long c1[NPADS];
            setp(e, "chop_rand", 1);
            int n = 0, ok = 1;
            for (int k = 0; k < NPADS; k++) if (e->cue[k] >= 0) { n++; if (k && (e->cue[k - 1] < 0 || e->cue[k] - e->cue[k - 1] < (long)(0.07 * SR))) ok = 0; }
            memcpy(c1, e->cue, sizeof c1);
            setp(e, "chop_rand", 1);
            CHECK(n >= 48 && ok && memcmp(c1, e->cue, sizeof c1), "RANDOM CHOPS: %d chops through all banks (as many as fit 70 ms apart), in order, 70 ms apart, new ones each press", n);
            setp(e, "undo", 1);
            CHECK(!memcmp(c1, e->cue, sizeof c1), "UNDO takes RANDOM CHOPS back");
            setp(e, "find", 1);
        }
        {   /* 1.0.21: MPC's own STOP: the sequence stopping (HAS_TRANSPORT) stops what plays; an all-notes-off too; both logged */
            int16_t bk4[256];
            setp(e, "play_mode", 1); setp(e, "sel_pad", 0);
            msg(e, 0x90, 72, 100); msg(e, 0x80, 72, 0); for (int j = 0; j < 20; j++) render(e, bk4, 128);
            int on1 = 0; for (int j = 0; j < NV; j++) on1 += e->v[j].active && !e->v[j].done_fade;
            set_param(e, "transport", "1"); set_param(e, "transport", "0");
            for (int j = 0; j < 200; j++) render(e, bk4, 128);
            int on2 = 0; for (int j = 0; j < NV; j++) on2 += e->v[j].active;
            msg(e, 0x90, 72, 100); msg(e, 0x80, 72, 0); for (int j = 0; j < 20; j++) render(e, bk4, 128);
            msg(e, 0xB0, 123, 0); for (int j = 0; j < 400; j++) render(e, bk4, 128);
            int on3 = 0; for (int j = 0; j < NV; j++) on3 += e->v[j].active;
            unsigned n = e->ilog_n; int lt = 0, lc = 0;
            for (unsigned k = n > 8 ? n - 8 : 0; k < n; k++) { lt += e->ilog[k % ILOG_N].i == -1; lc += e->ilog[k % ILOG_N].i == -223; }
            CHECK(on1 > 0 && on2 == 0 && on3 == 0 && lt == 2 && lc == 1, "MPC's STOP (the sequence stopping) stops a CONTINUE chop; an all-notes-off too; both in the input log (%d %d %d, %d %d)", on1, on2, on3, lt, lc);
            setp(e, "play_mode", 0);
        }
        {   /* 1.0.19: MPC sends a button press twice (input log 2026-10-30: key_up 1, 79 blocks, 1): one step, not two */
            float p0 = e->val[P_PITCH];
            int16_t bk3[256];
            memset(e->trg_blk, 0, sizeof e->trg_blk);
            set_param(e, "key_up", "1"); for (int j = 0; j < 79; j++) render(e, bk3, 128); set_param(e, "key_up", "1");
            float p1 = e->val[P_PITCH];
            for (int j = 0; j < 400; j++) render(e, bk3, 128);
            set_param(e, "key_down", "1"); for (int j = 0; j < 30; j++) render(e, bk3, 128); set_param(e, "key_down", "1");
            CHECK(p1 == p0 + 1 && e->val[P_PITCH] == p0, "a button MPC sends twice steps once: KEY + one semitone (%+g), KEY - back (%+g)", p1 - p0, e->val[P_PITCH] - p0);
        }
        {   /* 1.0.19: CUE a Q-Link click at a time: about a waveform column each, from where it is */
            setp(e, "zoom", 0); setp(e, "sel_pad", 1);
            long v0, vl; view_of(e, e->cur, &v0, &vl);
            long c0 = e->cue[1];
            float x = e->val[P_P_POS];
            for (int k = 0; k < 4; k++) { x += 1 / 128.0f; setp(e, "p_pos", x); x = e->val[P_P_POS]; }
            long moved = e->cue[1] - c0, col = vl / NCOL;
            CHECK(moved > 3 * col && moved < 6 * col, "CUE: 4 Q-Link clicks move the chop about 4 columns (%.1f)", (double)moved / col);
            setp(e, "undo", 1);
        }
        {   /* 1.0.18: a double tap on a chop's tab selects it (MPC flips the slot's switch: 1, then 0 the next time) */
            setp(e, "zoom", 0); setp(e, "sel_pad", 0);
            int pad[NMARK], off[NMARK], k1 = -1, k2 = -1;
            tab_slots(e, e->cur, pad, off);
            for (int k = 0; k < NMARK; k++) if (pad[k] > 0) { if (k1 < 0) k1 = k; else if (pad[k] != pad[k1]) { k2 = k; break; } }
            char tk[8];
            snprintf(tk, sizeof tk, "tt%02d", k1); setp(e, tk, 1);
            int s1 = (int)lrintf(e->val[P_SEL_PAD]);
            int16_t bk2[256]; for (int j = 0; j < 50; j++) render(e, bk2, 128);
            snprintf(tk, sizeof tk, "tt%02d", k2); setp(e, tk, 1);
            int s2 = (int)lrintf(e->val[P_SEL_PAD]);
            for (int j = 0; j < 50; j++) render(e, bk2, 128);
            snprintf(tk, sizeof tk, "tt%02d", k1); setp(e, tk, 0);   /* sent as 0: still a tap */
            int s3 = (int)lrintf(e->val[P_SEL_PAD]);
            CHECK(k1 >= 0 && k2 >= 0 && s1 == pad[k1] && s2 == pad[k2] && s3 == pad[k1], "a double tap on a tab selects its chop (slot %d: pad %d, slot %d: pad %d, again as 0: %d)", k1, s1, k2, s2, s3);
        }
        setp(e, "zoom", 0);
        setp(e, "pad_tap", GRID_SLOT(0));
    }

    /* 3. pads in time: Sync to 120 BPM at 90: a 1-bar pad lasts 2.0 s instead of 2.67 */
    setp(e, "sel_pad", 0); setp(e, "p_len", 5); setp(e, "p_pos", 0);
    set_param(e, "lfo_bpm", "90");
    int16_t *a = play(e, 36, 1400, 1400);
    double l90 = g_life;
    write_wav("01 pad A1 at 90", a, 1400 * 128); free(a);
    set_param(e, "lfo_bpm", "120");
    a = play(e, 36, 1400, 1400);
    double l120 = g_life;
    write_wav("02 pad A1 synced to 120", a, 1400 * 128);
    CHECK(fabs(l90 - 2.667) < 0.12, "1 bar at 90 BPM: %.2f s", l90);
    CHECK(fabs(l120 - 2.0) < 0.12, "synced to 120 BPM: %.2f s", l120);
    free(a);
    {   /* 0.7: MAIN's KEY + moves the key a semitone at a time and the chop stays in time */
        for (int k = 0; k < 5; k++) setp(e, "key_up", 1);
        a = play(e, 36, 1400, 1400);
        CHECK((int)lrintf(e->val[P_PITCH]) == 5 && fabs(g_life - l120) < 0.03, "KEY + five times: +5 st, still in time at 120 (%.2f s vs %.2f)", g_life, l120);
        free(a);
        for (int k = 0; k < 5; k++) setp(e, "key_down", 1);
        CHECK(e->val[P_PITCH] == 0, "KEY - back to the sound's own key");
    }
    {   /* 0.7: a Q-Link step on NUDGE (1/128 of its range) moves the chop a column of the view, at most 4 ms */
        long c0 = e->cue[0];
        setp(e, "p_nudge", 2.0 / 128);
        long d = e->cue[0] - c0;
        char nb[160]; get_param(e, "disp_pad", nb, sizeof nb);
        long col = e->cur->z.len / NCOL; if (col > lrintf(0.010f * SR)) col = lrintf(0.010f * SR);
        CHECK(labs(d - col) < 3 && e->val[P_P_NUDGE] == 0 && strstr(nb, "NUDGED"), "NUDGE: one Q-Link step moves the chop a column (%.1f ms) and says so: %s", d / SR * 1000, nb);
        setp(e, "zoom", 16);
        c0 = e->cue[0]; setp(e, "p_nudge", -2.0 / 128); d = c0 - e->cue[0];
        CHECK(labs(d - MIN_VIEW / NCOL) < 3, "zoomed in all the way, a step is one column (%ld frames)", d);
        setp(e, "zoom", 0); setp(e, "p_pos", 0);
    }
    setp(e, "tempo_mult", 0);
    a = play(e, 36, 2000, 2000);
    double lh = g_life;
    free(a);
    CHECK(fabs(lh - 4.0) < 0.15, "HALF time at 120: %.2f s", lh);
    setp(e, "tempo_mult", 1); setp(e, "sync", 0);
    a = play(e, 36, 1400, 1400);
    CHECK(fabs(g_life - 2.667) < 0.12, "Sync OFF plays at the sound's own tempo (%.2f s)", g_life);
    free(a);
    setp(e, "sync", 1);

    /* 4. pads in key: Vocal Aah (C) moved to D: 2 semitones up, length kept (no tempo, no sync) */
    pick(e, "Vocal Aah");
    setp(e, "sel_pad", 0); setp(e, "p_len", 8); setp(e, "p_pos", 0);
    a = play(e, 36, 1000, 1000);
    double f0 = pitch(a, 30000, 2048); long len0 = last_loud(a, 1000 * 128);
    free(a);
    key_name(e->cur->key, kn, sizeof kn);
    setp(e, "key", 3);   /* D */
    a = play(e, 36, 1000, 1000);
    double f1 = pitch(a, 30000, 2048); long len1 = last_loud(a, 1000 * 128);
    write_wav("03 vocal moved to D", a, 1000 * 128); free(a);
    CHECK(f0 > 250 && f0 < 272, "the vocal plays at its pitch (%.1f Hz, key %s)", f0, kn);
    CHECK(fabs(f1 / (f0 * pow(2, 2 / 12.0)) - 1) < 0.02, "key D: %.1f Hz (+2 st)", f1);
    CHECK(labs(len1 - len0) < 0.1 * SR, "same length (%.2f vs %.2f s)", len1 / SR, len0 / SR);
    setp(e, "key", 0); setp(e, "pitch", -5);
    a = play(e, 36, 1000, 1000);
    double f2 = pitch(a, 30000, 2048); free(a);
    CHECK(fabs(f2 / (f0 * pow(2, -5 / 12.0)) - 1) < 0.02, "Pitch -5: %.1f Hz", f2);
    setp(e, "pitch", 0);
    {   /* 0.7: MAIN's KEY readout: the sound's key, and where KEY - and + took it */
        char k0[32], k1[32], want[40], a0[16], b0[16];
        setp(e, "sound_key", 1 + 0);   /* C MAJ */
        get_param(e, "disp_key", k0, sizeof k0);
        setp(e, "key_up", 1); setp(e, "key_up", 1);
        get_param(e, "disp_key", k1, sizeof k1);
        key_name(0, a0, sizeof a0); key_name(2, b0, sizeof b0); snprintf(want, sizeof want, "%s > %s", a0, b0);
        CHECK(!strcmp(k0, "C MAJ") && !strcmp(k1, want), "KEY readout: %s, then after + +: %s", k0, k1);
        setp(e, "pitch", 0); setp(e, "sound_key", 0);
    }
    /* Keyboard: the selected pad across the keys */
    setp(e, "keyboard", 1);
    a = play(e, 67, 1000, 1000);
    double fk = pitch(a, 30000, 2048); free(a);
    CHECK(fabs(fk / (f0 * pow(2, 7 / 12.0)) - 1) < 0.02, "Keyboard G3: %.1f Hz (+7)", fk);
    a = play(e, 60, 200, 600);
    CHECK(rms(a, 300 * 128, 600 * 128) < 0.003, "Keyboard notes let go on note-off");
    free(a);
    setp(e, "keyboard", 0);

    /* 5. pad settings */
    setp(e, "p_reverse", 1);
    a = play(e, 36, 1000, 1000);
    CHECK(rms(a, 0, 4410) < rms(a, 60000, 64410) * 0.5, "Reverse starts from the (quiet) tail");
    free(a);
    setp(e, "p_reverse", 0);
    a = play(e, 36, 1000, 1000); double cb = centroid(a, 30000), rb = rms(a, 20000, 60000); free(a);
    setp(e, "p_filter", -0.8);
    a = play(e, 36, 1000, 1000); double cl = centroid(a, 30000); free(a);
    setp(e, "p_filter", 0); setp(e, "p_level", -12);
    a = play(e, 36, 1000, 1000); double rl = rms(a, 20000, 60000); free(a);
    setp(e, "p_level", 0);
    CHECK(cl < cb * 0.7, "Filter low-pass darkens (%.0f vs %.0f Hz)", cl, cb);
    CHECK(fabs(20 * log10(rl / rb) + 12) < 1.5, "Level -12 dB: %.1f dB", 20 * log10(rl / rb));
    setp(e, "p_mode", 3);   /* GATE */
    a = play(e, 36, 100, 600);
    CHECK(rms(a, 300 * 128, 600 * 128) < 0.003, "GATE stops on note-off");
    free(a);
    setp(e, "p_mode", 4);   /* HOLD: plays on after note-off until hit again */
    { int16_t blk[256]; msg(e, 0x90, 36, 100); for (int i = 0; i < 50; i++) render(e, blk, 128); msg(e, 0x80, 36, 0);
      for (int i = 0; i < 100; i++) render(e, blk, 128);
      int on1 = 0; for (int i = 0; i < NV; i++) on1 += e->v[i].active && !e->v[i].released;
      msg(e, 0x90, 36, 100); for (int i = 0; i < 200; i++) render(e, blk, 128);
      int on2 = 0; for (int i = 0; i < NV; i++) on2 += e->v[i].active;
      CHECK(on1 == 1 && on2 == 0, "HOLD: latched after note-off (%d), a second hit stops it (%d)", on1, on2); }
    setp(e, "p_mode", 0);
    /* choke: pads 1 and 2 in group 1 */
    setp(e, "p_choke", 1); setp(e, "sel_pad", 1); setp(e, "p_choke", 1); setp(e, "sel_pad", 0);
    { int16_t blk[256]; msg(e, 0x90, 36, 100); for (int i = 0; i < 20; i++) render(e, blk, 128); msg(e, 0x90, 37, 100);
      for (int i = 0; i < 20; i++) render(e, blk, 128);
      int p0 = 0; for (int i = 0; i < NV; i++) p0 += e->v[i].active && e->v[i].pad == 0;
      CHECK(p0 == 0, "choke: pad A2 cuts pad A1"); msg(e, 0xB0, 120, 0); }
    CHECK(e->last_pad == 1 && (int)lrintf(e->val[P_SEL_PAD]) == 1, "a played pad becomes the selected one");
    setp(e, "p_choke", 0); setp(e, "sel_pad", 0); setp(e, "p_choke", 0);

    /* 5b. ONE SHOT and CONTINUE, and pads kept in the order of their chops */
    pick(e, "Break 90");
    set_param(e, "lfo_bpm", "90");
    setp(e, "find", 1);
    a = play(e, 36, 1000, 1000);
    double one = g_life; free(a);
    setp(e, "play_mode", 1);   /* CONTINUE */
    a = play(e, 36, 2000, 2000);
    double cont = g_life; free(a);
    long r0, r1; pad_region(e, e->cur, 0, &r0, &r1);
    CHECK(one < (r1 - r0) / SR + 0.1 && cont > 4.5, "ONE SHOT plays its chop (%.2f s), CONTINUE plays on to the end (%.2f s)", one, cont);
    { int16_t blk[256]; msg(e, 0x90, 36, 100); for (int i = 0; i < 100; i++) render(e, blk, 128); msg(e, 0x90, 41, 100);
      for (int i = 0; i < 20; i++) render(e, blk, 128);
      int p0 = 0, p5 = 0; for (int i = 0; i < NV; i++) { p0 += e->v[i].active && e->v[i].pad == 0; p5 += e->v[i].active && e->v[i].pad == 5; }
      CHECK(p0 == 0 && p5 == 1, "CONTINUE: hitting another pad cuts over to it"); msg(e, 0xB0, 120, 0); }
    setp(e, "play_mode", 0);
    { long c3 = e->cue[3], c6 = e->cue[6];
      setp(e, "sel_pad", 3); setp(e, "p_key", 5);   /* mark the pad, then move its chop past pad 6's */
      setp(e, "p_pos", (double)(c6 + 2000) / e->cur->z.len);
      int sorted = 1; for (int i = 1; i < 16; i++) sorted &= e->cue[i] > e->cue[i - 1];
      int sp = (int)lrintf(e->val[P_SEL_PAD]);
      CHECK(sorted && sp == 6 && e->pad[6].key == 5 && e->cue[3] != c3, "a cue moved past others: pads renumber in time order, the selection and the pad's settings follow (now pad %d)", sp + 1);
      setp(e, "p_key", 0); }

    /* 5c. which pad a note plays: the Live II's C-major pad mode (72, 74, 76 ..), chromatic pads, Learn Pads, the bank */
    {
        int16_t blk[256];
        #define PAD_OF(n) (msg(e, 0x90, (n), 100), render(e, blk, 128), e->last_pad)
        int a1 = PAD_OF(72), a2 = PAD_OF(74), a3 = PAD_OF(76), a4 = PAD_OF(77), a8 = PAD_OF(84);
        msg(e, 0xB0, 120, 0);
        CHECK(a1 == 0 && a2 == 1 && a3 == 2 && a4 == 3 && a8 == 7, "scale pads (72, 74, 76, 77 .. 84) play pads 1, 2, 3, 4 .. 8");
        CHECK(PAD_OF(37) == 1, "chromatic pads still work (37 plays pad 2)");
        msg(e, 0xB0, 120, 0);
        setp(e, "cue_count", 1); setp(e, "find", 1); setp(e, "bank", 1);
        int bb1 = PAD_OF(72), bc1 = PAD_OF(36);
        msg(e, 0xB0, 120, 0);
        CHECK(bb1 == 16 && bc1 == 16, "BANK B on screen: pad 1's note (72, or 36) plays B1 (%d %d)", bb1, bc1);
        int ob1 = PAD_OF(100), oa1 = PAD_OF(72), ok_b = opt(e, P_BANK) == 1;
        msg(e, 0xB0, 120, 0);
        CHECK(ob1 == 16 && oa1 == 16 && ok_b, "a note of an MPC's own bank B (100 = B1) plays B1, and after it 72 still plays BANK B's pad 1 (%d %d), BANK stays B", ob1, oa1);
        char pads[96]; get_param(e, "disp_pads", pads, sizeof pads);
        CHECK(e->hit_note == 72 && e->hit_pad == 16 && strstr(pads, "MPC NOTES"), "the last note and its pad are kept (72 > B1); the SOUND page says which notes play: %s", pads);
        setp(e, "cue_count", 3); setp(e, "find", 1); setp(e, "bank", 3);   /* 1.0.11: BANK D: pads play D, and BANK stays D */
        int dd1 = PAD_OF(72), dd2 = PAD_OF(74), ok_d = opt(e, P_BANK) == 3;
        msg(e, 0xB0, 120, 0);
        CHECK(dd1 == 48 && dd2 == 49 && ok_d, "BANK D on screen: pads 1 and 2 play D1 and D2 (%d %d), BANK stays D", dd1, dd2);
        setp(e, "bank", 0); setp(e, "cue_count", 0); setp(e, "find", 1); e->abs_banks = 0;
        e->last_pad = -1; msg(e, 0x90, 70, 100); render(e, blk, 128);
        char buf[128]; get_param(e, "disp_pad", buf, sizeof buf);
        CHECK(e->last_pad == -1 && strstr(buf, "LEARN PADS"), "a note on no pad says so: %s", buf);
        {   /* 1.0.23: MIDI KEYS ALL 64: notes 36..99 play chops 1..64 whatever BANK shows; the other banks' chops faded on the
             * waveform in their pads' colours */
            setp(e, "cue_count", 3); setp(e, "find", 1); setp(e, "bank", 0); setp(e, "keymap", 1);
            int k1 = PAD_OF(36), k17 = PAD_OF(52), k64 = PAD_OF(99), b0 = opt(e, P_BANK);
            msg(e, 0xB0, 120, 0); render(e, blk, 128);
            setp(e, "keymap", 0); setp(e, "zoom", 0); setp(e, "bank", 0); setp(e, "sel_pad", 0);
            char buf0[32]; get_param(e, "display_rev", buf0, sizeof buf0);
            long v0_, vl_; view_of(e, e->cur, &v0_, &vl_);
            int f17 = e->cue[16] >= 0 ? e->vis[P_OV000 + (int)((double)(e->cue[16] - v0_) * NCOL / vl_) - P_VIS0] : -1;
            (void)b0;
            CHECK(k1 == 0 && k17 == 16 && k64 == 63 && f17 == 52, "MIDI KEYS ALL 64: notes 36, 52, 99 play chops 1, 17, 64 whatever the bank (%d %d %d); chop 17 shows faded in pad 1's colour (frame %d)", k1, k17, k64, f17);
            setp(e, "cue_count", 0); setp(e, "find", 1);
        }
        {   /* 1.0.15: LAYER GROUP: pads 1 and 6 in group 1, a hit on either plays both; out of the group, one again */
            setp(e, "sel_pad", 0); setp(e, "p_layer", 1); setp(e, "sel_pad", 5); setp(e, "p_layer", 1); setp(e, "sel_pad", 0);
            msg(e, 0xB0, 120, 0); render(e, blk, 128);
            #define PADS_ON(a, b) ({ int ka = 0, kb_ = 0; for (int j = 0; j < NV; j++) if (e->v[j].active && !e->v[j].done_fade) { ka += e->v[j].pad == (a); kb_ += e->v[j].pad == (b); } ka * 10 + kb_; })
            msg(e, 0x90, 72, 100); render(e, blk, 128);
            int both1 = PADS_ON(0, 5);
            msg(e, 0x80, 72, 0); msg(e, 0xB0, 120, 0); render(e, blk, 128);
            msg(e, 0x90, 81, 100); render(e, blk, 128);   /* pad 6 */
            int both6 = PADS_ON(0, 5);
            msg(e, 0x80, 81, 0); msg(e, 0xB0, 120, 0); render(e, blk, 128);
            char st2[8192]; get_param(e, "state", st2, sizeof st2);
            chrono_t *e4 = create(NULL); wait_sound(e4); set_param(e4, "state", st2);
            int kept = e4->pad[0].layer == 1 && e4->pad[5].layer == 1 && e4->pad[1].layer == 0;
            destroy(e4);
            setp(e, "sel_pad", 5); setp(e, "p_layer", 0); setp(e, "sel_pad", 0); setp(e, "p_layer", 0);
            msg(e, 0x90, 72, 100); render(e, blk, 128);
            int one = PADS_ON(0, 5);
            msg(e, 0x80, 72, 0); msg(e, 0xB0, 120, 0); render(e, blk, 128);
            CHECK(both1 == 11 && both6 == 11 && one == 10 && kept, "LAYER GROUP: pad 1 or pad 6 plays both (%d %d), out of the group one (%d), the project keeps the groups (%d)", both1, both6, one, kept);
        }
        setp(e, "learn_pads", 1);
        get_param(e, "disp_pad", buf, sizeof buf);
        CHECK(strstr(buf, "HIT PAD 1") != NULL, "Learn Pads asks for pad 1");
        for (int k = 0; k < 16; k++) { msg(e, 0x90, 48 + k * 2, 100); msg(e, 0x80, 48 + k * 2, 0); render(e, blk, 128); }
        msg(e, 0xB0, 120, 0);
        int l3 = PAD_OF(52), l16 = PAD_OF(78), old = PAD_OF(72);
        msg(e, 0xB0, 120, 0);
        setp(e, "cue_count", 1); setp(e, "bank", 1); setp(e, "find", 1);
        int b3 = PAD_OF(52);
        msg(e, 0xB0, 120, 0); setp(e, "bank", 0); setp(e, "cue_count", 0); setp(e, "find", 1);
        CHECK(l3 == 2 && l16 == 15 && old != 0, "learned: 52 plays pad 3, 78 pad 16, the old layout is gone (%d %d %d)", l3, l16, old);
        CHECK(b3 == 18, "bank B: the same pad plays chop 19 (%d)", b3 + 1);
        default_padmap(e);   /* the rest of the test plays notes from 36 */
        #undef PAD_OF
    }

    /* 5d. 0.8: four banks numbered 1-64, CHOP says what it did, ADD and DELETE, zero-cross snap, smooth drags, the tempo */
    {
        int16_t blk[256]; char buf[160];
        #define PAD_OF(n) (msg(e, 0x90, (n), 100), render(e, blk, 128), e->last_pad)
        setp(e, "cue_count", 3); setp(e, "find", 1);   /* EQUAL, 64 slices */
        get_param(e, "disp_pad", buf, sizeof buf);
        CHECK(strstr(buf, "CHOPPED BY EQUAL:  64 CHOPS") != NULL, "CHOP says what it did: %s", buf);
        setp(e, "find", 1); get_param(e, "disp_pad", buf, sizeof buf);
        CHECK(strstr(buf, "THE SAME") != NULL, "CHOP again with nothing changed says so: %s", buf);
        setp(e, "bank", 2); int c1 = PAD_OF(72); setp(e, "bank", 3); int d16 = PAD_OF(98);
        msg(e, 0xB0, 120, 0);
        get_param(e, "display_rev", buf, sizeof buf);
        int first = -1, lamp = e->vis[P_LP00 - P_VIS0];
        for (int k = 0; k < NMARK && first < 0; k++) if (e->vis[NVIS1 + k]) first = (e->vis[NVIS1 + k] - 1) / COLS_PER_MARK;
        CHECK(c1 == 32 && d16 == 63 && first == 48 && lamp == 3 * 3 + 1, "bank C pad 1 plays chop 33, bank D pad 16 chop 64; bank D's first tab reads 49 (%d %d %d, lamp %d)", c1 + 1, d16 + 1, first + 1, lamp);
        setp(e, "bank", 0); setp(e, "sel_pad", 0);
        setp(e, "cue_count", 0); setp(e, "find", 1);   /* 16 again */
        long len = e->cur->z.len;
        setp(e, "snap", 1);
        setp(e, "chop_add", 1);
        int sp = (int)lrintf(e->val[P_SEL_PAD]); long f = e->cue[sp], nb = 0; for (int k = 0; k < NPADS; k++) nb += e->cue[k] >= 0;
        int zc = f >= 1 && (zmid(&e->cur->z, f - 1) <= 0) != (zmid(&e->cur->z, f) <= 0);
        int sorted = 1; for (int k = 1; k < nb; k++) sorted &= e->cue[k] > e->cue[k - 1];
        CHECK(nb == 17 && zc && sorted && labs(f - len / 2) < 0.006 * SR, "ADD puts a chop mid-view on a zero crossing, the pads stay in order (pad %d, %ld chops)", sp + 1, nb);
        setp(e, "chop_del", 1);
        nb = 0; for (int k = 0; k < NPADS; k++) nb += e->cue[k] >= 0;
        CHECK(nb == 16 && e->cue[sp] != f, "DELETE takes it away again (%ld chops)", nb);
        setp(e, "snap", 0);
        setp(e, "sel_pad", 4); setp(e, "zoom", 16); setp(e, "drag_speed", 0);
        { long c4 = e->cue[4], v0, vl; char k[8];
          view_of(e, e->cur, &v0, &vl);
          snprintf(k, sizeof k, "td%02d", (int)((e->cue[4] - v0) * NCOL / vl) / COLS_PER_MARK);
          for (int s = 0; s < 100; s++) setp(e, k, fmin(1.0, 0.5 + 0.02 * (s + 1)));   /* on to the end, then pinned there */
          CHECK(e->cue[4] - c4 >= 1 && e->cue[4] - c4 <= 8, "FINEST at the deepest zoom: 100 small drag steps move the chop a few frames (%ld)", e->cue[4] - c4); }
        setp(e, "zoom", 0); setp(e, "drag_speed", 2);
        set_param(e, "lfo_bpm", "90");
        float r0 = sync_rate(e, e->cur);
        setp(e, "bpm_fine", -0.09);
        float r1 = sync_rate(e, e->cur);
        setp(e, "sound_bpm", 90 - 39); setp(e, "bpm_fine", 0);
        float r2 = sync_rate(e, e->cur); char bd[32]; get_param(e, "sound_bpm_display", bd, sizeof bd);
        CHECK(fabsf(r0 - 90 / e->cur->bpm) < 1e-4f && fabsf(r1 - 90 / (e->cur->bpm - 0.09f)) < 1e-4f && fabsf(r2 - 1) < 1e-6f && !strcmp(bd, "90 BPM"),
              "BPM FINE trims the tempo found (%.4f > %.4f), SOUND BPM sets it outright (%s: %.4f)", r0, r1, bd, r2);
        setp(e, "sound_bpm", 0);
        #undef PAD_OF
    }

    /* 5f. 1.0: what a file's name says, HALF SPEED, WARP-SPEED, hits kept clean and in time through a long stretch */
    {
        inst_t *t = calloc(1, sizeof *t); t->bpm = 184.3f; t->key = -1;
        name_hints(t, "User - Soul Loop 92 BPM Am"); int b1 = (int)t->bpm, k1 = t->key;
        memset(t, 0, sizeof *t); t->key = -1; name_hints(t, "_HELLA FIRE_ 70 BPM @Soundealersamples"); int b2 = (int)t->bpm, k2 = t->key;
        memset(t, 0, sizeof *t); t->key = -1; name_hints(t, "Rhodes C# minor 85bpm"); int b3 = (int)t->bpm, k3 = t->key;
        free(t);
        CHECK(b1 == 92 && k1 == 12 + 9 && b2 == 70 && k2 < 0 && b3 == 85 && k3 == 12 + 1, "names: 92 BPM Am (%d %d), 70 BPM (%d %d), C# minor 85bpm (%d %d)", b1, k1, b2, k2, b3, k3);
        setp(e, "sel_pad", 0); setp(e, "p_len", 5); setp(e, "p_pos", 0); setp(e, "sync", 1); setp(e, "play_mode", 0);
        set_param(e, "lfo_bpm", "90");
        int16_t *a = play(e, 36, 2800, 2800); double l1 = g_life; free(a);
        setp(e, "half", 1);
        a = play(e, 36, 2800, 2800); double l2 = g_life; free(a);
        setp(e, "half", 0);
        CHECK(fabs(l2 - 2 * l1) < 0.15, "HALF SPEED: a bar lasts twice as long (%.2f s, %.2f s), in time for double-time patterns", l1, l2);
        pick(e, "Vocal Aah");   /* a voice: its pitch shows the half speed */
        setp(e, "sel_pad", 0); setp(e, "p_pos", 0); setp(e, "p_len", 8);
        setp(e, "p_fx", FX_WARP); setp(e, "p_fx_rate", 4); setp(e, "p_fx_amt", 0.3); setp(e, "p_fx_mix", 1);
        a = play(e, 36, 600, 600); double fw = pitch(a, 30000, 2048), rw = rms(a, 0, 600 * 128);
        write_wav("09 vocal through WARP-SPEED", a, 600 * 128); free(a);
        setp(e, "p_fx", 0);
        a = play(e, 36, 600, 600); double fd = pitch(a, 30000, 2048), rd = rms(a, 0, 600 * 128); free(a);
        CHECK(rw > 0.3 * rd && fabs(fw / fd - 0.5) < 0.03, "WARP-SPEED plays it at half speed, an octave down (%.1f Hz from %.1f; rms %.3f vs %.3f)", fw, fd, rw, rd);
        pick(e, "Break 90");
        setp(e, "sel_pad", 0); setp(e, "p_pos", 0); setp(e, "p_len", 8);
        /* CONTINUE through the whole break at 120: every kick and snare lands where 120 BPM puts it */
        setp(e, "play_mode", 1); set_param(e, "lfo_bpm", "120");
        a = play(e, 36, 1500, 1500);
        write_wav("10 break continued at 120", a, 1500 * 128);
        double rate = 120.0 / e->cur->bpm; int on = 0, tot = 0;
        static const char *const KS = "x...x.x...x.x...x...x.x..x.xx..x";   /* kicks and snares */
        for (int st = 1; st < 32; st++) if (KS[st] == 'x') {
            long src = (long)((st * 60.0 / 90 / 4 + (st & 1 ? 0.028 : 0)) * SR), out = (long)(src / rate);
            if (out + 441 >= 1500 * 128) continue;
            tot++;
            double ra = drms(a, out - 40, out + 400), rb = drms(a, out - 700, out - 260);   /* the slope: the hit's click, not a tail before it */
            on += ra > 2.5 * rb;
            if (ra <= 2.5 * rb) printf("        hit at step %d (out %.3f s): %.4f after, %.4f before\n", st, out / SR, ra, rb);
        }
        free(a);
        setp(e, "play_mode", 0);
        CHECK(on >= tot - 1, "a long stretch at 120: %d of %d kicks and snares sharp and on time", on, tot);
    }

    /* 5e. 0.9: UNDO (ten steps), RESET on PITCH & SPEED, the pads' FX */
    {
        int16_t blk[256];
        setp(e, "sel_pad", 2);
        long c2 = e->cue[2]; int n0 = 0; for (int k = 0; k < NPADS; k++) n0 += e->cue[k] >= 0;
        setp(e, "p_pos", e->val[P_P_POS] + 0.05);
        for (int b = 0; b < 150; b++) render(e, blk, 128);   /* a pause: the next edit is a step of its own */
        int sp2 = (int)lrintf(e->val[P_SEL_PAD]);
        setp(e, "chop_del", 1);
        int n1 = 0; for (int k = 0; k < NPADS; k++) n1 += e->cue[k] >= 0;
        setp(e, "undo", 1);
        int n2 = 0; for (int k = 0; k < NPADS; k++) n2 += e->cue[k] >= 0;
        setp(e, "undo", 1);
        char buf[160]; get_param(e, "disp_pad", buf, sizeof buf);
        CHECK(n1 == n0 - 1 && n2 == n0 && e->cue[2] == c2 && sp2 == 2, "UNDO takes back DELETE, then the move (%d > %d > %d chops; %s)", n0, n1, n2, buf);
        for (int k = 0; k < 12; k++) setp(e, "undo", 1);
        get_param(e, "disp_pad", buf, sizeof buf);
        CHECK(strstr(buf, "NOTHING TO UNDO") != NULL, "past the last step: %s", buf);
        setp(e, "sel_pad", 0); setp(e, "p_key", 5); setp(e, "p_speed", 0.5); setp(e, "p_reverse", 1); setp(e, "p_reset", 1);
        CHECK(e->pad[0].key == 0 && e->pad[0].speed == 0 && !e->pad[0].reverse && e->val[P_P_KEY] == 0, "RESET: the pad's pitch, speed and reverse back to plain");
        setp(e, "p_len", 8); setp(e, "sync", 0);
        int16_t *a0 = play(e, 36, 300, 300); double cb = centroid(a0, 0), rb = rms(a0, 0, 300 * 128); /* a0 kept: each preset against it */
        int bad = 0; double clp = 0, chp = 0;
        for (int f = 1; f < 17; f++) {
            setp(e, "p_fx", f);
            int16_t *a = play(e, 36, 300, 300);
            double r = rms(a, 0, 300 * 128), c = centroid(a, 0);
            int nan = 0; for (long k = 0; k < 300 * 128 * 2; k++) nan |= a[k] == -32768;
            if (r < 0.05 * rb || r > 3 * rb || nan) { bad++; printf("        FX %s: rms %.3f (dry %.3f)\n", OPT_P_FX[f], r, rb); }
            if (f == FX_LP) clp = c;
            if (f == FX_HP) chp = c;
            if (f == FX_PHONE) write_wav("08 break through TELEPHONE", a, 300 * 128);
            {   /* how much it changes the sound: the wave itself, and its slope (the highs: a hat, a snare's crack) */
                double dd = 0, dr = 1e-9, sd = 0, sr = 1e-9;
                for (long k = 2; k < 300 * 128 * 2; k++) {
                    double q = (double)a[k] - a0[k], qs = q - ((double)a[k - 2] - a0[k - 2]), s0 = (double)a0[k] - a0[k - 2];
                    dd += q * q; dr += (double)a0[k] * a0[k]; sd += qs * qs; sr += s0 * s0;
                }
                double ch = fmax(sqrt(dd / dr), sqrt(sd / sr));
                if (ch < 0.15) { bad++; printf("        FX %s barely changes the sound (%.2f)\n", OPT_P_FX[f], ch); }
            }
            free(a);
        }
        CHECK(!bad && clp < 0.7 * cb && chp > 1.3 * cb, "16 FX presets play cleanly and each changes the sound; LOW PASS darkens (%.0f Hz), HIGH PASS brightens (%.0f vs %.0f)", clp, chp, cb);
        free(a0);
        setp(e, "p_fx", FX_PHONE); setp(e, "p_fx_amt", 0.3); setp(e, "fx_copy", 1);
        int all = 1; for (int k = 0; k < NPADS; k++) all &= e->pad[k].fx == FX_PHONE && fabsf(e->pad[k].fx_amt - 0.3f) < 1e-6f;
        static char st2[16384]; get_param(e, "state", st2, sizeof st2);
        chrono_t *e4 = create(NULL); wait_sound(e4); set_param(e4, "state", st2); wait_sound(e4);
        CHECK(all && e4->pad[5].fx == FX_PHONE && fabsf(e4->pad[5].fx_amt - 0.3f) < 1e-4f, "COPY TO ALL PADS, and the FX come back with the project");
        destroy(e4);
        setp(e, "fx_clear", 1); setp(e, "sync", 1);
        int none = 1; for (int k = 0; k < NPADS; k++) none &= e->pad[k].fx == 0;
        CHECK(none, "CLEAR ALL FX");
        {   /* 1.0.4: the preset list without OFF, and its own switch */
            int sp0 = (int)lrintf(e->val[P_SEL_PAD]);
            setp(e, "p_fx_pick", FX_TAPE - 1); int a1 = e->pad[sp0].fx;
            setp(e, "p_fx_on", 0); int a2 = e->pad[sp0].fx, k2 = e->pad[sp0].fx_keep;
            setp(e, "p_fx_on", 1); int a3 = e->pad[sp0].fx;
            CHECK(a1 == FX_TAPE && e->val[P_P_FX_ON] == 1 && a2 == 0 && k2 == FX_TAPE && a3 == FX_TAPE,
                  "picking TAPE turns the FX on; the switch turns them off and back on to TAPE (%d %d %d)", a1, a2, a3);
            setp(e, "p_fx_on", 0);
        }
        {   /* 1.0.22: FX layers: a double tap on a preset (its two taps pick it, then its layer switch) layers it after the
             * pad's preset, which stays; the chain plays both; a second double tap takes it off; the project keeps them */
            setp(e, "sel_pad", 0); setp(e, "p_fx_pick", FX_TAPE - 1); setp(e, "p_fx_on", 1); setp(e, "p_fx_amt", 0.8);
            int16_t *w1 = play(e, 36, 300, 300);
            setp(e, "p_fx_pick", FX_LOFI - 1); setp(e, "p_fx_pick", FX_LOFI - 1); setp(e, "fl00", 1);   /* a double tap on LOFI */
            int m1 = e->pad[0].fx, l1 = e->pad[0].fx_lay[0];
            {   /* 1.0.24: both lit on the FX page: TAPE (its own) and LOFI (layered), the rest off */
                char rv[16]; get_param(e, "display_rev", rv, sizeof rv);
                int others = 0; for (int k = 0; k < 16; k++) if (k != FX_TAPE - 1 && k != FX_LOFI - 1) others += e->vis[NVIS2 + k];
                CHECK(e->vis[NVIS2 + FX_TAPE - 1] == 1 && e->vis[NVIS2 + FX_LOFI - 1] == 2 && others == 0,
                      "the FX page lights the pad's own preset and its layers (TAPE %d, LOFI %d, others %d)", e->vis[NVIS2 + FX_TAPE - 1], e->vis[NVIS2 + FX_LOFI - 1], others);
            }
            msg(e, 0x90, 36, 100); { int16_t bk[256]; render(e, bk, 128); }
            int nu = 0; for (int j = 0; j < NV; j++) if (e->v[j].active && e->v[j].pad == 0 && !e->v[j].done_fade) nu = e->v[j].nu;
            msg(e, 0xB0, 120, 0);
            int16_t *w2 = play(e, 36, 300, 300);
            double diff = 0, ref = 0;
            for (long j = 0; j < 300 * 256; j++) { double d = w2[j] - w1[j]; diff += d * d; ref += (double)w1[j] * w1[j]; }
            diff = sqrt(diff / (300 * 256)); ref = sqrt(ref / (300 * 256));
            char fxl[200]; get_param(e, "disp_fx", fxl, sizeof fxl);
            static char st3[16384]; get_param(e, "state", st3, sizeof st3);
            chrono_t *e5 = create(NULL); wait_sound(e5); set_param(e5, "state", st3); wait_sound(e5);
            int kept = e5->pad[0].fx == FX_TAPE && e5->pad[0].fx_lay[0] == FX_LOFI;
            destroy(e5);
            int16_t bk2[256]; for (int j = 0; j < 300; j++) render(e, bk2, 128);
            setp(e, "p_fx_pick", FX_LOFI - 1); setp(e, "p_fx_pick", FX_LOFI - 1); setp(e, "fl00", 0);   /* again (MPC's switch flipped back) */
            int m2 = e->pad[0].fx, l2 = e->pad[0].fx_lay[0];
            CHECK(m1 == FX_TAPE && l1 == FX_LOFI && nu == 2 && diff > 0.1 * ref && strstr(fxl, "TAPE + LOFI") && kept && m2 == FX_TAPE && l2 == 0,
                  "FX layers: a double tap on LOFI layers it after TAPE (%d %d, %d running, %.0f%% different: %s); the project keeps it; again takes it off (%d %d)",
                  m1, l1, nu, 100 * diff / ref, fxl, m2, l2);
            free(w1); free(w2);
            for (int j = 0; j < 600; j++) render(e, bk2, 128);
            setp(e, "p_fx_pick", FX_LOFI - 1); setp(e, "p_fx_pick", FX_LOFI - 1); setp(e, "fl00", 1);   /* LOFI layered again */
            for (int j = 0; j < 600; j++) render(e, bk2, 128);
            setp(e, "p_fx_pick", FX_LOFI - 1);   /* a tap on it */
            int m3 = e->pad[0].fx, l3 = e->pad[0].fx_lay[0];
            CHECK(m3 == FX_LOFI && l3 == FX_TAPE, "a tap on a layered preset swaps it with the pad's own: nothing drops out (%d %d)", m3, l3);
            setp(e, "p_fx_on", 0); setp(e, "p_fx_amt", 0.6); memset(e->pad[0].fx_lay, 0, sizeof e->pad[0].fx_lay);
        }
        /* 1.0.2: DRIVE, TONE, WIDTH, and FX changed while a pad plays */
        setp(e, "p_fx", 0); setp(e, "p_len", 8); setp(e, "sync", 0);
        int16_t *d0 = play(e, 36, 300, 300); double t0 = centroid(d0, 0); free(d0);
        setp(e, "p_fx_tone", 0.8);
        int16_t *d1 = play(e, 36, 300, 300); double t1 = centroid(d1, 0); free(d1);
        setp(e, "p_fx_tone", -0.8);
        int16_t *d2 = play(e, 36, 300, 300); double t2 = centroid(d2, 0); free(d2);
        setp(e, "p_fx_tone", 0); setp(e, "p_fx_width", 0);
        int16_t *d3 = play(e, 36, 300, 300); double side = 0; for (long k = 0; k < 300 * 128; k++) side += fabs((double)d3[2 * k] - d3[2 * k + 1]); free(d3);
        setp(e, "p_fx_width", 0.5);
        CHECK(t1 > 1.15 * t0 && t2 < 0.85 * t0 && side < 1, "TONE brightens (%.0f) and darkens (%.0f Hz from %.0f); WIDTH 0 is mono", t1, t2, t0);
        {   /* a preset picked while the pad plays is heard at once */
            int16_t blk2[256], *o = calloc(600 * 256, sizeof(int16_t));
            msg(e, 0x90, 36, 100);
            for (int b = 0; b < 600; b++) { if (b == 300) { setp(e, "p_fx", FX_LP); setp(e, "p_fx_amt", 0.9); } render(e, o + b * 256, 128); }
            msg(e, 0xB0, 120, 0); (void)blk2;
            double before = centroid(o, 100 * 128), after = centroid(o, 450 * 128); free(o);
            CHECK(after < 0.5 * before, "an FX picked while the pad plays takes at once (centroid %.0f > %.0f Hz)", before, after);
            setp(e, "p_fx", 0); setp(e, "p_fx_amt", 0.6);
        }
        setp(e, "sync", 1);
    }

    /* 6. the drum layer, on a sound of keys and drums */
    inst_t *mx = make_inst("Mix", (long)(6 * SR), mixed);
    use_inst(e, mx);
    CHECK(mx->drums_ready, "drums separated (%d cues)", e->cue[15] >= 0 ? 16 : 0);
    setp(e, "sel_pad", 0); setp(e, "p_pos", 0); setp(e, "p_len", 8); setp(e, "sync", 0);
    long N6 = (long)(5 * SR);
    /* how much of each part an output holds: its projection on the part (1 = all of it, as in FULL) */
    double sfk = 0, sfd = 0, snk = 0, snd = 0, sok = 0, sod = 0, ek = 0, ed = 0;
    int16_t *full = play(e, 36, 2000, 2000);
    setp(e, "drums", 1); int16_t *nod = play(e, 36, 2000, 2000);
    setp(e, "drums", 2); int16_t *only = play(e, 36, 2000, 2000);
    setp(e, "drums", 0);
    for (long i = 2205; i < N6; i++) {
        double k = keys_part(i, 0) * 32767, d = drum_part(i, 0) * 32767;
        double f = 0.5 * (full[2 * i] + full[2 * i + 1]), x = 0.5 * (nod[2 * i] + nod[2 * i + 1]), y = 0.5 * (only[2 * i] + only[2 * i + 1]);
        sfk += f * k; sfd += f * d; snk += x * k; snd += x * d; sok += y * k; sod += y * d; ek += k * k; ed += d * d;
    }
    double n_keys = snk / sfk, n_drums = snd / sfd, o_keys = sok / sfk, o_drums = sod / sfd;
    write_wav("04 mix full", full, 2000 * 128); write_wav("05 mix no drums", nod, 2000 * 128); write_wav("06 mix drums only", only, 2000 * 128);
    free(full); free(nod); free(only);
    CHECK(n_drums / n_keys < 0.316, "NO DRUMS keeps %.0f%% of the keys and %.0f%% of the drums (drums %.1f dB under)", n_keys * 100, n_drums * 100, 20 * log10(n_keys / n_drums));
    CHECK(o_keys / o_drums < 0.316, "DRUMS ONLY keeps %.0f%% of the drums and %.0f%% of the keys (keys %.1f dB under)", o_drums * 100, o_keys * 100, 20 * log10(o_drums / o_keys));

    /* 7. state round trip */
    setp(e, "sel_pad", 5); setp(e, "p_key", 3); setp(e, "p_mode", 2); setp(e, "p_reverse", 1); setp(e, "key", 7); setp(e, "p_pos", 0.5);
    pick(e, "Break 90");
    setp(e, "sel_pad", 5); setp(e, "p_key", 3); setp(e, "p_mode", 2); setp(e, "p_reverse", 1); setp(e, "key", 7); setp(e, "p_pos", 0.5);
    static char st[16384]; get_param(e, "state", st, sizeof st);
    int sp5 = (int)lrintf(e->val[P_SEL_PAD]); long cue5 = e->cue[sp5];
    chrono_t *e2 = create(NULL); wait_sound(e2);
    set_param(e2, "state", st); wait_sound(e2);
    { int16_t blk[256]; for (int i = 0; i < 10; i++) render(e2, blk, 128); }
    CHECK(e2->cue[sp5] == cue5 && e2->pad[sp5].key == 3 && e2->pad[sp5].mode == 2 && e2->pad[sp5].reverse && opt(e2, P_KEY) == 7
          && (int)lrintf(e2->val[P_SEL_PAD]) == sp5 && fabsf(e2->val[P_P_KEY] - 3) < 0.01f, "state round trip (cues, pads, key, selection)");
    destroy(e2);
    char buf[256];
    get_param(e, "disp_info", buf, sizeof buf); printf("        disp_info: %s\n", buf);
    get_param(e, "disp_pad", buf, sizeof buf); printf("        disp_pad:  %s\n", buf);
    get_param(e, "disp_play", buf, sizeof buf); printf("        disp_play: %s\n", buf);

    /* 7b. 0.6: no key for a drum loop, SOUND KEY, STOP, LEARN PADS cancelled, RESET PADS, the drive browser */
    {
        int16_t blk[256];
        CHECK(e->cur->key < 0, "Break 90 (drums only) has no key (fit %.2f)", e->cur->key_conf);
        setp(e, "sound_key", 3); setp(e, "key", 5);   /* the sound is D major; to E */
        CHECK(key_shift(e, e->cur) == 2, "SOUND KEY D MAJ, KEY E: up 2 (%d)", key_shift(e, e->cur));
        setp(e, "sound_key", 0); setp(e, "key", 0);
        setp(e, "play_mode", 1); setp(e, "sel_pad", 0); setp(e, "p_mode", 0);
        msg(e, 0x90, 36, 100); for (int i = 0; i < 50; i++) render(e, blk, 128);
        setp(e, "stop", 1); for (int i = 0; i < 40; i++) render(e, blk, 128);
        int on = 0; for (int i = 0; i < NV; i++) on += e->v[i].active;
        CHECK(on == 0, "STOP ends a CONTINUE");
        setp(e, "play_mode", 0);
        setp(e, "learn_pads", 1); msg(e, 0x90, 74, 100); render(e, blk, 128); setp(e, "learn_pads", 1);
        CHECK(e->learn == 0 && pad_for_note(e, 72) == 0 && pad_for_note(e, 74) == 1, "LEARN PADS tapped again cancels it: the pads as before");
        setp(e, "learn_pads", 1); msg(e, 0x90, 74, 100); render(e, blk, 128);
        for (long f = 0; f < 21 * SR; f += 128) render(e, blk, 128);
        CHECK(e->learn == 0 && pad_for_note(e, 72) == 0, "LEARN PADS gives up after 20 s with no pad");
        msg(e, 0xB0, 120, 0);
        e->padmap[72] = 40;
        setp(e, "reset_pads", 1);
        CHECK(pad_for_note(e, 72) == 0, "RESET PADS: the built-in notes again");
        /* the browser on a folder of its own: a sub-folder first, then the WAVs; a tap plays a preview, a second loads */
        char cwd[400], dir[512], p[600];
        if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
        snprintf(dir, sizeof dir, "%s/build/demo/browse", cwd);
        snprintf(p, sizeof p, "rm -rf '%s'; mkdir -p '%s/Sub folder' '%s/Many'", dir, dir, dir); if (system(p)) {}
        int16_t *w = play(e, 36, 300, 300);
        write_wav("browse/Loop A", w, 300 * 128); free(w);
        w = play(e, 38, 300, 300);
        write_wav("browse/Loop B", w, 300 * 128);
        for (int k = 0; k < 25; k++) { char nm[64]; snprintf(nm, sizeof nm, "browse/Many/Hit %02d", k); write_wav(nm, w, 20 * 128); }
        free(w);
        pthread_mutex_lock(&e->mu); snprintf(e->br_dir, sizeof e->br_dir, "%s", dir); e->br_req = 1; pthread_cond_signal(&e->bcv); pthread_mutex_unlock(&e->mu);
        wait_browser(e);
        char r1[96], r2[96], r3[96], pathline[200];
        get_param(e, "br_1", r1, sizeof r1); get_param(e, "br_2", r2, sizeof r2); get_param(e, "br_3", r3, sizeof r3);
        get_param(e, "disp_path", pathline, sizeof pathline);
        CHECK(!strcmp(r1, "Many  /") && !strcmp(r2, "Sub folder  /") && !strcmp(r3, "Loop A.wav"), "the browser lists the folders first, then the WAVs (%s | %s | %s | %s)", r1, r2, r3, pathline);
        set_param(e, "br_3", "1");
        double pv = preview_rms(e);
        char on3[8]; get_param(e, "br_3_on", on3, sizeof on3); get_param(e, "disp_path", pathline, sizeof pathline);
        CHECK(pv > 0.01 && !strcmp(e->cur->name, "Built-in - Break 90") && atoi(on3) == 1 && strstr(pathline, "TAP IT AGAIN"),
              "a tap on a WAV plays its preview (%.3f rms), lights its row, the sound stays: %s", pv, pathline);
        setp(e, "stop", 1); for (int i = 0; i < 40; i++) render(e, blk, 128);
        CHECK(!e->aud_z, "STOP ends the preview");
        set_param(e, "br_3", "1");
        wait_sound(e);
        get_param(e, "state", st, sizeof st);
        CHECK(e->cur && !strcmp(e->cur->name, "Loop A") && e->browsed && strstr(st, "spath=") != NULL,
              "a second tap loads it, and the state keeps its path");
        /* the sound arrows on a browsed sound: the next WAV of its folder, played from its start; round the ends */
        setp(e, "sound", e->val[P_SOUND] + 1);
        wait_browser(e); wait_sound(e);
        pv = preview_rms(e);
        char on4[8]; get_param(e, "br_4_on", on4, sizeof on4);
        CHECK(!strcmp(e->cur->name, "Loop B") && pv > 0.005 && e->aud_z == &e->cur->z && atoi(on4) == 1,
              "the sound arrow steps to the folder's next WAV (%s), plays it (%.3f rms), its row lit", e->cur->name, pv);
        setp(e, "sound", e->val[P_SOUND] + 1); wait_browser(e); wait_sound(e);
        char after_end[64]; snprintf(after_end, sizeof after_end, "%s", e->cur->name);
        setp(e, "sound", e->val[P_SOUND] - 1); wait_browser(e); wait_sound(e);
        CHECK(!strcmp(after_end, "Loop A") && !strcmp(e->cur->name, "Loop B") && (int)lrintf(e->val[P_SOUND]) == 80,
              "past the last WAV it goes round to the first (%s), and back (%s)", after_end, e->cur->name);
        { int16_t blk2[256]; msg(e, 0x90, 36, 100); for (int i = 0; i < 40; i++) render(e, blk2, 128); msg(e, 0xB0, 120, 0); }
        CHECK(!e->aud_z, "a pad hit ends the preview");
        /* a folder longer than the screen: the scroll strip moves the rows, the bar shows where */
        set_param(e, "br_1", "1"); wait_browser(e);
        get_param(e, "display_rev", r1, sizeof r1);
        int sb0 = e->vis[P_SB - P_VIS0];
        setp(e, "br_drag", 0.4); setp(e, "br_drag", 0.4);   /* down a fifth of the strip: two rows */
        get_param(e, "br_1", r1, sizeof r1); get_param(e, "display_rev", r2, sizeof r2);
        int top1 = e->br_top, sb1 = e->vis[P_SB - P_VIS0];
        for (int k = 0; k < 20; k++) setp(e, "br_drag", 0.4);
        int top2 = e->br_top;
        CHECK(top1 == 2 && !strcmp(r1, "Hit 02.wav") && sb0 == 1 && sb1 > 1 && top2 == 15 && e->val[P_BR_DRAG] == 0.5f,
              "the scroll strip: a drag down moves two rows on (%s), the bar follows (%d > %d), it stops at the last row (top %d)", r1, sb0, sb1, top2);
        {   /* the data wheel, as the wrapper sends it: the value read back plus a step; a push loads */
            char cv[16], shown[160];
            get_param(e, "br_cursor", cv, sizeof cv); setp(e, "br_cursor", atoi(cv) + 1);
            get_param(e, "br_cursor", cv, sizeof cv); setp(e, "br_cursor", atoi(cv) + 1);
            get_param(e, "br_cursor_display", shown, sizeof shown);
            double wp = preview_rms(e);
            CHECK(e->br_sel == 1 && e->br_top <= 1 && strstr(shown, "Hit 01.wav") && wp > 0.01, "the data wheel highlights row by row and plays each (%s)", shown);
            setp(e, "br_enter", 1); wait_sound(e);
            CHECK(!strcmp(e->cur->name, "Hit 01"), "pushing the wheel loads it (%s)", e->cur->name);
        }
        {   /* 0.9: the rows as touch strips: a wheel click on a touched row highlights it, the next steps on; a drag scrolls; a
             * double tap loads */
            char shown[160];
            setp(e, "bd03", 0.51);
            int first = e->br_sel, top0 = e->br_top;
            setp(e, "bd03", 0.52);
            get_param(e, "br_cursor_display", shown, sizeof shown);
            CHECK(first == top0 + 2 && e->br_sel == first + 1 && fabsf(e->val[P_BD03] - 0.52f) < 1e-6f, "a wheel click on row 3 highlights it, the next moves on (%s)", shown);
            setp(e, "bd01", 0.547); setp(e, "bd01", 0.595); setp(e, "bd01", 0.642); setp(e, "bd01", 0.690);   /* a drag, as the MPC sent one */
            CHECK(e->br_top >= top0 + 1 && e->br_top <= top0 + 3, "a drag up on a row scrolls the list on (top %d > %d)", top0, e->br_top);
            {   /* the drag that froze in 1.0.6 (input log 2026-10-28): MPC's value runs to the end and stays there */
                int t1 = e->br_top;
                setp(e, "bd01", 0.80); setp(e, "bd01", 1.0); setp(e, "bd01", 1.0); setp(e, "bd01", 1.0); setp(e, "bd01", 1.0);
                int t2 = e->br_top;
                setp(e, "bd01", 0.913); setp(e, "bd01", 0.739); setp(e, "bd01", 0.586);   /* then back the other way */
                int t3 = e->br_top;
                CHECK(t2 >= t1 + 4 && t3 < t2, "pinned at the strip's end the list goes on scrolling (%d > %d), and back when the finger turns (%d)", t1, t2, t3);
            }
            {   /* 1.0.10: a swipe follows the finger (a row per 1/8 of the strip), then coasts on (input log 2026-10-28: 0.543 ..
                 * 1.000 in 6 moves over ~30 blocks) */
                int16_t bk[256];
                pthread_mutex_lock(&e->mu); e->br_top = 0; e->br_pos = 0; e->br_g_on = 0; e->br_vel = 0; pthread_mutex_unlock(&e->mu);
                for (int j = 0; j < 40; j++) render(e, bk, 128);
                static const float sw[] = { 0.543f, 0.630f, 0.717f, 0.826f, 0.934f, 1.0f };
                for (int k = 0; k < 6; k++) { setp(e, "bd07", sw[k]); for (int j = 0; j < 5; j++) render(e, bk, 128); }
                int tg = e->br_top;
                for (int j = 0; j < 400; j++) render(e, bk, 128);
                int tf = e->br_top;
                pthread_mutex_lock(&e->mu); e->br_top = 5; e->br_pos = 5; e->br_g_on = 0; e->br_vel = 0; pthread_mutex_unlock(&e->mu);
                static const float sl[] = { 0.98f, 0.96f, 0.94f };   /* a slow drag back: a row or less, no coast */
                for (int k = 0; k < 3; k++) { setp(e, "bd07", sl[k]); for (int j = 0; j < 15; j++) render(e, bk, 128); }
                for (int j = 0; j < 100; j++) render(e, bk, 128);
                CHECK(tg >= 3 && tg <= 5 && tf > tg + 4 && e->br_top >= 4 && e->br_top <= 5,
                      "a swipe follows the finger (top %d), coasts on after (top %d of %d), a slow drag a row at a time (%d)", tg, tf, e->br_n - NROWS, e->br_top);
            }
            {   /* 1.0.13: a fast flick back (input log: 1.000 then 0.195) moves the list; a double tap while it coasts opens the row
                 * that was under the finger */
                int16_t bk[256];
                pthread_mutex_lock(&e->mu); e->br_top = 12; e->br_pos = 12; e->br_g_on = 0; e->br_vel = 0; pthread_mutex_unlock(&e->mu);
                for (int j = 0; j < 40; j++) render(e, bk, 128);
                setp(e, "bd07", 1.0f); setp(e, "bd07", 0.195f);
                for (int j = 0; j < 14; j++) render(e, bk, 128);
                int tb = e->br_top;
                pthread_mutex_lock(&e->mu); e->br_top = 0; e->br_pos = 0; e->br_g_on = 0; e->br_vel = 0; pthread_mutex_unlock(&e->mu);
                for (int j = 0; j < 40; j++) render(e, bk, 128);
                static const float sw2[] = { 0.543f, 0.630f, 0.717f, 0.826f };
                for (int k = 0; k < 4; k++) { setp(e, "bd06", sw2[k]); for (int j = 0; j < 5; j++) render(e, bk, 128); }
                for (int j = 0; j < 16; j++) render(e, bk, 128);
                int ttap = e->br_top;
                for (int j = 0; j < 100; j++) render(e, bk, 128);
                int tlater = e->br_top;
                setp(e, "bt03", 1);
                CHECK(tb <= 12 - 5 && tlater > ttap && e->br_sel == ttap + 2, "a fast flick back moves the list (12 > %d); a double tap while it coasts opens the row tapped (%d, the list at %d then, %d now)", tb, e->br_sel, ttap, tlater);
                wait_browser(e); wait_sound(e);
            }
            {   int16_t bk[256];
                {   /* 1.0.14: the wheel near the strip's end: handed back to the middle, it goes on either way MPC sends it */
                    pthread_mutex_lock(&e->mu); e->br_sel = 3; e->br_top = 0; e->br_pos = 0; e->br_vel = 0; e->br_g_on = 0; pthread_mutex_unlock(&e->mu);
                    for (int j = 0; j < 40; j++) render(e, bk, 128);
                    e->sp_x[P_BD02] = 0.85f;
                    setp(e, "bd02", 0.86f);   /* a click: past 0.85, the engine reports 0.5 */
                    int r1 = e->val[P_BD02] == 0.5f && e->br_sel == 4;
                    setp(e, "bd02", 0.87f);   /* MPC not yet: from its own value */
                    setp(e, "bd02", 0.51f);   /* MPC took the middle */
                    setp(e, "bd02", 0.52f);
                    setp(e, "bd02", 0.51f);   /* and back a click */
                    CHECK(r1 && e->br_sel == 6, "the wheel near the strip's end: reported back to the middle, each click a row either way MPC sends it (row %d)", e->br_sel);
                }
            }
            {   /* 1.0.9: the wheel on a focused PAGE DOWN, as MPC sent it: counting up from 0 again after each burst */
                pthread_mutex_lock(&e->mu); e->br_sel = 0; e->br_top = 0; pthread_mutex_unlock(&e->mu);
                static const float wv[] = { 0.01f, 0.02f, 0.03f, 0.01f, 0.02f, 0.01f, 0.02f, 0.03f, 0.04f };
                int16_t bk[256];
                for (int k = 0; k < 9; k++) { if (k == 3 || k == 5) for (int j = 0; j < 25; j++) render(e, bk, 128); setp(e, "br_pgdn", wv[k]); }
                int sw9 = e->br_sel;
                pthread_mutex_lock(&e->mu); e->br_top = 0; e->br_pos = 0; e->br_g_on = 0; e->br_vel = 0; pthread_mutex_unlock(&e->mu);
                setp(e, "br_pgdn", 1); setp(e, "br_pgdn", 1);
                setp(e, "br_pgdn", 0);   /* its release */
                CHECK(sw9 == 0 && e->br_top == NROWS, "the wheel on PAGE DOWN does nothing (highlight %d), a press pages once (top %d)", sw9, e->br_top);
            }
            char want[96]; snprintf(want, sizeof want, "%s", e->br[e->br_top + 4].name);
            setp(e, "bt05", 1); wait_sound(e);
            CHECK(strstr(want, e->cur->name) == want, "a double tap on row 5 loads its WAV (%s)", e->cur->name);
        }
        setp(e, "br_up", 1); wait_browser(e);   /* BACK: the folder it came out of is lit */
        CHECK(e->br_sel >= 0 && !strcmp(e->br[e->br_sel].name, "Many"), "BACK goes up a folder with the one it left highlighted (%s)", e->br_sel >= 0 ? e->br[e->br_sel].name : "-");
        {   /* the data wheel turning BACK itself (it has the focus after the tap): the highlight moves, as the MPC sent it */
            int s0 = e->br_sel;
            setp(e, "br_up", 0.01); setp(e, "br_up", 0.02); setp(e, "br_up", 0.01);
            CHECK(e->br_sel == s0 && !strcmp(e->br_dir, dir), "the wheel on BACK does nothing, it does not go up again (%d > %d)", s0, e->br_sel);
            set_param(e, "br_1", "1"); wait_browser(e);   /* into Many: it opens at its top */
            CHECK(e->br_top == 0 && e->br_sel < 0, "a folder opens at its top (top %d, highlight %d)", e->br_top, e->br_sel);
            setp(e, "br_up", 1); wait_browser(e);
        }
        {   /* 1.0.8: BACK goes back to the page the folder was on */
            char d2[600], q[700];
            snprintf(d2, sizeof d2, "%s/../browse2", dir);
            for (int k = 0; k < 15; k++) { snprintf(q, sizeof q, "mkdir -p '%s/F%02d'", d2, k); if (system(q)) {} }
            pthread_mutex_lock(&e->mu); snprintf(e->br_dir, sizeof e->br_dir, "%s", d2); e->br_req = 1; e->br_depth = 0; pthread_cond_signal(&e->bcv); pthread_mutex_unlock(&e->mu);
            wait_browser(e);
            setp(e, "br_pgdn", 1);
            int t0 = e->br_top;
            set_param(e, "br_10", "1"); wait_browser(e);
            setp(e, "br_up", 0); setp(e, "br_up", 1); wait_browser(e);
            CHECK(t0 == 5 && e->br_top == t0 && e->br_sel >= 0 && !strcmp(e->br[e->br_sel].name, "F14"), "BACK: the page it was on (top %d), the folder lit (%s)", e->br_top, e->br_sel >= 0 ? e->br[e->br_sel].name : "-");
            {   /* a double tap MPC sends as 1, then 0 the next time (a switch it flips): both open the row's folder */
                int16_t bk[256];
                setp(e, "br_home", 1); wait_browser(e);
                pthread_mutex_lock(&e->mu); snprintf(e->br_dir, sizeof e->br_dir, "%s", d2); e->br_req = 1; pthread_cond_signal(&e->bcv); pthread_mutex_unlock(&e->mu);
                wait_browser(e);
                setp(e, "bt02", 1); wait_browser(e);
                int in1 = strstr(e->br_dir, "/F01") != NULL;
                setp(e, "br_up", 1); wait_browser(e);
                for (int k = 0; k < 80; k++) render(e, bk, 128);
                setp(e, "bt02", 0); wait_browser(e);
                int in2 = strstr(e->br_dir, "/F01") != NULL;
                setp(e, "br_up", 1); wait_browser(e);
                for (int k = 0; k < 80; k++) render(e, bk, 128);
                pthread_mutex_lock(&e->mu); e->br_sel = 2; pthread_mutex_unlock(&e->mu);
                setp(e, "br_enter", 1); wait_browser(e);
                int in3 = strstr(e->br_dir, "/F02") != NULL;
                setp(e, "br_up", 1); wait_browser(e);
                for (int k = 0; k < 80; k++) render(e, bk, 128);
                pthread_mutex_lock(&e->mu); e->br_sel = 3; pthread_mutex_unlock(&e->mu);
                setp(e, "br_enter", 0); wait_browser(e);
                int in4 = strstr(e->br_dir, "/F03") != NULL;
                CHECK(in1 && in2 && in3 && in4, "a double tap and the wheel's push open a folder every time, sent as 1 or as 0 (%d %d %d %d)", in1, in2, in3, in4);
            }
        }
        setp(e, "br_home", 1); wait_browser(e);
        get_param(e, "br_1", r1, sizeof r1);
        get_param(e, "br_2", r2, sizeof r2);   /* the drives: the sound list's folder first */
        CHECK(!strcmp(r1, "USER SOUNDS  /"), "the drives list starts with the sound list (%s | %s)", r1, r2);
        set_param(e, "br_1", "1"); wait_browser(e);
        set_param(e, "br_2", "1"); wait_sound(e);
        pv = preview_rms(e);
        CHECK(!strcmp(e->cur->name, "Built-in - Soul Keys") && !e->browsed && pv > 0.01, "there, a tap loads a built-in and plays it (%s)", e->cur->name);
        chrono_t *e3 = create(NULL); wait_sound(e3);
        set_param(e3, "state", st); wait_sound(e3);
        CHECK(e3->cur && !strcmp(e3->cur->name, "Loop A"), "a project brings the browsed sound back");
        destroy(e3);
        pick(e, "Break 90");
    }

    /* 8. every pad of a found set plays cleanly */
    if (!quick) {
        for (int p = 0; p < NPADS; p++) e->pad[p] = PAD_DEFAULT;
        setp(e, "sync", 1); set_param(e, "lfo_bpm", "96"); setp(e, "key", 0); setp(e, "chop_mode", 0);
        int bad = 0;
        for (int p = 0; p < 16; p++) {
            a = play(e, 36 + p, 200, 400);
            if (rms(a, 0, 8820) < 0.002) { bad++; long r0, r1; pad_region(e, e->cur, p, &r0, &r1); printf("        silent pad %d: cue %ld..%ld\n", p, r0, r1); }
            if (p == 0) write_wav("07 break pad A1 at 96", a, 400 * 128);
            free(a);
        }
        CHECK(!bad, "16 pads sound (%d silent)", bad);
    }

    /* 9. time per block, every voice stretching (x86: relative only) */
    { int16_t blk[256]; set_param(e, "lfo_bpm", "120"); setp(e, "key", 5);
      for (int q = 0; q < 3; q++) {
          setp(e, "quality", q);
          for (int k = 0; k < NV; k++) msg(e, 0x90, 36 + k, 100);
          for (int i = 0; i < 50; i++) render(e, blk, 128);
          double worst = 0, t0 = now();
          for (int i = 0; i < 1000; i++) { double s = now(); render(e, blk, 128); worst = fmax(worst, now() - s); }
          printf("        %s, %d voices: %.0f us a block, worst %.0f us\n", OPT_QUALITY[q], NV, (now() - t0) / 1000 * 1e6, worst * 1e6);
          msg(e, 0xB0, 120, 0);
      } }
    destroy(e);
    printf(fails ? "FAILED: %d\n" : "PASSED\n", fails);
    return fails != 0;
}
