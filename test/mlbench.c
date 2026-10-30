/* Feasibility check for an on-device stem split (SCNet-sized): how many multiply-adds a second the MPC's free cores do on
 * the matrix shapes an SCNet LSTM step and its 3x3 convolutions need. A NEON register-blocked SGEMM (4 rows x 16 columns
 * of C a micro-step, B packed), one thread and two threads (cores 0-1; MPC's audio sits on 2-3).
 * Build for armhf, run from /tmp on the MPC (nothing installed). Prints GMAC/s per shape. */
#include <arm_neon.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

/* C[M x N] += A[M x K] * B[K x N]; N a multiple of 16, rows of B contiguous */
static void sgemm(int M, int N, int K, const float *A, const float *B, float *C) {
    int m = 0;
    for (; m + 4 <= M; m += 4)
        for (int n = 0; n < N; n += 16) {
            float32x4_t c[4][4];
            for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) c[i][j] = vld1q_f32(C + (m + i) * N + n + 4 * j);
            for (int k = 0; k < K; k++) {
                const float *b = B + k * N + n;
                float32x4_t b0 = vld1q_f32(b), b1 = vld1q_f32(b + 4), b2 = vld1q_f32(b + 8), b3 = vld1q_f32(b + 12);
                for (int i = 0; i < 4; i++) {
                    float32x4_t a = vdupq_n_f32(A[(m + i) * K + k]);
                    c[i][0] = vmlaq_f32(c[i][0], a, b0); c[i][1] = vmlaq_f32(c[i][1], a, b1);
                    c[i][2] = vmlaq_f32(c[i][2], a, b2); c[i][3] = vmlaq_f32(c[i][3], a, b3);
                }
            }
            for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) vst1q_f32(C + (m + i) * N + n + 4 * j, c[i][j]);
        }
    for (; m < M; m++) for (int k = 0; k < K; k++) for (int n = 0; n < N; n++) C[m * N + n] += A[m * K + k] * B[k * N + n];
}

typedef struct { int M, N, K, reps; const float *A, *B; float *C; } job_t;
static void *run(void *p) { job_t *j = p; for (int r = 0; r < j->reps; r++) sgemm(j->M, j->N, j->K, j->A, j->B, j->C); return NULL; }

static double bench(int M, int N, int K, int threads) {
    float *A = calloc((size_t)M * K, 4), *B = calloc((size_t)K * N, 4), *C = calloc((size_t)M * N * threads, 4);
    for (int i = 0; i < M * K; i++) A[i] = (i % 7) * 0.01f;
    for (int i = 0; i < K * N; i++) B[i] = (i % 5) * 0.01f;
    double mac = (double)M * N * K;
    int reps = (int)(2e9 / mac / 4) + 1;   /* about half a second at 1 GMAC/s */
    job_t j[2];
    pthread_t th[2];
    double t0 = now();
    for (int t = 0; t < threads; t++) {
        j[t] = (job_t){ M, N, K, reps, A, B, C + (size_t)t * M * N };
        pthread_create(&th[t], NULL, run, &j[t]);
    }
    for (int t = 0; t < threads; t++) pthread_join(th[t], NULL);
    double dt = now() - t0;
    free(A); free(B); free(C);
    return mac * reps * threads / dt / 1e9;
}

int main(void) {
    static const struct { const char *what; int M, N, K; } S[] = {
        { "LSTM 128, freq path (batch 474)", 474, 512, 256 },
        { "LSTM 128, time path (batch 57)", 57, 512, 256 },
        { "LSTM 256, freq path (batch 238)", 238, 1024, 512 },
        { "LSTM 256, time path (batch 57)", 57, 1024, 512 },
        { "fusion conv 3x3 256->256 (im2col)", 512, 256, 2304 },
        { "global conv 3x3 32->32 (im2col)", 616, 32, 288 },
    };
    printf("%-36s %10s %10s\n", "shape (M x N x K)", "1 thread", "2 threads");
    for (unsigned s = 0; s < sizeof S / sizeof S[0]; s++)
        printf("%-36s %7.2f GMAC/s %7.2f GMAC/s\n", S[s].what, bench(S[s].M, S[s].N, S[s].K, 1), bench(S[s].M, S[s].N, S[s].K, 2));
    return 0;
}
