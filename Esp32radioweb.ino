/*
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

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SD_MMC.h>
#include <ArduinoJson.h>
#include <Audio.h>
#include <SPIFFS.h>
#include <EEPROM.h>
#include <vector>

// Piny ESP32 CAM
#define LED_STATUS     33   // Wbudowany LED
#define BUTTON_PIN     0    // Boot button (pullup wewnętrzny)
#define I2S_DOUT      13    // SD DAT3 (wolny w trybie 1-bit)
#define I2S_BCLK      12    // SD DAT2 (wolny w trybie 1-bit)  
#define I2S_LRC       4     // SD DAT1 (wolny w trybie 1-bit)

// EEPROM adresy
#define EEPROM_SIZE 512
#define LAST_STATION_ADDR 0
#define LAST_VOLUME_ADDR 4

// Button timing constants
#define SHORT_PRESS_TIME 50     // Minimum press time (ms)
#define LONG_PRESS_TIME 3000    // Long press threshold (ms)
#define DEBOUNCE_TIME 50        // Debounce time (ms)

// Obiekty globalne
WebServer server(80);
Audio audio;
bool isPlaying = false;
String currentStation = "";
String stationName = "";
String streamTitle = "";
String streamBitrate = "";
String streamGenre = "";
bool sdCardWorking = false;
bool useSpiffs = false;
int currentStationIndex = 0;

// Button handling variables
bool buttonPressed = false;
bool buttonLongPressed = false;
unsigned long buttonPressStart = 0;
unsigned long lastButtonCheck = 0;
bool buttonState = HIGH;
bool lastButtonState = HIGH;

// Sleep timer variables
unsigned long sleepTimer = 0;
bool sleepTimerActive = false;
unsigned long sleepTimerStart = 0;

// Struktura dla stacji radiowych
struct RadioStation {
  String name;
  String url;
  String genre;
};

// Playlisty
std::vector<RadioStation> playlist;
std::vector<RadioStation> favorites;

struct Config {
  String ssid;
  String password;
  int volume;
  String defaultStation;
  bool autoPlay;
  String adminPassword;
} config;

// Forward declarations
void testLED();
void blinkLED(int times, int delayMs);
void initStorage();
bool testSDCard();
void createDirectories();
void loadConfig();
void saveConfig();
void loadPlaylists();
void savePlaylists();
void connectWiFi();
void handleButton();
void nextStation();
void startRadio(String url);
void stopRadio();
void toggleRadio();
void updateStatusLED();
void setupWebServer();
String getEnhancedHTML();
String getConfigHTML();
String getStationManagerHTML();
void saveLastStation();
void loadLastStation();
void setSleepTimer(int minutes);
void checkSleepTimer();
void audio_info_callback(Audio::msg_t m);

// Callback dla metadanych
void audio_info_callback(Audio::msg_t m) {
  String type = String(m.s);
  String msg = String(m.msg);
  
  Serial.printf("%s: %s\n", m.s, m.msg);
  
  if (type == "streamtitle") {
    streamTitle = msg;
    Serial.print(">>> UTWOR: ");
    Serial.println(streamTitle);
  }
  else if (type == "station_name") {
    stationName = msg;
    Serial.print(">>> STACJA: ");
    Serial.println(stationName);
  }
  else if (type == "bitrate") {
    streamBitrate = msg + " kbps";
    Serial.print(">>> BITRATE: ");
    Serial.println(streamBitrate);
  }
  else if (type == "icy_description") {
    Serial.print(">>> OPIS: ");
    Serial.println(msg);
  }
  else if (type == "icy_url") {
    Serial.print(">>> URL: ");
    Serial.println(msg);
  }
  else if (type == "icy_genre") {
    streamGenre = msg;
    Serial.print(">>> GATUNEK: ");
    Serial.println(streamGenre);
  }
  else if (type == "lasthost") {
    Serial.print(">>> HOST: ");
    Serial.println(msg);
  }
}

// Callback'i dla kompatybilności
void audio_info(const char *info) {
  Serial.print("[OLD-INFO] ");
  Serial.println(info);
}

void audio_id3data(const char *info) {
  Serial.print("[ID3] ");
  Serial.println(info);
}

void audio_showstation(const char *info) {
  Serial.print("[OLD-STATION] ");
  Serial.println(info);
}

void audio_showstreamtitle(const char *info) {
  Serial.print("[OLD-STREAMTITLE] ");
  Serial.println(info);
}

void audio_bitrate(const char *info) {
  Serial.print("[OLD-BITRATE] ");
  Serial.println(info);
}

void audio_commercial(const char *info) {
  Serial.print("[COMMERCIAL] ");
  Serial.println(info);
}

void audio_icyurl(const char *info) {
  Serial.print("[OLD-ICYURL] ");
  Serial.println(info);
}

void audio_lasthost(const char *info) {
  Serial.print("[OLD-LASTHOST] ");
  Serial.println(info);
}

void audio_eof_mp3(const char *info) {
  Serial.print("[EOF_MP3] ");
  Serial.println(info);
  isPlaying = false;
  currentStation = "";
}

void audio_eof_stream(const char *info) {
  Serial.print("[EOF_STREAM] ");
  Serial.println(info);
  Serial.println("Stream zakonczony - proba ponownego polaczenia za 3 sekundy...");
  if (!currentStation.isEmpty()) {
    delay(3000);
    startRadio(currentStation);
  }
}

void audio_icycast(const char *info) {
  Serial.print("[ICYCAST] ");
  Serial.println(info);
}

void audio_streaminfo(const char *info) {
  Serial.print("[STREAMINFO] ");
  Serial.println(info);
}

void audio_codecinfo(const char *info) {
  Serial.print("[CODEC] ");
  Serial.println(info);
}

void audio_eof_speech(const char *info) {
  Serial.print("[EOF_SPEECH] ");
  Serial.println(info);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n=====================================");
  Serial.println("ESP32 CAM Internet Radio v2.2");
  Serial.println("=====================================");
  Serial.println("PINOUT:");
  Serial.println("  SD Card (1-bit): CLK=14, CMD=15, DAT0=2");
  Serial.println("  I2S Audio: DOUT=13, BCLK=12, LRC=4");
  Serial.println("  Button: GPIO0 (BOOT)");
  Serial.println("    - Krótkie naciśnięcie: Start/Stop");
  Serial.println("    - Długie naciśnięcie (2s): Następna stacja");
  Serial.println("  Status LED: GPIO33");
  Serial.println("=====================================\n");
  
  pinMode(LED_STATUS, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  
  // Inicjalizacja EEPROM
  EEPROM.begin(EEPROM_SIZE);
  
  // Sygnalizacja startu
  testLED();
  blinkLED(3, 200);
  
  // Inicjalizacja
  initStorage();
  loadConfig();
  loadPlaylists();
  connectWiFi();
  
  // Inicjalizacja audio
  Serial.println("Inicjalizacja I2S Audio...");
  Audio::audio_info_callback = audio_info_callback;
  
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setInBufferSize(32768);     //32KB zamiast domyślnych ~8KB
  audio.setVolume(config.volume);
  audio.setConnectionTimeout(10000, 15000);
  audio.forceMono(false);
  Serial.println("I2S Audio OK - callback zarejestrowany");
  
  // Web server
  setupWebServer();
  server.begin();
  
  Serial.println("\nSystem gotowy!");
  Serial.print("IP: ");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(WiFi.localIP());
    Serial.println("\nOtworz przegladarke i wejdz na:");
    Serial.print("http://");
    Serial.println(WiFi.localIP());
    
    // Autoplay ostatniej stacji
    loadLastStation();
    if (config.autoPlay && !config.defaultStation.isEmpty()) {
      Serial.println("Autoplay wlaczone - start za 2 sekundy...");
      delay(2000);
      startRadio(config.defaultStation);
    }
  } else {
    Serial.println(WiFi.softAPIP());
    Serial.println("\nPolacz sie z WiFi: ESP32_Radio_Config");
    Serial.println("Haslo: 12345678");
    Serial.print("Nastepnie otworz: http://");
    Serial.println(WiFi.softAPIP());
  }
  
  digitalWrite(LED_STATUS, HIGH);
}

void loop() {
  server.handleClient();
  audio.loop();
  handleButton();
  updateStatusLED();
  checkSleepTimer();
  
  // Okresowe sprawdzanie metadanych
  static unsigned long lastMetaCheck = 0;
  static unsigned long lastDebugInfo = 0;
  static unsigned long startStreamTime = 0;
  static bool waitingForMetadata = false;
  
  if (isPlaying && startStreamTime == 0) {
    startStreamTime = millis();
    waitingForMetadata = true;
  } else if (!isPlaying) {
    startStreamTime = 0;
    waitingForMetadata = false;
  }
  
  if (waitingForMetadata && (millis() - startStreamTime > 10000)) {
    waitingForMetadata = false;
    Serial.println("\n=== RAPORT METADANYCH (10s od startu) ===");
    if (streamTitle.isEmpty()) {
      Serial.println("!!! BRAK METADANYCH UTWORU !!!");
    } else {
      Serial.println("METADANE ODBIERANE POPRAWNIE!");
      Serial.print("Utwor: ");
      Serial.println(streamTitle);
    }
    Serial.println("==========================================\n");
  }
  
  if (millis() - lastDebugInfo > 30000) {
    lastDebugInfo = millis();
    if (isPlaying) {
      Serial.println("\n=== STATUS ODTWARZANIA ===");
      Serial.print("Stacja: ");
      Serial.println(stationName);
      Serial.print("URL: ");
      Serial.println(currentStation);
      Serial.print("Tytul: ");
      Serial.println(streamTitle.isEmpty() ? "(brak metadanych)" : streamTitle);
      Serial.print("Bitrate: ");
      Serial.println(streamBitrate.isEmpty() ? "(nieznany)" : streamBitrate);
      Serial.print("Gatunek: ");
      Serial.println(streamGenre);
      if (sleepTimerActive) {
        unsigned long remaining = (sleepTimer - (millis() - sleepTimerStart)) / 60000;
        Serial.print("Sleep timer: ");
        Serial.print(remaining);
        Serial.println(" min pozostalo");
      }
      Serial.println("==========================\n");
    }
  }
  
  delay(10);
}

void testLED() {
  for (int i = 0; i < 5; i++) {
    digitalWrite(LED_STATUS, HIGH);
    delay(100);
    digitalWrite(LED_STATUS, LOW);
    delay(100);
  }
}

void blinkLED(int times, int delayMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_STATUS, HIGH);
    delay(delayMs);
    digitalWrite(LED_STATUS, LOW);
    delay(delayMs);
  }
}

void initStorage() {
  Serial.println("Inicjalizacja storage...");
  bool sdMounted = false;
  
  Serial.println("Proba montowania SD w trybie 1-bit, 10MHz...");
  if (SD_MMC.begin("/sdcard", true, false, 10000)) {
    Serial.println("SD: 1-bit mode, 10MHz - OK");
    sdMounted = true;
  }
  
  if (!sdMounted) {
    Serial.println("Proba montowania SD w trybie 1-bit, 20MHz...");
    if (SD_MMC.begin("/sdcard", true, false, 20000)) {
      Serial.println("SD: 1-bit mode, 20MHz - OK");
      sdMounted = true;
    }
  }
  
  if (!sdMounted) {
    Serial.println("Proba montowania SD w trybie 1-bit, domyslna czest...");
    if (SD_MMC.begin("/sdcard", true)) {
      Serial.println("SD: 1-bit mode, default freq - OK");
      sdMounted = true;
    }
  }
  
  if (!sdMounted) {
    Serial.println("UWAGA: Nie mozna zamontowac karty SD!");
  }
  
  if (sdMounted) {
    sdCardWorking = testSDCard();
    
    if (sdCardWorking) {
      Serial.println("Karta SD dziala poprawnie");
      createDirectories();
      
      uint8_t cardType = SD_MMC.cardType();
      Serial.print("Typ karty: ");
      switch(cardType) {
        case CARD_MMC: Serial.println("MMC"); break;
        case CARD_SD: Serial.println("SDSC"); break;
        case CARD_SDHC: Serial.println("SDHC"); break;
        case CARD_UNKNOWN: Serial.println("Nieznany"); break;
        default: Serial.println("Brak"); break;
      }
      
      uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
      Serial.printf("Rozmiar: %lluMB\n", cardSize);
      Serial.printf("Uzyte: %lluMB\n", SD_MMC.usedBytes() / (1024 * 1024));
      Serial.printf("Wolne: %lluMB\n", (SD_MMC.cardSize() - SD_MMC.usedBytes()) / (1024 * 1024));
    } else {
      Serial.println("Test karty SD nieudany");
      SD_MMC.end();
    }
  }
  
  // Fallback na SPIFFS
  if (!sdCardWorking) {
    Serial.println("Uzywam SPIFFS jako backup");
    if (SPIFFS.begin(true)) {
      useSpiffs = true;
      Serial.println("SPIFFS OK");
    } else {
      Serial.println("SPIFFS blad - tylko RAM");
    }
  }
}

bool testSDCard() {
  for (int attempt = 1; attempt <= 5; attempt++) {
    Serial.printf("Test SD - proba %d/5\n", attempt);
    
    String testPath = "/test_" + String(millis()) + ".txt";
    String testData = "Test " + String(attempt) + " " + String(millis());
    
    File testFile = SD_MMC.open(testPath, FILE_WRITE);
    if (testFile) {
      testFile.print(testData);
      testFile.flush();
      testFile.close();
      
      delay(100);
      
      testFile = SD_MMC.open(testPath, FILE_READ);
      if (testFile) {
        String readData = testFile.readString();
        testFile.close();
        SD_MMC.remove(testPath);
        
        if (readData == testData) {
          Serial.println("Test SD - sukces!");
          return true;
        }
      }
    }
    delay(200);
  }
  return false;
}

void createDirectories() {
  const char* dirs[] = {"/config", "/playlists", "/logs"};
  for (const char* dir : dirs) {
    if (!SD_MMC.exists(dir)) {
      if (SD_MMC.mkdir(dir)) {
        Serial.printf("Utworzono folder: %s\n", dir);
      }
      delay(50);
    }
  }
}

void loadConfig() {
  // Domyślne wartości
  config.ssid = "YourWiFi";
  config.password = "YourPassword";
  config.volume = 15;
  config.defaultStation = "http://195.150.20.245/rmf_fm";
  config.autoPlay = true;
  config.adminPassword = "admin123";
  
  String configPath = sdCardWorking ? "/config/config.json" : 
                     (useSpiffs ? "/config.json" : "");
  
  if (!configPath.isEmpty()) {
    File configFile = sdCardWorking ? SD_MMC.open(configPath) : SPIFFS.open(configPath);
    if (configFile) {
      StaticJsonDocument<1024> doc;
      DeserializationError error = deserializeJson(doc, configFile);
      configFile.close();
      
      if (!error) {
        config.ssid = doc["ssid"] | config.ssid;
        config.password = doc["password"] | config.password;
        config.volume = doc["volume"] | config.volume;
        config.defaultStation = doc["defaultStation"] | config.defaultStation;
        config.autoPlay = doc["autoPlay"] | config.autoPlay;
        config.adminPassword = doc["adminPassword"] | config.adminPassword;
        Serial.println("Konfiguracja zaladowana");
      }
    }
  }
}

void saveConfig() {
  StaticJsonDocument<1024> doc;
  doc["ssid"] = config.ssid;
  doc["password"] = config.password;
  doc["volume"] = config.volume;
  doc["defaultStation"] = config.defaultStation;
  doc["autoPlay"] = config.autoPlay;
  doc["adminPassword"] = config.adminPassword;
  
  String jsonString;
  serializeJson(doc, jsonString);
  
  bool saved = false;
  
  if (sdCardWorking) {
    for (int attempt = 1; attempt <= 5; attempt++) {
      if (SD_MMC.exists("/config/config.json")) {
        SD_MMC.remove("/config/config.json");
        delay(50);
      }
      
      File configFile = SD_MMC.open("/config/config.json", FILE_WRITE);
      if (configFile) {
        configFile.print(jsonString);
        configFile.flush();
        configFile.close();
        
        delay(200);
        
        File verifyFile = SD_MMC.open("/config/config.json", FILE_READ);
        if (verifyFile) {
          String verify = verifyFile.readString();
          verifyFile.close();
          
          if (verify == jsonString) {
            Serial.println("Config zapisany na SD");
            saved = true;
            break;
          }
        }
      }
      
      if (!saved && attempt < 5) {
        delay(500);
      }
    }
  } else if (useSpiffs) {
    File configFile = SPIFFS.open("/config.json", FILE_WRITE);
    if (configFile) {
      configFile.print(jsonString);
      configFile.close();
      saved = true;
      Serial.println("Config zapisany w SPIFFS");
    }
  }
  
  if (!saved) {
    Serial.println("BLAD: Nie udalo sie zapisac konfiguracji!");
  }
}

void loadPlaylists() {
  // Domyślne stacje
  playlist.clear();
  playlist.push_back({"RMF FM", "http://195.150.20.245/rmf_fm", "Pop"});
  playlist.push_back({"RMF Classic", "https://rs102-krk.rmfstream.pl/rmf_classic", "Classical"});
  playlist.push_back({"RMF Maxx", "https://rs102-krk.rmfstream.pl/rmf_maxxx", "Dance"});
  playlist.push_back({"Radio ZET", "http://stream.rcs.revma.com/ypqt40u0x1zuv", "Pop"});
  playlist.push_back({"TOK FM", "http://195.150.20.245/tok_fm", "Talk"});
  playlist.push_back({"Antyradio", "http://an.cdn.eurozet.pl/ant-waw.mp3", "Rock"});
  playlist.push_back({"Radio Zlote Przeboje", "http://stream.rcs.revma.com/7kqg3htqn1zuv", "Oldies"});
  playlist.push_back({"Radio WAWA", "http://stream.rcs.revma.com/7r5cxydcn1zuv", "Pop"});
  playlist.push_back({"Radio 357", "http://stream3.nadaje.com:8048/rock", "Alternative"});
  
  String playlistPath = sdCardWorking ? "/playlists/stations.json" : 
                       (useSpiffs ? "/stations.json" : "");
  
  if (!playlistPath.isEmpty()) {
    File playlistFile = sdCardWorking ? SD_MMC.open(playlistPath) : SPIFFS.open(playlistPath);
    if (playlistFile) {
      DynamicJsonDocument doc(4096);
      DeserializationError error = deserializeJson(doc, playlistFile);
      playlistFile.close();
      
      if (!error) {
        playlist.clear();
        JsonArray stations = doc["stations"];
        for (JsonObject station : stations) {
          RadioStation s;
          s.name = station["name"] | "";
          s.url = station["url"] | "";
          s.genre = station["genre"] | "";
          if (!s.name.isEmpty() && !s.url.isEmpty()) {
            playlist.push_back(s);
          }
        }
        Serial.printf("Zaladowano %d stacji\n", playlist.size());
      }
    }
  }
}

void savePlaylists() {
  DynamicJsonDocument doc(4096);
  JsonArray stations = doc.createNestedArray("stations");
  
  for (const auto& station : playlist) {
    JsonObject s = stations.createNestedObject();
    s["name"] = station.name;
    s["url"] = station.url;
    s["genre"] = station.genre;
  }
  
  String jsonString;
  serializeJson(doc, jsonString);
  
  if (sdCardWorking) {
    File playlistFile = SD_MMC.open("/playlists/stations.json", FILE_WRITE);
    if (playlistFile) {
      playlistFile.print(jsonString);
      playlistFile.flush();
      playlistFile.close();
      Serial.println("Playlista zapisana");
    }
  } else if (useSpiffs) {
    File playlistFile = SPIFFS.open("/stations.json", FILE_WRITE);
    if (playlistFile) {
      playlistFile.print(jsonString);
      playlistFile.close();
    }
  }
}

void connectWiFi() {
  Serial.print("Laczenie z WiFi: ");
  Serial.println(config.ssid);
  
  WiFi.begin(config.ssid.c_str(), config.password.c_str());
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    digitalWrite(LED_STATUS, !digitalRead(LED_STATUS));
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("WiFi polaczone! IP: ");
    Serial.println(WiFi.localIP());
    digitalWrite(LED_STATUS, HIGH);
  } else {
    Serial.println();
    Serial.println("Brak polaczenia WiFi - uruchamiam AP");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ESP32_Radio_Config", "12345678");
    Serial.print("Access Point IP: ");
    Serial.println(WiFi.softAPIP());
    blinkLED(10, 100);
  }
}

void handleButton() {
  unsigned long currentTime = millis();
  
  // Debouncing
  if (currentTime - lastButtonCheck < DEBOUNCE_TIME) {
    return;
  }
  
  lastButtonCheck = currentTime;
  bool reading = digitalRead(BUTTON_PIN);
  
  // Detect button state change
  if (reading != lastButtonState) {
    if (reading == LOW) {
      // Button pressed down
      buttonPressStart = currentTime;
      buttonPressed = true;
      buttonLongPressed = false;
    } else {
      // Button released
      if (buttonPressed) {
        unsigned long pressDuration = currentTime - buttonPressStart;
        
        if (pressDuration >= LONG_PRESS_TIME) {
          // Long press - next station
          Serial.println("Przycisk - dlugie nacisnicie: nastepna stacja");
          nextStation();
        } else if (pressDuration >= SHORT_PRESS_TIME) {
          // Short press - start/stop
          Serial.println("Przycisk - krotkie nacisnicie: start/stop");
          toggleRadio();
        }
        
        buttonPressed = false;
      }
    }
    
    lastButtonState = reading;
  }
  
  // Check for long press while button is still held
  if (buttonPressed && !buttonLongPressed && 
      (currentTime - buttonPressStart >= LONG_PRESS_TIME)) {
    buttonLongPressed = true;
    // Visual feedback for long press
    blinkLED(2, 100);
  }
}

void toggleRadio() {
  if (isPlaying) {
    stopRadio();
    Serial.println("Radio zatrzymane przez przycisk");
  } else {
    // Start last played station or first in playlist
    String urlToPlay = "";
    if (!config.defaultStation.isEmpty()) {
      urlToPlay = config.defaultStation;
    } else if (playlist.size() > 0) {
      urlToPlay = playlist[currentStationIndex].url;
    }
    
    if (!urlToPlay.isEmpty()) {
      startRadio(urlToPlay);
      Serial.println("Radio uruchomione przez przycisk");
    }
  }
}

void nextStation() {
  if (playlist.size() == 0) return;
  
  currentStationIndex = (currentStationIndex + 1) % playlist.size();
  String nextUrl = playlist[currentStationIndex].url;
  Serial.print("Przechodzenie na stacje: ");
  Serial.println(playlist[currentStationIndex].name);
  startRadio(nextUrl);
  
  // Visual feedback
  blinkLED(3, 150);
}

void startRadio(String url) {
  Serial.print("Startowanie radia: ");
  Serial.println(url);
  
  audio.stopSong();
  delay(100);
  
  // Reset metadanych
  streamTitle = "";
  stationName = "";
  streamBitrate = "";
  streamGenre = "";
  
  if (audio.connecttohost(url.c_str())) {
    isPlaying = true;
    currentStation = url;
    config.defaultStation = url;
    saveLastStation();
    Serial.println("Radio uruchomione!");
    
    // Znajdź nazwę stacji na liście
    for (size_t i = 0; i < playlist.size(); i++) {
      if (playlist[i].url == url) {
        stationName = playlist[i].name;
        currentStationIndex = i;
        break;
      }
    }
  } else {
    Serial.println("Blad uruchamiania radia!");
    isPlaying = false;
    currentStation = "";
  }
}

void stopRadio() {
  Serial.println("Zatrzymywanie radia...");
  audio.stopSong();
  isPlaying = false;
  currentStation = "";
  streamTitle = "";
  stationName = "";
  streamBitrate = "";
  streamGenre = "";
  
  // Stop sleep timer when radio is stopped
  sleepTimerActive = false;
  sleepTimer = 0;
}

void setSleepTimer(int minutes) {
  if (minutes > 0) {
    sleepTimer = minutes * 60000UL; // Convert to milliseconds
    sleepTimerStart = millis();
    sleepTimerActive = true;
    Serial.printf("Sleep timer ustawiony na %d minut\n", minutes);
  } else {
    sleepTimerActive = false;
    sleepTimer = 0;
    Serial.println("Sleep timer anulowany");
  }
}

void checkSleepTimer() {
  if (sleepTimerActive && isPlaying) {
    unsigned long elapsed = millis() - sleepTimerStart;
    if (elapsed >= sleepTimer) {
      Serial.println("Sleep timer - zatrzymywanie radia");
      stopRadio();
      sleepTimerActive = false;
      sleepTimer = 0;
      
      // Visual feedback
      blinkLED(5, 200);
    }
  }
}

void updateStatusLED() {
  static unsigned long lastBlink = 0;
  static bool ledState = false;
  
  if (isPlaying) {
    digitalWrite(LED_STATUS, HIGH);
  } else if (WiFi.status() == WL_CONNECTED) {
    if (millis() - lastBlink > 2000) {
      lastBlink = millis();
      ledState = !ledState;
      digitalWrite(LED_STATUS, ledState);
    }
  } else {
    if (millis() - lastBlink > 200) {
      lastBlink = millis();
      ledState = !ledState;
      digitalWrite(LED_STATUS, ledState);
    }
  }
}

void saveLastStation() {
  // Zapisz indeks ostatniej stacji w EEPROM
  EEPROM.put(LAST_STATION_ADDR, currentStationIndex);
  EEPROM.put(LAST_VOLUME_ADDR, config.volume);
  EEPROM.commit();
}

void loadLastStation() {
  // Wczytaj ostatnią stację z EEPROM
  int lastIndex;
  int lastVolume;
  
  EEPROM.get(LAST_STATION_ADDR, lastIndex);
  EEPROM.get(LAST_VOLUME_ADDR, lastVolume);
  
  if (lastIndex >= 0 && lastIndex < playlist.size()) {
    currentStationIndex = lastIndex;
    config.defaultStation = playlist[lastIndex].url;
  }
  
  if (lastVolume > 0 && lastVolume <= 21) {
    config.volume = lastVolume;
    audio.setVolume(config.volume);
  }
}

void setupWebServer() {
  // Główna strona
  server.on("/", []() {
    server.send(200, "text/html", getEnhancedHTML());
  });
  
  // Konfiguracja
  server.on("/config", []() {
    server.send(200, "text/html", getConfigHTML());
  });
  
  // Menedżer stacji
  server.on("/stations", []() {
    server.send(200, "text/html", getStationManagerHTML());
  });
  
  // API endpoints
  server.on("/api/play", []() {
    if (server.hasArg("url")) {
      startRadio(server.arg("url"));
      server.send(200, "text/plain", "OK");
    } else {
      server.send(400, "text/plain", "Missing URL");
    }
  });
  
  server.on("/api/start_last", []() {
    toggleRadio();
    server.send(200, "text/plain", "OK");
  });
  
  server.on("/api/stop", []() {
    stopRadio();
    server.send(200, "text/plain", "OK");
  });
  
  server.on("/api/volume", []() {
    if (server.hasArg("level")) {
      int vol = server.arg("level").toInt();
      if (vol >= 0 && vol <= 21) {
        config.volume = vol;
        audio.setVolume(config.volume);
        saveLastStation();
        server.send(200, "text/plain", "OK");
      } else {
        server.send(400, "text/plain", "Invalid volume");
      }
    } else {
      server.send(400, "text/plain", "Missing level");
    }
  });
  
  server.on("/api/sleep_timer", []() {
    if (server.hasArg("minutes")) {
      int minutes = server.arg("minutes").toInt();
      setSleepTimer(minutes);
      server.send(200, "text/plain", "OK");
    } else {
      server.send(400, "text/plain", "Missing minutes");
    }
  });
  
  server.on("/api/status", []() {
    String json = "{";
    json += "\"playing\":" + String(isPlaying ? "true" : "false") + ",";
    json += "\"station\":\"" + stationName + "\",";
    json += "\"title\":\"" + streamTitle + "\",";
    json += "\"bitrate\":\"" + streamBitrate + "\",";
    json += "\"genre\":\"" + streamGenre + "\",";
    json += "\"volume\":" + String(config.volume) + ",";
    json += "\"sleepTimer\":" + String(sleepTimerActive ? "true" : "false") + ",";
    if (sleepTimerActive) {
      unsigned long remaining = (sleepTimer - (millis() - sleepTimerStart)) / 60000;
      json += "\"sleepRemaining\":" + String(remaining);
    } else {
      json += "\"sleepRemaining\":0";
    }
    json += "}";
    server.send(200, "application/json", json);
  });
  
  server.on("/api/stations", []() {
    String json = "{\"stations\":[";
    for (size_t i = 0; i < playlist.size(); i++) {
      if (i > 0) json += ",";
      json += "{\"name\":\"" + playlist[i].name + "\",";
      json += "\"url\":\"" + playlist[i].url + "\",";
      json += "\"genre\":\"" + playlist[i].genre + "\"}";
    }
    json += "]}";
    server.send(200, "application/json", json);
  });
  
  // Dodawanie stacji
  server.on("/api/add_station", []() {
    if (server.hasArg("name") && server.hasArg("url") && server.hasArg("genre")) {
      RadioStation newStation;
      newStation.name = server.arg("name");
      newStation.url = server.arg("url");
      newStation.genre = server.arg("genre");
      playlist.push_back(newStation);
      savePlaylists();
      server.send(200, "text/plain", "Station added");
    } else {
      server.send(400, "text/plain", "Missing parameters");
    }
  });
  
  // Usuwanie stacji
  server.on("/api/remove_station", []() {
    if (server.hasArg("index")) {
      int index = server.arg("index").toInt();
      if (index >= 0 && index < playlist.size()) {
        playlist.erase(playlist.begin() + index);
        savePlaylists();
        server.send(200, "text/plain", "Station removed");
      } else {
        server.send(400, "text/plain", "Invalid index");
      }
    } else {
      server.send(400, "text/plain", "Missing index");
    }
  });
  
  // Zapisz konfigurację WiFi
  server.on("/api/save_config", []() {
    if (server.hasArg("ssid") && server.hasArg("password")) {
      config.ssid = server.arg("ssid");
      config.password = server.arg("password");
      if (server.hasArg("autoplay")) {
        config.autoPlay = server.arg("autoplay") == "true";
      }
      saveConfig();
      server.send(200, "text/plain", "Config saved - restarting...");
      delay(1000);
      ESP.restart();
    } else {
      server.send(400, "text/plain", "Missing parameters");
    }
  });
}

String getEnhancedHTML() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<title>Internet Radio</title>";
  html += "<meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body{font-family:'Segoe UI',Arial,sans-serif;margin:0;padding:20px;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);color:white}";
  html += ".container{max-width:900px;margin:0 auto;background:rgba(255,255,255,0.1);padding:30px;border-radius:20px;backdrop-filter:blur(10px);box-shadow:0 8px 32px rgba(0,0,0,0.3)}";
  html += "h1{text-align:center;font-size:2.5em;margin-bottom:10px;text-shadow:2px 2px 4px rgba(0,0,0,0.5)}";
  html += ".status{background:rgba(255,255,255,0.2);padding:20px;border-radius:15px;margin-bottom:25px;backdrop-filter:blur(5px)}";
  html += ".now-playing{font-size:1.4em;font-weight:bold;margin-bottom:10px;color:#ffeb3b}";
  html += ".controls{display:flex;justify-content:center;gap:15px;margin:25px 0;flex-wrap:wrap}";
  html += "button{padding:12px 25px;border:none;border-radius:25px;cursor:pointer;font-size:16px;font-weight:bold;transition:all 0.3s ease;text-transform:uppercase}";
  html += ".play-btn{background:linear-gradient(45deg,#4CAF50,#45a049);color:white;box-shadow:0 4px 15px rgba(76,175,80,0.4)}";
  html += ".stop-btn{background:linear-gradient(45deg,#f44336,#d32f2f);color:white;box-shadow:0 4px 15px rgba(244,67,54,0.4)}";
  html += ".config-btn{background:linear-gradient(45deg,#2196F3,#1976D2);color:white;box-shadow:0 4px 15px rgba(33,150,243,0.4)}";
  html += ".timer-btn{background:linear-gradient(45deg,#FF9800,#F57C00);color:white;box-shadow:0 4px 15px rgba(255,152,0,0.4);font-size:12px;padding:8px 15px}";
  html += "button:hover{transform:translateY(-2px);box-shadow:0 6px 20px rgba(0,0,0,0.3)}";
  html += ".volume-control{display:flex;align-items:center;justify-content:center;gap:15px;margin:20px 0}";
  html += "input[type=range]{width:200px;height:8px;border-radius:4px;background:#ddd;outline:none;-webkit-appearance:none}";
  html += "input[type=range]::-webkit-slider-thumb{appearance:none;width:20px;height:20px;border-radius:50%;background:#4CAF50;cursor:pointer}";
  html += ".timer-controls{text-align:center;margin:20px 0;padding:15px;background:rgba(255,255,255,0.1);border-radius:10px}";
  html += ".timer-status{margin-top:10px;font-weight:bold;color:#ffeb3b}";
  html += ".stations{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:15px;margin-top:25px}";
  html += ".station{background:rgba(255,255,255,0.15);padding:15px;border-radius:12px;cursor:pointer;transition:all 0.3s ease;backdrop-filter:blur(5px)}";
  html += ".station:hover{background:rgba(255,255,255,0.25);transform:translateY(-3px)}";
  html += ".station-name{font-weight:bold;font-size:1.1em;margin-bottom:5px}";
  html += ".station-genre{color:#b0bec5;font-size:0.9em}";
  html += ".metadata{background:rgba(0,0,0,0.2);padding:15px;border-radius:10px;margin:15px 0;font-family:monospace;font-size:17px}";  // Increased font size
  html += "@media (max-width:600px){.container{padding:15px}.controls{flex-direction:column}.stations{grid-template-columns:1fr}.timer-controls button{margin:5px;font-size:10px;padding:6px 12px}}";
  html += "</style></head><body>";
  html += "<div class='container'>";
  html += "<h1>🎵 Internet Radio</h1>";
  
  // Status i now playing
  html += "<div class='status'>";
  html += "<div class='now-playing' id='nowPlaying'>Loading...</div>";
  html += "<div class='metadata' id='metadata'></div>";
  html += "</div>";
  
  // Kontrola głośności
  html += "<div class='volume-control'>";
  html += "🔈 <input type='range' id='volumeSlider' min='0' max='21' value='" + String(config.volume) + "' onchange='setVolume(this.value)'> 🔊";
  html += "<span id='volumeValue'>" + String(config.volume) + "</span>";
  html += "</div>";
  
  // Sleep Timer
  html += "<div class='timer-controls'>";
  html += "<h4>⏰ Sleep Timer</h4>";
  html += "<button class='timer-btn' onclick='setSleepTimer(30)'>30 MIN</button>";
  html += "<button class='timer-btn' onclick='setSleepTimer(60)'>1 GODZ</button>";
  html += "<button class='timer-btn' onclick='setSleepTimer(120)'>2 GODZ</button>";
  html += "<button class='timer-btn' onclick='setSleepTimer(0)'>ANULUJ</button>";
  html += "<div class='timer-status' id='timerStatus'></div>";
  html += "</div>";
  
  // Kontrole
  html += "<div class='controls'>";
  html += "<button class='play-btn' onclick='startLastRadio()'>▶️ PLAY</button>";  // New play button
  html += "<button class='stop-btn' onclick='stopRadio()'>⏹️ STOP</button>";
  html += "<button class='config-btn' onclick='location.href=\"/config\"'>⚙️ CONFIG</button>";
  html += "<button class='config-btn' onclick='location.href=\"/stations\"'>📻 STATIONS</button>";
  html += "</div>";
  
  // Lista stacji
  html += "<h3>📻 Available Stations</h3>";
  html += "<div class='stations' id='stationsList'></div>";
  
  html += "</div>";
  
  // JavaScript
  html += "<script>";
  html += "function playStation(url,name) { ";
  html += "  fetch('/api/play?url=' + encodeURIComponent(url)); ";
  html += "  setTimeout(updateStatus, 1000); ";
  html += "}";
  html += "function startLastRadio() { ";
  html += "  fetch('/api/start_last'); ";
  html += "  setTimeout(updateStatus, 1000); ";
  html += "}";
  html += "function stopRadio() { ";
  html += "  fetch('/api/stop'); ";
  html += "  setTimeout(updateStatus, 500); ";
  html += "}";
  html += "function setVolume(vol) { ";
  html += "  fetch('/api/volume?level=' + vol); ";
  html += "  document.getElementById('volumeValue').innerText = vol; ";
  html += "}";
  html += "function setSleepTimer(minutes) { ";
  html += "  fetch('/api/sleep_timer?minutes=' + minutes); ";
  html += "  setTimeout(updateStatus, 500); ";
  html += "}";
  html += "function updateStatus() { ";
  html += "  fetch('/api/status') ";
  html += "    .then(response => response.json()) ";
  html += "    .then(data => { ";
  html += "      const nowPlaying = document.getElementById('nowPlaying'); ";
  html += "      const metadata = document.getElementById('metadata'); ";
  html += "      const timerStatus = document.getElementById('timerStatus'); ";
  html += "      if (data.playing) { ";
  html += "        nowPlaying.innerHTML = '🎶 ' + data.station; ";
  html += "        let meta = ''; ";
  html += "        if (data.title) meta += '<strong>♪ ' + data.title + '</strong><br>'; ";
  html += "        if (data.bitrate) meta += '📊 ' + data.bitrate + ' | '; ";
  html += "        if (data.genre) meta += '🎵 ' + data.genre; ";
  html += "        metadata.innerHTML = meta || 'No metadata available'; ";
  html += "      } else { ";
  html += "        nowPlaying.innerHTML = '⏹️ Radio Stopped'; ";
  html += "        metadata.innerHTML = 'Ready to play...'; ";
  html += "      } ";
  html += "      if (data.sleepTimer && data.sleepRemaining > 0) { ";
  html += "        timerStatus.innerHTML = '⏰ Timer aktywny: ' + data.sleepRemaining + ' min pozostało'; ";
  html += "      } else { ";
  html += "        timerStatus.innerHTML = ''; ";
  html += "      } ";
  html += "    }); ";
  html += "}";
  html += "function loadStations() { ";
  html += "  fetch('/api/stations') ";
  html += "    .then(response => response.json()) ";
  html += "    .then(data => { ";
  html += "      const list = document.getElementById('stationsList'); ";
  html += "      list.innerHTML = ''; ";
  html += "      data.stations.forEach((station, index) => { ";
  html += "        const div = document.createElement('div'); ";
  html += "        div.className = 'station'; ";
  html += "        div.onclick = () => playStation(station.url, station.name); ";
  html += "        div.innerHTML = `<div class='station-name'>${station.name}</div><div class='station-genre'>${station.genre}</div>`; ";
  html += "        list.appendChild(div); ";
  html += "      }); ";
  html += "    }); ";
  html += "}";
  html += "setInterval(updateStatus, 3000); ";
  html += "updateStatus(); ";
  html += "loadStations(); ";
  html += "</script>";
  
  html += "</body></html>";
  return html;
}

String getConfigHTML() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<title>Radio - Configuration</title>";
  html += "<meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body{font-family:'Segoe UI',Arial,sans-serif;margin:0;padding:20px;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);color:white}";
  html += ".container{max-width:600px;margin:0 auto;background:rgba(255,255,255,0.1);padding:30px;border-radius:20px;backdrop-filter:blur(10px)}";
  html += "h1{text-align:center;margin-bottom:30px}";
  html += ".form-group{margin-bottom:20px}";
  html += "label{display:block;margin-bottom:8px;font-weight:bold}";
  html += "input,select{width:100%;padding:12px;border:none;border-radius:8px;font-size:16px;box-sizing:border-box}";
  html += "button{width:100%;padding:15px;border:none;border-radius:25px;background:linear-gradient(45deg,#4CAF50,#45a049);color:white;font-size:18px;font-weight:bold;cursor:pointer;margin-top:20px}";
  html += "button:hover{background:linear-gradient(45deg,#45a049,#4CAF50)}";
  html += ".back-btn{background:linear-gradient(45deg,#2196F3,#1976D2);margin-bottom:20px}";
  html += "input[type=checkbox]{width:auto;margin-right:10px}";
  html += ".info-panel{background:rgba(255,255,255,0.1);padding:15px;border-radius:10px;margin-bottom:20px;font-size:14px}";
  html += "</style></head><body>";
  html += "<div class='container'>";
  html += "<h1>⚙️ Configuration</h1>";
  html += "<button class='back-btn' onclick='location.href=\"/\"'>🏠 Back to Radio</button>";
  
  // Info panel about button usage
  html += "<div class='info-panel'>";
  html += "<h4>🎛️ Button Control (GPIO0):</h4>";
  html += "• <strong>Krótkie naciśnięcie:</strong> Start/Stop radia<br>";
  html += "• <strong>Długie naciśnięcie (2s):</strong> Następna stacja";
  html += "</div>";
  
  html += "<form id='configForm'>";
  html += "<div class='form-group'>";
  html += "<label>WiFi Network Name (SSID):</label>";
  html += "<input type='text' id='ssid' value='" + config.ssid + "'>";
  html += "</div>";
  html += "<div class='form-group'>";
  html += "<label>WiFi Password:</label>";
  html += "<input type='password' id='password' value='" + config.password + "'>";
  html += "</div>";
  html += "<div class='form-group'>";
  html += "<input type='checkbox' id='autoplay' " + String(config.autoPlay ? "checked" : "") + ">";
  html += "<label for='autoplay'>Auto-play last station on startup</label>";
  html += "</div>";
  html += "<button type='button' onclick='saveConfig()'>💾 Save & Restart</button>";
  html += "</form>";
  html += "</div>";
  html += "<script>";
  html += "function saveConfig() {";
  html += "  const ssid = document.getElementById('ssid').value;";
  html += "  const password = document.getElementById('password').value;";
  html += "  const autoplay = document.getElementById('autoplay').checked;";
  html += "  if (ssid && password) {";
  html += "    fetch('/api/save_config?ssid=' + encodeURIComponent(ssid) + '&password=' + encodeURIComponent(password) + '&autoplay=' + autoplay);";
  html += "    alert('Configuration saved! Device will restart...');";
  html += "  } else {";
  html += "    alert('Please fill in all fields');";
  html += "  }";
  html += "}";
  html += "</script></body></html>";
  return html;
}

String getStationManagerHTML() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<title>Radio - Station Manager</title>";
  html += "<meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body{font-family:'Segoe UI',Arial,sans-serif;margin:0;padding:20px;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);color:white}";
  html += ".container{max-width:800px;margin:0 auto;background:rgba(255,255,255,0.1);padding:30px;border-radius:20px;backdrop-filter:blur(10px)}";
  html += "h1{text-align:center;margin-bottom:30px}";
  html += ".form-group{margin-bottom:15px}";
  html += "label{display:block;margin-bottom:5px;font-weight:bold}";
  html += "input,select{width:100%;padding:10px;border:none;border-radius:8px;font-size:14px;box-sizing:border-box}";
  html += "button{padding:10px 20px;border:none;border-radius:20px;cursor:pointer;font-weight:bold;margin:5px}";
  html += ".add-btn{background:linear-gradient(45deg,#4CAF50,#45a049);color:white}";
  html += ".remove-btn{background:linear-gradient(45deg,#f44336,#d32f2f);color:white}";
  html += ".back-btn{background:linear-gradient(45deg,#2196F3,#1976D2);color:white;width:100%;margin-bottom:20px}";
  html += ".station-item{background:rgba(255,255,255,0.15);padding:15px;margin:10px 0;border-radius:10px;display:flex;justify-content:space-between;align-items:center}";
  html += ".station-info{flex-grow:1}";
  html += ".station-name{font-weight:bold;font-size:1.1em}";
  html += ".station-url{color:#b0bec5;font-size:0.9em;word-break:break-all}";
  html += "</style></head><body>";
  html += "<div class='container'>";
  html += "<h1>📻 Station Manager</h1>";
  html += "<button class='back-btn' onclick='location.href=\"/\"'>🏠 Back to Radio</button>";
  html += "<h3>➕ Add New Station</h3>";
  html += "<div class='form-group'>";
  html += "<label>Station Name:</label>";
  html += "<input type='text' id='newName' placeholder='e.g. My Favorite Radio'>";
  html += "</div>";
  html += "<div class='form-group'>";
  html += "<label>Stream URL:</label>";
  html += "<input type='text' id='newUrl' placeholder='http://example.com/stream'>";
  html += "</div>";
  html += "<div class='form-group'>";
  html += "<label>Genre:</label>";
  html += "<input type='text' id='newGenre' placeholder='e.g. Pop, Rock, Jazz'>";
  html += "</div>";
  html += "<button class='add-btn' onclick='addStation()'>➕ Add Station</button>";
  html += "<h3>📋 Current Stations</h3>";
  html += "<div id='stationsList'></div>";
  html += "</div>";
  html += "<script>";
  html += "function addStation() {";
  html += "  const name = document.getElementById('newName').value;";
  html += "  const url = document.getElementById('newUrl').value;";
  html += "  const genre = document.getElementById('newGenre').value;";
  html += "  if (name && url && genre) {";
  html += "    fetch('/api/add_station?name=' + encodeURIComponent(name) + '&url=' + encodeURIComponent(url) + '&genre=' + encodeURIComponent(genre))";
  html += "      .then(() => {";
  html += "        document.getElementById('newName').value = '';";
  html += "        document.getElementById('newUrl').value = '';";
  html += "        document.getElementById('newGenre').value = '';";
  html += "        loadStations();";
  html += "      });";
  html += "  } else {";
  html += "    alert('Please fill in all fields');";
  html += "  }";
  html += "}";
  html += "function removeStation(index) {";
  html += "  if (confirm('Are you sure you want to remove this station?')) {";
  html += "    fetch('/api/remove_station?index=' + index).then(() => loadStations());";
  html += "  }";
  html += "}";
  html += "function loadStations() {";
  html += "  fetch('/api/stations')";
  html += "    .then(response => response.json())";
  html += "    .then(data => {";
  html += "      const list = document.getElementById('stationsList');";
  html += "      list.innerHTML = '';";
  html += "      data.stations.forEach((station, index) => {";
  html += "        const div = document.createElement('div');";
  html += "        div.className = 'station-item';";
  html += "        div.innerHTML = `";
  html += "          <div class='station-info'>";
  html += "            <div class='station-name'>${station.name} (${station.genre})</div>";
  html += "            <div class='station-url'>${station.url}</div>";
  html += "          </div>";
  html += "          <button class='remove-btn' onclick='removeStation(${index})'>🗑️ Remove</button>";
  html += "        `;";
  html += "        list.appendChild(div);";
  html += "      });";
  html += "    });";
  html += "}";
  html += "loadStations();";
  html += "</script></body></html>";
  return html;
}