#pragma once
#include <Arduino.h>

class WiFiClient {
public:
    virtual ~WiFiClient() {}
};
class WiFiClientSecure : public WiFiClient {
public:
    void setInsecure() {}
};
