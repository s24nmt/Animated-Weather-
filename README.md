# Animated-Weather- for lilygo t-embed cc1101 plus
This Arduino sketch implements a Wi-Fi-connected smart clock and 7-day weather station for the LILYGO T-Embed CC1101 Plus (ESP32-S3). It renders tear-free animated graphics on the 1.9-inch ST7789 IPS display, pulls live weather data from Open-Meteo without requiring an API key.
Install the ESP32 Board PackageOpen Arduino IDE (version 2.x recommended) and go to File > Preferences (or Arduino IDE > Settings on macOS).In the Additional Boards Manager URLs field, 
paste the official Espressif index link and click OK:   Plaintexthttps://espressif.github.io/arduino-esp32/package_esp32_index.json

Compile & Flash the Sketch
Paste your code into a new sketch and optionally update wifiSSID and wifiPass at the top before uploading.
Click Upload (the right-arrow icon).
If the upload fails or no COM port appears: Press and hold the center rotary encoder button (GPIO 0) while plugging in the USB-C cable (or while pressing the side reset button), then release it once Writing at 0x00010000... appears in the console.
Once flashed, if it cannot connect to your saved Wi-Fi, connect your phone to the T-Embed-Setup Wi-Fi hotspot and open [http://192.168.4.1](http://192.168.4.1) (or long-press the center button to scan the on-screen QR code).
