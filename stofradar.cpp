#include <stdbool.h>

#include <ArduinoJson.h>
#include <WiFiClient.h>
#include <ESP8266HTTPClient.h>

#include "stofradar.h"

static WiFiClient *client;
static HTTPClient http;

void stofradar_begin(WiFiClient &wifiClient, const char *user_agent)
{
    client = &wifiClient;

    // configure HTTP client: non-chunked mode, disable connection re-use
    http.useHTTP10(true);
    http.setTimeout(10000);
    http.setUserAgent(user_agent);
}

bool stofradar_get(double latitude, double longitude, JsonDocument & json)
{
    bool result = false;
    char url[128];

    sprintf(url, "http://stofradar.nl:9000/air?lat=%.6f&lon=%.6f", latitude, longitude);
    if (http.begin(*client, url)) {
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
            DeserializationError error = deserializeJson(json, http.getStream());
            result = (error == DeserializationError::Ok);
        }
        http.end();
    }
    return result;
}
