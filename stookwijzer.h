#include <stdbool.h>

void stookwijzer_begin(WiFiClient &wifiClient, const char *user_agent);
bool stookwijzer_get(double latitude, double longitude, JsonDocument & props);

