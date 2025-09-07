simple internet radio using esp32 cam
* ESP32-CAM Internet Radio with I2S Audio Output
 * Version: 2.2 - Enhanced with Play Button, Sleep Timer & Smart Button
 * 
 * POLĄCZENIA HARDWARE:
 * ====================
 * 
 * KARTA SD (tryb 1-bit - OBOWIĄZKOWY!):
 * - SD_CLK  -> GPIO14 (HS2_CLK)
 * - SD_CMD  -> GPIO15 (HS2_CMD) 
 * - SD_DATA0 -> GPIO2 (HS2_DATA0)
 * - SD_DATA1 -> GPIO4 (NIE UŻYWANY - wolny dla I2S)
 * - SD_DATA2 -> GPIO12 (NIE UŻYWANY - wolny dla I2S)
 * - SD_DATA3 -> GPIO13 (NIE UŻYWANY - wolny dla I2S)
 * - VCC -> 3.3V
 * - GND -> GND
 * 
 * I2S AUDIO (np. MAX98357A, PCM5102):
 * - BCLK -> GPIO12 (wolny w trybie SD 1-bit)
 * - LRC/WS -> GPIO4 (wolny w trybie SD 1-bit) 
 * - DIN/DOUT -> GPIO13 (wolny w trybie SD 1-bit)
 * - VIN -> 5V
 * - GND -> GND
 * 
 * KONTROLA:
 * - Button -> GPIO0 (wbudowany BOOT button)
 *   * Krótkie naciśnięcie: Start/Stop
 *   * Długie naciśnięcie (2s): Następna stacja
 * - Status LED -> GPIO33 (wbudowany LED)
 * 
 * Autor: ESP32 Community
 * Licencja: MIT
 */
