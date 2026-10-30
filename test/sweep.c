/* Device check of what makes Q-Link sweeps cost (build for armhf, run from /tmp on the MPC; nothing installed): the bench's
 * sweep (8 held notes, four random parameters turned every block), first with every parameter, then leaving one suspect out
 * at a time; a suspect whose absence brings the p99 down is the cost. */
#include "../src/chrono.c"
#include <unistd.h>

static double tcpu(void) { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec * 1e6 + t.tv_nsec * 1e-3; }
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? 1 : x > y ? -1 : 0; }

static void mixed(chrono_t *e, int skip, const char *label) {
    int16_t blk[256];
    static double t[3000];
    unsigned r = 7;
    load_preset(e, 0);
    for (int i = 0; i < 40; i++) render(e, blk, 128);
    for (int k = 0; k < 8; k++) { uint8_t on[3] = { 0x90, (uint8_t)(36 + k), 100 }; midi(e, on, 3); }
    for (int b = 0; b < 3000; b++) {
        if (b % 172 == 0) for (int k = 0; k < 8; k++) { uint8_t on[3] = { 0x90, (uint8_t)(36 + k), 100 }; midi(e, on, 3); }
        double s = tcpu();
        for (int q = 0; q < 4; q++) {
            int p;
            do { r = r * 1103515245u + 12345u; p = (r >> 8) % NP; } while (PKIND[p] == K_TEXT || PKIND[p] == K_VIS || p == skip);
            r = r * 1103515245u + 12345u;
            char v[32]; snprintf(v, sizeof v, "%g", PMIN[p] + (PMAX[p] - PMIN[p]) * ((r >> 8) & 0xffff) / 65535.0);
            set_param(e, KEYS[p], v);
        }
        render(e, blk, 128);
        t[b] = tcpu() - s;
    }
    double mean = 0; for (int b = 0; b < 3000; b++) mean += t[b];
    qsort(t, 3000, sizeof(double), cmpd);
    printf("%-18s mean %5.1f%%  p99 %5.1f%%  max %5.1f%%\n", label, mean / 3000 / 29.02, t[30] / 29.02, t[0] / 29.02);
    uint8_t off[3] = { 0xB0, 120, 0 }; midi(e, off, 3);
    for (int i = 0; i < 20; i++) render(e, blk, 128);
}

int main(void) {
    chrono_t *e = create(NULL);
    int16_t blk[256];
    for (int i = 0; i < 2000 && (!e->cur || e->pending); i++) { render(e, blk, 128); usleep(5000); }
    static const int SUS[] = { P_GRAIN, P_DIFFUSE, P_BLUR, P_COLOR, P_FORMANT, P_FORMANT_MODE, P_FREEZE_MODE, P_FREEZE_LATCH,
                               P_GRAIN_CYCLES, P_MAP_MODE, P_STRETCH, P_TRANSIENT, P_PRESET, P_SOUND, P_HOLD_TIME, P_WASH_MIX };
    mixed(e, -1, "everything");
    for (unsigned i = 0; i < sizeof SUS / sizeof SUS[0]; i++) { char l[40]; snprintf(l, sizeof l, "without %s", KEYS[SUS[i]]); mixed(e, SUS[i], l); }
    destroy(e);
    return 0;
}
