// Host tests for json_lite + PhaseStatus parsing (no hardware needed).

#include <cstdio>
#include <string>

#include "json_lite.h"
#include "phase_state_view.h"

namespace {

int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                        \
            ++failures;                                                 \
        }                                                               \
    } while (0)

const char* kSample =
    "{\"ok\":true,\"phase\":\"zero_done_speaking\",\"phase_version\":7,"
    "\"task_id\":\"\",\"threshold_s\":120,\"prep_elapsed_ms\":1234,"
    "\"last_event_id\":\"e-1\",\"last_source\":\"gpio\","
    "\"last_action\":\"zero_start\",\"last_result\":\"ok\","
    "\"keys\":\"ready\",\"blocked\":false}\n";

}  // namespace

int main() {
    // ---- primitive extraction ----
    std::string s;
    CHECK(json_lite::get_string("{\"a\":\"hello\"}", "a", s) && s == "hello");
    int i = 0;
    CHECK(json_lite::get_int("{\"n\":-42}", "n", i) && i == -42);
    CHECK(json_lite::get_int("{\"n\":0}", "n", i) && i == 0);
    bool b = true;
    CHECK(json_lite::get_bool("{\"flag\":false}", "flag", b) && b == false);
    CHECK(json_lite::get_bool("{\"flag\":true}", "flag", b) && b == true);

    // Missing field.
    CHECK(!json_lite::get_string("{\"a\":\"x\"}", "missing", s));
    // Wrong type.
    CHECK(!json_lite::get_string("{\"a\":12}", "a", s));
    CHECK(!json_lite::get_int("{\"n\":\"12\"}", "n", i));
    // Empty string is a present, empty value.
    s = "sentinel";
    CHECK(json_lite::get_string("{\"a\":\"\"}", "a", s) && s.empty());

    // Escapes.
    CHECK(json_lite::get_string("{\"a\":\"q\\\"b\\\\c\\nd\\te\"}", "a", s));
    CHECK(s == "q\"b\\c\nd\te");
    CHECK(json_lite::get_string("{\"a\":\"\\u4e2d\"}", "a", s));
    CHECK(s == "\xe4\xb8\xad");  // UTF-8 for 中

    // Unknown nested values are skipped, later keys still readable.
    CHECK(json_lite::get_string(
        "{\"obj\":{\"x\":[1,2,{\"y\":\"z\"}]},\"phase\":\"prep_ready\"}",
        "phase", s) &&
        s == "prep_ready");

    // Malformed / truncated / trailing garbage.
    CHECK(!json_lite::get_string("not json", "a", s));
    CHECK(!json_lite::get_string("{\"a\":\"x\"", "a", s));
    CHECK(!json_lite::get_string("{\"a\":\"x\"} garbage", "a", s));
    CHECK(!json_lite::get_int("{\"n\":12x}", "n", i));

    // ---- PhaseStatus parsing ----
    PhaseStatus st;
    CHECK(parse_phase_status(kSample, st));
    CHECK(st.valid);
    CHECK(st.phase == "zero_done_speaking");
    CHECK(st.phase_version == 7);
    CHECK(st.task_id.empty());
    CHECK(st.threshold_s == 120);
    CHECK(st.last_event_id == "e-1");
    CHECK(st.last_source == "gpio");
    CHECK(st.last_action == "zero_start");
    CHECK(st.last_result == "ok");
    CHECK(st.keys == "ready");
    CHECK(st.blocked == false);

    // Minimal response: optional fields may be absent.
    PhaseStatus minimal;
    CHECK(parse_phase_status("{\"phase\":\"preview_ready\",\"phase_version\":0}\n",
                             minimal));
    CHECK(minimal.valid && minimal.phase == "preview_ready" &&
          minimal.phase_version == 0);
    CHECK(minimal.last_event_id.empty());

    // Missing required fields.
    PhaseStatus bad;
    CHECK(!parse_phase_status("{\"phase\":\"preview_ready\"}", bad));
    CHECK(!parse_phase_status("{\"phase_version\":3}", bad));
    CHECK(!parse_phase_status("{\"phase\":3,\"phase_version\":3}", bad));
    CHECK(!parse_phase_status("{\"phase\":\"preview_ready\",\"phase_version\":3",
                              bad));
    CHECK(!bad.valid);

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_json: ok\n");
    return 0;
}
