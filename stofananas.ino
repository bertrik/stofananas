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

typedef struct {
    bool hasRgbLed;
    uint32_t magic;
} savedata_t;

typedef struct {
    int pm;
    int hue;
} pmlevel_t;

static savedata_t savedata;

static WiFiManager wifiManager;
static WiFiClient wifiClient;
static WiFiClientSecure secureWifiClient;
static char espid[64];

static CRGB leds1[1];
static CRGB leds7[7];
static CRGB color;
static char line[120];
static int num_fetch_failures = 0;
static float latitude, longitude, accuracy;
static bool have_location = false;

// see https://raw.githubusercontent.com/FastLED/FastLED/gh-pages/images/HSV-rainbow-with-desc.jpg
static const pmlevel_t pmlevels[] = {
    { 0, 160 },                 // blue
    { 15, 96 },                 // green
    { 30, 64 },                 // yellow
    { 60, 0 },                  // red
    { 120, -32 },               // pink
    { -1, 0 }                   // END
};

static void set_led(CRGB crgb)
{
    // remember last color
    color = crgb;
    // update on the hardware
    FastLED.showColor(color);
}

static void save_config(void)
{
    savedata.magic = SAVEDATA_MAGIC;
    EEPROM.put(0, savedata);
    EEPROM.commit();
}

static void show_help(const cmd_t * cmds)
{
    for (const cmd_t * cmd = cmds; cmd->cmd != NULL; cmd++) {
        printf("%10s: %s\n", cmd->name, cmd->help);
    }
}

static int do_help(int argc, char *argv[]);

static bool fetch_url(const char *host, int port, const char *path, String & response)
{
    HTTPClient httpClient;
    httpClient.begin(wifiClient, host, port, path, false);
    httpClient.setTimeout(20000);
    httpClient.setUserAgent(espid);

    printf("> GET http://%s:%d%s\n", host, port, path);
    int res = httpClient.GET();

    // evaluate result
    bool result = (res == HTTP_CODE_OK);
    response = result ? httpClient.getString() : httpClient.errorToString(res);
    httpClient.end();
    printf("< %d: %s\n", res, response.c_str());
    return result;
}

static bool fetch_pm(double latitude, double longitude, const char *item, double &value)
{
    DynamicJsonDocument doc(1024);
    String response;
    char path[128];

    // fetch
    sprintf(path, "/air/%.6f/%.6f", latitude, longitude);
    if (fetch_url("stofradar.nl", 9000, path, response)) {
        // decode
        if (deserializeJson(doc, response) == DeserializationError::Ok) {
            value = doc[item];
            return true;
        }
    }
    return false;
}

static int do_get(int argc, char *argv[])
{
    double pm2_5;
    if (fetch_pm(latitude, longitude, "pm2.5", pm2_5)) {
        set_led(interpolate(pm2_5, pmlevels));
    } else {
        printf("fetch_pm failed!\n");
        return -1;
    }
    return CMD_OK;
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
        if (strcmp(item, "rgb") == 0) {
            bool rgb = (atoi(value) != 0);
            printf("Setting rgb to '%s'\n", rgb ? "true" : "false");
            savedata.hasRgbLed = rgb;
            save_config();
        }
    }

    printf("config.rgb         = %s\n", savedata.hasRgbLed ? "true" : "false");
    printf("config.magic       = %08X\n", savedata.magic);
    return CMD_OK;
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

    return CMD_OK;
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
    httpClient.begin(secureWifiClient,
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

static int do_geolocate(int argc, char *argv[])
{
    float latitude, longitude, accuracy;
    bool ok = geolocate(latitude, longitude, accuracy);
    if (!ok) {
        return -1;
    }

    printf("Latitude = %f, Longitude = %f, Accuracy = %f\n", latitude, longitude, accuracy);
    printf("https://google.com/maps/place/%f,%f\n", latitude, longitude);
    return CMD_OK;
}

static int do_reboot(int argc, char *argv[])
{
    ESP.restart();
    return CMD_OK;
}

static int do_error(int argc, char *argv[])
{
    if (argc > 1) {
        num_fetch_failures = atoi(argv[1]);
    }
    printf("fetch failures:%d\n", num_fetch_failures);
    return CMD_OK;
}

static int do_led(int argc, char *argv[])
{
    if (argc < 2) {
        return -1;
    }
    int rgb = strtoul(argv[1], NULL, 16);
    set_led(rgb);

    return CMD_OK;
}

const cmd_t commands[] = {
    { "help", do_help, "Show help" },
    { "get", do_get, "[id] GET the PM2.5 value from stofradar.nl" },
    { "config", do_config, "[clear|set] Manipulate configuration" },
    { "pm", do_pm, "<pm> Simulate PM value and update the LED" },
    { "geo", do_geolocate, "Perform a wifi geo-localisation" },
    { "reboot", do_reboot, "Reboot" },
    { "error", do_error, "[fetch] [decode] Simulate a fetch/decode error" },
    { "led", do_led, "<RRGGBB> Set the LED to a specific value (hex)" },
    { NULL, NULL, NULL }
};

static int do_help(int argc, char *argv[])
{
    show_help(commands);
    return CMD_OK;
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
    snprintf(espid, sizeof(espid), "esp8266-pmlamp-%06x", ESP.getChipId());

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

    // Set geo API wifi client insecure, the geo API requires https but we can't verify the signature
    secureWifiClient.setInsecure();

    // indicate white during config
    set_led(CRGB::Gray);

    // connect to wifi
    printf("Starting WIFI manager (%s)...\n", WiFi.SSID().c_str());
    wifiManager.setConfigPortalTimeout(120);
    wifiManager.autoConnect("ESP-PMLAMP");
}

void loop(void)
{
    // fetch a new value every POLL_INTERVAL
    static unsigned int period_last = -1;
    unsigned int period = millis() / POLL_INTERVAL;
    if (period != period_last) {
        period_last = period;
        if (!have_location) {
            // try to determine location
            have_location = geolocate(latitude, longitude, accuracy);
        }
        if (have_location) {
            // fetch PM and update LED
            String json;
            double pm2_5;
            if (fetch_pm(latitude, longitude, "pm2.5", pm2_5)) {
                num_fetch_failures = 0;
                printf("PM2.5=%f, lat=%f, lon=%f\n", pm2_5, latitude, longitude);
                set_led(interpolate(pm2_5, pmlevels));
            } else {
                num_fetch_failures++;
                printf("Fetch failure %d\n", num_fetch_failures);
                // reboot if too many fetch failures occur
                if (num_fetch_failures > 10) {
                    printf("Too many failures, rebooting ...");
                    ESP.restart();
                }
            }
        }
    }

    // flash LED when there is an error
    if (num_fetch_failures > 0) {
        bool flash = (millis() / 500) % 2;

        // update on the hardware
        FastLED.showColor(color, flash ? 200 : 255);
    }

    // parse command line
    while (Serial.available()) {
        char c = Serial.read();
        bool haveLine = EditLine(c, &c);
        Serial.write(c);
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
}

