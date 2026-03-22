/*
 * Fuzz test for protocol parser and base64.
 * Feeds random/malformed data to find crashes.
 *
 * Build: clang -Isrc -Isrc/third_party -fsanitize=address,undefined \
 *        -o build/fuzz_protocol tests/fuzz_protocol.c \
 *        src/util.c src/protocol.c src/compat.c src/third_party/cJSON.c \
 *        -framework CoreFoundation
 * Run:   ./build/fuzz_protocol
 */

#include "util.h"
#include "protocol.h"
#include "third_party/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FUZZ_ROUNDS 100000
#define MAX_INPUT_SIZE 4096

static unsigned int seed;

static int rand_int(int max) {
    seed = seed * 1103515245 + 12345;
    return (seed >> 16) % max;
}

static void fuzz_json_parser(void)
{
    printf("Fuzzing JSON parser (%d rounds)...\n", FUZZ_ROUNDS);

    char buf[MAX_INPUT_SIZE];
    for (int i = 0; i < FUZZ_ROUNDS; i++) {
        /* Generate random data of random length */
        int len = rand_int(256) + 1;
        for (int j = 0; j < len; j++)
            buf[j] = (char)rand_int(256);
        buf[len] = '\0';

        /* Feed to parser — should never crash */
        cJSON *req = protocol_parse_request(buf);
        if (req) {
            /* If it parsed, try to get fields */
            const char *cmd = protocol_get_command(req);
            (void)cmd;
            cJSON *args = protocol_get_arguments(req);
            (void)args;
            cJSON_Delete(req);
        }
    }
    printf("  JSON parser: %d rounds, no crashes\n", FUZZ_ROUNDS);
}

static void fuzz_base64(void)
{
    printf("Fuzzing base64 (%d rounds)...\n", FUZZ_ROUNDS);

    char buf[MAX_INPUT_SIZE];
    for (int i = 0; i < FUZZ_ROUNDS; i++) {
        /* Random binary data for encode */
        int len = rand_int(512);
        unsigned char data[512];
        for (int j = 0; j < len; j++)
            data[j] = (unsigned char)rand_int(256);

        char *enc = base64_encode(data, (size_t)len);
        if (enc) {
            /* Decode what we encoded — should round-trip */
            size_t dec_len;
            unsigned char *dec = base64_decode(enc, &dec_len);
            if (dec) {
                if (dec_len != (size_t)len || memcmp(dec, data, (size_t)len) != 0) {
                    printf("  FAIL: base64 round-trip mismatch at round %d\n", i);
                    free(dec);
                    free(enc);
                    exit(1);
                }
                free(dec);
            }
            free(enc);
        }

        /* Random string for decode (may be invalid base64) */
        int slen = rand_int(128) + 1;
        for (int j = 0; j < slen; j++)
            buf[j] = (char)(rand_int(96) + 32); /* printable ASCII */
        buf[slen] = '\0';

        size_t out_len;
        unsigned char *result = base64_decode(buf, &out_len);
        free(result); /* May be NULL, that's fine */
    }
    printf("  base64: %d rounds, no crashes\n", FUZZ_ROUNDS);
}

static void fuzz_protocol_responses(void)
{
    printf("Fuzzing protocol response builders (%d rounds)...\n", FUZZ_ROUNDS / 10);

    for (int i = 0; i < FUZZ_ROUNDS / 10; i++) {
        /* Random error class and desc */
        char cls[64], desc[256];
        int clen = rand_int(60) + 1;
        int dlen = rand_int(250) + 1;
        for (int j = 0; j < clen; j++) cls[j] = (char)(rand_int(94) + 33);
        cls[clen] = '\0';
        for (int j = 0; j < dlen; j++) desc[j] = (char)(rand_int(94) + 33);
        desc[dlen] = '\0';

        char *resp = protocol_build_error(cls, desc, NULL);
        free(resp);

        /* Build response with random JSON value */
        cJSON *val = cJSON_CreateString(desc);
        resp = protocol_build_response(val, NULL);
        free(resp);
    }
    printf("  Protocol builders: %d rounds, no crashes\n", FUZZ_ROUNDS / 10);
}

int main(void)
{
    seed = (unsigned int)time(NULL);
    printf("=== Fuzz Tests (seed=%u) ===\n\n", seed);

    fuzz_json_parser();
    fuzz_base64();
    fuzz_protocol_responses();

    printf("\n=== All fuzz tests passed ===\n");
    return 0;
}
