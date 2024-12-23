#include <stdbool.h>

#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>

#include <ArduinoJson.h>

#include "geolocate.h"

bool geolocate(WiFiClient &wifiClient, float &latitude, float &longitude, float &accuracy)
{
    // scan for networks
    printf("Scanning...");
    int n = WiFi.scanNetworks();
    printf("%d APs found...", n);

    // create JSON request
    DynamicJsonDocument doc(4096);
    doc["considerIp"] = "true";
    JsonArray aps = doc.createNestedArray("wifiAccessPoints");
    int num = 0;
    for (int i = 0; i < n; i++) {
        // skip hidden and "nomap" APs
        if (WiFi.isHidden(i) || WiFi.SSID(i).endsWith("_nomap")) {
            continue;
        }
        JsonObject ap = aps.createNestedObject();
        ap["macAddress"] = WiFi.BSSIDstr(i);
        ap["signalStrength"] = WiFi.RSSI(i);
        if (++num == 20) {
            // limit scan to some arbitrary number to avoid alloc fails later
            break;
        }
    }
    printf("%d APs used\n", num);

    String json;
    serializeJson(doc, json);

    // send JSON with POST
    HTTPClient httpClient;
    httpClient.begin(wifiClient, "stofradar.nl", 9000, "/v1/geolocate");
    httpClient.addHeader("Content-Type", "application/json");
    int res = httpClient.POST(json);
    bool result = (res == HTTP_CODE_OK);
    String response = result ? httpClient.getString() : httpClient.errorToString(res);
    httpClient.end();
    if (res != HTTP_CODE_OK) {
        printf("HTTP code %d\n", res);
        return false;
    }
    // parse response
    printf("%s\n", response.c_str());
    doc.clear();
    if (deserializeJson(doc, response) != DeserializationError::Ok) {
        printf("Failed to deserialize JSON!\n");
        return false;
    }

    JsonObject location = doc["location"];
    latitude = location["lat"];
    longitude = location["lng"];
    accuracy = doc["accuracy"];

    return true;
}

