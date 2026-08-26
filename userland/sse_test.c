#include <unistd.h>
#include <stdint.h>
#include <stdio.h>

// xmmintrin.h pulls in mm_malloc.h for _mm_malloc/_mm_free (unused
// here), which unconditionally includes <stdlib.h> -- unavailable in
// this freestanding, nostdlib build. Defining this guard macro first
// makes mm_malloc.h's own #ifndef skip its body entirely.
#define _MM_MALLOC_H_INCLUDED
#include <xmmintrin.h>
#include <smmintrin.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    int pid = getpid();
    double increment = (pid % 2 == 0) ? 1.5 : 2.5;
    double scalar_sum = 0.0;

    __m128 vec_inc = _mm_set1_ps((float)increment);
    __m128 vec_sum = _mm_setzero_ps();

    for (int i = 0; i < 20; i++) {
        printf("[sse pid=%d] iter=%d\n", pid, i);
        scalar_sum += increment;
        vec_sum = _mm_add_ps(vec_sum, vec_inc);
        for (volatile uint32_t spin = 0; spin < 3000000; spin++) {
        }
    }
    vec_sum = _mm_floor_ps(vec_sum); // exercises SSE4.1, once after accumulation

    int scalar_result = (int)(scalar_sum * 10.0);
    float vec_lane0 = _mm_cvtss_f32(vec_sum);
    int vec_result = (int)vec_lane0;

    int expected_scalar = (int)(increment * 20.0 * 10.0);
    int expected_vec = (int)(increment * 20.0);

    printf("[sse pid=%d] scalar=%d vec=%d\n", pid, scalar_result, vec_result);

    if (scalar_result != expected_scalar || vec_result != expected_vec) {
        printf("[sse pid=%d] FAILED: expected scalar=%d vec=%d\n", pid, expected_scalar, expected_vec);
        return 1;
    }

    printf("[sse pid=%d] passed\n", pid);
    return 0;
}
