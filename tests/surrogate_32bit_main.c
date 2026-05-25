/*
 * 32-bit portable code surrogate test driver.
 *
 * Compiled on Linux with `gcc -m32 -std=c99 -Werror` against:
 *   - src/protocol.c        (QGA JSON-RPC parsing — portable)
 *   - src/third_party/cJSON.c (third-party JSON, portable)
 *   - tests/surrogate_32bit_main.c (this file)
 *
 * Purpose: catch int-width / struct-layout / endianness / integer-truncation
 * regressions in portable code without requiring access to old Intel Mac
 * hardware. Scope is deliberately narrow per universal_upgrade.md §4.5 / D16 /
 * H04 — util.c includes compat.h and uses POSIX surface that needs
 * _POSIX_C_SOURCE; selftest.c and log.c also pull macOS-specific deps.
 *
 * This driver is a standalone test, NOT a wrapper around tests/test_unit.c
 * (which is set up for macOS native builds with -framework CoreFoundation).
 *
 * Test surface:
 *   - JSON parse round-trip via protocol_parse_request + protocol_get_*
 *   - QGA request shape with execute/arguments/id
 *   - Edge cases: NULL inputs, empty strings, malformed JSON
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "protocol.h"
#include "third_party/cJSON.h"

static int pass_count = 0;
static int fail_count = 0;

#define ASSERT(name, cond) do { \
    if (cond) { printf("  PASS: %s\n", name); pass_count++; } \
    else { printf("  FAIL: %s (line %d)\n", name, __LINE__); fail_count++; } \
} while (0)

#define ASSERT_STR_EQ(name, got, expected) do { \
    if ((got) && (expected) && strcmp((got), (expected)) == 0) { \
        printf("  PASS: %s\n", name); pass_count++; \
    } else { \
        printf("  FAIL: %s (got '%s', expected '%s')\n", name, \
            (got) ? (got) : "NULL", (expected) ? (expected) : "NULL"); \
        fail_count++; \
    } \
} while (0)

/* ---- 32-bit invariants ---- */
static void test_sizeof_invariants(void)
{
    printf("\n--- 32-bit sizeof invariants ---\n");

    /* The whole point of this surrogate: confirm we're actually running 32-bit
     * code. If sizeof(void*) is 8, the CI runner forgot the -m32 flag and
     * the test isn't doing what it claims. */
    ASSERT("sizeof(void*) == 4 (we are actually 32-bit)", sizeof(void *) == 4);
    ASSERT("sizeof(int) == 4", sizeof(int) == 4);
    ASSERT("sizeof(int32_t) == 4", sizeof(int32_t) == 4);
    ASSERT("sizeof(int64_t) == 8", sizeof(int64_t) == 8);
    ASSERT("sizeof(uint32_t) == 4", sizeof(uint32_t) == 4);
    ASSERT("sizeof(uint64_t) == 8", sizeof(uint64_t) == 8);
}

/* ---- Protocol parsing tests ---- */
static void test_protocol_parse_valid(void)
{
    printf("\n--- protocol_parse_request: valid inputs ---\n");

    const char *req = "{\"execute\":\"guest-ping\"}";
    cJSON *parsed = protocol_parse_request(req);
    ASSERT("parse simple request", parsed != NULL);
    ASSERT_STR_EQ("extract command name", protocol_get_command(parsed), "guest-ping");
    ASSERT("no arguments on bare request", protocol_get_arguments(parsed) == NULL);
    ASSERT("no id on bare request", protocol_get_id(parsed) == NULL);
    cJSON_Delete(parsed);

    const char *req_with_args =
        "{\"execute\":\"guest-set-time\",\"arguments\":{\"time\":1234567890},\"id\":42}";
    parsed = protocol_parse_request(req_with_args);
    ASSERT("parse request with arguments + id", parsed != NULL);
    ASSERT_STR_EQ("command name with args", protocol_get_command(parsed), "guest-set-time");
    cJSON *args = protocol_get_arguments(parsed);
    ASSERT("arguments present", args != NULL);
    cJSON *id = protocol_get_id(parsed);
    ASSERT("id present", id != NULL);
    ASSERT("id is number 42", id != NULL && id->valueint == 42);
    cJSON_Delete(parsed);
}

static void test_protocol_parse_delimited(void)
{
    printf("\n--- protocol_parse_request: 0xFF-delimited (guest-sync-delimited) ---\n");

    /* guest-sync-delimited wraps the request with 0xFF bytes. The parser
     * should skip leading 0xFF before handing to cJSON. */
    char req[64];
    req[0] = (char)0xff;
    req[1] = (char)0xff;
    strcpy(req + 2, "{\"execute\":\"guest-sync\"}");

    cJSON *parsed = protocol_parse_request(req);
    ASSERT("parse delimited request", parsed != NULL);
    ASSERT_STR_EQ("delimited command name", protocol_get_command(parsed), "guest-sync");
    cJSON_Delete(parsed);
}

static void test_protocol_parse_edge_cases(void)
{
    printf("\n--- protocol_parse_request: edge cases ---\n");

    ASSERT("NULL input returns NULL", protocol_parse_request(NULL) == NULL);
    ASSERT("empty string returns NULL", protocol_parse_request("") == NULL);

    /* Malformed JSON */
    ASSERT("malformed JSON returns NULL", protocol_parse_request("{notvalid") == NULL);

    /* No execute field — parses but no command */
    cJSON *parsed = protocol_parse_request("{\"foo\":\"bar\"}");
    ASSERT("parse JSON without execute", parsed != NULL);
    ASSERT("no command without execute", protocol_get_command(parsed) == NULL);
    cJSON_Delete(parsed);
}

/* ---- Response building tests ---- */
static void test_protocol_build_response(void)
{
    printf("\n--- protocol_build_response / empty / error ---\n");

    cJSON *id = cJSON_CreateNumber(7);
    char *resp = protocol_build_empty_response(id);
    ASSERT("empty response non-null", resp != NULL);
    /* Should contain "return":{} and "id":7 */
    ASSERT("empty response has return", resp && strstr(resp, "\"return\"") != NULL);
    ASSERT("empty response has id", resp && strstr(resp, "\"id\"") != NULL);
    free(resp);

    char *err = protocol_build_error("GenericError", "test error", id);
    ASSERT("error response non-null", err != NULL);
    ASSERT("error response has error class", err && strstr(err, "GenericError") != NULL);
    ASSERT("error response has desc", err && strstr(err, "test error") != NULL);
    free(err);

    cJSON_Delete(id);
}

/* ---- cJSON round-trip (the actually-portable JSON library) ---- */
static void test_cjson_roundtrip(void)
{
    printf("\n--- cJSON round-trip ---\n");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "test");
    cJSON_AddNumberToObject(root, "value", 42);
    cJSON_AddBoolToObject(root, "flag", 1);

    char *encoded = cJSON_PrintUnformatted(root);
    ASSERT("cJSON encode produces output", encoded != NULL);

    cJSON *decoded = cJSON_Parse(encoded);
    ASSERT("cJSON decode parses encoded output", decoded != NULL);

    cJSON *name = cJSON_GetObjectItemCaseSensitive(decoded, "name");
    ASSERT_STR_EQ("round-trip string", name ? name->valuestring : NULL, "test");

    cJSON *value = cJSON_GetObjectItemCaseSensitive(decoded, "value");
    ASSERT("round-trip number", value && value->valueint == 42);

    cJSON *flag = cJSON_GetObjectItemCaseSensitive(decoded, "flag");
    ASSERT("round-trip boolean", flag && cJSON_IsTrue(flag));

    free(encoded);
    cJSON_Delete(decoded);
    cJSON_Delete(root);
}

int main(void)
{
    printf("=== 32-bit surrogate test driver ===\n");
    printf("Built: %s\n", VERSION);
    printf("Target: Linux/-m32 portable subset (protocol.c + cJSON.c)\n");

    test_sizeof_invariants();
    test_protocol_parse_valid();
    test_protocol_parse_delimited();
    test_protocol_parse_edge_cases();
    test_protocol_build_response();
    test_cjson_roundtrip();

    printf("\n=== Results ===\n");
    printf("Pass: %d\n", pass_count);
    printf("Fail: %d\n", fail_count);

    return fail_count == 0 ? 0 : 1;
}
