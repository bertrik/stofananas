#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <Arduino.h>

#include <ESP8266HTTPClient.h>

#include "fwupdate.h"
#include "fwversion.h"

static HTTPClient http;
static FS *_fs;
static WiFiClient *_client;
static String _update_path;
static String _update_page;
static String _url = "";

static unsigned long update_started = 0;

#define printf Serial.printf

void fwupdate_begin(FS & fs, WiFiClient &wifiClient)
{
    _fs = &fs;
    _client = &wifiClient;

    Update.runAsync(true);

    http.useHTTP10(true); // disable chunked stream
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
}

static String template_processor(const String & string)
{
    if (string == "fw_version") {
        return FW_VERSION;
    }
    return string;
}

static void handleGet(AsyncWebServerRequest *request)
{
    request->send(*_fs, _update_page, "text/html", false, template_processor);
}

/*
 * Called either at the start of HTTP update, or at the end of POST file update.
 */
static void handleRequest(AsyncWebServerRequest *request)
{
    for (size_t i = 0; i < request->args(); i++) {
        printf("%s=%s\n", request->getParam(i)->name().c_str(), request->getParam(i)->value().c_str());
    }
    String type = request->getParam("type", true)->value();
    if (type == "http") {
        // start of http remote file update
        _url = request->getParam("url", true)->value();
    }
    if (type == "post") {
        // end of POST local file update
        ESP.restart();
    }
    request->redirect(_update_path);
}

static void handleUpload(AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final)
{
    if (index == 0) {
        update_started = millis();
        uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
        if (!Update.begin(maxSketchSpace, U_FLASH, LED_BUILTIN)) {
            printf("Update.begin() failed!\n");
            return;
        }
        request->client()->setNoDelay(true);
    }

    Update.write(data, len);
    if (final) {
        Update.end(true);
        unsigned long duration = millis() - update_started;
        printf("done, took %ld ms\n", duration);
    }
}

static void handleReboot(AsyncWebServerRequest *request)
{
    request->redirect(_update_path);
    ESP.restart();
}

void fwupdate_serve(AsyncWebServer &server, const char *update_path, const char *update_page)
{
    _update_path = update_path;
    _update_page = update_page;

    // register ourselves with the server
    server.on(update_path, HTTP_GET, handleGet);
    server.on(update_path, HTTP_POST, handleRequest, handleUpload);
    server.on("/reboot", HTTP_POST, handleReboot);
}

static bool fwupdate_http(String &url)
{
    bool result = false;
    if (http.begin(*_client, url)) {
        printf("GET %s ... ", url.c_str());
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
                    result = true;
                }
            }
        }
        http.end();
    }
    if (!result) {
        printf("FAIL\n");
    }
    return result;
}

void fwupdate_loop(void)
{
    if (_url != "") {
        update_started = millis();
        fwupdate_http(_url);
        unsigned long duration = millis() - update_started;
        printf("done, took %ld ms\n", duration);

        ESP.restart();
        _url = "";
    }
}


