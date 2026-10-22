#include <unity.h>
#include <cstring>
#include "bike2watch.h"
#include "settings.h"
#include "web_dashboard.h"
#include "update_check.h"

void setUp() {}
void tearDown() {}

void test_parse_too_short() {
    uint8_t data[] = {0x00};
    IndoorBikeData result;
    TEST_ASSERT_FALSE(parseIndoorBikeData(data, 1, result));
}

void test_parse_speed_only() {
    uint8_t data[] = {0x00, 0x00, 0xE8, 0x03};
    IndoorBikeData result;
    TEST_ASSERT_TRUE(parseIndoorBikeData(data, sizeof(data), result));
    TEST_ASSERT_TRUE(result.hasSpeed);
    TEST_ASSERT_EQUAL_UINT16(1000, result.instantSpeedRaw);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, result.speedKmh());
    TEST_ASSERT_FALSE(result.hasCadence);
    TEST_ASSERT_FALSE(result.hasPower);
}

void test_parse_no_speed_when_more_data_flag_set() {
    uint8_t data[] = {0x01, 0x00};
    IndoorBikeData result;
    TEST_ASSERT_TRUE(parseIndoorBikeData(data, sizeof(data), result));
    TEST_ASSERT_FALSE(result.hasSpeed);
}

void test_parse_speed_and_cadence() {
    uint8_t data[] = {0x04, 0x00, 0xC4, 0x09, 0xA0, 0x00};
    IndoorBikeData result;
    TEST_ASSERT_TRUE(parseIndoorBikeData(data, sizeof(data), result));
    TEST_ASSERT_TRUE(result.hasSpeed);
    TEST_ASSERT_EQUAL_UINT16(2500, result.instantSpeedRaw);
    TEST_ASSERT_TRUE(result.hasCadence);
    TEST_ASSERT_EQUAL_UINT16(160, result.instantCadenceRaw);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 80.0f, result.cadenceRpm());
}

void test_parse_speed_cadence_power() {
    uint8_t data[] = {0x44, 0x00, 0xB8, 0x0B, 0xB4, 0x00, 0xC8, 0x00};
    IndoorBikeData result;
    TEST_ASSERT_TRUE(parseIndoorBikeData(data, sizeof(data), result));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 30.0f, result.speedKmh());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 90.0f, result.cadenceRpm());
    TEST_ASSERT_EQUAL_INT16(200, result.instantPower);
}

void test_parse_all_fields() {
    uint8_t data[] = {
        0xFF, 0x00,
        0xD0, 0x07, 0x78, 0x00, 0x76, 0x00,
        0x34, 0x12, 0x00, 0x0A, 0x00, 0x96, 0x00, 0x91, 0x00,
    };
    IndoorBikeData result;
    TEST_ASSERT_TRUE(parseIndoorBikeData(data, sizeof(data), result));
    TEST_ASSERT_FALSE(result.hasSpeed);
    TEST_ASSERT_EQUAL_UINT16(2000, result.avgSpeedRaw);
    TEST_ASSERT_EQUAL_UINT16(120, result.instantCadenceRaw);
    TEST_ASSERT_EQUAL_UINT32(4660, result.totalDistance);
    TEST_ASSERT_EQUAL_INT16(10, result.resistanceLevel);
    TEST_ASSERT_EQUAL_INT16(150, result.instantPower);
    TEST_ASSERT_EQUAL_INT16(145, result.avgPower);
}

void test_parse_truncated_data() {
    uint8_t data[] = {0x04, 0x00, 0xE8, 0x03};
    IndoorBikeData result;
    TEST_ASSERT_FALSE(parseIndoorBikeData(data, sizeof(data), result));
}

void test_csc_zero_input_no_change() {
    CscAccumulator csc;
    updateCsc(csc, 0.0f, 0.0f, 1000);
    TEST_ASSERT_EQUAL_UINT32(0, csc.cumulativeWheelRevs);
    TEST_ASSERT_EQUAL_UINT16(0, csc.cumulativeCrankRevs);
}

void test_csc_cadence_accumulation() {
    CscAccumulator csc;
    updateCsc(csc, 0.0f, 60.0f, 1000);
    TEST_ASSERT_EQUAL_UINT16(1, csc.cumulativeCrankRevs);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, csc.fractionalCrankRevs);
}

void test_csc_cadence_fractional() {
    CscAccumulator csc;
    updateCsc(csc, 0.0f, 80.0f, 1000);
    TEST_ASSERT_EQUAL_UINT16(1, csc.cumulativeCrankRevs);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.333f, csc.fractionalCrankRevs);
    updateCsc(csc, 0.0f, 80.0f, 1000);
    TEST_ASSERT_EQUAL_UINT16(2, csc.cumulativeCrankRevs);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.666f, csc.fractionalCrankRevs);
}

void test_csc_speed_to_wheel_revs() {
    CscAccumulator csc;
    updateCsc(csc, 30.0f, 0.0f, 1000);
    TEST_ASSERT_EQUAL_UINT32(3, csc.cumulativeWheelRevs);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.975f, csc.fractionalWheelRevs);
}

void test_csc_crank_event_time() {
    CscAccumulator csc;
    updateCsc(csc, 0.0f, 60.0f, 1000);
    TEST_ASSERT_EQUAL_UINT16(1024, csc.lastCrankEventTime);
    updateCsc(csc, 0.0f, 60.0f, 1000);
    TEST_ASSERT_EQUAL_UINT16(2048, csc.lastCrankEventTime);
}

void test_csc_counters_wrap() {
    CscAccumulator csc;
    csc.cumulativeCrankRevs = 65535;
    updateCsc(csc, 0.0f, 60.0f, 1000);
    TEST_ASSERT_EQUAL_UINT16(0, csc.cumulativeCrankRevs);
}

void test_build_csc_crank_only() {
    CscAccumulator csc;
    csc.cumulativeCrankRevs = 42;
    csc.lastCrankEventTime = 5120;
    uint8_t buf[16];
    size_t len = buildCscMeasurement(buf, csc, false, true);
    TEST_ASSERT_EQUAL(5, len);
    TEST_ASSERT_EQUAL(0x02, buf[0]);
    TEST_ASSERT_EQUAL(42, buf[1] | (buf[2] << 8));
    TEST_ASSERT_EQUAL(5120, buf[3] | (buf[4] << 8));
}

void test_build_csc_wheel_and_crank() {
    CscAccumulator csc;
    csc.cumulativeWheelRevs = 1000;
    csc.lastWheelEventTime = 2048;
    csc.cumulativeCrankRevs = 100;
    csc.lastCrankEventTime = 4096;
    uint8_t buf[16];
    size_t len = buildCscMeasurement(buf, csc, true, true);
    TEST_ASSERT_EQUAL(11, len);
    TEST_ASSERT_EQUAL(0x03, buf[0]);
    uint32_t wr = buf[1] | (buf[2] << 8) | (buf[3] << 16) | (buf[4] << 24);
    TEST_ASSERT_EQUAL_UINT32(1000, wr);
}

void test_build_cp_measurement() {
    uint8_t buf[8];
    size_t len = buildCpMeasurement(buf, 200);
    TEST_ASSERT_EQUAL(4, len);
    int16_t power = (int16_t)(buf[2] | (buf[3] << 8));
    TEST_ASSERT_EQUAL_INT16(200, power);
}

void test_csc_custom_wheel_circumference() {
    CscAccumulator csc;
    csc.wheelCircumferenceM = 1.0f;  // 1 meter wheel = easy math
    // 3.6 km/h = 1 m/s, so in 1 second we travel 1m = exactly 1 rev
    updateCsc(csc, 3.6f, 0.0f, 1000);
    TEST_ASSERT_EQUAL_UINT32(1, csc.cumulativeWheelRevs);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, csc.fractionalWheelRevs);
}

void test_csc_very_slow_speed_clamps_ticks() {
    CscAccumulator csc;
    csc.wheelCircumferenceM = 2.0f;
    csc.fractionalWheelRevs = 0.99f;
    // 0.01 km/h -> period far above 64 s; must clamp instead of overflowing.
    updateCsc(csc, 0.01f, 0.0f, 60000);
    TEST_ASSERT_TRUE(csc.cumulativeWheelRevs >= 1);
    TEST_ASSERT_EQUAL_UINT16(65535, cscTicksPerRev(1000.0f));
    TEST_ASSERT_EQUAL_UINT16(0, cscTicksPerRev(-1.0f));
    TEST_ASSERT_EQUAL_UINT16(1024, cscTicksPerRev(1.0f));
}

void test_version_prerelease_ordering() {
    TEST_ASSERT_TRUE(isNewerVersion("v1.2.0", "1.2.0-beta.3"));      // release > its beta
    TEST_ASSERT_FALSE(isNewerVersion("v1.2.0-beta.3", "1.2.0"));     // beta < release
    TEST_ASSERT_TRUE(isNewerVersion("v1.2.0-beta.10", "1.2.0-beta.2"));
    TEST_ASSERT_FALSE(isNewerVersion("v1.2.0-beta.2", "1.2.0-beta.10"));
    TEST_ASSERT_FALSE(isNewerVersion("v1.2.0-beta.2", "1.2.0-beta.2"));
    TEST_ASSERT_TRUE(isNewerVersion("v1.3.0-beta.1", "1.2.0"));      // next minor beta > old release
    TEST_ASSERT_TRUE(isNewerVersion("v1.2.0-rc.1", "1.2.0-beta.9")); // alpha order
    TEST_ASSERT_NULL(versionPrerelease("1.2.0"));
    TEST_ASSERT_NULL(versionPrerelease("not-a-version"));
    TEST_ASSERT_EQUAL_STRING("beta.2", versionPrerelease("v1.2.0-beta.2"));
}

void test_mqtt_topic_prefix_validation() {
    TEST_ASSERT_TRUE(isValidMqttTopicPrefix("bike2watch"));
    TEST_ASSERT_TRUE(isValidMqttTopicPrefix("home/bike-2_watch"));
    TEST_ASSERT_FALSE(isValidMqttTopicPrefix(""));
    TEST_ASSERT_FALSE(isValidMqttTopicPrefix(nullptr));
    TEST_ASSERT_FALSE(isValidMqttTopicPrefix("a/#"));
    TEST_ASSERT_FALSE(isValidMqttTopicPrefix("a/+/b"));
    TEST_ASSERT_FALSE(isValidMqttTopicPrefix("a\"b"));
    TEST_ASSERT_FALSE(isValidMqttTopicPrefix("a\\b"));
    TEST_ASSERT_FALSE(isValidMqttTopicPrefix("has space"));
    TEST_ASSERT_FALSE(isValidMqttTopicPrefix("/leading"));
    TEST_ASSERT_FALSE(isValidMqttTopicPrefix("trailing/"));
}

void test_status_json_contains_expected_fields() {
    Bike2WatchStatus status;
    status.bikeConnected = true;
    status.watchConnected = false;
    status.hasSpeed = true;
    status.hasCadence = true;
    status.hasPower = true;
    status.hasResistance = true;
    status.speedKmh = 25.5f;
    status.cadenceRpm = 85.0f;
    status.power = 180;
    status.resistanceLevel = 12;
    status.canSetResistance = true;
    status.resistanceMin = -5.0f;
    status.resistanceMax = 25.0f;
    status.resistanceStep = 0.5f;
    status.wheelRevs = 12345;
    status.crankRevs = 678;
    status.uptimeSec = 90;
    status.deviceName = "Bike2Watch";
    status.wifiMode = "WiFi";
    status.ipAddress = "192.168.1.42";
    status.mqttEnabled = true;
    status.mqttConnected = false;
    status.pinnedDeviceMissing = true;
    status.pinnedDeviceName = "Kickr Bike";

    char buf[512];
    size_t len = buildStatusJson(status, buf, sizeof(buf));

    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"bikeConnected\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"watchConnected\":false"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"speedKmh\":25.50"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"cadenceRpm\":85.0"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"power\":180"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"hasResistance\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"resistanceLevel\":12"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"canSetResistance\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"resistanceMin\":-5.0"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"resistanceMax\":25.0"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"resistanceStep\":0.5"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"wheelRevs\":12345"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"crankRevs\":678"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"uptimeSec\":90"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"deviceName\":\"Bike2Watch\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"wifiMode\":\"WiFi\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ipAddress\":\"192.168.1.42\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"mqttEnabled\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"mqttConnected\":false"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"pinnedDeviceMissing\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"pinnedDeviceName\":\"Kickr Bike\""));
}

// Default (unpinned, plug & play) Bike2WatchStatus must report the new fields
// as "not missing" / empty — a fresh dashboard with nothing configured
// should never show the "device not found" warning.
void test_status_json_defaults_no_pin_warning() {
    Bike2WatchStatus status;
    char buf[512];
    size_t len = buildStatusJson(status, buf, sizeof(buf));

    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"pinnedDeviceMissing\":false"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"pinnedDeviceName\":\"\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"hasResistance\":false"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"resistanceLevel\":0"));
    // No bike connected -> no Control Point either, regardless of the
    // (unused while false) fallback range fields.
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"canSetResistance\":false"));
}

// pinnedDeviceName is sourced from a BLE advertisement — untrusted input.
// A nearby device advertising a name containing '"' or '\' must not be
// able to break the JSON response.
void test_status_json_escapes_device_name() {
    Bike2WatchStatus status;
    status.pinnedDeviceMissing = true;
    status.pinnedDeviceName = "Evil\"Bike\\Name";

    char buf[512];
    size_t len = buildStatusJson(status, buf, sizeof(buf));

    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"pinnedDeviceName\":\"Evil\\\"Bike\\\\Name\""));
}

void test_status_json_too_small_buffer_returns_zero() {
    Bike2WatchStatus status;
    char buf[4];
    size_t len = buildStatusJson(status, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(0, len);
}

// ── Firmware update: version comparison ──────────────────────────────
void test_version_newer_patch() {
    TEST_ASSERT_TRUE(isNewerVersion("v0.1.5", "0.1.4"));
}

void test_version_newer_minor() {
    TEST_ASSERT_TRUE(isNewerVersion("v0.2.0", "0.1.9"));
}

void test_version_newer_major() {
    TEST_ASSERT_TRUE(isNewerVersion("v1.0.0", "0.9.9"));
}

void test_version_equal_not_newer() {
    TEST_ASSERT_FALSE(isNewerVersion("v0.1.4", "0.1.4"));
}

void test_version_older_not_newer() {
    TEST_ASSERT_FALSE(isNewerVersion("v0.1.0", "0.1.4"));
    TEST_ASSERT_FALSE(isNewerVersion("v0.0.9", "0.1.4"));
}

void test_version_leading_v_stripped() {
    TEST_ASSERT_FALSE(isNewerVersion("v1.2.0", "1.2.0"));
    TEST_ASSERT_TRUE(isNewerVersion("V1.2.1", "1.2.0"));
}

void test_version_missing_patch_defaults_zero() {
    TEST_ASSERT_TRUE(isNewerVersion("v1.2", "1.1.9"));
    TEST_ASSERT_FALSE(isNewerVersion("v1.2", "1.2.0"));
}

void test_version_garbage_not_newer() {
    TEST_ASSERT_FALSE(isNewerVersion("not-a-version", "0.1.4"));
    TEST_ASSERT_FALSE(isNewerVersion("", "0.1.4"));
    TEST_ASSERT_FALSE(isNewerVersion(nullptr, "0.1.4"));
}

// ── Firmware update: bounded JSON field extraction ────────────────────
void test_extract_json_string_field_basic() {
    const char json[] = "{\"id\":1,\"tag_name\":\"v1.2.0\",\"name\":\"Release 1.2.0\"}";
    char out[32];
    TEST_ASSERT_TRUE(extractJsonStringField(json, strlen(json), "tag_name", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("v1.2.0", out);
}

void test_extract_json_string_field_with_whitespace_after_colon() {
    const char json[] = "{\"tag_name\": \"v1.2.0\"}";
    char out[32];
    TEST_ASSERT_TRUE(extractJsonStringField(json, strlen(json), "tag_name", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("v1.2.0", out);
}

void test_extract_json_string_field_missing_key_returns_false() {
    const char json[] = "{\"tag_name\":\"v1.2.0\"}";
    char out[32] = "unchanged";
    TEST_ASSERT_FALSE(extractJsonStringField(json, strlen(json), "body", out, sizeof(out)));
}

void test_extract_json_string_field_truncated_value_still_copies_partial() {
    // Simulates web_server.cpp's bounded ~8KB read of GitHub's response
    // being cut off mid-value (most relevant to the possibly-huge "body"
    // field) -- the caller still gets back whatever was captured.
    const char json[] = "{\"body\":\"Lorem ipsum dolor sit amet, this release note got cu";
    char out[16];
    TEST_ASSERT_TRUE(extractJsonStringField(json, strlen(json), "body", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Lorem ipsum dol", out);
}

void test_extract_json_string_field_unescapes_quotes_and_backslashes() {
    const char json[] = "{\"body\":\"She said \\\"hi\\\" then C:\\\\path\"}";
    char out[64];
    TEST_ASSERT_TRUE(extractJsonStringField(json, strlen(json), "body", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("She said \"hi\" then C:\\path", out);
}

// ── Firmware update: picking the right release asset for this board ──
void test_find_asset_download_url_picks_matching_board() {
    const char json[] =
        "{\"name\":\"esp32-s3-devkitc-1 mentioned here too\","
        "\"assets\":["
        "{\"name\":\"bike2watch-esp32dev-v1.2.0.bin\","
        "\"browser_download_url\":\"https://example.com/esp32dev.bin\"},"
        "{\"name\":\"bike2watch-esp32-s3-devkitc-1-v1.2.0.bin\","
        "\"browser_download_url\":\"https://example.com/s3.bin\"}"
        "]}";
    char out[128];

    TEST_ASSERT_TRUE(findAssetDownloadUrl(json, strlen(json), "esp32dev", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://example.com/esp32dev.bin", out);

    TEST_ASSERT_TRUE(findAssetDownloadUrl(json, strlen(json), "esp32-s3-devkitc-1", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://example.com/s3.bin", out);
}

void test_find_asset_download_url_ignores_top_level_release_name() {
    // The release's own top-level "name" (its title, before "assets")
    // mentions "esp32dev" -- must not be mistaken for an asset match.
    const char json[] =
        "{\"name\":\"esp32dev nightly build\","
        "\"assets\":[{\"name\":\"bike2watch-esp32-s3-devkitc-1-v1.2.0.bin\","
        "\"browser_download_url\":\"https://example.com/s3.bin\"}]}";
    char out[128];
    TEST_ASSERT_FALSE(findAssetDownloadUrl(json, strlen(json), "esp32dev", out, sizeof(out)));
}

void test_find_asset_download_url_no_match_returns_false() {
    const char json[] =
        "{\"assets\":[{\"name\":\"bike2watch-esp32dev-v1.2.0.bin\","
        "\"browser_download_url\":\"https://example.com/esp32dev.bin\"}]}";
    char out[128] = "unchanged";
    TEST_ASSERT_FALSE(findAssetDownloadUrl(json, strlen(json), "esp32-s3-devkitc-1", out, sizeof(out)));
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_too_short);
    RUN_TEST(test_parse_speed_only);
    RUN_TEST(test_parse_no_speed_when_more_data_flag_set);
    RUN_TEST(test_parse_speed_and_cadence);
    RUN_TEST(test_parse_speed_cadence_power);
    RUN_TEST(test_parse_all_fields);
    RUN_TEST(test_parse_truncated_data);
    RUN_TEST(test_csc_zero_input_no_change);
    RUN_TEST(test_csc_cadence_accumulation);
    RUN_TEST(test_csc_cadence_fractional);
    RUN_TEST(test_csc_speed_to_wheel_revs);
    RUN_TEST(test_csc_very_slow_speed_clamps_ticks);
    RUN_TEST(test_version_prerelease_ordering);
    RUN_TEST(test_mqtt_topic_prefix_validation);
    RUN_TEST(test_csc_crank_event_time);
    RUN_TEST(test_csc_counters_wrap);
    RUN_TEST(test_build_csc_crank_only);
    RUN_TEST(test_build_csc_wheel_and_crank);
    RUN_TEST(test_build_cp_measurement);
    RUN_TEST(test_csc_custom_wheel_circumference);
    RUN_TEST(test_status_json_contains_expected_fields);
    RUN_TEST(test_status_json_defaults_no_pin_warning);
    RUN_TEST(test_status_json_escapes_device_name);
    RUN_TEST(test_status_json_too_small_buffer_returns_zero);
    RUN_TEST(test_version_newer_patch);
    RUN_TEST(test_version_newer_minor);
    RUN_TEST(test_version_newer_major);
    RUN_TEST(test_version_equal_not_newer);
    RUN_TEST(test_version_older_not_newer);
    RUN_TEST(test_version_leading_v_stripped);
    RUN_TEST(test_version_missing_patch_defaults_zero);
    RUN_TEST(test_version_garbage_not_newer);
    RUN_TEST(test_extract_json_string_field_basic);
    RUN_TEST(test_extract_json_string_field_with_whitespace_after_colon);
    RUN_TEST(test_extract_json_string_field_missing_key_returns_false);
    RUN_TEST(test_extract_json_string_field_truncated_value_still_copies_partial);
    RUN_TEST(test_extract_json_string_field_unescapes_quotes_and_backslashes);
    RUN_TEST(test_find_asset_download_url_picks_matching_board);
    RUN_TEST(test_find_asset_download_url_ignores_top_level_release_name);
    RUN_TEST(test_find_asset_download_url_no_match_returns_false);
    return UNITY_END();
}
