#include <Arduino.h>

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("================================");
    Serial.println("HELLO FROM ESP32-S3");
    Serial.println("Firmware Started Successfully");
    Serial.println("================================");
}

void loop() {
    Serial.println("Running...");
    delay(1000);
}