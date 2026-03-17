#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <SoftwareSerial.h>
#include <time.h>

/* ================== SERIAL ================== */
#define RX_PIN D5
#define TX_PIN D6

SoftwareSerial megaSerial(RX_PIN, TX_PIN);

/* ================== WIFI ================== */
const char* ssid   = "6ixmindslabs";
const char* pass   = "12345678";
const char* apiKey = "JXHY3LPHVIUF6UAI";

WiFiClient client;

/* ================== QUEUE ================== */
#define MAX_QUEUE 20
String queue[MAX_QUEUE];
int qStart = 0;
int qEnd   = 0;

/* ================== TIMING ================== */
const unsigned long WAIT_BEFORE_UPLOAD = 15000;
const unsigned long TS_DELAY           = 16000;

bool uploadWindowActive = false;
unsigned long uploadStartTime = 0;
unsigned long lastTSsend = 0;

/* ================== CLEAN DATA ================== */
String cleanData(String s) {

  String out = "";

  for (int i = 0; i < s.length(); i++) {
    char c = s[i];

    if (c >= 32 && c <= 126)
      out += c;
    else
      out += ' ';
  }

  out.trim();
  return out;
}

/* ================== WIFI CONNECT ================== */
void connectWiFi() {

  WiFi.begin(ssid, pass);

  Serial.print("Connecting WiFi");

  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi FAILED");
  }
}

/* ================== WAIT FOR NTP ================== */
void waitForNTP() {

  Serial.println("Waiting for NTP time...");

  time_t now = time(nullptr);

  while (now < 100000) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }

  Serial.println("\nNTP time acquired");
}

/* ================== SEND TIME ================== */
void sendTimeToArduino() {

  time_t now = time(nullptr);

  if (now < 100000) {
    Serial.println("NTP not ready");
    return;
  }

  struct tm *t = localtime(&now);

  char buffer[40];

  sprintf(buffer,
  "TIME|%04d-%02d-%02d %02d:%02d:%02d",
  t->tm_year + 1900,
  t->tm_mon + 1,
  t->tm_mday,
  t->tm_hour,
  t->tm_min,
  t->tm_sec);

  megaSerial.println(buffer);

  Serial.print("Sent Time: ");
  Serial.println(buffer);
  delay(100);
}

/* ================== SETUP ================== */
void setup() {

  Serial.begin(9600);
  megaSerial.begin(9600);

  Serial.println("\nESP8266 Attendance Uploader STARTED");

  connectWiFi();

  configTime(19800, 0, "pool.ntp.org", "time.nist.gov");

  waitForNTP();
}

/* ================== LOOP ================== */
void loop() {

  /* ---------- READ FROM MEGA ---------- */
  if (megaSerial.available()) {

    String line = megaSerial.readStringUntil('\n');
    line.trim();

    if (line == "GETTIME") {
      sendTimeToArduino();
      return;
    }

    if (line.startsWith("ATT|")) {

      line.remove(0,4);
      line = cleanData(line);

      enqueue(line);

      Serial.println("Queued ATT: " + line);

      if (!uploadWindowActive) {
        uploadWindowActive = true;
        uploadStartTime = millis();
        Serial.println("Upload window started (15s)");
      }

    } else {
      Serial.println("IGNORED: " + line);
    }
  }

  /* ---------- UPLOAD TO THINGSPEAK ---------- */
  if (uploadWindowActive && millis() - uploadStartTime >= WAIT_BEFORE_UPLOAD) {

    if (WiFi.status() != WL_CONNECTED) {
      connectWiFi();
      return;
    }

    if (!queueEmpty() && millis() - lastTSsend >= TS_DELAY) {

      String data = dequeue();
      data = cleanData(data);

      Serial.println("Uploading: " + data);

      if (sendToThingSpeak(data)) {
        Serial.println("Upload SUCCESS");
      }
      else {
        Serial.println("Upload FAILED → re-queued");
        enqueue(data);
      }

      lastTSsend = millis();
    }

    if (queueEmpty()) {
      uploadWindowActive = false;
      Serial.println("All attendance uploaded ✔");
    }
  }
}

/* ================== QUEUE ================== */
void enqueue(String s) {

  queue[qEnd] = s;
  qEnd = (qEnd + 1) % MAX_QUEUE;

  if (qEnd == qStart) {
    qStart = (qStart + 1) % MAX_QUEUE;
    Serial.println("Queue overflow → oldest dropped");
  }
}

String dequeue() {

  if (queueEmpty()) return "";

  String s = queue[qStart];
  qStart = (qStart + 1) % MAX_QUEUE;

  return s;
}

bool queueEmpty() {
  return qStart == qEnd;
}

/* ================== THINGSPEAK ================== */
bool sendToThingSpeak(String data) {
  String encoded = urlEncode(data);
  String url = "http://api.thingspeak.com/update?api_key=";
  url += apiKey;
  url += "&field1=" + encoded;

  Serial.println("URL:");
  Serial.println(url);

  HTTPClient http;

  http.begin(client, url);

  int code = http.GET();

  http.end();

  Serial.print("HTTP CODE: ");
  Serial.println(code);

  return (code == 200);
}

/* ================== URL ENCODE ================== */
String urlEncode(String s) {

  String out = "";

  for (char c : s) {

    if (c == ' ')      out += "%20";
    else if (c == '|') out += "%7C";
    else if (c == ':') out += "%3A";
    else               out += c;
  }

  return out;
}