/*
 * Unit tests for individual functions.
 * Tests base64, protocol, utility functions in isolation.
 *
 * Build: clang -Isrc -Isrc/third_party -o build/test_unit tests/test_unit.c \
 *        src/util.c src/protocol.c src/compat.c src/third_party/cJSON.c \
 *        -framework CoreFoundation
 * Run:   ./build/test_unit
 */

#include "util.h"
#include "protocol.h"
#include "compat.h"
#include "third_party/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pass = 0, fail = 0;

#define ASSERT(name, cond) do { \
    if (cond) { printf("  PASS: %s\n", name); pass++; } \
    else { printf("  FAIL: %s (line %d)\n", name, __LINE__); fail++; } \
} while(0)

#define ASSERT_STR(name, got, expected) do { \
    if ((got) && (expected) && strcmp(got, expected) == 0) { printf("  PASS: %s\n", name); pass++; } \
    else { printf("  FAIL: %s (got '%s', expected '%s')\n", name, got ? got : "NULL", expected); fail++; } \
} while(0)

/* ---- Base64 Tests ---- */

static void test_base64(void)
{
    printf("\n--- Base64 ---\n");

    /* Encode */
    char *enc = base64_encode((const unsigned char *)"hello", 5);
    ASSERT_STR("encode 'hello'", enc, "aGVsbG8=");
    free(enc);

    enc = base64_encode((const unsigned char *)"", 0);
    ASSERT_STR("encode empty", enc, "");
    free(enc);

    enc = base64_encode((const unsigned char *)"a", 1);
    ASSERT_STR("encode 'a'", enc, "YQ==");
    free(enc);

    enc = base64_encode((const unsigned char *)"ab", 2);
    ASSERT_STR("encode 'ab'", enc, "YWI=");
    free(enc);

    enc = base64_encode((const unsigned char *)"abc", 3);
    ASSERT_STR("encode 'abc'", enc, "YWJj");
    free(enc);

    /* Decode */
    size_t len;
    unsigned char *dec = base64_decode("aGVsbG8=", &len);
    ASSERT("decode 'hello' length", len == 5);
    ASSERT("decode 'hello' content", dec && memcmp(dec, "hello", 5) == 0);
    free(dec);

    dec = base64_decode("", &len);
    ASSERT("decode empty length", len == 0);
    free(dec);

    /* Round-trip with binary data */
    unsigned char binary[] = {0x00, 0xFF, 0x80, 0x7F, 0x01, 0xFE};
    enc = base64_encode(binary, 6);
    dec = base64_decode(enc, &len);
    ASSERT("binary round-trip length", len == 6);
    ASSERT("binary round-trip content", dec && memcmp(dec, binary, 6) == 0);
    free(enc);
    free(dec);

    /* Invalid input */
    dec = base64_decode(NULL, &len);
    ASSERT("decode NULL returns NULL", dec == NULL);

    dec = base64_decode("x", &len);  /* Not a multiple of 4 */
    ASSERT("decode invalid length returns NULL", dec == NULL);
}

/* ---- Protocol Tests ---- */

static void test_protocol(void)
{
    printf("\n--- Protocol ---\n");

    /* Parse valid request */
    cJSON *req = protocol_parse_request("{\"execute\":\"guest-ping\"}");
    ASSERT("parse valid request", req != NULL);
    ASSERT_STR("get command", protocol_get_command(req), "guest-ping");
    ASSERT("no arguments", protocol_get_arguments(req) == NULL);
    ASSERT("no id", protocol_get_id(req) == NULL);
    cJSON_Delete(req);

    /* Parse with arguments and id */
    req = protocol_parse_request("{\"execute\":\"guest-sync\",\"arguments\":{\"id\":12345},\"id\":99}");
    ASSERT("parse with args", req != NULL);
    ASSERT_STR("command name", protocol_get_command(req), "guest-sync");
    ASSERT("has arguments", protocol_get_arguments(req) != NULL);
    ASSERT("has id", protocol_get_id(req) != NULL);
    cJSON_Delete(req);

    /* Parse with 0xFF prefix (sync-delimited) */
    req = protocol_parse_request("\xff{\"execute\":\"test\"}");
    ASSERT("parse with 0xFF prefix", req != NULL);
    ASSERT_STR("command after 0xFF", protocol_get_command(req), "test");
    cJSON_Delete(req);

    /* Parse invalid */
    req = protocol_parse_request("not json");
    ASSERT("parse invalid returns NULL", req == NULL);

    req = protocol_parse_request("");
    ASSERT("parse empty returns NULL", req == NULL);

    req = protocol_parse_request(NULL);
    ASSERT("parse NULL returns NULL", req == NULL);

    /* Build response */
    cJSON *val = cJSON_CreateObject();
    char *resp = protocol_build_response(val, NULL);
    ASSERT("build response not NULL", resp != NULL);
    ASSERT("response has return", resp && strstr(resp, "\"return\""));
    free(resp);

    /* Build error */
    resp = protocol_build_error("TestError", "test desc", NULL);
    ASSERT("build error not NULL", resp != NULL);
    ASSERT("error has class", resp && strstr(resp, "TestError"));
    ASSERT("error has desc", resp && strstr(resp, "test desc"));
    free(resp);

    /* Build empty response */
    resp = protocol_build_empty_response(NULL);
    ASSERT("empty response not NULL", resp != NULL);
    ASSERT("empty response has return", resp && strstr(resp, "\"return\""));
    free(resp);
}

/* ---- String Utility Tests ---- */

static void test_util(void)
{
    printf("\n--- Utilities ---\n");

    /* str_trim */
    char s1[] = "  hello  ";
    ASSERT_STR("trim spaces", str_trim(s1), "hello");

    char s2[] = "\t\n test \n\t";
    ASSERT_STR("trim whitespace", str_trim(s2), "test");

    char s3[] = "nopad";
    ASSERT_STR("trim no-op", str_trim(s3), "nopad");

    char s4[] = "   ";
    ASSERT_STR("trim all spaces", str_trim(s4), "");

    ASSERT("trim NULL", str_trim(NULL) == NULL);

    /* safe_strdup */
    char *dup = safe_strdup("test");
    ASSERT_STR("strdup", dup, "test");
    free(dup);

    ASSERT("strdup NULL", safe_strdup(NULL) == NULL);

    /* read_file */
    char *content = read_file("/etc/hosts", NULL);
    ASSERT("read /etc/hosts", content != NULL);
    ASSERT("hosts has localhost", content && strstr(content, "localhost"));
    free(content);

    content = read_file("/nonexistent/file", NULL);
    ASSERT("read nonexistent returns NULL", content == NULL);
}

/* ---- Compat Tests ---- */

static void test_compat(void)
{
    printf("\n--- Compat ---\n");

    compat_init();
    const os_version_t *ver = compat_os_version();
    ASSERT("version major > 0", ver->major > 0);
    ASSERT("version detected", ver->major >= 10);

    /* compat_strndup */
    char *s = compat_strndup("hello world", 5);
    ASSERT_STR("strndup 5", s, "hello");
    free(s);

    s = compat_strndup("short", 100);
    ASSERT_STR("strndup longer than string", s, "short");
    free(s);

    ASSERT("strndup NULL", compat_strndup(NULL, 5) == NULL);
}

int main(void)
{
    printf("=== Unit Tests ===\n");

    test_base64();
    test_protocol();
    test_util();
    test_compat();

    printf("\n=== Results: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
