#ifndef AsyncElegantOTA_h
#define AsyncElegantOTA_h

#warning AsyncElegantOTA library is deprecated, Please consider moving to newer ElegantOTA library which now comes with an Async Mode. Learn More: https://docs.elegantota.pro/async-mode/ 

#include "Arduino.h"
#include "stdlib_noniso.h"

#if defined(ESP8266)
    #include "ESP8266WiFi.h"
    #include "ESPAsyncTCP.h"
    #include "flash_hal.h"
    #include "FS.h"
#elif defined(ESP32)
    #include "WiFi.h"
    #include "AsyncTCP.h"
    #include "Update.h"
#endif

#include "Hash.h"
#include "ESPAsyncWebServer.h"
#include "FS.h"

#include "elegantWebpage.h"


class AsyncElegantOtaClass{

    public:
        using FlashGate = bool (*)();

        void setID(const char* id);
        void setFlashGate(FlashGate gate);
        void begin(AsyncWebServer *server, const char* username = "", const char* password = "");
        void abort();
        void loop();
        void restart();

    private:
        AsyncWebServer *_server;

        String getID();

        String _id = getID();
        String _username = "";
        String _password = "";
        bool _authRequired = false;
        FlashGate _flashGate = nullptr;
        bool _uploadInProgress = false;
        bool _uploadSucceeded = false;

};

extern AsyncElegantOtaClass AsyncElegantOTA;

#endif
