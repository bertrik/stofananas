#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <EEPROM.h>

#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <ESP8266HTTPClient.h>
#include <FastLED.h>
#include <ArduinoJson.h>

#include "print.h"
#include "cmdproc.h"
#include "editline.h"

#include <Arduino.h>

#define DATA_PIN    D2

#define SAVEDATA_MAGIC  0xCAFEBABE

typedef struct {
    char luftdatenid[16];
    uint32_t magic;
} savedata_t;

typedef struct {
    int pm;
    int hue;
} pmlevel_t;

static savedata_t savedata;
static char esp_id[16];

static WiFiManager wifiManager;
static WiFiManagerParameter luftdatenIdParam("luftdatenid", "Luftdaten ID", "", sizeof(savedata_t));

static CRGB led;
static CHSV color;
static char line[120];

// see https://raw.githubusercontent.com/FastLED/FastLED/gh-pages/images/HSV-rainbow-with-desc.jpg
static const pmlevel_t pmlevels[] = {
    { 0, 160 },     // blue
    { 25, 96 },     // green
    { 50, 64 },     // yellow
    { 100, 0 },     // red
    { 200, -32 },   // pink
    { -1, 0 }     // END
};

static void wifiManagerCallback(void)
{
    strcpy(savedata.luftdatenid, luftdatenIdParam.getValue());
    savedata.magic = SAVEDATA_MAGIC;

    print("Saving data to EEPROM: luftdatenid='%s'\n", savedata.luftdatenid);
    EEPROM.put(0, savedata);
    EEPROM.commit();
}

void setup(void)
{
    Serial.begin(115200);
    print("\nESP-STOFANANAS\n");
    EditInit(line, sizeof(line));

    // init config
    EEPROM.begin(sizeof(savedata));
    EEPROM.get(0, savedata);

    // get ESP id
    sprintf(esp_id, "%08X", ESP.getChipId());
    print("ESP ID: %s\n", esp_id);

    // config led
    FastLED.addLeds < PL9823, DATA_PIN > (&led, 1);
    led = CRGB::Yellow;
    FastLED.show();

    // connect to wifi
    print("Starting WIFI manager ...\n");
    wifiManager.addParameter(&luftdatenIdParam);
    wifiManager.setSaveConfigCallback(wifiManagerCallback);
    wifiManager.setConfigPortalTimeout(120);
    if (savedata.magic == SAVEDATA_MAGIC) {
        wifiManager.autoConnect("ESP-STOFANANAS");
    } else {
        wifiManager.startConfigPortal("ESP-STOFANANAS");
    }

    // turn off LED
    led = CRGB::Black;
    FastLED.show();
}

static void show_help(const cmd_t * cmds)
{
    for (const cmd_t * cmd = cmds; cmd->cmd != NULL; cmd++) {
        print("%10s: %s\n", cmd->name, cmd->help);
    }
}

static int do_help(int argc, char *argv[]);

static bool decode_json(String json, const char *item, float *value)
{
    static DynamicJsonDocument doc(4096);
    deserializeJson(doc, json);

    int meas_num = 0;
    float meas_sum = 0.0;

    // iterate over all measurements
    JsonArray root = doc.as < JsonArray > ();
    for (JsonObject meas:root) {
        JsonArray sensordatavalues = meas["sensordatavalues"];

        // iterate over all sensor data values (P0, P1, P2, etc)
        for (JsonObject sensordatavalue:sensordatavalues) {
            const char *value_type = sensordatavalue["value_type"];
            float value = sensordatavalue["value"];
            if (strcmp(item, value_type) == 0) {
                meas_sum += value;
                meas_num++;
            }
        }
    }

    if (meas_num > 0) {
        *value = meas_sum / meas_num;
        return true;
    }

    return false;
}

static bool fetch_luftdaten(String url, String &response)
{
    // perform the GET
    WiFiClient wifiClient;
    HTTPClient httpClient;
    httpClient.begin(wifiClient, url);
    int res = httpClient.GET();
    bool result = (res == HTTP_CODE_OK);
    response = result ? httpClient.getString() : httpClient.errorToString(res);
    httpClient.end();

    return result;
}

static bool fetch_sensor(String luftdatenid, String &response)
{
    String url = "http://api.luftdaten.info/v1/sensor/" + luftdatenid + "/";
    return fetch_luftdaten(url, response);
}

static bool fetch_with_filter(String filter, String &response)
{
    String url = "http://api.luftdaten.info/v1/filter/" + filter;
    return fetch_luftdaten(url, response);
}

static int do_get(int argc, char *argv[])
{
    // perform the GET
    String json;
    if (fetch_sensor(savedata.luftdatenid, json)) {
        // decode it
        float pm = 0.0;
        if (decode_json(json, "P1", &pm)) {
            print("PM average: %f\n", pm);
        } else {
            print("JSON decode failed!\n");
            return -1;
        }
    } else {
        print("GET failed\n");
        return -2;
    }
    return 0;
}

static int do_config(int argc, char *argv[])
{
    if ((argc > 1) && (strcmp(argv[1], "clear") == 0)) {
        print("Clearing config\n");
        savedata.magic = 0;
        EEPROM.put(0, savedata);
        EEPROM.commit();
    }

    if ((argc > 2) && (strcmp(argv[1], "set") == 0)) {
        char *id = argv[2];
        print("Setting id to '%s'\n", id);
        strcpy(savedata.luftdatenid, id);
        savedata.magic = SAVEDATA_MAGIC;
        EEPROM.put(0, savedata);
        EEPROM.commit();
    }

    if ((argc > 1) && (strcmp(argv[1], "auto") == 0)) {
        print("Auto-determining luftdaten id...\n");
        float lat, lon, acc;
        String json;
        if (geolocate(lat, lon, acc)) {
            char filter[64];
            snprintf(filter, sizeof(filter), "area=%f,%f,0.1", lat, lon);
            if (fetch_with_filter(filter, json)) {
                Serial.println(json);
                DynamicJsonDocument doc(4096);
                deserializeJson(doc, json);

                // find first element for PIN 1
                JsonArray root = doc.as < JsonArray > ();
                for (JsonObject meas:root) {
                    JsonObject sensor = meas["sensor"];
                    int pin = sensor["pin"];
                    if (pin == 1) {
                        String id = sensor["id"];
                        strcpy(savedata.luftdatenid, id.c_str());
                        savedata.magic = SAVEDATA_MAGIC;
                        EEPROM.put(0, savedata);
                        EEPROM.commit();
                        break;
                    }
                }
            }
        }
    }

    print("config.luftdatenid = %s\n", savedata.luftdatenid);
    print("config.magic       = %08X\n", savedata.magic);

    return 0;
}

static CHSV interpolate(float pm, const pmlevel_t table[])
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
    return CHSV(hue, 255, 255);
}

static int do_pm(int argc, char *argv[])
{
    if (argc < 2) {
        return -1;
    }

    float pm = atoi(argv[1]);
    color = interpolate(pm, pmlevels);
//    print("pm=%d => color = #%02X%02X%02X\n", (int) pm, color.r, color.g, color.b);

    return 0;
}

static bool geolocate(float &latitude, float &longitude, float &accuracy)
{
    // scan for networks
    Serial.print("Scanning...");
    int n = WiFi.scanNetworks();
    Serial.printf("%d networks found\n", n);

    // create JSON request
    DynamicJsonDocument doc(4096);
    doc["considerIp"] = "true";
    JsonArray aps = doc.createNestedArray("wifiAccessPoints");
    for (int i = 0; i < n; i++) {
        JsonObject ap = aps.createNestedObject();
        ap["macAddress"] = WiFi.BSSIDstr(i);
        ap["signalStrength"] = WiFi.RSSI(i);
    }
    String json;
    serializeJson(doc, json);

    // perform a POST request
    Serial.println(json);
    WiFiClientSecure wifiClient;
    wifiClient.setInsecure();
    HTTPClient httpClient;
    httpClient.begin(wifiClient, "https://location.services.mozilla.com/v1/geolocate?key=test");
    httpClient.addHeader("Content-Type", "application/json");
    int res = httpClient.POST(json);
    bool result = (res == HTTP_CODE_OK);
    String response = result ? httpClient.getString() : httpClient.errorToString(res);
    httpClient.end();
    if (res != HTTP_CODE_OK) {
        return false;
    }

    // parse response
    Serial.println(response);
    doc.clear();
    DeserializationError err = deserializeJson(doc, response);
    if (err != DeserializationError::Ok) {
        Serial.print("Failed to deserialize JSON!\n");
        return false;
    }

    JsonObject location = doc["location"];
    accuracy = doc["accuracy"];
    latitude = location["lat"];
    longitude = location["lng"];

    return true;
}

static int do_geolocate(int argc, char *argv[])
{
    float latitude, longitude, accuracy;
    bool ok = geolocate(latitude, longitude, accuracy);
    if (ok) {
        Serial.printf("Latitude = %f, Longitude = %f, Accuracy = %f\n", latitude, longitude, accuracy);
    }

    return ok ? 0 : -1;
}

const cmd_t commands[] = {
    { "help", do_help, "Show help" },
    { "get", do_get, "GET the PM10 value from Luftdaten" },
    { "config", do_config, "[auto|clear|set] Manipulate configuration of Luftdaten id" },
    { "pm", do_pm, "Simulate PM value and update the LED" },
    { "geo", do_geolocate, "Perform a wifi geo-localisation" },
    { NULL, NULL, NULL }
};

static int do_help(int argc, char *argv[])
{
    show_help(commands);
    return 0;
}

#define POLL_INTERVAL 300000

void loop(void)
{
    // fetch a new value every POLL_INTERVAL
    static unsigned int period_last = -1;
    unsigned int period = millis() / POLL_INTERVAL;
    if (period != period_last) {
        period_last = period;
        String json;
        float pm;
        if (fetch_sensor(savedata.luftdatenid, json) && decode_json(json, "P1", &pm)) {
            print("PM=%f\n", pm);
            color = interpolate(pm, pmlevels);
        }
    }

    // show on LED
    led = color;
    FastLED.show();

    // parse command line
    bool haveLine = false;
    if (Serial.available()) {
        char c;
        haveLine = EditLine(Serial.read(), &c);
        Serial.print(c);
    }
    if (haveLine) {
        int result = cmd_process(commands, line);
        switch (result) {
        case CMD_OK:
            print("OK\n");
            break;
        case CMD_NO_CMD:
            break;
        case CMD_UNKNOWN:
            print("Unknown command, available commands:\n");
            show_help(commands);
            break;
        default:
            print("%d\n", result);
            break;
        }
        print(">");
    }
}

