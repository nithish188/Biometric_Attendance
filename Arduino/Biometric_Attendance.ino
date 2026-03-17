#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_Fingerprint.h>
#include <RTClib.h>
#include <EEPROM.h>

/* ---------- HARDWARE ---------- */
#define fingerSerial Serial3
#define sim800 Serial2

LiquidCrystal_I2C lcd(0x27,16,2);
Adafruit_Fingerprint finger(&fingerSerial);
RTC_DS3231 rtc;

/* ---------- LIMITS ---------- */
#define EEPROM_SMS_DAY 4095
//#define MAX_STUDENTS 52
//int maxStudents = 52;

/* ---------- ATTENDANCE WINDOW ---------- */
#define START_HOUR 9
#define END_HOUR 10

/* ---------- STUDENT DATA ---------- */
#define NAME_SIZE   20
#define ROLL_SIZE   15
#define PHONE_SIZE  13
#define YEAR_SIZE   2

#define STUDENT_BLOCK (NAME_SIZE + ROLL_SIZE + PHONE_SIZE + YEAR_SIZE)

#define MAX_STUDENTS 75


String studentName[MAX_STUDENTS+1];
String rollNumber[MAX_STUDENTS+1];
String yearOfStudy[MAX_STUDENTS+1];
String parentPhone[MAX_STUDENTS+1];

unsigned long lastSendTime[MAX_STUDENTS+1] = {0}; // stores last send time for each student
const unsigned long sendInterval = 15000; // 15 seconds

int lastAttendanceDay[MAX_STUDENTS + 1] = {0};
bool presentToday[MAX_STUDENTS+1] = {false};
bool smsSentToday = false;
bool timeSynced = false;

/* ---------- FLAGS ---------- */
bool enrollMode = false;

/* ---------- LCD ---------- */
void showDefaultLCD() {
  DateTime now = rtc.now();

  int total = 0;
  for(int i=1;i<=MAX_STUDENTS;i++)
    if(studentName[i].length()) total++;

  char line2[17];
  snprintf(line2,17,"%02d:%02d:%02d %d/%d",
           now.hour(), now.minute(), now.second(),
           total, MAX_STUDENTS);

  lcd.setCursor(0,0);
  lcd.print("Biometric       ");
  lcd.setCursor(0,1);
  lcd.print(line2);
}

void showLCD(const char* l1,const char* l2=""){
  lcd.clear();
  lcd.setCursor(0,0); lcd.print(l1);
  lcd.setCursor(0,1); lcd.print(l2);
}

/* ---------- EEPROM ---------- */
void saveStudentEEPROM(int id) {

  int base = (id - 1) * STUDENT_BLOCK;

  for(int i=0;i<NAME_SIZE;i++)
    EEPROM.write(base+i,
      i < studentName[id].length() ? studentName[id][i] : 0);

  for(int i=0;i<ROLL_SIZE;i++)
    EEPROM.write(base+NAME_SIZE+i,
      i < rollNumber[id].length() ? rollNumber[id][i] : 0);

  for(int i=0;i<PHONE_SIZE;i++)
    EEPROM.write(base+NAME_SIZE+ROLL_SIZE+i,
      i < parentPhone[id].length() ? parentPhone[id][i] : 0);

  for(int i=0;i<YEAR_SIZE;i++)
    EEPROM.write(base+NAME_SIZE+ROLL_SIZE+PHONE_SIZE+i,
      i < yearOfStudy[id].length() ? yearOfStudy[id][i] : 0);
}


String sanitizeEEPROM(String s) {
  String out = "";
  for (int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c >= 32 && c <= 126) {   // printable ASCII only
      out += c;
    }
  }
  out.trim();
  return out;
}

void readStudentEEPROM(int id) {

  int base = (id - 1) * STUDENT_BLOCK;

  char buf[21];

  for(int i=0;i<NAME_SIZE;i++)
    buf[i] = EEPROM.read(base+i);
  buf[NAME_SIZE] = 0;
  studentName[id] = sanitizeEEPROM(String(buf));

  for(int i=0;i<ROLL_SIZE;i++)
    buf[i] = EEPROM.read(base+NAME_SIZE+i);
  buf[ROLL_SIZE] = 0;
  rollNumber[id] = sanitizeEEPROM(String(buf));

  for(int i=0;i<PHONE_SIZE;i++)
    buf[i] = EEPROM.read(base+NAME_SIZE+ROLL_SIZE+i);
  buf[PHONE_SIZE] = 0;
  parentPhone[id] = sanitizeEEPROM(String(buf));

  for(int i=0;i<YEAR_SIZE;i++)
    buf[i] = EEPROM.read(base+NAME_SIZE+ROLL_SIZE+PHONE_SIZE+i);
  buf[YEAR_SIZE] = 0;
  yearOfStudy[id] = sanitizeEEPROM(String(buf));
}


void clearEEPROM(){
  Serial.println("EEPROM CLEAR START");
  showLCD("Clearing Data");
  for(int i=0;i<EEPROM.length();i++) EEPROM.write(i,0);
  for(int i=1;i<=MAX_STUDENTS;i++)
    studentName[i]=rollNumber[i]=yearOfStudy[i]=parentPhone[i]="";
  delay(2000);
  Serial.println("EEPROM CLEARED");
}

void flushSerial() {
  delay(50);
  while (Serial.available()) Serial.read();
}

String sanitizeName(String s) {
  String out = "";
  for (int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c >= 32 && c <= 126) {   // printable ASCII only
      out += c;
    }
  }
  out.trim();
  return out;
}

/* ---------- SETUP ---------- */
void setup(){

  Serial.begin(9600);
  Serial1.begin(9600);      // ESP8266
  sim800.begin(9600);       // GSM
  fingerSerial.begin(57600);

  lcd.init();
  lcd.backlight();

  Serial.println("System Booting...");

  if (!rtc.begin())
  {
    Serial.println("RTC ERROR");
    showLCD("RTC ERROR");
    while (1);
  }

  if (rtc.lostPower())
  {
    Serial.println("RTC lost power (expected if no battery)");
  }

  delay(5000);   // allow ESP to connect WiFi

  Serial.println("Requesting time from ESP...");
  Serial1.println("GETTIME");

  /* ---------- WAIT FOR TIME FROM ESP (MAX 10s) ---------- */

  unsigned long startWait = millis();
  unsigned long timeout = 10000;

  while (millis() - startWait < timeout)
  {
    if (Serial1.available())
    {
      String incoming = Serial1.readStringUntil('\n');
      incoming.trim();

      Serial.print("Received: ");
      Serial.println(incoming);

      if (incoming.startsWith("TIME|"))
      {
        int yr,mo,dy,hr,mn,sc;

        sscanf(incoming.c_str(),
        "TIME|%d-%d-%d %d:%d:%d",
        &yr,&mo,&dy,&hr,&mn,&sc);

        rtc.adjust(DateTime(yr,mo,dy,hr,mn,sc));

        Serial.println("RTC synced from ESP");

        break;
      }
    }
  }

  Serial.print("RTC Time: ");
  DateTime now = rtc.now();
  Serial.print(now.year()); Serial.print("-");
  Serial.print(now.month()); Serial.print("-");
  Serial.print(now.day()); Serial.print(" ");
  Serial.print(now.hour()); Serial.print(":");
  Serial.print(now.minute()); Serial.print(":");
  Serial.println(now.second());

  for(int i = 1; i <= MAX_STUDENTS; i++) {
    readStudentEEPROM(i);
  }

  Serial.println("=== BIOMETRIC SYSTEM READY ===");
}

/* ---------- LOOP ---------- */
void loop(){

  if(!enrollMode){
    showDefaultLCD();
    checkAttendance();
    checkAndSendAbsentees();
  }

  if(Serial.available()){
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    if(cmd=="enroll"){ enrollMode=true; enrollStudent(); enrollMode=false; }
    else if(cmd=="update"){ enrollMode=true; updateStudent(); enrollMode=false; }
    else if(cmd=="clear") clearEEPROM();
    else if(cmd=="time") setTime();

  }

    if (Serial1.available()) {

    String incoming = Serial1.readStringUntil('\n');
    incoming.trim();

    Serial.print("Received: ");
    Serial.println(incoming);

    if (incoming.startsWith("TIME|")) {

      int yr, mo, dy, hr, mn, sc;

      sscanf(incoming.c_str(),
            "TIME|%d-%d-%d %d:%d:%d",
            &yr, &mo, &dy, &hr, &mn, &sc);

      rtc.adjust(DateTime(yr, mo, dy, hr, mn, sc));

      Serial.println("RTC updated from ESP");
    }
  }
}

/* ---------- ATTENDANCE ---------- */
void checkAttendance(){
  DateTime now = rtc.now();

  /* ---------- TIME WINDOW CHECK ---------- */
  if(now.hour() < START_HOUR || now.hour() >= END_HOUR){
    if(finger.getImage() == FINGERPRINT_OK){
      showLCD("Out of Time");
      Serial.println("SCAN IGNORED -> Outside attendance window");
      delay(2000);
    }
    return;
  }

  /* ---------- FINGER CAPTURE ---------- */
  if(finger.getImage() != FINGERPRINT_OK) return;
  if(finger.image2Tz() != FINGERPRINT_OK) return;

  /* ---------- SEARCH ---------- */
  if(finger.fingerFastSearch() != FINGERPRINT_OK){
    showLCD("Unknown Finger");
    Serial.println("UNKNOWN FINGER");
    delay(2000);
    return;
  }

  int id   = finger.fingerID;
  int conf = finger.confidence;

  if(id < 1 || id > MAX_STUDENTS || studentName[id].length() == 0){
    Serial.println("INVALID STUDENT ID");
    return;
  }

  /* ---------- DUPLICATE CHECK ---------- */
  if(lastAttendanceDay[id] == now.day()){
    showLCD("Already Marked");
    Serial.print("DUPLICATE SCAN -> ");
    Serial.println(studentName[id]);
    delay(2000);
    return;
  }

  /* ---------- RECORD ATTENDANCE ---------- */
  lastAttendanceDay[id] = now.day();
  presentToday[id] = true;

  showLCD(studentName[id].c_str(), rollNumber[id].c_str());

  Serial.print("ATTENDANCE RECORDED -> ");
  Serial.print(studentName[id]);
  Serial.print(" | ID ");
  Serial.print(id);
  Serial.print(" | CONFIDENCE ");
  Serial.println(conf);

    // Send attendance every 15 seconds until uploaded
  if(millis() - lastSendTime[id] >= sendInterval){
    lastSendTime[id] = millis();
    // --- SEND TO ESP VIA SERIAL ---
    String dataStr = String(now.year())+"-"+(now.month()<10?"0":"")+String(now.month())
      +"-"+(now.day()<10?"0":"")+String(now.day())
      +" "+(now.hour()<10?"0":"")+String(now.hour())
      +":"+(now.minute()<10?"0":"")+String(now.minute())
      +":"+(now.second()<10?"0":"")+String(now.second())
      +"  "+ rollNumber[id] +"  "+ studentName[id];
    Serial1.println("ATT|" + dataStr); // one-way to ESP
    Serial.print("Sending attendance -> "); Serial.println(dataStr);
  }

  delay(3000);
}

/* ---------- ENROLL ---------- */
void enrollStudent(){
  Serial.println("--- ENROLL START ---");
  showLCD("Enter Student ID");
  Serial.println("Enter Student ID: ");
  int id = readNumber();
  if (id < 1 || id > MAX_STUDENTS) {
    Serial.println("Invalid ID");
    showLCD("Invalid ID");
    delay(2000);
    return;
  }


  //Serial.print("Student ID: "); Serial.println(id);

  showLCD("Enter Name"); Serial.println("Student Name: "); studentName[id] = sanitizeName(readText());
  showLCD("Enter Roll"); Serial.println("Student roll number: ");rollNumber[id]=readText();
  showLCD("Enter Year"); Serial.println("Student year: ");yearOfStudy[id]=readText();
  showLCD("Parent Phone"); Serial.println("Enter Parent Phone:");

  while(true){
    String raw = readText();
    String formatted = formatPhoneNumber(raw);

    if(formatted != ""){
      parentPhone[id] = formatted;
      Serial.print("Saved as: ");
      Serial.println(parentPhone[id]);
      break;
    }
    else{
      Serial.println("Invalid number! Try again:");
      showLCD("Invalid Phone");
    }
  }

  if(enrollFingerprint(id)){
    saveStudentEEPROM(id);
    showLCD("Completed");
    Serial.println("ENROLL SUCCESS");
  }else{
    Serial.println("ENROLL FAILED");
  }
  delay(2000);
}

bool enrollFingerprint(int id){
  Serial.println("Place finger...");
  showLCD("Place Finger");
  while(finger.getImage()!=FINGERPRINT_OK);
  finger.image2Tz(1);
  Serial.println("First image OK");

  showLCD("Remove Finger");
  delay(2000);
  while(finger.getImage()!=FINGERPRINT_NOFINGER);

  Serial.println("Place again...");
  showLCD("Place Again");
  while(finger.getImage()!=FINGERPRINT_OK);
  finger.image2Tz(2);
  Serial.println("Second image OK");

  if(finger.createModel()!=FINGERPRINT_OK) return false;
  if(finger.storeModel(id)!=FINGERPRINT_OK) return false;

  Serial.print("Fingerprint stored at ID ");
  Serial.println(id);
  return true;
}

/* ---------- UPDATE ---------- */
void updateStudent() {
  Serial.println("--- UPDATE START ---");
  showLCD("Enter Student ID");

  int id = readNumber();

  if(id < 1 || id > MAX_STUDENTS || studentName[id].length() == 0)
  {
    Serial.println("Invalid ID");
    return;
  }

  // NAME
  showLCD("Update Name?");
  Serial.println("Enter new name or press ENTER to skip:");
  String t = readText();
  if(t.length()) studentName[id] = sanitizeName(t);

  // ROLL
  showLCD("Update Roll?");
  Serial.println("Enter new roll or press ENTER to skip:");
  t = readText();
  if(t.length()) rollNumber[id] = t;

  // PHONE
  showLCD("Update Phone?");
  Serial.println("Enter new phone or press ENTER to skip:");

  while(true)
  {
    String raw = readText();

    if(raw.length() == 0) break;   // skip

    String formatted = formatPhoneNumber(raw);

    if(formatted != "")
    {
      parentPhone[id] = formatted;
      Serial.print("Saved as: ");
      Serial.println(parentPhone[id]);
      break;
    }
    else
    {
      Serial.println("Invalid number! Try again:");
      showLCD("Invalid Phone");
    }
  }

  // FINGERPRINT (optional)
  Serial.println("Update fingerprint? (y/n)");
  while(Serial.available()==0);
  char ch = Serial.read();

  if(ch == 'y' || ch == 'Y')
    enrollFingerprint(id);

  saveStudentEEPROM(id);

  Serial.println("UPDATE SUCCESS");
  showLCD("Updated");
  delay(2000);
}

/* ---------- TIME SET ---------- */
void setTime(){
  int h, m, s;

  showLCD("Set Hour");
  Serial.println("Enter Hour (0-23):");
  h = readNumber();
  delay(500);
  showLCD("Set Minute");
  Serial.println("Enter Minute (0-59):");
  m = readNumber();
  delay(500);
  showLCD("Set Second");
  Serial.println("Enter Second (0-59):");
  s = readNumber();
  delay(500);
  DateTime now = rtc.now();
  rtc.adjust(DateTime(now.year(), now.month(), now.day(), h, m, s));

  showLCD("Time Updated");
  Serial.print("TIME SET TO -> ");
  Serial.print(h); Serial.print(":");
  Serial.print(m); Serial.print(":");
  Serial.println(s);

  delay(2000);
}

/* ---------- ABSENT SMS ---------- */
void checkAndSendAbsentees(){
  DateTime now = rtc.now();

  int lastDay = EEPROM.read(EEPROM_SMS_DAY);

  // Send only between 10:00–10:59 AM and only once per day
  if (now.hour() >= END_HOUR && now.hour() < (END_HOUR + 1) &&
      now.day() != lastDay) {

    Serial.println("Sending absentee SMS...");

    for(int i=1;i<=MAX_STUDENTS;i++){
      if(studentName[i].length()>0 && !presentToday[i]){
        sendAbsentSMS(i);
        delay(5000);
      }
    }

    EEPROM.write(EEPROM_SMS_DAY, now.day());
    Serial.println("All SMS sent");
  }

  // Reset attendance flags at midnight
  if(now.hour()==0 && now.minute()==0){
    for(int i=1;i<=MAX_STUDENTS;i++)
      presentToday[i]=false;
  }
}

String sanitizeSMS(String s) {
  String out = "";
  for (int i = 0; i < s.length(); i++) {
    char c = s[i];
    if ((c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
        c == ' ' || c == '/' || c == '-' || c == '(' || c == ')') {
      out += c;
    }
  }
  out.trim();
  return out;
}

/* ---------- SMS ---------- */
void sendAbsentSMS(int id) {

  char phone[16];
  char msg[120];

  String num = parentPhone[id];
  num.trim();
  if (num.length() == 11) num = "+91" + num;
  if (num.length() < 13) {
    Serial.println("Invalid phone number");
    return;
  }
  num.toCharArray(phone, sizeof(phone));

  DateTime now = rtc.now();

  snprintf(msg, sizeof(msg),
           "Dear parent, your son/daughter %s (%s) was absent on %02d/%02d/%04d - HOD/ECE MPNMJEC.",
           studentName[id].c_str(),
           rollNumber[id].c_str(),
           now.day(), now.month(), now.year());

  const int MAX_RETRY = 3;

  for (int attempt = 1; attempt <= MAX_RETRY; attempt++) {

    Serial.print("SMS Attempt ");
    Serial.println(attempt);

    sim800.println("AT+CMGF=1");
    delay(1500);

    sim800.print("AT+CMGS=\"");
    sim800.print(phone);
    sim800.println("\"");
    delay(2000);

    // Send body slowly
    for (int i = 0; msg[i] != '\0'; i++) {
      sim800.write(msg[i]);
      delay(20);
    }

    delay(1000);
    sim800.write(26); // CTRL+Z

    // ---- WAIT FOR RESULT ----
    unsigned long t = millis();
    bool success = false;
    bool failure = false;

    while (millis() - t < 20000) {

      // refresh time display
      showDefaultLCD();

      // allow serial commands like "time", "enroll"
      if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        if(cmd=="time") setTime();
      }

      if (sim800.available()) {
        String resp = sim800.readString();
        Serial.println(resp);

        if (resp.indexOf("+CMGS:") != -1) {
          success = true;
          break;
        }
        if (resp.indexOf("ERROR") != -1) {
          break;
        }
      }
    }

    if (success) {
      Serial.println("SMS SENT SUCCESSFULLY");
      return;   // 🔴 STOP ALL RETRIES
    }

    Serial.println("SMS FAILED — retrying...");
    delay(8000);   // GSM cooldown before retry
  }

  Serial.println("SMS FAILED AFTER MAX RETRIES");
}

String formatPhoneNumber(String num) {
  //num.trim();

  String digits = "";

  for (int i = 0; i < num.length(); i++)
  {
    //if (isDigit(num.charAt(i)))
      digits += num.charAt(i);
  }

  Serial.print("RAW: ["); Serial.print(num); Serial.println("]");
  Serial.print("DIGITS: "); Serial.println(digits);
  Serial.print("LEN: "); Serial.println(digits.length());

  // 10 digit → Indian
  if (digits.length() == 10)
    return "+91" + digits;

  // 91XXXXXXXXXX
  if (digits.length() == 12 && digits.startsWith("91"))
    return "+" + digits;

  // already with +91
  if (digits.length() == 13 && digits.startsWith("91"))
    return "+" + digits.substring(1);

  return "";
}

/* ---------- AT CMD ---------- */
void sendSMSCommand(String cmd){
  sim800.println(cmd);
  delay(1000);
  while(sim800.available())
    Serial.write(sim800.read());
}

/* ---------- SERIAL HELPERS ---------- */
String readText() {
  String s = "";

  while (true)
  {
    if (Serial.available())
    {
      s = Serial.readStringUntil('\n');
      s.trim();

      if (s.length() > 0)
        return s;
    }
  }
}


int readNumber() {
  String s = readText();
  return s.toInt();

}