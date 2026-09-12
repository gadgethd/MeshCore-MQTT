#include <AsyncElegantOTA.h>

AsyncElegantOtaClass AsyncElegantOTA;

void AsyncElegantOtaClass::setID(const char* id){
    _id = id;
}

void AsyncElegantOtaClass::setFlashGate(FlashGate gate){
    _flashGate = gate;
}

void AsyncElegantOtaClass::abort(){
    if (_uploadInProgress || Update.isRunning()) {
        Update.abort();
    }
    _uploadInProgress = false;
    _uploadSucceeded = false;
}

void AsyncElegantOtaClass::begin(AsyncWebServer *server, const char* username, const char* password){
    _server = server;

    if(strlen(username) > 0){
        _authRequired = true;
        _username = username;
        _password = password;
    }else{
        _authRequired = false;
        _username = "";
        _password = "";
    }

    _server->on("/update/identity", HTTP_GET, [&](AsyncWebServerRequest *request){
        if(_authRequired){
            if(!request->authenticate(_username.c_str(), _password.c_str())){
                return request->requestAuthentication();
            }
        }
        #if defined(ESP8266)
            request->send(200, "application/json", "{\"id\": \""+_id+"\", \"hardware\": \"ESP8266\"}");
        #elif defined(ESP32)
            request->send(200, "application/json", "{\"id\": \""+_id+"\", \"hardware\": \"ESP32\"}");
        #endif
    });

    _server->on("/update", HTTP_GET, [&](AsyncWebServerRequest *request){
        if(_authRequired){
            if(!request->authenticate(_username.c_str(), _password.c_str())){
                return request->requestAuthentication();
            }
        }
        AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", ELEGANT_HTML, ELEGANT_HTML_SIZE);
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    });

    _server->on("/update", HTTP_POST, [&](AsyncWebServerRequest *request) {
        if(_authRequired){
            if(!request->authenticate(_username.c_str(), _password.c_str())){
                return request->requestAuthentication();
            }
        }
        // the request handler is triggered after the upload has finished... 
        // create the response, add header, and send response
        const bool upload_ok = _uploadSucceeded && !Update.hasError();
        AsyncWebServerResponse *response = request->beginResponse(upload_ok?200:500, "text/plain", upload_ok?"OK":"FAIL");
        response->addHeader("Connection", "close");
        response->addHeader("Access-Control-Allow-Origin", "*");
        request->send(response);
        if (upload_ok) restart();
    }, [&](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        //Upload handler chunks in data
        if(_authRequired){
            if(!request->authenticate(_username.c_str(), _password.c_str())){
                return request->requestAuthentication();
            }
        }

        if (!index) {
            abort();
            if(!request->hasParam("MD5", true)) {
                return request->send(400, "text/plain", "MD5 parameter missing");
            }

            if(!Update.setMD5(request->getParam("MD5", true)->value().c_str())) {
                return request->send(400, "text/plain", "MD5 parameter invalid");
            }

            // The server is not proof that MQTT teardown completed. Check the
            // lifecycle barrier immediately before allocating/writing flash.
            if (_flashGate != nullptr && !_flashGate()) {
                return request->send(503, "text/plain", "MQTT stop not verified; OTA refused");
            }

            #if defined(ESP8266)
                int cmd = (filename == "filesystem") ? U_FS : U_FLASH;
                Update.runAsync(true);
                size_t fsSize = ((size_t) &_FS_end - (size_t) &_FS_start);
                uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
                if (!Update.begin((cmd == U_FS)?fsSize:maxSketchSpace, cmd)){ // Start with max available size
            #elif defined(ESP32)
                int cmd = (filename == "filesystem") ? U_SPIFFS : U_FLASH;
                if (!Update.begin(UPDATE_SIZE_UNKNOWN, cmd)) { // Start with max available size
            #endif
                Update.printError(Serial);
                abort();
                return request->send(400, "text/plain", "OTA could not begin");
            }
            _uploadInProgress = true;
        }

        // Write chunked data to the free sketch space
        if(len){
            if (!_uploadInProgress || (_flashGate != nullptr && !_flashGate())) {
                abort();
                return request->send(503, "text/plain", "MQTT stop not verified; OTA refused");
            }
            if (Update.write(data, len) != len) {
                abort();
                return request->send(400, "text/plain", "OTA write failed");
            }
        }
            
        if (final) { // if the final flag is set then this is the last frame of data
            if (!_uploadInProgress || (_flashGate != nullptr && !_flashGate())) {
                abort();
                return request->send(503, "text/plain", "MQTT stop not verified; OTA refused");
            }
            if (!Update.end(true)) { //true to set the size to the current progress
                Update.printError(Serial);
                abort();
                return request->send(400, "text/plain", "Could not end OTA");
            }
            _uploadInProgress = false;
            _uploadSucceeded = true;
        }else{
            return;
        }
    });
}

// deprecated, keeping for backward compatibility
void AsyncElegantOtaClass::loop() {
}

void AsyncElegantOtaClass::restart() {
    yield();
    delay(1000);
    yield();
    ESP.restart();
}

String AsyncElegantOtaClass::getID(){
    String id = "";
    #if defined(ESP8266)
        id = String(ESP.getChipId());
    #elif defined(ESP32)
        id = String((uint32_t)ESP.getEfuseMac(), HEX);
    #endif
    id.toUpperCase();
    return id;
}
