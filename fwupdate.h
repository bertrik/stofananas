#include <FS.h>
#include <WiFiClient.h>
#include <ESPAsyncWebServer.h>

void fwupdate_begin(FS & fs, WiFiClient &wifiClient);
void fwupdate_serve(AsyncWebServer &server, const char *update_path, const char *update_page);
void fwupdate_loop(void);

