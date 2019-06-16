#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include "ESP8266HTTPClient.h"
#include <FastLED.h>
#include "ArduinoJson.h"

#include "print.h"
#include "cmdproc.h"
#include "editline.h"

#include <Arduino.h>

#define DATA_PIN    D4

static char esp_id[16];
static WiFiManager wifiManager;
static WiFiClient wifiClient;
static CRGB led;
static char line[120];

void setup(void)
{
    Serial.begin(115200);
    Serial.print("\nESP-STOFANANAS\n");
    EditInit(line, sizeof(line));

    // get ESP id
    sprintf(esp_id, "%08X", ESP.getChipId());
    Serial.print("ESP ID: ");
    Serial.println(esp_id);

    // connect to wifi
    Serial.println("Starting WIFI manager ...");
    wifiManager.setConfigPortalTimeout(120);
    wifiManager.autoConnect("ESP-STOFANANAS");

    // config led
    FastLED.addLeds < PL9823, DATA_PIN > (&led, 1);
}

static CRGB table[8] = {
    CRGB::Black, CRGB::Blue, CRGB::Cyan, CRGB::Green, CRGB::Yellow, CRGB::Red, CRGB::Purple
};

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
            Serial.print("value_type: ");
            Serial.println(String(value_type));
            Serial.print("value: ");
            Serial.println(String(value));
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

static int do_get(int argc, char *argv[])
{
    WiFiClient wifiClient;
    HTTPClient httpClient;

    // perform the GET
    print("HTTP begin ...");
    String url = "http://api.luftdaten.info/v1/sensor/12246/";
    httpClient.begin(wifiClient, url);
    print("GET ...");
    int res = httpClient.sendRequest("GET");
    print("code %d\n", res);
    String json = "";
    if (res == HTTP_CODE_OK) {
        print("Response:");
        json = httpClient.getString();
    }
    httpClient.end();

    Serial.print("JSON: ");
    Serial.println(json);

    float pm = 0.0;
    if (decode_json(json, "P1", &pm)) {
        Serial.printf("Particulate matter: %f\n", pm);
    }

    return 0;
}

const cmd_t commands[] = {
    { "help", do_help, "Show help" },
    { "get", do_get, "Do a get" },
    { NULL, NULL, NULL }
};

static int do_help(int argc, char *argv[])
{
    show_help(commands);
    return 0;
}

void loop(void)
{
    // cycle intensity and hue
    unsigned long ms = millis();
    int hue = (ms / 10) % 256;
    int intensity = (1.0 + cos(ms / 1000.0)) * 127;
    led.setHSV(hue, 128, intensity);
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
