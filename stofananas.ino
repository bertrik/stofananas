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

#define DATA_PIN    D4

#define SAVEDATA_MAGIC  0xCAFEBABE

typedef struct {
    char luftdatenid[16];
    uint32_t magic;
} savedata_t;

typedef struct {
    int pm;
    CRGB color;
} pmlevel_t;

static savedata_t savedata;
static char esp_id[16];

static WiFiManager wifiManager;
static WiFiManagerParameter luftdatenIdParam("luftdatenid", "Luftdaten ID", "", sizeof(savedata_t));
static WiFiClient wifiClient;

static CRGB led;
static char line[120];

static const pmlevel_t pmlevels[] = {
    { 0, CRGB::Grey },
    { 25, CRGB::Cyan },
    { 50, CRGB::Yellow },
    { 100, CRGB::Red },
    { 200, CRGB::Purple },
    // terminating entry
    { -1, CRGB::Purple }
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
            const char *value = sensordatavalue["value"];
            if (strcmp(item, value_type) == 0) {
                meas_num++;
                meas_sum += strtof(value, NULL);
            }
        }
    }

    if (meas_num > 0) {
        *value = meas_sum / meas_num;
        return true;
    }

    return false;
}

static String fetch_json(const char *luftdatenid)
{
    char url[64];
    snprintf(url, sizeof(url), "http://api.luftdaten.info/v1/sensor/%s/", luftdatenid);

    // perform the GET
    print("Executing GET to %s...", url);
    HTTPClient httpClient;
    WiFiClient wifiClient;
    httpClient.begin(wifiClient, url);
    int res = httpClient.sendRequest("GET");
    print("%d\n", res);
    String json = (res == HTTP_CODE_OK) ? httpClient.getString() : "";
    httpClient.end();
    return json;
}

static int do_get(int argc, char *argv[])
{
    // perform the GET
    String json = fetch_json(savedata.luftdatenid);
    print("JSON: ");
    Serial.println(json);

    // decode it
    float pm = 0.0;
    if (decode_json(json, "P1", &pm)) {
        print("PM average: %f\n", pm);
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

    print("config.luftdatenid = %s\n", savedata.luftdatenid);
    print("config.magic       = %08X\n", savedata.magic);

    return 0;
}

static CRGB interpolate(float pm, const pmlevel_t table[])
{
    CRGB color;
    for (const pmlevel_t * pmlevel = table; pmlevel->pm >= 0; pmlevel++) {
        const pmlevel_t *next = pmlevel + 1;
        color = pmlevel->color;
        if ((pm >= pmlevel->pm) && (pm < next->pm)) {
            uint16_t frac = 65536 * (pm - pmlevel->pm) / (next->pm - pmlevel->pm);
            return color.lerp16(next->color, frac);
        }
    }
    return color;
}

static int do_pm(int argc, char *argv[])
{
    if (argc < 2) {
        return -1;
    }

    float pm = atoi(argv[1]);
    CRGB color = interpolate(pm, pmlevels);
    print("pm=%d => color = #%02X%02X%02X\n", (int) pm, color.r, color.g, color.b);
    led = color;
    FastLED.show();

    return 0;
}

const cmd_t commands[] = {
    { "help", do_help, "Show help" },
    { "get", do_get, "Do HTTP GET" },
    { "config", do_config, "Show/clear config" },
    { "pm", do_pm, "Simulate PM value" },
    { NULL, NULL, NULL }
};

static int do_help(int argc, char *argv[])
{
    show_help(commands);
    return 0;
}

void loop(void)
{
    // fetch a new value every minute
    static unsigned long int last_minute = -1;
    unsigned long int ms = millis();
    unsigned long int minute = (ms / 60000);
    if (minute != last_minute) {
        last_minute = minute;
        String json = fetch_json(savedata.luftdatenid);
        float pm;
        if (decode_json(json, "P1", &pm)) {
            print("PM=%f\n", pm);
            CRGB color = interpolate(pm, pmlevels);
            led = color;
            FastLED.show();
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
}

