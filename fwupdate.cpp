#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <Arduino.h>

#include "fwupdate.h"

static FS *_fs;
static String _update_path;
static String _update_page;

static unsigned long update_started = 0;
static size_t estimated_size;
static size_t progress = 0;
static int last_chunk = 0;

#define printf Serial.printf

void fwupdate_begin(FS & fs)
{
    _fs = &fs;
    Update.runAsync(true);
}

static void handleGet(AsyncWebServerRequest *request)
{
    request->send(*_fs, _update_page, "text/html");
}

static void handleRequest(AsyncWebServerRequest *request)
{
    unsigned long duration = millis() - update_started;
    printf("done, took %ld ms\n", duration);
    request->redirect(_update_path);
}

static void handleUpload(AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final)
{
    if (index == 0) {
        estimated_size = ESP.getSketchSize();
        progress= 0;
        last_chunk = 0;
        printf("begin, free heap=%u, current sketch=%u: ", ESP.getFreeHeap(), ESP.getSketchSize());
        uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
        update_started = millis();
        if (!Update.begin(maxSketchSpace, U_FLASH)) {
            printf("Update.begin() failed!\n");
        }
        request->client()->setNoDelay(true);
    }

    Update.write(data, len);
    int chunk = Update.progress() / 4096;
    digitalWrite(LED_BUILTIN, chunk & 1);
    if (chunk != last_chunk) {
        printf(".");
        last_chunk = chunk;
    }

    if (final) {
        Update.end(true);
        request->redirect(_update_path);
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
    server.on("/reboot", HTTP_GET, handleReboot);
}
