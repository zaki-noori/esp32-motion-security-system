// ESP32 Motion Security System - Zaki Noori
// PIR alerts in any light; LDR switches the external LED on only in the dark (power saving)
// Wiring:
//   PIR:  VCC -> 5V rail, GND -> GND, OUT -> GPIO25
//   LDR:  3.3V rail -> LDR -> GPIO34 -> 10k resistor -> GND   (dark = LOW reading)
//   LED:  GPIO13 -> 220 ohm resistor -> LED long leg, LED short leg -> GND
//
// Telegram commands:
//   /status          live LDR level, PIR state, threshold
//   /threshold 400   set the dark threshold (saved, survives power off)
//   /help            list commands

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <Preferences.h>

// Wi-Fi and Telegram credentials live in secrets.h (not uploaded to GitHub).
// Copy secrets.example.h to secrets.h and fill in your own details.
#include "secrets.h"

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);
Preferences prefs;

// Hardware Pin Definitions
const int pirPin = 25;       // PIR motion sensor output
const int ldrPin = 34;       // LDR voltage divider analog input (ADC1 - works with Wi-Fi)
const int extLedPin = 13;    // External LED on breadboard
const int onboardLedPin = 2; // ESP32 built-in blue LED

// Calibration
int lightThreshold = 1000;            // Readings BELOW this count as dark (0-4095). Loaded from memory at boot.
const unsigned long cooldown = 10000; // 10s between alerts, LEDs stay on for this long
const unsigned long checkInterval = 2000; // How often to check Telegram for commands

unsigned long lastAlert = 0;
unsigned long lastCommandCheck = 0;
bool alertActive = false;

String statusReport() {
  int lightLevel = analogRead(ldrPin);
  int motionDetected = digitalRead(pirPin);

  String msg = "SYSTEM STATUS\n";
  msg += "LDR Level: " + String(lightLevel) + " (" + (lightLevel < lightThreshold ? "dark" : "bright") + ")\n";
  msg += "Dark threshold: below " + String(lightThreshold) + "\n";
  msg += "PIR State: " + String(motionDetected == HIGH ? "MOTION" : "clear") + "\n";
  msg += "Alert cooldown: " + String(alertActive ? "active" : "ready") + "\n";
  msg += "Wi-Fi signal: " + String(WiFi.RSSI()) + " dBm\n";
  msg += "Uptime: " + String(millis() / 60000) + " min";
  return msg;
}

void handleCommands() {
  int numNewMessages = bot.getUpdates(bot.last_message_received + 1);

  while (numNewMessages) {
    for (int i = 0; i < numNewMessages; i++) {
      String chatId = bot.messages[i].chat_id;
      String text = bot.messages[i].text;
      text.trim();

      // Only accept commands from the owner's chat
      if (chatId != CHAT_ID) {
        bot.sendMessage(chatId, "Unauthorised.", "");
        continue;
      }

      Serial.println("Command received: " + text);

      if (text.startsWith("/status")) {
        bot.sendMessage(chatId, statusReport(), "");

      } else if (text.startsWith("/threshold")) {
        String value = text.substring(10);
        value.trim();

        if (value.length() == 0) {
          bot.sendMessage(chatId, "Current threshold: " + String(lightThreshold) + "\nTo change it: /threshold 400", "");
        } else {
          int newThreshold = value.toInt();
          if (newThreshold < 1 || newThreshold > 4095) {
            bot.sendMessage(chatId, "Threshold must be between 1 and 4095.", "");
          } else {
            lightThreshold = newThreshold;
            prefs.putInt("threshold", lightThreshold);
            bot.sendMessage(chatId, "Threshold set to " + String(lightThreshold) + " (saved).\nCurrent LDR Level: " + String(analogRead(ldrPin)), "");
          }
        }

      } else if (text.startsWith("/help") || text.startsWith("/start")) {
        bot.sendMessage(chatId,
          "Commands:\n"
          "/status - live sensor readings\n"
          "/threshold - show dark threshold\n"
          "/threshold 400 - set dark threshold\n"
          "/help - this list", "");

      } else {
        bot.sendMessage(chatId, "Unknown command. Send /help", "");
      }
    }
    numNewMessages = bot.getUpdates(bot.last_message_received + 1);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  // Pin Configuration
  pinMode(pirPin, INPUT);
  pinMode(extLedPin, OUTPUT);
  pinMode(onboardLedPin, OUTPUT);

  // Ensure LEDs start OFF
  digitalWrite(extLedPin, LOW);
  digitalWrite(onboardLedPin, LOW);

  // Load saved threshold (defaults to 1000 the first time)
  prefs.begin("security", false);
  lightThreshold = prefs.getInt("threshold", 1000);

  // Connect to Wi-Fi Network
  Serial.print("Connecting to Wi-Fi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("\nWi-Fi connected! IP address: " + WiFi.localIP().toString());

  // Telegram connection security bypass for ESP32
  client.setInsecure();

  // PIR stabilisation period - output is unreliable before this
  Serial.println("Warming up PIR (60s) - keep clear of the sensor...");
  delay(60000);
  Serial.println("Ready.");

  // Boot notification
  String bootMsg = "Security System Online: Monitoring active.\n";
  bootMsg += "LDR Level: " + String(analogRead(ldrPin)) + "\n";
  bootMsg += "Dark threshold: below " + String(lightThreshold) + "\n";
  bootMsg += "Send /status or /help";
  bool sent = bot.sendMessage(CHAT_ID, bootMsg, "");
  Serial.println(sent ? "Telegram: boot message sent" : "Telegram: boot message FAILED");
}

void loop() {
  int lightLevel = analogRead(ldrPin);
  int motionDetected = digitalRead(pirPin);
  bool isDark = lightLevel < lightThreshold;
  unsigned long now = millis();

  // Diagnostic printout to Serial Monitor
  Serial.print("LDR Level: ");
  Serial.print(lightLevel);
  Serial.print(isDark ? " (dark)" : " (bright)");
  Serial.print(" | PIR State: ");
  Serial.println(motionDetected);

  // Motion in ANY light: onboard LED + Telegram alert.
  // External LED only when dark (power saving).
  if (motionDetected == HIGH && !alertActive) {
    digitalWrite(onboardLedPin, HIGH);

    String message;
    if (isDark) {
      Serial.println(">>> MOTION DETECTED IN THE DARK - external LED ON <<<");
      digitalWrite(extLedPin, HIGH);
      message = "SECURITY ALERT!\nMotion detected in the dark.\nExternal light activated.\nLDR Level: " + String(lightLevel);
    } else {
      Serial.println(">>> MOTION DETECTED IN LIGHT - external LED stays off <<<");
      message = "SECURITY ALERT!\nMotion detected (area is lit).\nLDR Level: " + String(lightLevel);
    }
    bool sent = bot.sendMessage(CHAT_ID, message, "");
    Serial.println(sent ? "Telegram: alert sent" : "Telegram: alert FAILED");

    lastAlert = millis();
    alertActive = true;
  }

  // Turn LEDs off once the cooldown has passed
  if (alertActive && now - lastAlert >= cooldown) {
    digitalWrite(extLedPin, LOW);
    digitalWrite(onboardLedPin, LOW);
    alertActive = false;
  }

  // Check Telegram for commands every 2 seconds
  if (now - lastCommandCheck >= checkInterval) {
    handleCommands();
    lastCommandCheck = millis();
  }

  delay(500); // Polling interval
}
