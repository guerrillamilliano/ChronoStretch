/* Device timing for ChronoStretch 0.2 (build for armhf, run from /tmp on the MPC; nothing installed): how long the worker
 * takes to analyse a 3-minute sound (tempo, key, the screen's waveform) and to separate its drums, on the thread's CPU clock
 * and the wall clock; and render() with every voice stretching. */
#include "../src/chrono.c"
#include <unistd.h>

static double tcpu(void) { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static double wall(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int main(void) {
    /* a 3-minute sound: Break 90 over and over, with a keys chord on top */
    zone_t brk;
    if (!synth_builtin(5, &brk)) return 1;
    long n = (long)(180 * SR);
    inst_t *in = calloc(1, sizeof(inst_t));
    snprintf(in->name, sizeof in->name, "3 minutes");
    in->z.nch = 2; in->z.len = (int)n; in->z.data = malloc(sizeof(int16_t) * n * 2);
    for (long i = 0; i < n; i++)
        for (int c = 0; c < 2; c++) {
            float k = 0.1f * (sinf(2 * PI * 174.6f * i / SR) + sinf(2 * PI * 220.0f * i / SR) + sinf(2 * PI * 261.6f * i / SR));
            float b = brk.data[(i % brk.len) * 2 + c] / 32768.0f;
            in->z.data[2 * i + c] = (int16_t)lrintf(clampf(0.6f * b + k, -1, 1) * 32767);
        }
    in->sec = n / SR;
    double c0 = tcpu(), w0 = wall();
    fft_t t; fft_init(&t, 1024); analyse_onsets(in, &t); fft_free(&t);
    analyse_tempo(in); analyse_key(in); analyse_wave(in);
    double c1 = tcpu(), w1 = wall();
    char k[16]; key_name(in->key, k, sizeof k);
    printf("analysis of 3:00: %.2f s CPU, %.2f s wall (%.1f BPM, %s)\n", c1 - c0, w1 - w0, in->bpm, k);
    int g = 0;
    separate_drums(in, &g, 0);
    double c2 = tcpu(), w2 = wall();
    printf("drum separation of 3:00: %.1f s CPU, %.1f s wall\n", c2 - c1, w2 - w1);
    free_inst(in);
    return 0;
}
