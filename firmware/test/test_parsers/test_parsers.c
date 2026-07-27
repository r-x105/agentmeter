// Native (host) tests for the pure provider parsers.
//   pio test -d firmware -e native
// The parser sources are #included directly so no src build is needed
// (test_build_src = no keeps Arduino code out of the native build).

#include <unity.h>

#include "../../src/providers/json_mini.c"
#include "../../src/providers/anthropic_parse.c"
#include "../../src/providers/openrouter_parse.c"
#include "../../src/providers/openai_parse.c"

void setUp(void) {}
void tearDown(void) {}

// ---- json_mini ----

static void test_json_number_basic(void) {
    float v = 0;
    TEST_ASSERT_TRUE(json_find_number("{\"usage\": 12.34}", "usage", &v));
    TEST_ASSERT_FLOAT_WITHIN(0.001, 12.34, v);
}

static void test_json_number_null_and_absent(void) {
    float v = 0;
    TEST_ASSERT_FALSE(json_find_number("{\"limit\": null}", "limit", &v));
    TEST_ASSERT_TRUE(json_is_null("{\"limit\": null}", "limit"));
    TEST_ASSERT_FALSE(json_find_number("{\"other\": 5}", "limit", &v));
    TEST_ASSERT_FALSE(json_is_null("{\"other\": 5}", "limit"));
}

static void test_json_number_negative_and_int(void) {
    float v = 0;
    TEST_ASSERT_TRUE(json_find_number("{\"n\":-3}", "n", &v));
    TEST_ASSERT_FLOAT_WITHIN(0.001, -3.0, v);
}

static void test_json_string(void) {
    char out[32];
    TEST_ASSERT_TRUE(json_find_string("{\"label\":\"sk-or-v1-abc\"}", "label", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("sk-or-v1-abc", out);
    TEST_ASSERT_FALSE(json_find_string("{\"label\": 42}", "label", out, sizeof(out)));
}

static void test_json_key_not_value_match(void) {
    // A value that looks like a key must not match: "usage" appears as a
    // string VALUE first here; the extractor must find the actual key.
    float v = 0;
    TEST_ASSERT_TRUE(json_find_number("{\"note\":\"usage\",\"usage\":7}", "usage", &v));
    TEST_ASSERT_FLOAT_WITHIN(0.001, 7.0, v);
}

// ---- anthropic ----

static ProviderSlot slot_fixture(void) {
    ProviderSlot s;
    memset(&s, 0, sizeof(s));
    s.primary.reset_mins = s.secondary.reset_mins = -1;
    return s;
}

static void test_anthropic_ok_basic(void) {
    ProviderSlot s = slot_fixture();
    long now = 1782500000L;
    // Real header shapes: utilization 0.0-1.0, reset = unix ts.
    TEST_ASSERT_TRUE(anthropic_parse_ok("0.62", "0.31", "allowed",
        "1782510800", "1782800000", now, &s));
    TEST_ASSERT_FLOAT_WITHIN(0.01, 62.0, s.primary.value);
    TEST_ASSERT_FLOAT_WITHIN(0.01, 31.0, s.secondary.value);
    TEST_ASSERT_EQUAL_STRING("allowed", s.status);
    TEST_ASSERT_EQUAL_STRING("Session", s.primary.label);
    TEST_ASSERT_EQUAL_STRING("Weekly", s.secondary.label);
    TEST_ASSERT_EQUAL(METRIC_PCT, s.primary.kind);
    TEST_ASSERT_EQUAL((1782510800L - now) / 60, s.primary.reset_mins);
    TEST_ASSERT_EQUAL((1782800000L - now) / 60, s.secondary.reset_mins);
    TEST_ASSERT_TRUE(s.primary.present);
    TEST_ASSERT_TRUE(s.secondary.present);
}

static void test_anthropic_missing_headers(void) {
    ProviderSlot s = slot_fixture();
    TEST_ASSERT_FALSE(anthropic_parse_ok("", "", "", "", "", 1782500000L, &s));
    TEST_ASSERT_FALSE(anthropic_parse_ok("garbage", "", "", "", "", 1782500000L, &s));
    TEST_ASSERT_FALSE(s.primary.present);  // slot untouched
}

static void test_anthropic_no_ntp_yet(void) {
    // Before NTP sync time() is near epoch — reset must be unknown, not huge.
    ProviderSlot s = slot_fixture();
    TEST_ASSERT_TRUE(anthropic_parse_ok("0.5", "0.2", "allowed",
        "1782510800", "1782800000", 12345L, &s));
    TEST_ASSERT_EQUAL(-1, s.primary.reset_mins);
    TEST_ASSERT_EQUAL(-1, s.secondary.reset_mins);
}

static void test_anthropic_reset_in_past(void) {
    TEST_ASSERT_EQUAL(0, reset_mins_from_unix("1782000000", 1782500000L));
    TEST_ASSERT_EQUAL(-1, reset_mins_from_unix("", 1782500000L));
    TEST_ASSERT_EQUAL(-1, reset_mins_from_unix("nonsense", 1782500000L));
}

static void test_anthropic_limited(void) {
    ProviderSlot s = slot_fixture();
    long now = 1782500000L;
    // Start from a known-good state, then apply a 429.
    anthropic_parse_ok("0.62", "0.31", "allowed", "1782510800", "1782800000", now, &s);
    anthropic_apply_limited("0.35", "1782512000", now, &s);
    TEST_ASSERT_FLOAT_WITHIN(0.01, 100.0, s.primary.value);   // session forced full
    TEST_ASSERT_FLOAT_WITHIN(0.01, 35.0, s.secondary.value);  // real weekly kept
    TEST_ASSERT_EQUAL_STRING("limited", s.status);
    TEST_ASSERT_EQUAL((1782512000L - now) / 60, s.primary.reset_mins);
}

static void test_anthropic_limited_no_headers(void) {
    // 429 with no headers: keep last-known values, still force session full.
    ProviderSlot s = slot_fixture();
    long now = 1782500000L;
    anthropic_parse_ok("0.62", "0.31", "allowed", "1782510800", "1782800000", now, &s);
    int old_reset = s.primary.reset_mins;
    anthropic_apply_limited("", "", now, &s);
    TEST_ASSERT_FLOAT_WITHIN(0.01, 100.0, s.primary.value);
    TEST_ASSERT_FLOAT_WITHIN(0.01, 31.0, s.secondary.value);
    TEST_ASSERT_EQUAL(old_reset, s.primary.reset_mins);
}

// ---- openrouter ----

static void test_openrouter_capped(void) {
    ProviderSlot s = slot_fixture();
    const char* body =
        "{\"data\":{\"label\":\"sk-or-v1-abc\",\"usage\":12.34,\"limit\":50,"
        "\"is_free_tier\":false}}";
    TEST_ASSERT_TRUE(openrouter_parse_ok(body, &s));
    TEST_ASSERT_EQUAL(METRIC_MONEY, s.primary.kind);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 12.34, s.primary.value);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 50.0, s.primary.limit);
    TEST_ASSERT_EQUAL(-1, s.primary.reset_mins);
    TEST_ASSERT_FALSE(s.secondary.present);
    TEST_ASSERT_EQUAL_STRING("allowed", s.status);
}

static void test_openrouter_uncapped(void) {
    ProviderSlot s = slot_fixture();
    const char* body = "{\"data\":{\"usage\":3.5,\"limit\":null}}";
    TEST_ASSERT_TRUE(openrouter_parse_ok(body, &s));
    TEST_ASSERT_FLOAT_WITHIN(0.001, 3.5, s.primary.value);
    TEST_ASSERT_TRUE(s.primary.limit <= 0.0f);   // uncapped
}

static void test_openrouter_exhausted(void) {
    ProviderSlot s = slot_fixture();
    TEST_ASSERT_TRUE(openrouter_parse_ok("{\"data\":{\"usage\":50.0,\"limit\":50}}", &s));
    TEST_ASSERT_EQUAL_STRING("limited", s.status);
}

static void test_openrouter_bad_body(void) {
    ProviderSlot s = slot_fixture();
    TEST_ASSERT_FALSE(openrouter_parse_ok("{\"error\":\"unauthorized\"}", &s));
    TEST_ASSERT_FALSE(openrouter_parse_ok("", &s));
    TEST_ASSERT_FALSE(s.primary.present);
}

// ---- openai / codex ----
// Fixture captured 2026-07-26 from a live Plus account (phase-0 validation):
// note the PRIMARY window is weekly and secondary_window is null — labels
// must derive from limit_window_seconds.
static const char* WHAM_PLUS =
    "{\"user_id\":\"user-x\",\"plan_type\":\"plus\",\"rate_limit\":{"
    "\"allowed\":true,\"limit_reached\":false,"
    "\"primary_window\":{\"used_percent\":0,\"limit_window_seconds\":604800,"
    "\"reset_after_seconds\":604800,\"reset_at\":1785715900},"
    "\"secondary_window\":null},\"code_review_rate_limit\":null,"
    "\"credits\":{\"has_credits\":false,\"unlimited\":false,\"balance\":\"0\"}}";

static void test_openai_usage_weekly_primary(void) {
    ProviderSlot s = slot_fixture();
    bool limited = true;
    TEST_ASSERT_TRUE(openai_parse_usage(WHAM_PLUS, &s, &limited));
    TEST_ASSERT_FALSE(limited);
    TEST_ASSERT_EQUAL_STRING("Weekly", s.primary.label);
    TEST_ASSERT_FLOAT_WITHIN(0.01, 0.0, s.primary.value);
    TEST_ASSERT_EQUAL(604800 / 60, s.primary.reset_mins);
    TEST_ASSERT_FALSE(s.secondary.present);
    TEST_ASSERT_EQUAL_STRING("allowed", s.status);
}

static void test_openai_usage_two_windows(void) {
    ProviderSlot s = slot_fixture();
    bool limited = false;
    const char* body =
        "{\"rate_limit\":{\"allowed\":true,\"limit_reached\":true,"
        "\"primary_window\":{\"used_percent\":83.5,\"limit_window_seconds\":18000,"
        "\"reset_after_seconds\":3600},"
        "\"secondary_window\":{\"used_percent\":41,\"limit_window_seconds\":604800,"
        "\"reset_after_seconds\":86400}}}";
    TEST_ASSERT_TRUE(openai_parse_usage(body, &s, &limited));
    TEST_ASSERT_TRUE(limited);
    TEST_ASSERT_EQUAL_STRING("Session", s.primary.label);   // 5h window
    TEST_ASSERT_FLOAT_WITHIN(0.01, 83.5, s.primary.value);
    TEST_ASSERT_EQUAL(60, s.primary.reset_mins);
    TEST_ASSERT_TRUE(s.secondary.present);
    TEST_ASSERT_EQUAL_STRING("Weekly", s.secondary.label);
    TEST_ASSERT_FLOAT_WITHIN(0.01, 41.0, s.secondary.value);
    TEST_ASSERT_EQUAL(1440, s.secondary.reset_mins);
    TEST_ASSERT_EQUAL_STRING("limited", s.status);
}

static void test_openai_usage_bad(void) {
    ProviderSlot s = slot_fixture();
    bool limited = false;
    TEST_ASSERT_FALSE(openai_parse_usage("{\"detail\":\"unauthorized\"}", &s, &limited));
    TEST_ASSERT_FALSE(openai_parse_usage(
        "{\"rate_limit\":{\"primary_window\":null}}", &s, &limited));
    TEST_ASSERT_FALSE(s.primary.present);
}

static void test_openai_refresh(void) {
    char at[64], rt[64];
    long exp = 0;
    const char* body =
        "{\"id_token\":\"eyJx.y.z\",\"access_token\":\"eyJa.b.c\","
        "\"refresh_token\":\"rt.1.new\",\"expires_in\":864000,"
        "\"token_type\":\"Bearer\"}";
    TEST_ASSERT_TRUE(openai_parse_refresh(body, at, sizeof(at), rt, sizeof(rt), &exp));
    TEST_ASSERT_EQUAL_STRING("eyJa.b.c", at);
    TEST_ASSERT_EQUAL_STRING("rt.1.new", rt);
    TEST_ASSERT_EQUAL(864000, exp);
    // Missing refresh_token must fail — never overwrite a good RT with junk.
    TEST_ASSERT_FALSE(openai_parse_refresh(
        "{\"access_token\":\"eyJa\",\"expires_in\":10}", at, sizeof(at),
        rt, sizeof(rt), &exp));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_json_number_basic);
    RUN_TEST(test_json_number_null_and_absent);
    RUN_TEST(test_json_number_negative_and_int);
    RUN_TEST(test_json_string);
    RUN_TEST(test_json_key_not_value_match);
    RUN_TEST(test_anthropic_ok_basic);
    RUN_TEST(test_anthropic_missing_headers);
    RUN_TEST(test_anthropic_no_ntp_yet);
    RUN_TEST(test_anthropic_reset_in_past);
    RUN_TEST(test_anthropic_limited);
    RUN_TEST(test_anthropic_limited_no_headers);
    RUN_TEST(test_openrouter_capped);
    RUN_TEST(test_openrouter_uncapped);
    RUN_TEST(test_openrouter_exhausted);
    RUN_TEST(test_openrouter_bad_body);
    RUN_TEST(test_openai_usage_weekly_primary);
    RUN_TEST(test_openai_usage_two_windows);
    RUN_TEST(test_openai_usage_bad);
    RUN_TEST(test_openai_refresh);
    return UNITY_END();
}
