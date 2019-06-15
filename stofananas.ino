#include <stdbool.h>
#include <stdint.h>

#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include "ESP8266HTTPClient.h"
#include <FastLED.h>

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

static int do_get(int argc, char *argv[])
{
    WiFiClient wifiClient;
    HTTPClient httpClient;

    print("HTTP begin ...");
    String url = "http://api.luftdaten.info/v1/sensor/12246/";
    httpClient.begin(wifiClient, url);
    print("GET ...");
    int res = httpClient.sendRequest("GET");
    print("code %d\n", res);
    if (res == HTTP_CODE_OK) {
        print("Response:");
        Serial.println(httpClient.getString());
    }
    httpClient.end();
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
    // show led color
    int idx = (millis() / 1000) % 7;
    led = table[idx];
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
