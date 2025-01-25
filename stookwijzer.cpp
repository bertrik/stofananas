#include <stdbool.h>

#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>

#include "stookwijzer.h"

#define printf Serial.printf

static StaticJsonDocument < 256 > filter;       // https://arduinojson.org/v6/assistant
static WiFiClient *client;
static HTTPClient http;

void stookwijzer_begin(WiFiClient &wifiClient, const char *user_agent)
{
    client = &wifiClient;

    // configure HTTP client: follow redirects, non-chunked mode, disable connection re-use
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.useHTTP10(true);
    http.setReuse(false);
    http.setUserAgent(user_agent);

    // pre-define the streaming JSON filter
    filter["features"][0]["properties"]["pc4"] = true;
    filter["features"][0]["properties"]["model_runtime"] = true;
    filter["features"][0]["properties"]["lki"] = true;
    filter["features"][0]["properties"]["wind"] = true;
    filter["features"][0]["properties"]["advies_0"] = true;
    filter["features"][0]["properties"]["definitief_0"] = true;
    printf("JSON filter:\n");
    serializeJsonPretty(filter, Serial);
    printf("\n");
}

bool stookwijzer_get(double latitude, double longitude, JsonDocument & props)
{
    double delta = 0.00001;
    char url[300];
    sprintf(url, "https://data.rivm.nl/geo/alo/wms?SERVICE=WMS&VERSION=1.3.0&REQUEST=GetFeatureInfo"
            "&QUERY_LAYERS=stookwijzer_v2&LAYERS=stookwijzer_v2&info_format=application/json&feature_count=1"
            "&I=0&J=0&WIDTH=1&HEIGHT=1&CRS=CRS:84&BBOX=%.5f,%.5f,%.5f,%.5f", longitude - delta,
            latitude - delta, longitude + delta, latitude + delta);

    bool result = false;
    if (http.begin(*client, url)) {
        printf("GET %s... ", url);
        int httpCode = http.GET();
        printf("%d\n", httpCode);
        if (httpCode == HTTP_CODE_OK) {
            DynamicJsonDocument doc(2048);
            printf("Deserializing from HTTP... ");
            DeserializationError error =
                deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
            printf("%s\n", error.c_str());
            props.set(doc["features"][0]["properties"]);
            result = (error == DeserializationError::Ok);
        }
        http.end();
    }
    return result;
}
