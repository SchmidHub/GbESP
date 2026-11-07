// GrindByWeight – Hardwaretest
// Testet: SSD1306-Display, NAU7802 + Waegezelle, Drehencoder (Drehen + Taster), Deep Sleep / Wecken per Taster
//
// Bedienung:
//   - Drehen        -> Encoder-Zaehler aendert sich
//   - kurz druecken -> Klick-Zaehler steigt
//   - 2 s halten    -> Deep Sleep, Wecken durch erneuten Druck

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SparkFun_Qwiic_Scale_NAU7802_Arduino_Library.h>
#include "esp_sleep.h"
#include "driver/rtc_io.h"

constexpr gpio_num_t PIN_SW  = GPIO_NUM_0;  // D0 – Encoder-Taster (Weckpin)
constexpr int        PIN_CLK = 1;           // D1 – Encoder A
constexpr int        PIN_DT  = 2;           // D2 – Encoder B
constexpr int        PIN_SDA = 22;          // D4
constexpr int        PIN_SCL = 23;          // D5

constexpr uint32_t LONG_PRESS_MS = 2000;

Adafruit_SSD1306 display(128, 64, &Wire, -1);
NAU7802 scale;

bool displayOk = false;
bool scaleOk   = false;

volatile int32_t encPos  = 0;  // Anzahl Rastungen
volatile uint8_t encPrev = 0;  // letzter Zustand (CLK<<1 | DT)
volatile int8_t  encAcc  = 0;  // Teilschritte seit letzter Raststellung
uint32_t clicks = 0;

// Quadratur-Zustandsautomat: gueltige Uebergaenge zaehlen +1/-1, ungueltige 0.
// Prellen erzeugt Vor-/Zurueck-Paare, die sich gegenseitig aufheben.
void IRAM_ATTR onEncoder() {
  static const int8_t table[16] = { 0, -1,  1,  0,
                                    1,  0,  0, -1,
                                   -1,  0,  0,  1,
                                    0,  1, -1,  0 };
  uint8_t cur = (digitalRead(PIN_CLK) << 1) | digitalRead(PIN_DT);
  encAcc  = encAcc + table[(encPrev << 2) | cur];
  encPrev = cur;

  // Erst in der Raststellung (beide HIGH) entscheiden, ob eine Rastung vollstaendig war
  if (cur == 0b11) {
    if      (encAcc >=  2) encPos = encPos - 1;
    else if (encAcc <= -2) encPos = encPos + 1;
    encAcc = 0;
  }
}

const char* wakeReason() {
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT1:      return "Taster";
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "Reset";
    default:                         return "anderer";
  }
}

void goToSleep() {
  Serial.println("Gehe in Deep Sleep ...");

  if (displayOk) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 20);
    display.print("Deep Sleep...");
    display.setCursor(0, 36);
    display.print("Taster loslassen");
    display.display();
  }

  // Erst loslassen, sonst weckt der gedrueckte Taster sofort wieder
  while (digitalRead(PIN_SW) == LOW) delay(10);
  delay(300);

  if (displayOk) display.ssd1306_command(SSD1306_DISPLAYOFF);
  if (scaleOk)   scale.powerDown();

  // Pull-up im LP-Bereich aktiv halten, sonst floatet der Pin im Deep Sleep
  rtc_gpio_init(PIN_SW);
  rtc_gpio_set_direction(PIN_SW, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en(PIN_SW);
  rtc_gpio_pulldown_dis(PIN_SW);

  esp_sleep_enable_ext1_wakeup(1ULL << PIN_SW, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_deep_sleep_start();
}

void setup() {
  Serial.setTxTimeoutMs(0);  // nicht blockieren, wenn kein Serial Monitor liest
  Serial.begin(115200);
  delay(1500);  // USB-Serial nach dem Start Zeit geben

  // Nach dem Wecken: Pin vom LP-Bereich zurueck an den normalen GPIO geben
  rtc_gpio_deinit(PIN_SW);

  Serial.println("\n=== GrindByWeight Hardwaretest ===");
  Serial.printf("Weckgrund: %s\n", wakeReason());

  pinMode(PIN_SW,  INPUT_PULLUP);
  pinMode(PIN_CLK, INPUT_PULLUP);
  pinMode(PIN_DT,  INPUT_PULLUP);
  encPrev = (digitalRead(PIN_CLK) << 1) | digitalRead(PIN_DT);
  attachInterrupt(digitalPinToInterrupt(PIN_CLK), onEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_DT),  onEncoder, CHANGE);

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);

  displayOk = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  Serial.printf("Display:  %s\n", displayOk ? "OK" : "FEHLER");
  if (displayOk) display.setTextColor(SSD1306_WHITE);

  scaleOk = scale.begin(Wire);  // weckt den NAU7802 auch nach powerDown() wieder auf
  Serial.printf("NAU7802:  %s\n", scaleOk ? "OK" : "FEHLER");
}

void loop() {
  // --- Taster: Klicks zaehlen, langes Druecken -> Sleep ---
  static bool     armed      = false;  // nach dem Wecken erst loslassen abwarten
  static bool     lastSw     = HIGH;
  static uint32_t pressStart = 0;

  bool sw = digitalRead(PIN_SW);

  if (!armed) {
    if (sw == HIGH) armed = true;
  } else {
    if (lastSw == HIGH && sw == LOW) pressStart = millis();
    if (lastSw == LOW && sw == HIGH && millis() - pressStart > 30) {
      clicks++;
      Serial.printf("Klick #%lu\n", clicks);
    }
    if (sw == LOW && millis() - pressStart > LONG_PRESS_MS) goToSleep();
  }
  lastSw = sw;

  // --- Waage: Rohwert lesen ---
  static int32_t raw = 0;
  if (scaleOk && scale.available()) raw = scale.getReading();

  // --- Anzeige ---
  static uint32_t lastDraw = 0;
  if (millis() - lastDraw > 150) {
    lastDraw = millis();
    int32_t pos = encPos;

    if (displayOk) {
      display.clearDisplay();
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.printf("NAU: %s", scaleOk ? "" : "FEHLER");
      if (scaleOk) display.print(raw);
      display.setCursor(0, 12);
      display.printf("Encoder: %ld", pos);
      display.setCursor(0, 24);
      display.printf("Klicks: %lu  %s", clicks, sw == LOW ? "[X]" : "[ ]");
      display.setCursor(0, 36);
      display.printf("Weckgrund: %s", wakeReason());
      display.setCursor(0, 54);
      display.print("2s halten = Sleep");
      display.display();
    }

    static uint32_t lastLog = 0;
    if (millis() - lastLog > 500) {
      lastLog = millis();
      Serial.printf("NAU: %ld  Encoder: %ld  Klicks: %lu\n", raw, pos, clicks);
    }
  }
}