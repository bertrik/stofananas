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
#include <ArduinoOTA.h>

#include "print.h"
#include "cmdproc.h"
#include "editline.h"

#include <Arduino.h>

#define DATA_PIN_1LED   D2
#define DATA_PIN_7LED   D4

#define SAVEDATA_MAGIC  0xCAFEBABE

#define POLL_INTERVAL 300000


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
    // we send the colour to two sets of LEDs: a single LED on pin D2, a star of 7 LEDs on pin D4
    fill_solid(leds1, 1, crgb);
    fill_solid(leds7, 7, crgb);
    FastLED.show();

    // turn off built-in ESP8266 blue LED on pin D4
    digitalWrite(D4, 1);
}

static void save_luftdaten(int id)
{
    snprintf(savedata.luftdatenid, sizeof(savedata.luftdatenid), "%d", id);
    savedata.magic = SAVEDATA_MAGIC;
    EEPROM.put(0, savedata);
    EEPROM.commit();
}

static void wifiManagerCallback(void)
{
    strcpy(savedata.luftdatenid, luftdatenIdParam.getValue());
    savedata.magic = SAVEDATA_MAGIC;

    print("Saving data to EEPROM: luftdatenid='%s'\n", savedata.luftdatenid);
    EEPROM.put(0, savedata);
    EEPROM.commit();
}

static void show_help(const cmd_t * cmds)
{
    for (const cmd_t * cmd = cmds; cmd->cmd != NULL; cmd++) {
        print("%10s: %s\n", cmd->name, cmd->help);
    }
}

static int do_help(int argc, char *argv[]);

static bool decode_json(String json, const char *item, float *value, float *latitude, float *longitude)
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
    Serial.printf("> %s\n", url.c_str());

    WiFiClient wifiClient;
    HTTPClient httpClient;
    httpClient.begin(wifiClient, url);
    int res = httpClient.GET();
    bool result = (res == HTTP_CODE_OK);
    response = result ? httpClient.getString() : httpClient.errorToString(res);
    httpClient.end();

    Serial.printf("< %s\n", response.c_str());
    return result;
}

static bool fetch_sensor(String luftdatenid, String & response)
{
    String url = "http://api.luftdaten.info/v1/sensor/" + luftdatenid + "/";
    return fetch_luftdaten(url, response);
}

static bool fetch_with_filter(String filter, String & response)
{
    String url = "http://api.luftdaten.info/v1/filter/" + filter;
    return fetch_luftdaten(url, response);
}

static bool find_closest(String json, float lat, float lon, int &id)
{
    DynamicJsonDocument doc(10000);
    if (deserializeJson(doc, json) != DeserializationError::Ok) {
        return false;
    }
    // find closest element for PIN 1
    id = -1;
    float min_d2 = 1000;
    JsonArray root = doc.as < JsonArray > ();
    for (JsonObject meas:root) {
        JsonObject sensor = meas["sensor"];
        int pin = sensor["pin"];
        if (pin == 1) {
            JsonObject location = meas["location"];
            float latitude = location["latitude"];
            float longitude = location["longitude"];

            float dlon = cos(M_PI * lat / 180.0) * (longitude - lon);
            float dlat = (latitude - lat);
            float d2 = dlon * dlon + dlat * dlat;
            if (d2 < min_d2) {
                min_d2 = d2;
                id = sensor["id"];
            }
        }
    }
    return (id > 0);
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
            print("PM avg: %f, lat: %f, lon: %f\n", pm, lat, lon);
            print("https://maps.luftdaten.info/#14/%f/%f\n", lat, lon);
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
        memset(&savedata, 0, sizeof(savedata));
        EEPROM.put(0, savedata);
        EEPROM.commit();
    }

    if ((argc > 2) && (strcmp(argv[1], "set") == 0)) {
        int id = atoi(argv[2]);
        print("Setting id to '%d'\n", id);
        save_luftdaten(id);
    }

    if ((argc > 1) && (strcmp(argv[1], "auto") == 0)) {
        print("Attempting autoconfig...");
        int id;
        if (autoconfig(id)) {
            print("OK\n");
            save_luftdaten(id);
        } else {
            print("FAIL\n");
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
    set_led(interpolate(pm, pmlevels));
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

    // send JSON with POST, insecure because we can't verify the certificate
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
    if (deserializeJson(doc, response) != DeserializationError::Ok) {
        Serial.print("Failed to deserialize JSON!\n");
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
    // geolocate
    float lat, lon, acc;
    if (!geolocate(lat, lon, acc)) {
        print("geolocate failed!\n");
        return false;
    }
    // search in increasingly large area
    for (int i = 0; i < 10; i++, acc *= 2) {
        // yield() in a loop, although it's not clear from the documentation if it's needed or not
        yield();

        // fetch nearby sensors
        char filter[64];
        snprintf(filter, sizeof(filter), "area=%f,%f,%f", lat, lon, acc / 1000);
        String json;
        if (!fetch_with_filter(filter, json)) {
            print("fetch_with_filter failed!\n");
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

    Serial.printf("Latitude = %f, Longitude = %f, Accuracy = %f\n", latitude, longitude, accuracy);
    Serial.printf("http://google.com/maps/place/%f,%f\n", latitude, longitude);
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
    print("\nESP-STOFANANAS\n");
    EditInit(line, sizeof(line));

    // init config
    EEPROM.begin(sizeof(savedata));
    EEPROM.get(0, savedata);

    // get ESP id
    snprintf(esp_id, sizeof(esp_id), "%08X", ESP.getChipId());
    print("ESP ID: %s\n", esp_id);

    // config led
    FastLED.addLeds < WS2812B, DATA_PIN_1LED, RGB > (leds1, 1);
    FastLED.addLeds < WS2812B, DATA_PIN_7LED, GRB > (leds7, 7);
    animate();

    // connect to wifi
    print("Starting WIFI manager ...\n");
    wifiManager.addParameter(&luftdatenIdParam);
    wifiManager.setSaveConfigCallback(wifiManagerCallback);
    wifiManager.setConfigPortalTimeout(120);
    if (savedata.magic == SAVEDATA_MAGIC) {
        wifiManager.autoConnect("ESP-PMLAMP");
    } else {
        wifiManager.startConfigPortal("ESP-PMLAMP");
    }

    // setup OTA
    ArduinoOTA.setHostname("esp-pmlamp");
    ArduinoOTA.setPassword("pmlamp");
    ArduinoOTA.begin();

    // try autoconfig if id was not set
    if (strlen(savedata.luftdatenid) == 0) {
        set_led(CRGB::White);
        FastLED.show();
        int id;
        if (autoconfig(id)) {
            save_luftdaten(id);
            print("Autoconfig set %d\n", id);
        }
    }
    // turn off LED
    set_led(CRGB::Black);
    FastLED.show();
}

void loop(void)
{
    // fetch a new value every POLL_INTERVAL
    static unsigned int period_last = -1;
    unsigned int period = millis() / POLL_INTERVAL;
    if (period != period_last) {
        period_last = period;
        String json;
        float pm, lat, lon;
        if (fetch_sensor(savedata.luftdatenid, json) && decode_json(json, "P1", &pm, &lat, &lon)) {
            print("PM=%f, lat=%f, lon=%f\n", pm, lat, lon);
            set_led(interpolate(pm, pmlevels));
        }
    }

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
    // allow OTA
    ArduinoOTA.handle();
}
