#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <EEPROM.h>

#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <ESP8266HTTPClient.h>
#include <FastLED.h>
#include <ArduinoJson.h>

#include "cmdproc.h"
#include "editline.h"

#include <Arduino.h>

#define printf Serial.printf

// we send the colour to two sets of LEDs: a single LED on pin D2, a star of 7 LEDs on pin D4
#define DATA_PIN_1LED   D2
#define DATA_PIN_7LED   D4

#define SAVEDATA_MAGIC  0xCAFEBABE

#define POLL_INTERVAL           300000
#define LUFTDATEN_TIMEOUT_MS    10000

#define KM_PER_DEGREE   40075.0/360.0

typedef struct {
    char luftdatenid[16];
    bool hasRgbLed;
    uint32_t magic;
} savedata_t;

typedef struct {
    int pm;
    int hue;
} pmlevel_t;

static savedata_t savedata;

static WiFiManager wifiManager;
static WiFiManagerParameter luftdatenIdParam("luftdatenid", "Luftdaten ID", "", sizeof(savedata_t));
static WiFiClientSecure wifiClientSecure;
static WiFiClient wifiClient;

static CRGB leds1[1];
static CRGB leds7[7];
static CHSV color;
static char line[120];

// see https://raw.githubusercontent.com/FastLED/FastLED/gh-pages/images/HSV-rainbow-with-desc.jpg
static const pmlevel_t pmlevels[] = {
    { 0, 160 },                 // blue
    { 25, 96 },                 // green
    { 50, 64 },                 // yellow
    { 100, 0 },                 // red
    { 200, -32 },               // pink
    { -1, 0 }                   // END
};

static void set_led(CRGB crgb)
{
    FastLED.showColor(crgb);
}

static void save_config(void)
{
    EEPROM.put(0, savedata);
    EEPROM.commit();
}

static void save_luftdaten(int id)
{
    snprintf(savedata.luftdatenid, sizeof(savedata.luftdatenid), "%d", id);
    savedata.magic = SAVEDATA_MAGIC;
    save_config();
}

static void wifiManagerCallback(void)
{
    strcpy(savedata.luftdatenid, luftdatenIdParam.getValue());
    savedata.magic = SAVEDATA_MAGIC;

    printf("Saving data to EEPROM: luftdatenid='%s'\n", savedata.luftdatenid);
    save_config();
}

static void show_help(const cmd_t * cmds)
{
    for (const cmd_t * cmd = cmds; cmd->cmd != NULL; cmd++) {
        printf("%10s: %s\n", cmd->name, cmd->help);
    }
}

static int do_help(int argc, char *argv[]);

static bool decode_json(String json, const char *item, float *value, float *latitude,
                        float *longitude)
{
    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, json) != DeserializationError::Ok) {
        return false;
    }

    int meas_num = 0;
    float meas_sum = 0.0;

    // iterate over all measurements
    JsonArray root = doc.as < JsonArray > ();
    for (JsonObject meas:root) {
        // get the particulate matter measurement item
        JsonArray sensordatavalues = meas["sensordatavalues"];
        for (JsonObject sensordatavalue:sensordatavalues) {
            const char *value_type = sensordatavalue["value_type"];
            float value = sensordatavalue["value"];
            if (strcmp(item, value_type) == 0) {
                meas_sum += value;
                meas_num++;
            }
        }
        // get the WGS84 location
        JsonObject location = meas["location"];
        *latitude = location["latitude"];
        *longitude = location["longitude"];
    }

    if (meas_num > 0) {
        *value = meas_sum / meas_num;
        return true;
    }

    return false;
}

static bool fetch_luftdaten(String url, String & response)
{
    HTTPClient httpClient;
    httpClient.begin(wifiClient, url);
    httpClient.setTimeout(LUFTDATEN_TIMEOUT_MS);
    httpClient.setFollowRedirects(true);
    httpClient.setRedirectLimit(3);

    // retry GET a few times until we get a valid HTTP code
    int res = 0;
    for (int i = 0; i < 3; i++) {
        printf("> GET %s\n", url.c_str());
        res = httpClient.GET();
        if (res > 0) {
            break;
        }
    }

    // evaluate result
    bool result = (res == HTTP_CODE_OK);
    response = result ? httpClient.getString() : httpClient.errorToString(res);
    httpClient.end();
    printf("< %d: %s\n", res, response.c_str());
    return result;
}

static bool fetch_sensor(String luftdatenid, String & response)
{
    String url = "http://data.sensor.community/airrohr/v1/sensor/" + luftdatenid + "/";
    return fetch_luftdaten(url, response);
}

static bool fetch_with_filter(String filter, String & response)
{
    String url = "http://data.sensor.community/airrohr/v1/filter/" + filter;
    return fetch_luftdaten(url, response);
}

static bool find_closest(String json, float lat, float lon, int &closest_id)
{
    DynamicJsonDocument doc(10000);
    if (deserializeJson(doc, json) != DeserializationError::Ok) {
        return false;
    }
    // find closest element for PIN 1
    closest_id = -1;
    float closest_d = 1000;
    JsonArray root = doc.as < JsonArray > ();
    for (JsonObject meas:root) {
        JsonObject sensor = meas["sensor"];
        int id = sensor["id"];
        int pin = sensor["pin"];
        if (pin == 1) {
            JsonObject location = meas["location"];
            float latitude = location["latitude"];
            float longitude = location["longitude"];

            float dlon = KM_PER_DEGREE * cos(M_PI * lat / 180.0) * (longitude - lon);
            float dlat = KM_PER_DEGREE * (latitude - lat);
            float d = sqrt(dlon * dlon + dlat * dlat);
            printf("* %5d: %.3f km\n", id, d);
            if (d < closest_d) {
                closest_d = d;
                closest_id = id;
            }
        }
    }
    return (closest_id > 0);
}

static int do_get(int argc, char *argv[])
{
    // perform the GET
    String json;
    if (fetch_sensor(savedata.luftdatenid, json)) {
        // decode it
        float pm = 0.0;
        float lat, lon;
        if (decode_json(json, "P1", &pm, &lat, &lon)) {
            printf("PM avg: %f, lat: %f, lon: %f\n", pm, lat, lon);
            printf("https://maps.luftdaten.info/#14/%.4f/%.4f\n", lat, lon);
        } else {
            printf("JSON decode failed!\n");
            return -1;
        }
    } else {
        printf("GET failed\n");
        return -2;
    }
    return 0;
}

static int do_config(int argc, char *argv[])
{
    if ((argc > 1) && (strcmp(argv[1], "clear") == 0)) {
        printf("Clearing config\n");
        memset(&savedata, 0, sizeof(savedata));
        save_config();
    }

    if ((argc > 3) && (strcmp(argv[1], "set") == 0)) {
        char *item = argv[2];
        char *value = argv[3];
        if (strcmp(item, "id") == 0) {
            int id = atoi(value);
            printf("Setting id to '%d'\n", id);
            save_luftdaten(id);
        }
        if (strcmp(item, "rgb") == 0) {
            bool rgb = (atoi(value) != 0);
            printf("Setting rgb to '%s'\n", rgb ? "true" : "false");
            savedata.hasRgbLed = rgb;
            save_config();
        }
    }

    if ((argc > 1) && (strcmp(argv[1], "auto") == 0)) {
        printf("Attempting autoconfig...\n");
        int id;
        if (autoconfig(id)) {
            printf("OK\n");
            save_luftdaten(id);
        } else {
            printf("FAIL\n");
        }
    }

    printf("config.luftdatenid = %s\n", savedata.luftdatenid);
    printf("config.rgb         = %s\n", savedata.hasRgbLed ? "true" : "false");
    printf("config.magic       = %08X\n", savedata.magic);

    return 0;
}

static CRGB interpolate(float pm, const pmlevel_t table[])
{
    int hue = 0;
    for (const pmlevel_t * pmlevel = table; pmlevel->pm >= 0; pmlevel++) {
        const pmlevel_t *next = pmlevel + 1;
        if ((pm >= pmlevel->pm) && (pm < next->pm)) {
            hue = map(pm, pmlevel->pm, next->pm, pmlevel->hue, next->hue);
            break;
        }
        hue = pmlevel->hue;
    }
    CRGB rgb = CHSV(hue, 255, 255);
    printf("RGB=#%02X%02X%02X\n", rgb.r, rgb.g, rgb.b);
    return rgb;
}

static int do_pm(int argc, char *argv[])
{
    if (argc < 2) {
        return -1;
    }

    float pm = atoi(argv[1]);
    set_led(interpolate(pm, pmlevels));

    return 0;
}

static bool geolocate(float &latitude, float &longitude, float &accuracy)
{
    // scan for networks
    printf("Scanning...");
    int n = WiFi.scanNetworks();
    printf("%d APs found...", n);

    // create JSON request
    DynamicJsonDocument doc(4096);
    doc["considerIp"] = "true";
    JsonArray aps = doc.createNestedArray("wifiAccessPoints");
    int num = 0;
    for (int i = 0; i < n; i++) {
        // skip hidden and "nomap" APs
        if (WiFi.isHidden(i) || WiFi.SSID(i).endsWith("_nomap")) {
            continue;
        }
        JsonObject ap = aps.createNestedObject();
        ap["macAddress"] = WiFi.BSSIDstr(i);
        ap["signalStrength"] = WiFi.RSSI(i);
        if (++num == 20) {
            // limit scan to some arbitrary number to avoid alloc fails later
            break;
        }
    }
    printf("%d APs used\n", num);

    String json;
    serializeJson(doc, json);

    // send JSON with POST
    HTTPClient httpClient;
    httpClient.begin(wifiClientSecure,
                     "https://location.services.mozilla.com/v1/geolocate?key=test");
    httpClient.addHeader("Content-Type", "application/json");
    int res = httpClient.POST(json);
    bool result = (res == HTTP_CODE_OK);
    String response = result ? httpClient.getString() : httpClient.errorToString(res);
    httpClient.end();
    if (res != HTTP_CODE_OK) {
        return false;
    }
    // parse response
    printf("%s\n", response.c_str());
    doc.clear();
    if (deserializeJson(doc, response) != DeserializationError::Ok) {
        printf("Failed to deserialize JSON!\n");
        return false;
    }

    JsonObject location = doc["location"];
    latitude = location["lat"];
    longitude = location["lng"];
    accuracy = doc["accuracy"];

    return true;
}

static bool autoconfig(int &id)
{
    char filter[100];

    // white/gray LED during autoconfig
    set_led(CRGB::Gray);

    // geolocate
    float lat, lon, acc;
    if (!geolocate(lat, lon, acc)) {
        printf("geolocate failed!\n");
        return false;
    }
    // search in increasingly larger area
    for (float radius = 0.1; radius < 30; radius *= sqrt(2.0)) {
        // yield() in a loop, although it's not clear from the documentation if it's needed or not
        yield();

        // fetch nearby sensors
        snprintf(filter, sizeof(filter), "type=HPM,SDS011,PMS7003&area=%.5f,%.5f,%.3f", lat, lon,
                 radius);
        String json;
        if (!fetch_with_filter(filter, json)) {
            printf("fetch_with_filter failed!\n");
            return false;
        }
        // find closest one
        if (find_closest(json, lat, lon, id)) {
            return true;
        }
    }

    return false;
}

static int do_geolocate(int argc, char *argv[])
{
    float latitude, longitude, accuracy;
    bool ok = geolocate(latitude, longitude, accuracy);
    if (!ok) {
        return -1;
    }

    printf("Latitude = %f, Longitude = %f, Accuracy = %f\n", latitude, longitude, accuracy);
    printf("https://google.com/maps/place/%f,%f\n", latitude, longitude);
    return 0;
}

static int do_reboot(int argc, char *argv[])
{
    ESP.restart();
    return 0;
}

const cmd_t commands[] = {
    { "help", do_help, "Show help" },
    { "get", do_get, "GET the PM10 value from Luftdaten" },
    { "config", do_config, "[auto|clear|set] Manipulate configuration of Luftdaten id" },
    { "pm", do_pm, "Simulate PM value and update the LED" },
    { "geo", do_geolocate, "Perform a wifi geo-localisation" },
    { "reboot", do_reboot, "Reboot" },
    { NULL, NULL, NULL }
};

static int do_help(int argc, char *argv[])
{
    show_help(commands);
    return 0;
}

static void animate(void)
{
    int h = 0;
    int s = 255;
    int v = 0;

    // fade in
    for (v = 0; v < 255; v++) {
        set_led(CHSV(h, s, v));
        delay(1);
    }
    // cycle colours
    for (h = 0; h < 255; h++) {
        set_led(CHSV(h, s, v));
        delay(4);
    }
    // fade out
    for (v = 255; v >= 0; v--) {
        set_led(CHSV(h, s, v));
        delay(1);
    }
}

void setup(void)
{
    Serial.begin(115200);
    printf("\nESP-STOFANANAS\n");
    EditInit(line, sizeof(line));

    // init config
    EEPROM.begin(sizeof(savedata));
    EEPROM.get(0, savedata);

    // config led
    if (savedata.hasRgbLed) {
        FastLED.addLeds < WS2812B, DATA_PIN_1LED, RGB > (leds1, 1).setCorrection(TypicalSMD5050);
    } else {
        FastLED.addLeds < WS2812B, DATA_PIN_1LED, GRB > (leds1, 1).setCorrection(TypicalSMD5050);
    }
    FastLED.addLeds < WS2812B, DATA_PIN_7LED, GRB > (leds7, 7).setCorrection(TypicalSMD5050);
    animate();

    // connect to wifi
    printf("Starting WIFI manager ...\n");
    wifiManager.addParameter(&luftdatenIdParam);
    wifiManager.setSaveConfigCallback(wifiManagerCallback);
    wifiManager.setConfigPortalTimeout(120);
    if (savedata.magic == SAVEDATA_MAGIC) {
        wifiManager.autoConnect("ESP-PMLAMP");
    } else {
        wifiManager.startConfigPortal("ESP-PMLAMP");
    }

    // Set geo API wifi client insecure, the geo API requires https but we can't verify the signature
    wifiClientSecure.setInsecure();

    // turn off LED
    set_led(CRGB::Black);
}

void loop(void)
{
    static int num_fetch_failures = 0;
    static int num_decode_failures = 0;

    // fetch a new value every POLL_INTERVAL
    static unsigned int period_last = -1;
    unsigned int period = millis() / POLL_INTERVAL;
    if (period != period_last) {
        period_last = period;
        float pm, lat, lon;
        if (strlen(savedata.luftdatenid) > 0) {
            // fetch and decode JSON
            String json;
            if (fetch_sensor(savedata.luftdatenid, json)) {
                num_fetch_failures = 0;
                if (decode_json(json, "P1", &pm, &lat, &lon)) {
                    num_decode_failures = 0;
                    printf("PM=%f, lat=%f, lon=%f\n", pm, lat, lon);
                    set_led(interpolate(pm, pmlevels));
                } else {
                    if (++num_decode_failures >= 10) {
                        printf("Too many decode failures, reconfiguring ...");
                        strcpy(savedata.luftdatenid, "");
                    }
                }
            } else {
                // reboot if too many fetch failures occur
                if (++num_fetch_failures >= 10) {
                    printf("Too many failures, rebooting ...");
                    ESP.restart();
                }
            }
        } else {
            // try to autoconfig
            int id;
            if (autoconfig(id)) {
                save_luftdaten(id);
                printf("Autoconfig set %d\n", id);
                // trigger fetch
                period_last = -1;
            }
        }
    }
    // parse command line
    bool haveLine = false;
    if (Serial.available()) {
        char c;
        haveLine = EditLine(Serial.read(), &c);
        Serial.write(c);
    }
    if (haveLine) {
        int result = cmd_process(commands, line);
        switch (result) {
        case CMD_OK:
            printf("OK\n");
            break;
        case CMD_NO_CMD:
            break;
        case CMD_UNKNOWN:
            printf("Unknown command, available commands:\n");
            show_help(commands);
            break;
        default:
            printf("%d\n", result);
            break;
        }
        printf(">");
    }
}
