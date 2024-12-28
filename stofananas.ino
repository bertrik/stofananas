#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <ESP8266mDNS.h>
#include <ESP8266HTTPClient.h>

#include <ESPAsyncWebServer.h>
#include <ESPAsyncWiFiManager.h>

#include "LittleFS.h"

#include <FastLED.h>
#include <ArduinoJson.h>
#include <MiniShell.h>
#include <Arduino.h>

#include "config.h"
#include "fwupdate.h"
#include "fsimage.h"
#include "geolocate.h"

#define printf Serial.printf

// we send the colour to two sets of LEDs: a single LED on pin D2, a star of 7 LEDs on pin D4
#define DATA_PIN_1LED   D2
#define DATA_PIN_7LED   D4

#define POLL_INTERVAL           300000

typedef struct {
    bool hasRgbLed;
    float latitude;
    float longitude;
} savedata_t;

typedef struct {
    int pm;
    int hue;
} pmlevel_t;

static savedata_t savedata;

static WiFiClient wifiClient;
static AsyncWebServer server(80);
static MiniShell shell(&Serial);
static DNSServer dns;
static char espid[64];

static CRGB leds1[1];
static CRGB leds7[7];
static CRGB color;
static int num_fetch_failures = 0;
static float latitude, longitude, accuracy;

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

static void show_help(const cmd_t *cmds)
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
    sprintf(path, "/air?lat=%.6f&lon=%.6f", latitude, longitude);
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

static int do_geolocate(int argc, char *argv[])
{
    float latitude, longitude, accuracy;
    bool ok = geolocate(wifiClient, latitude, longitude, accuracy);
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

static int do_error(int argc, char *argv[])
{
    if (argc > 1) {
        num_fetch_failures = atoi(argv[1]);
    }
    printf("fetch failures:%d\n", num_fetch_failures);
    return 0;
}

static int do_led(int argc, char *argv[])
{
    if (argc < 2) {
        return -1;
    }
    int rgb = strtoul(argv[1], NULL, 16);
    set_led(rgb);

    return 0;
}

static int do_unpack(int argc, char *argv[])
{
    bool force = false;
    if (argc > 1) {
        force = (strcmp(argv[1], "force") == 0);
    }
    printf("Unpacking(force=%d)\n", force);
    fsimage_unpack(LittleFS, force);
    return 0;
}

static int do_update(int argc, char *argv[])
{
    int result = 0;
    const char *url = (argc > 1) ? argv[1] : "https://github.com/bertrik/stofananas/releases/latest/download/firmware.bin";

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (http.begin(client, url)) {
        printf("GET %s ... ", url);
        int httpCode = http.GET();
        printf("%d\n", httpCode);
        if (httpCode == HTTP_CODE_OK) {
            int contentLength = http.getSize();
            printf("Update.begin(%d) ... ", contentLength);
            if (Update.begin(contentLength, U_FLASH, LED_BUILTIN, 0)) {
                printf("OK\n");
                printf("Update.writeStream() ...");
                size_t written = Update.writeStream(http.getStream());
                printf("%d written\n", written);

                printf("Update.end() ... ");
                if (Update.end(true)) {
                    printf("OK\n");
                } else {
                    printf("FAIL\n");
                    result = -1;
                }
            } else {
                printf("FAIL\n");
                result = -1;
            }
        }
        http.end();
    } else {
        printf("http.begin() failed!\n");
        result = -3;
    }
    return result;
}

const cmd_t commands[] = {
    { "help", do_help, "Show help" },
    { "get", do_get, "[id] GET the PM2.5 value from stofradar.nl" },
    { "pm", do_pm, "<pm> Simulate PM value and update the LED" },
    { "geo", do_geolocate, "Perform a wifi geo-localisation" },
    { "reboot", do_reboot, "Reboot" },
    { "error", do_error, "[fetch] [decode] Simulate a fetch/decode error" },
    { "led", do_led, "<RRGGBB> Set the LED to a specific value (hex)" },
    { "unpack", do_unpack, "<force> Unpack files" },
    { "update", do_update, "[url] Update firmware from URL" },
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
    snprintf(espid, sizeof(espid), "esp8266-pmlamp-%06x", ESP.getChipId());

    Serial.begin(115200);
    printf("\nESP-STOFANANAS\n");

    // load settings, save defaults if necessary
    LittleFS.begin();
    fwupdate_begin(LittleFS);
    fsimage_unpack(LittleFS, false);
    config_begin(LittleFS, "/config.json");
    if (!config_load()) {
        printf("Loading config defaults\n");
        config_set_value("loc_latitude", "52.15517");
        config_set_value("loc_longitude", "5.38721");
        config_set_value("led_type", "grb");
        config_save();
    }
    savedata.hasRgbLed = (strcmp("rgb", config_get_value("led_type").c_str()) == 0);
    savedata.latitude = atof(config_get_value("loc_latitude").c_str());
    savedata.longitude = atof(config_get_value("loc_longitude").c_str());

    // config led
    if (savedata.hasRgbLed) {
        FastLED.addLeds < WS2812B, DATA_PIN_1LED, RGB > (leds1, 1).setCorrection(TypicalSMD5050);
        FastLED.addLeds < WS2812B, DATA_PIN_7LED, RGB > (leds7, 7).setCorrection(TypicalSMD5050);
    } else {
        FastLED.addLeds < WS2812B, DATA_PIN_1LED, GRB > (leds1, 1).setCorrection(TypicalSMD5050);
        FastLED.addLeds < WS2812B, DATA_PIN_7LED, GRB > (leds7, 7).setCorrection(TypicalSMD5050);
    }

    // animate LED then indicate white during config
    animate();
    set_led(CRGB::Gray);

    // connect to wifi
    printf("Starting WIFI manager (%s)...\n", WiFi.SSID().c_str());
    AsyncWiFiManager wifiManager(&server, &dns);
    wifiManager.autoConnect("ESP-PMLAMP");

    // set up web server
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    config_serve(server, "/config", "/config.html");
    fwupdate_serve(server, "/update", "update.html");
    MDNS.begin("stofananas");
    MDNS.addService("http", "tcp", 80);
    server.begin();

    // attempt geolocation
    if (!geolocate(wifiClient, latitude, longitude, accuracy) || accuracy > 100) {
        latitude = savedata.latitude;
        longitude = savedata.longitude;
    }
}

void loop(void)
{
    // fetch a new value every POLL_INTERVAL
    static unsigned int period_last = -1;
    unsigned int period = millis() / POLL_INTERVAL;
    if (period != period_last) {
        period_last = period;
        // fetch PM and update LED
        double pm2_5;
        if (fetch_pm(latitude, longitude, "pm2.5", pm2_5)) {
            num_fetch_failures = 0;
            printf("PM2.5=%.2f, lat=%.6f, lon=%.6f\n", pm2_5, latitude, longitude);
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
    // flash LED when there is an error
    if (num_fetch_failures > 0) {
        bool flash = (millis() / 500) % 2;

        // update on the hardware
        FastLED.showColor(color, flash ? 200 : 255);
    }
    // command line processing
    shell.process(">", commands);

    // keep MDNS alive
    MDNS.update();
}
