#include <stdbool.h>

void stofradar_begin(WiFiClient &wifiClient, const char *user_agent);
bool stofradar_get(double latitude, double longitude, JsonDocument & props);

