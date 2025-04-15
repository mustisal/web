#include <SPI.h>
#include <EthernetENC.h>
#include <EEPROM.h>
#include <ArduinoJson.h>

// EEPROM yapılandırma
#define EEPROM_SIZE 1024
#define USER_COUNT 5
#define MAX_USERNAME_LENGTH 10
#define MAX_PASSWORD_LENGTH 10
#define CONFIG_START_ADDRESS 0

// EEPROM'da saklanacak yapı
struct StoredConfig {
  char username[USER_COUNT][MAX_USERNAME_LENGTH + 1];
  char password[USER_COUNT][MAX_PASSWORD_LENGTH + 1];
  uint8_t userRole[USER_COUNT]; // 0: boş, 1: kullanıcı, 2: admin
  bool staticIP;
  byte ip[4];
  byte gateway[4];
  byte subnet[4];
  byte dns[4];
  uint16_t configVersion;
};

StoredConfig config;

// Fonksiyon prototipleri
bool loadConfig();
void setDefaultConfig();
void saveConfig();
void sendLoginPage(EthernetClient &client);
void handleLogin(EthernetClient &client, String postData);
void sendDashboard(EthernetClient &client);
void sendAdminPage(EthernetClient &client);
void sendUsersList(EthernetClient &client);
void handleUserUpdate(EthernetClient &client, String postData);
void handleNetworkUpdate(EthernetClient &client, String postData);
void sendNetworkConfig(EthernetClient &client);
void parseIPAddress(String input, byte* output);

// Ethernet sunucu yapılandırması
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
EthernetServer server(80);

// Oturum yönetimi
bool isLoggedIn = false;
unsigned long sessionTimeout = 600000; // 10 dakika
unsigned long lastActivity = 0;
uint8_t currentUserRole = 0;
String currentUsername = "";

// STM32 için EEPROM emülasyonu ve yönetimi için yardımcı fonksiyonlar
void eepromBegin() {
  // STM32 için EEPROM emülasyonu başlat
  EEPROM.begin();
}

void eepromCommit() {
  // STM32 EEPROM.commit() fonksiyonu Arduino STM32 core'da bulunmayabilir
  // Bu durumda boş bir fonksiyon bırakıyoruz
}

void setup() {
  // Seri port başlatma
  Serial.begin(9600);
  while (!Serial) {
    ; // Seri bağlantı bekleniyor
  }
  
  // EEPROM başlatma
  eepromBegin();
  
  // Yapılandırmayı yükle veya varsayılanları ayarla
  if (!loadConfig()) {
    Serial.println(F("Default configuration loaded"));
    setDefaultConfig();
    saveConfig();
  }
  
  // Ethernet başlatma
  Serial.println(F("Initializing Ethernet..."));
  if (config.staticIP) {
    Ethernet.begin(mac, config.ip, config.dns, config.gateway, config.subnet);
  } else {
    if (Ethernet.begin(mac) == 0) {
      Serial.println(F("DHCP Failed. Using default IP"));
      Ethernet.begin(mac, IPAddress(192, 168, 1, 177));
    }
  }
  
  // IP adresi gösterme
  Serial.print(F("Server is at "));
  Serial.println(Ethernet.localIP());
  
  // Web sunucusu başlatma
  server.begin();
}

void loop() {
  // Değişken tanımlamaları
  char c;
  int contentLength = 0;
  String postData = "";

  // Oturum kontrolü - 10 dakika sonra oturumu sonlandır
  if (isLoggedIn && (millis() - lastActivity > sessionTimeout)) {
    isLoggedIn = false;
    currentUserRole = 0;
    currentUsername = "";
    Serial.println(F("Session timeout"));
  }
  
  // İstemci bağlantısını kontrol et
  EthernetClient client = server.available();
  if (client) {
    Serial.println(F("New client connected"));
    Serial.print(F("Client IP: "));
    Serial.print(client.remoteIP());
    Serial.print(F(" Port: "));
    Serial.println(client.remotePort());
    boolean currentLineIsBlank = true;
    String httpMethod = "";
    String currentLine = "";
    String url = "";
    
    while (client.connected()) {
      if (!client.connected()) {
        Serial.println(F("Client disconnected unexpectedly"));
        Serial.print(F("Connection status: ")); Serial.println(client.status());
        Serial.print(F("Bytes available: ")); Serial.println(client.available());
        client.stop();
        return;
      }
      
      if (client.available()) {
        char c = client.read();
        // Serial.print(F("DEBUG: Received char: ")); Serial.println(c);
        // Serial.print(F("Client available bytes: ")); Serial.println(client.available());
        
        // HTTP isteğinin ilk satırını oku
        if (httpMethod == "") {
          String requestLine = client.readStringUntil('\r');
          client.read(); // '\n' karakterini atla
          
          int firstSpace = requestLine.indexOf(' ');
          int secondSpace = requestLine.indexOf(' ', firstSpace + 1);
          
          if (firstSpace != -1 && secondSpace != -1) {
            httpMethod = requestLine.substring(0, firstSpace);
            url = requestLine.substring(firstSpace + 1, secondSpace);
          }
        }
        
        // Satırı okuma
        // Tüm HTTP başlıklarını oku
        String headerLine = client.readStringUntil('\r');
        client.read(); // '\n' karakterini atla
        
        if (headerLine.length() == 0) {
          // Boş satır geldiğinde istek gövdesi başlar
          break;
        } else if (httpMethod == "") {
          // İstek satırını işle
          int firstSpace = headerLine.indexOf(' ');
          int secondSpace = headerLine.indexOf(' ', firstSpace + 1);
          
          if (firstSpace != -1 && secondSpace != -1) {
            httpMethod = headerLine.substring(0, firstSpace);
            url = headerLine.substring(firstSpace + 1, secondSpace);
          }
        }
        
        if (httpMethod == "POST") {
          // Content-Length başlığını kontrol et
          if (headerLine.startsWith("Content-Length: ")) {
            contentLength = headerLine.substring(16).toInt();
          }
        }
      }
      
      // POST verisini oku
      if (httpMethod == "POST" && contentLength > 0) {
        postData.reserve(contentLength);
        while (postData.length() < contentLength && client.connected()) {
          if (client.available()) {
            postData += (char)client.read();
          }
        }
      }
          Serial.println(F("Processing HTTP request - Method: "));
          Serial.println(httpMethod);
          Serial.println(F("URL: "));
          Serial.println(url);
          // POST istekleri için veri gövdesini okuma
          if (httpMethod == "POST") {
            // İstek başlıklarını okuma ve Content-Length'i bulma
            while (client.available() && contentLength == 0) {
              String line = client.readStringUntil('\r');
              client.read(); // '\n' karakterini atla
              if (line.startsWith("Content-Length: ")) {
                contentLength = line.substring(16).toInt();
              }
              if (line.length() == 0) break;
            }
            
            // POST verisini okuma
            for (int i = 0; i < contentLength; i++) {
              if (client.available())
                postData += (char)client.read();
            }
          }
          
          // İstek türünü belirleme ve yanıt gönderme
          if (url == "/") {
            sendLoginPage(client);
          } 
          else if (url == "/login" && httpMethod == "POST") {
            handleLogin(client, postData);
          }
          else if (url == "/dashboard" && isLoggedIn) {
            sendDashboard(client);
          }
          else if (url == "/admin" && isLoggedIn && currentUserRole == 2) {
            sendAdminPage(client);
          }
          else if (url == "/api/users" && isLoggedIn && currentUserRole == 2) {
            sendUsersList(client);
          }
          else if (url == "/api/user" && httpMethod == "POST" && isLoggedIn && currentUserRole == 2) {
            handleUserUpdate(client, postData);
          }
          else if (url == "/api/network" && httpMethod == "POST" && isLoggedIn && currentUserRole == 2) {
            handleNetworkUpdate(client, postData);
          }
          else if (url == "/api/network" && isLoggedIn && currentUserRole == 2) {
            sendNetworkConfig(client);
          }
          else if (url == "/logout") {
            isLoggedIn = false;
            currentUserRole = 0;
            currentUsername = "";
            client.println(F("HTTP/1.1 302 Found"));
            client.println(F("Location: /"));
            client.println(F("Connection: close"));
            client.println();
          }
          else {
            // Yetkilendirme kontrolü
            if (!isLoggedIn) {
              client.println(F("HTTP/1.1 302 Found"));
              client.println(F("Location: /"));
              client.println(F("Connection: close"));
              client.println();
            } else {
              // 404 - Sayfa bulunamadı
              client.println(F("HTTP/1.1 404 Not Found"));
              client.println(F("Content-Type: text/html"));
              client.println(F("Connection: close"));
              client.println();
              client.println(F("<!DOCTYPE HTML>"));
              client.println(F("<html><head><title>404 Not Found</title></head>"));
              client.println(F("<body><h1>404 Not Found</h1><p>The requested URL was not found.</p></body></html>"));
            }
          }
          break;
        }
        
        if (c == '\n') {
          currentLineIsBlank = true;
          currentLine = "";
        } else if (c != '\r') {
          currentLineIsBlank = false;
        }
      }
    }
    
    // Bağlantıyı kapat
    client.flush();
    delay(10); // Bağlantının tamamen kapanması için kısa bir bekleme
    client.stop();
    Serial.println(F("Client properly disconnected"));
  }
// Varsayılan yapılandırmayı ayarla
void setDefaultConfig() {
  memset(&config, 0, sizeof(config));
  
  // Admin kullanıcısı
  strcpy(config.username[0], "admin");
  strcpy(config.password[0], "password");
  config.userRole[0] = 2; // Admin
  
  // Varsayılan kullanıcı
  strcpy(config.username[1], "user");
  strcpy(config.password[1], "123456");
  config.userRole[1] = 1; // Kullanıcı
  
  // Ağ ayarları
  config.staticIP = false;
  config.ip[0] = 192; config.ip[1] = 168; config.ip[2] = 1; config.ip[3] = 177;
  config.gateway[0] = 192; config.gateway[1] = 168; config.gateway[2] = 1; config.gateway[3] = 1;
  config.subnet[0] = 255; config.subnet[1] = 255; config.subnet[2] = 255; config.subnet[3] = 0;
  config.dns[0] = 8; config.dns[1] = 8; config.dns[2] = 8; config.dns[3] = 8;
  
  config.configVersion = 1;
}

// EEPROM'dan yapılandırmayı yükle
bool loadConfig() {
  // Yapılandırmayı EEPROM'dan oku
  for (unsigned int i = 0; i < sizeof(config); i++) {
    *((uint8_t*)&config + i) = EEPROM.read(CONFIG_START_ADDRESS + i);
  }
  
  // Yapılandırma sürümünü kontrol et
  return config.configVersion == 1;
}

// EEPROM'a yapılandırmayı kaydet
void saveConfig() {
  // Yapılandırmayı EEPROM'a yaz
  for (unsigned int i = 0; i < sizeof(config); i++) {
    EEPROM.write(CONFIG_START_ADDRESS + i, *((uint8_t*)&config + i));
  }
  eepromCommit();
}

// Giriş sayfasını gönder
void sendLoginPage(EthernetClient &client) {
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: text/html"));
  client.println(F("Connection: close"));
  client.println();
  
  client.println(F("<!DOCTYPE HTML>"));
  client.println(F("<html>"));
  client.println(F("<head>"));
  client.println(F("<title>Login</title>"));
  client.println(F("<meta name='viewport' content='width=device-width, initial-scale=1'>"));
  client.println(F("<style>"));
  client.println(F("body { font-family: Arial, sans-serif; margin: 0; padding: 20px; background-color: #f5f5f5; }"));
  client.println(F(".login-container { max-width: 400px; margin: 100px auto; padding: 20px; background-color: white; border-radius: 5px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }"));
  client.println(F("h2 { text-align: center; color: #333; }"));
  client.println(F("input[type=text], input[type=password] { width: 100%; padding: 12px 20px; margin: 8px 0; display: inline-block; border: 1px solid #ccc; box-sizing: border-box; border-radius: 4px; }"));
  client.println(F("button { background-color: #4CAF50; color: white; padding: 14px 20px; margin: 8px 0; border: none; cursor: pointer; width: 100%; border-radius: 4px; }"));
  client.println(F("button:hover { opacity: 0.8; }"));
  client.println(F(".error { color: red; text-align: center; }"));
  client.println(F("@media screen and (max-width: 480px) { .login-container { margin: 20px auto; padding: 10px; } }"));
  client.println(F("</style>"));
  client.println(F("</head>"));
  client.println(F("<body>"));
  client.println(F("<div class='login-container'>"));
  client.println(F("<h2>Login</h2>"));
  client.println(F("<div id='error' class='error'></div>"));
  client.println(F("<form id='loginForm'>"));
  client.println(F("<div>"));
  client.println(F("<label for='username'>Username:</label>"));
  client.println(F("<input type='text' id='username' name='username' required>"));
  client.println(F("</div>"));
  client.println(F("<div>"));
  client.println(F("<label for='password'>Password:</label>"));
  client.println(F("<input type='password' id='password' name='password' required>"));
  client.println(F("</div>"));
  client.println(F("<button type='submit'>Login</button>"));
  client.println(F("</form>"));
  client.println(F("</div>"));
  
  client.println(F("<script>"));
  client.println(F("document.getElementById('loginForm').addEventListener('submit', function(e) {"));
  client.println(F("  e.preventDefault();"));
  client.println(F("  var username = document.getElementById('username').value;"));
  client.println(F("  var password = document.getElementById('password').value;"));
  client.println(F("  var xhr = new XMLHttpRequest();"));
  client.println(F("  xhr.open('POST', '/login', true);"));
  client.println(F("  xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');"));
  client.println(F("  xhr.onreadystatechange = function() {"));
  client.println(F("    if (xhr.readyState === 4) {"));
  client.println(F("      if (xhr.status === 200) {"));
  client.println(F("        window.location.href = '/dashboard';"));
  client.println(F("      } else {"));
  client.println(F("        document.getElementById('error').textContent = 'Invalid username or password';"));
  client.println(F("      }"));
  client.println(F("    }"));
  client.println(F("  };"));
  client.println(F("  xhr.send('username=' + encodeURIComponent(username) + '&password=' + encodeURIComponent(password));"));
  client.println(F("});"));
  client.println(F("</script>"));
  
  client.println(F("</body>"));
  client.println(F("</html>"));
}

// Giriş işlemini yönet
void handleLogin(EthernetClient &client, String postData) {
  String username = "";
  String password = "";
  
  // POST verilerini ayrıştır
  int usernameIndex = postData.indexOf("username=");
  int passwordIndex = postData.indexOf("&password=");
  
  if (usernameIndex != -1 && passwordIndex != -1) {
    username = postData.substring(usernameIndex + 9, passwordIndex);
    password = postData.substring(passwordIndex + 10);
    
    // URL kodlamasını çöz
    username.replace("+", " ");
    password.replace("+", " ");
    
    int userIndex = -1;
    for (int i = 0; i < USER_COUNT; i++) {
      if (strcmp(config.username[i], username.c_str()) == 0 && 
          strcmp(config.password[i], password.c_str()) == 0 &&
          config.userRole[i] > 0) {
        userIndex = i;
        break;
      }
    }
    
    if (userIndex != -1) {
      // Giriş başarılı
      isLoggedIn = true;
      currentUserRole = config.userRole[userIndex];
      currentUsername = username;
      lastActivity = millis();
      
      client.println(F("HTTP/1.1 200 OK"));
      client.println(F("Content-Type: text/html"));
      client.println(F("Connection: close"));
      client.println();
      client.println(F("<html><body><h1>Login successful</h1><script>window.location.href='/dashboard';</script></body></html>"));
      return;
    }
  }
  
  // Giriş başarısız
  client.println(F("HTTP/1.1 401 Unauthorized"));
  client.println(F("Content-Type: text/html"));
  client.println(F("Connection: close"));
  client.println();
  client.println(F("<html><body><h1>Login Failed</h1></body></html>"));
}

// Kontrol panelini gönder
void sendDashboard(EthernetClient &client) {
  lastActivity = millis();
  
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: text/html"));
  client.println(F("Connection: close"));
  client.println();
  
  client.println(F("<!DOCTYPE HTML>"));
  client.println(F("<html>"));
  client.println(F("<head>"));
  client.println(F("<title>Dashboard</title>"));
  client.println(F("<meta name='viewport' content='width=device-width, initial-scale=1'>"));
  client.println(F("<style>"));
  client.println(F("body { font-family: Arial, sans-serif; margin: 0; padding: 0; background-color: #f5f5f5; }"));
  client.println(F("header { background-color: #333; color: white; padding: 15px; display: flex; justify-content: space-between; align-items: center; }"));
  client.println(F("h1 { margin: 0; }"));
  client.println(F(".container { max-width: 1200px; margin: 20px auto; padding: 20px; background-color: white; border-radius: 5px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }"));
  client.println(F(".btn { background-color: #4CAF50; color: white; padding: 10px 15px; border: none; border-radius: 4px; cursor: pointer; text-decoration: none; display: inline-block; margin-right: 10px; }"));
  client.println(F(".btn-danger { background-color: #f44336; }"));
  client.println(F("table { width: 100%; border-collapse: collapse; margin-top: 20px; }"));
  client.println(F("th, td { padding: 12px 15px; text-align: left; border-bottom: 1px solid #ddd; }"));
  client.println(F("th { background-color: #f2f2f2; }"));
  client.println(F("tr:hover { background-color: #f5f5f5; }"));
  client.println(F(".status { display: flex; justify-content: space-between; flex-wrap: wrap; }"));
  client.println(F(".status-item { flex: 1; min-width: 200px; margin: 10px; padding: 15px; background-color: #e9e9e9; border-radius: 5px; }"));
  client.println(F("@media screen and (max-width: 600px) { .status-item { min-width: 100%; } }"));
  client.println(F("</style>"));
  client.println(F("</head>"));
  client.println(F("<body>"));
  
  client.println(F("<header>"));
  client.println(F("<h1>System Dashboard</h1>"));
  client.println(F("<div>"));
  client.print(F("<span>Welcome, "));
  client.print(currentUsername);
  client.println(F("</span>"));
  if (currentUserRole == 2) {
    client.println(F("<a href='/admin' class='btn'>Admin Panel</a>"));
  }
  client.println(F("<a href='/logout' class='btn btn-danger'>Logout</a>"));
  client.println(F("</div>"));
  client.println(F("</header>"));
  
  client.println(F("<div class='container'>"));
  client.println(F("<h2>System Status</h2>"));
  client.println(F("<div class='status'>"));
  
  client.println(F("<div class='status-item'>"));
  client.println(F("<h3>Network Status</h3>"));
  client.print(F("<p>IP Address: "));
  client.print(Ethernet.localIP()[0]);
  client.print(F("."));
  client.print(Ethernet.localIP()[1]);
  client.print(F("."));
  client.print(Ethernet.localIP()[2]);
  client.print(F("."));
  client.print(Ethernet.localIP()[3]);
  client.println(F("</p>"));
  client.print(F("<p>Connection Type: "));
  client.print(config.staticIP ? F("Static IP") : F("DHCP"));
  client.println(F("</p>"));
  client.println(F("</div>"));
  
  client.println(F("<div class='status-item'>"));
  client.println(F("<h3>System Info</h3>"));
  client.print(F("<p>Uptime: "));
  unsigned long uptimeSeconds = millis() / 1000;
  unsigned long uptimeMinutes = uptimeSeconds / 60;
  unsigned long uptimeHours = uptimeMinutes / 60;
  uptimeMinutes %= 60;
  uptimeSeconds %= 60;
  client.print(uptimeHours);
  client.print(F("h "));
  client.print(uptimeMinutes);
  client.print(F("m "));
  client.print(uptimeSeconds);
  client.println(F("s</p>"));
  client.println(F("<p>Available Memory: RAM info</p>"));
  client.println(F("</div>"));
  
  client.println(F("</div>"));
  client.println(F("</div>"));
  
  client.println(F("<script>"));
  client.println(F("function updateDashboard() {"));
  client.println(F("  var xhr = new XMLHttpRequest();"));
  client.println(F("  xhr.open('GET', '/api/status', true);"));
  client.println(F("  xhr.onreadystatechange = function() {"));
  client.println(F("    if (xhr.readyState === 4 && xhr.status === 200) {"));
  client.println(F("      // Update dashboard with new data"));
  client.println(F("    }"));
  client.println(F("  };"));
  client.println(F("  xhr.send();"));
  client.println(F("}"));
  client.println(F("// Update dashboard data every 5 seconds"));
  client.println(F("setInterval(updateDashboard, 5000);"));
  client.println(F("</script>"));
  
  client.println(F("</body>"));
  client.println(F("</html>"));
}
//--------- API Endpoints -----------------

// Kullanıcı listesini gönder
void sendUsersList(EthernetClient &client) {
  StaticJsonDocument<512> doc;
  JsonArray users = doc.createNestedArray("users");
  
  for (int i = 0; i < USER_COUNT; i++) {
    if (config.userRole[i] > 0) {
      JsonObject user = users.createNestedObject();
      user["username"] = config.username[i];
      user["role"] = config.userRole[i];
    }
  }
  
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: application/json"));
  client.println(F("Connection: close"));
  client.println();
  serializeJson(doc, client);
}

// Kullanıcı güncelleme işlemlerini yönet
void handleUserUpdate(EthernetClient &client, String postData) {
  String action = "";
  int index = -1;
  String username = "";
  String password = "";
  int role = 0;
  
  // POST verilerini ayrıştır
  int pos = 0;
  while (pos < postData.length()) {
    int nextPos = postData.indexOf('&', pos);
    if (nextPos == -1) nextPos = postData.length();
    
    String param = postData.substring(pos, nextPos);
    int equalPos = param.indexOf('=');
    if (equalPos != -1) {
      String key = param.substring(0, equalPos);
      String value = param.substring(equalPos + 1);
      value.replace("+", " ");
      
      if (key == "action") action = value;
      else if (key == "index") index = value.toInt();
      else if (key == "username") username = value;
      else if (key == "password") password = value;
      else if (key == "role") role = value.toInt();
    }
    pos = nextPos + 1;
  }
  
  bool success = false;
  if (action == "add" && index == -1) {
    // Yeni kullanıcı ekle
    for (int i = 0; i < USER_COUNT; i++) {
      if (config.userRole[i] == 0) {
        strncpy(config.username[i], username.c_str(), MAX_USERNAME_LENGTH);
        strncpy(config.password[i], password.c_str(), MAX_PASSWORD_LENGTH);
        config.userRole[i] = role;
        success = true;
        break;
      }
    }
  } else if (action == "update" && index >= 0 && index < USER_COUNT) {
    // Mevcut kullanıcıyı güncelle
    strncpy(config.username[index], username.c_str(), MAX_USERNAME_LENGTH);
    if (password.length() > 0) {
      strncpy(config.password[index], password.c_str(), MAX_PASSWORD_LENGTH);
    }
    config.userRole[index] = role;
    success = true;
  } else if (action == "delete" && index >= 0 && index < USER_COUNT) {
    // Kullanıcıyı sil
    config.username[index][0] = 0;
    config.password[index][0] = 0;
    config.userRole[index] = 0;
    success = true;
  }
  
  if (success) {
    saveConfig();
    client.println(F("HTTP/1.1 200 OK"));
  } else {
    client.println(F("HTTP/1.1 400 Bad Request"));
  }
  client.println(F("Content-Type: text/plain"));
  client.println(F("Connection: close"));
  client.println();
}

// Ağ yapılandırmasını gönder
void sendNetworkConfig(EthernetClient &client) {
  StaticJsonDocument<256> doc;
  doc["staticIP"] = config.staticIP;
  
  String ipStr = String(config.ip[0]) + "." + String(config.ip[1]) + "." + 
                 String(config.ip[2]) + "." + String(config.ip[3]);
  String subnetStr = String(config.subnet[0]) + "." + String(config.subnet[1]) + "." + 
                    String(config.subnet[2]) + "." + String(config.subnet[3]);
  String gatewayStr = String(config.gateway[0]) + "." + String(config.gateway[1]) + "." + 
                     String(config.gateway[2]) + "." + String(config.gateway[3]);
  String dnsStr = String(config.dns[0]) + "." + String(config.dns[1]) + "." + 
                 String(config.dns[2]) + "." + String(config.dns[3]);
  
  doc["ip"] = ipStr;
  doc["subnet"] = subnetStr;
  doc["gateway"] = gatewayStr;
  doc["dns"] = dnsStr;
  
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: application/json"));
  client.println(F("Connection: close"));
  client.println();
  serializeJson(doc, client);
}

// Ağ yapılandırması güncelleme işlemlerini yönet
void parseIPAddress(String input, byte* output) {
  int startIndex = 0;
  int endIndex = input.indexOf('.');
  for (int i = 0; i < 4; i++) {
    if (endIndex == -1) {
      endIndex = input.length();
    }
    output[i] = input.substring(startIndex, endIndex).toInt();
    startIndex = endIndex + 1;
    endIndex = input.indexOf('.', startIndex);
  }
}

// void parseIPAddress(String input, byte* output);

void handleNetworkUpdate(EthernetClient &client, String postData) {
  String ipType = "";
  String ip = "";
  String subnet = "";
  String gateway = "";
  String dns = "";
  
  // POST verilerini ayrıştır
  int pos = 0;
  while (pos < postData.length()) {
    int nextPos = postData.indexOf('&', pos);
    if (nextPos == -1) nextPos = postData.length();
    
    String param = postData.substring(pos, nextPos);
    int equalPos = param.indexOf('=');
    if (equalPos != -1) {
      String key = param.substring(0, equalPos);
      String value = param.substring(equalPos + 1);
      value.replace("+", " ");
      
      if (key == "ipType") ipType = value;
      else if (key == "ip") ip = value;
      else if (key == "subnet") subnet = value;
      else if (key == "gateway") gateway = value;
      else if (key == "dns") dns = value;
    }
    pos = nextPos + 1;
  }
  
  config.staticIP = (ipType == "static");
  
  if (config.staticIP) {
    // IP adresini ayrıştır ve kaydet
    parseIPAddress(ip, config.ip);
    parseIPAddress(subnet, config.subnet);
    parseIPAddress(gateway, config.gateway);
    parseIPAddress(dns, config.dns);
  }
  
  saveConfig();
  
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: text/plain"));
  client.println(F("Connection: close"));
  client.println();
}

// IP adresini ayrıştır
// Bu fonksiyon zaten tanımlı
  int userIndex = 0;
  while (input.length() > 0 && userIndex < 4) {
    int dotIndex = input.indexOf('.');
    if (dotIndex == -1) dotIndex = input.length();
    output[index++] = input.substring(0, dotIndex).toInt();
    input = input.substring(dotIndex + 1);
  }
}

// void parseIPAddress(String ipString, byte* ipArray);

// Admin sayfasını gönder
void sendAdminPage(EthernetClient &client) {
  lastActivity = millis();
  
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: text/html"));
  client.println(F("Connection: close"));
  client.println();
  
  client.println(F("<!DOCTYPE HTML>"));
  client.println(F("<html>"));
  client.println(F("<head>"));
  client.println(F("<title>Admin Panel</title>"));
  client.println(F("<meta name='viewport' content='width=device-width, initial-scale=1'>"));
  client.println(F("<style>"));
  client.println(F("body { font-family: Arial, sans-serif; margin: 0; padding: 0; background-color: #f5f5f5; }"));
  client.println(F("header { background-color: #333; color: white; padding: 15px; display: flex; justify-content: space-between; align-items: center; }"));
  client.println(F("h1 { margin: 0; }"));
  client.println(F(".container { max-width: 1200px; margin: 20px auto; padding: 20px; background-color: white; border-radius: 5px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }"));
  client.println(F(".tabs { display: flex; border-bottom: 1px solid #ddd; margin-bottom: 20px; }"));
  client.println(F(".tab { padding: 10px 15px; cursor: pointer; background-color: #f1f1f1; border: 1px solid #ddd; border-bottom: none; margin-right: 5px; border-radius: 5px 5px 0 0; }"));
  client.println(F(".tab.active { background-color: white; border-bottom: 1px solid white; margin-bottom: -1px; }"));
  client.println(F(".tab-content { display: none; }"));
  client.println(F(".tab-content.active { display: block; }"));
  client.println(F(".btn { background-color: #4CAF50; color: white; padding: 10px 15px; border: none; border-radius: 4px; cursor: pointer; text-decoration: none; display: inline-block; margin-right: 10px; }"));
  client.println(F(".btn-danger { background-color: #f44336; }"));
  client.println(F(".btn-back { background-color: #333; }"));
  client.println(F("table { width: 100%; border-collapse: collapse; margin-top: 20px; }"));
  client.println(F("th, td { padding: 12px 15px; text-align: left; border-bottom: 1px solid #ddd; }"));
  client.println(F("th { background-color: #f2f2f2; }"));
  client.println(F("tr:hover { background-color: #f5f5f5; }"));
  client.println(F("input[type=text], input[type=password], select { width: 100%; padding: 12px; border: 1px solid #ccc; border-radius: 4px; box-sizing: border-box; margin-top: 6px; margin-bottom: 16px; }"));
  client.println(F("input[type=submit] { background-color: #4CAF50; color: white; padding: 12px 20px; border: none; border-radius: 4px; cursor: pointer; }"));
  client.println(F("input[type=submit]:hover { background-color: #45a049; }"));
  client.println(F(".form-group { margin-bottom: 15px; }"));
  client.println(F(".form-row { display: flex; flex-wrap: wrap; margin-right: -15px; margin-left: -15px; }"));
  client.println(F(".form-col { flex: 0 0 50%; max-width: 50%; padding-right: 15px; padding-left: 15px; box-sizing: border-box; }"));
  client.println(F("@media screen and (max-width: 768px) { .form-col { flex: 0 0 100%; max-width: 100%; } }"));
  client.println(F("</style>"));
  client.println(F("</head>"));
  client.println(F("<body>"));
  
  client.println(F("<header>"));
  client.println(F("<h1>Admin Panel</h1>"));
  client.println(F("<div>"));
  client.print(F("<span>Admin: "));
  client.print(currentUsername);
  client.println(F("</span>"));
  client.println(F("<a href='/dashboard' class='btn btn-back'>Dashboard</a>"));
  client.println(F("<a href='/logout' class='btn btn-danger'>Logout</a>"));
  client.println(F("</div>"));
  client.println(F("</header>"));
  
  client.println(F("<div class='container'>"));
  client.println(F("<div class='tabs'>"));
  client.println(F("<div class='tab active' onclick='openTab(event, \"users\")'>User Management</div>"));
  client.println(F("<div class='tab' onclick='openTab(event, \"network\")'>Network Settings</div>"));
  client.println(F("</div>"));
  
  client.println(F("<div id='users' class='tab-content active'>"));
  client.println(F("<h2>User Management</h2>"));
  client.println(F("<button onclick='addUser()' class='btn'>Add User</button>"));
  client.println(F("<table id='user-table'>"));
  client.println(F("<thead>"));
  client.println(F("<tr>"));
  client.println(F("<th>Username</th>"));
  client.println(F("<th>Role</th>"));
  client.println(F("<th>Actions</th>"));
  client.println(F("</tr>"));
  client.println(F("</thead>"));
  client.println(F("<tbody id='user-list'>"));
  client.println(F("<!-- User list will be loaded here -->"));
  client.println(F("</tbody>"));
  client.println(F("</table>"));
  
  client.println(F("<div id='user-form' style='display:none;'>"));
  client.println(F("<h3 id='form-title'>Add User</h3>"));
  client.println(F("<form id='userForm'>"));
  client.println(F("<input type='hidden' id='userIndex' value='-1'>"));
  client.println(F("<div class='form-group'>"));
  client.println(F("<label for='username'>Username (max 10 characters):</label>"));
  client.println(F("<input type='text' id='username' name='username' maxlength='10' required>"));
  client.println(F("</div>"));
  client.println(F("<div class='form-group'>"));
  client.println(F("<label for='password'>Password (max 10 characters):</label>"));
  client.println(F("<input type='password' id='password' name='password' maxlength='10' required>"));
  client.println(F("</div>"));
  client.println(F("<div class='form-group'>"));
  client.println(F("<label for='role'>Role:</label>"));
  client.println(F("<select id='role' name='role'>"));
  client.println(F("<option value='1'>User</option>"));
  client.println(F("<option value='2'>Admin</option>"));
  client.println(F("</select>"));
  client.println(F("</div>"));
  client.println(F("<button type='submit' class='btn'>Save</button>"));
  client.println(F("<button type='button' onclick='cancelForm()' class='btn btn-danger'>Cancel</button>"));
  client.println(F("</form>"));
  client.println(F("</div>"));
  client.println(F("</div>"));
  
  client.println(F("<div id='network' class='tab-content'>"));
  client.println(F("<h2>Network Settings</h2>"));
  client.println(F("<form id='networkForm'>"));
  client.println(F("<div class='form-group'>"));
  client.println(F("<label for='ipType'>IP Configuration:</label>"));
  client.println(F("<select id='ipType' name='ipType' onchange='toggleIPFields()'>"));
  client.println(F("<option value='dhcp'>DHCP (Auto)</option>"));
  client.println(F("<option value='static'>Static IP</option>"));
  client.println(F("</select>"));
  client.println(F("</div>"));
  
  client.println(F("<div id='staticIP' style='display:none;'>"));
  client.println(F("<div class='form-row'>"));
  client.println(F("<div class='form-col'>"));
  client.println(F("<div class='form-group'>"));
  client.println(F("<label for='ip'>IP Address:</label>"));
  client.println(F("<input type='text' id='ip' name='ip' placeholder='192.168.1.177'>"));
  client.println(F("</div>"));
  client.println(F("</div>"));
  client.println(F("<div class='form-col'>"));
  client.println(F("<div class='form-group'>"));
  client.println(F("<label for='subnet'>Subnet Mask:</label>"));
  client.println(F("<input type='text' id='subnet' name='subnet' placeholder='255.255.255.0'>"));
  client.println(F("</div>"));
  client.println(F("</div>"));
  client.println(F("</div>"));
  
  client.println(F("<div class='form-row'>"));
  client.println(F("<div class='form-col'>"));
  client.println(F("<div class='form-group'>"));
  client.println(F("<label for='gateway'>Gateway:</label>"));
  client.println(F("<input type='text' id='gateway' name='gateway' placeholder='192.168.1.1'>"));
  client.println(F("</div>"));
  client.println(F("</div>"));
  client.println(F("<div class='form-col'>"));
  client.println(F("<div class='form-group'>"));
  client.println(F("<label for='dns'>DNS Server:</label>"));
  client.println(F("<input type='text' id='dns' name='dns' placeholder='8.8.8.8'>"));
  client.println(F("</div>"));
  client.println(F("</div>"));
  client.println(F("</div>"));
  client.println(F("</div>"));
  
  client.println(F("<button type='submit' class='btn'>Save Network Settings</button>"));
  client.println(F("</form>"));
  client.println(F("</div>"));
  client.println(F("</div>"));
  
  client.println(F("<script>"));
  client.println(F("// Sayfa yüklendiğinde kullanıcıları yükle"));
  client.println(F("window.onload = function() {"));
  client.println(F("  loadUsers();"));
  client.println(F("  loadNetworkSettings();"));
  client.println(F("};"));
  
  client.println(F("// Sekme değiştirme"));
  client.println(F("function openTab(evt, tabName) {"));
  client.println(F("  var i, tabcontent, tablinks;"));
  client.println(F("  tabcontent = document.getElementsByClassName('tab-content');"));
  client.println(F("  for (i = 0; i < tabcontent.length; i++) {"));
  client.println(F("    tabcontent[i].style.display = 'none';"));
  client.println(F("  }"));
  client.println(F("  tablinks = document.getElementsByClassName('tab');"));
  client.println(F("  for (i = 0; i < tablinks.length; i++) {"));
  client.println(F("    tablinks[i].className = tablinks[i].className.replace(' active', '');"));
  client.println(F("  }"));
  client.println(F("  document.getElementById(tabName).style.display = 'block';"));
  client.println(F("  evt.currentTarget.className += ' active';"));
  client.println(F("}"));
  
  client.println(F("// Kullanıcıları yükle"));
  client.println(F("function loadUsers() {"));
  client.println(F("  var xhr = new XMLHttpRequest();"));
  client.println(F("  xhr.open('GET', '/api/users', true);"));
  client.println(F("  xhr.onreadystatechange = function() {"));
  client.println(F("    if (xhr.readyState === 4 && xhr.status === 200) {"));
  client.println(F("      var users = JSON.parse(xhr.responseText);"));
  client.println(F("      var userList = document.getElementById('user-list');"));
  client.println(F("      userList.innerHTML = '';"));
  client.println(F("      users.forEach(function(user, index) {"));
  client.println(F("        if (user.role > 0) {"));
  client.println(F("          var row = document.createElement('tr');"));
  client.println(F("          row.innerHTML = '<td>' + user.username + '</td>' +"));
  client.println(F("                         '<td>' + (user.role == 2 ? 'Admin' : 'User') + '</td>' +"));
  client.println(F("                         '<td><button onclick=\"editUser(' + index + ')\" class=\"btn\">Edit</button> ' +"));
  client.println(F("                         '<button onclick=\"deleteUser(' + index + ')\" class=\"btn btn-danger\">Delete</button></td>';"));
  client.println(F("          userList.appendChild(row);"));
  client.println(F("        }"));
  client.println(F("      });"));
  client.println(F("    }"));
  client.println(F("  };"));
  client.println(F("  xhr.send();"));
  client.println(F("}"));
  
  client.println(F("// Ağ ayarlarını yükle"));
  client.println(F("function loadNetworkSettings() {"));
  client.println(F("  var xhr = new XMLHttpRequest();"));
  client.println(F("  xhr.open('GET', '/api/network', true);"));
  client.println(F("  xhr.onreadystatechange = function() {"));
  client.println(F("    if (xhr.readyState === 4 && xhr.status === 200) {"));
  client.println(F("      var settings = JSON.parse(xhr.responseText);"));
  client.println(F("      document.getElementById('ipType').value = settings.staticIP ? 'static' : 'dhcp';"));
  client.println(F("      document.getElementById('ip').value = settings.ip;"));
  client.println(F("      document.getElementById('subnet').value = settings.subnet;"));
  client.println(F("      document.getElementById('gateway').value = settings.gateway;"));
  client.println(F("      document.getElementById('dns').value = settings.dns;"));
  client.println(F("      toggleIPFields();"));
  client.println(F("    }"));
  client.println(F("  };"));
  client.println(F("  xhr.send();"));
  client.println(F("}"));
  
  client.println(F("// Kullanıcı ekle"));
  client.println(F("function addUser() {"));
  client.println(F("  document.getElementById('form-title').textContent = 'Add User';"));
  client.println(F("  document.getElementById('userIndex').value = '-1';"));
  client.println(F("  document.getElementById('username').value = '';"));
  client.println(F("  document.getElementById('password').value = '';"));
  client.println(F("  document.getElementById('role').value = '1';"));
  client.println(F("  document.getElementById('user-form').style.display = 'block';"));
  client.println(F("}"));
  
  client.println(F("// Kullanıcı düzenle"));
  client.println(F("function editUser(index) {"));
  client.println(F("  var xhr = new XMLHttpRequest();"));
  client.println(F("  xhr.open('GET', '/api/users', true);"));
  client.println(F("  xhr.onreadystatechange = function() {"));
  client.println(F("    if (xhr.readyState === 4 && xhr.status === 200) {"));
  client.println(F("      var users = JSON.parse(xhr.responseText);"));
  client.println(F("      var user = users[index];"));
  client.println(F("      document.getElementById('form-title').textContent = 'Edit User';"));
  client.println(F("      document.getElementById('userIndex').value = index;"));
  client.println(F("      document.getElementById('username').value = user.username;"));
  client.println(F("      document.getElementById('password').value = '';"));
  client.println(F("      document.getElementById('role').value = user.role;"));
  client.println(F("      document.getElementById('user-form').style.display = 'block';"));
  client.println(F("    }"));
  client.println(F("  };"));
  client.println(F("  xhr.send();"));
  client.println(F("}"));
  
  client.println(F("// Kullanıcı sil"));
  client.println(F("function deleteUser(index) {"));
  client.println(F("  if (confirm('Are you sure you want to delete this user?')) {"));
  client.println(F("    var xhr = new XMLHttpRequest();"));
  client.println(F("    xhr.open('POST', '/api/user', true);"));
  client.println(F("    xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');"));
  client.println(F("    xhr.onreadystatechange = function() {"));
  client.println(F("      if (xhr.readyState === 4 && xhr.status === 200) {"));
  client.println(F("        loadUsers();"));
  client.println(F("      }"));
  client.println(F("    };"));
  client.println(F("    xhr.send('action=delete&index=' + index);"));
  client.println(F("  }"));
  client.println(F("}"));
  
  client.println(F("// Form iptal"));
  client.println(F("function cancelForm() {"));
  client.println(F("  document.getElementById('user-form').style.display = 'none';"));
  client.println(F("}"));
  
  client.println(F("// IP alanları görünürlüğünü değiştir"));
  client.println(F("function toggleIPFields() {"));
  client.println(F("  var ipType = document.getElementById('ipType').value;"));
  client.println(F("  document.getElementById('staticIP').style.display = ipType === 'static' ? 'block' : 'none';"));
  client.println(F("}"));
  
  client.println(F("// Kullanıcı formu gönderimi"));
  client.println(F("document.getElementById('userForm').addEventListener('submit', function(e) {"));
  client.println(F("  e.preventDefault();"));
  client.println(F("  var index = document.getElementById('userIndex').value;"));
  client.println(F("  var username = document.getElementById('username').value;"));
  client.println(F("  var password = document.getElementById('password').value;"));
  client.println(F("  var role = document.getElementById('role').value;"));
  client.println(F("  var action = index === '-1' ? 'add' : 'update';"));
  
  client.println(F("  var xhr = new XMLHttpRequest();"));
  client.println(F("  xhr.open('POST', '/api/user', true);"));
  client.println(F("  xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');"));
  client.println(F("  xhr.onreadystatechange = function() {"));
  client.println(F("    if (xhr.readyState === 4 && xhr.status === 200) {"));
  client.println(F("      document.getElementById('user-form').style.display = 'none';"));
  client.println(F("      loadUsers();"));
  client.println(F("    }"));
  client.println(F("  };"));
  client.println(F("  xhr.send('action=' + action + '&index=' + index + '&username=' + encodeURIComponent(username) + '&password=' + encodeURIComponent(password) + '&role=' + role);"));
  client.println(F("});"));
  
  client.println(F("// Ağ ayarları formu gönderimi"));
  client.println(F("document.getElementById('networkForm').addEventListener('submit', function(e) {"));
  client.println(F("  e.preventDefault();"));
  client.println(F("  var ipType = document.getElementById('ipType').value;"));
  client.println(F("  var ip = document.getElementById('ip').value;"));
  client.println(F("  var subnet = document.getElementById('subnet').value;"));
  client.println(F("  var gateway = document.getElementById('gateway').value;"));
  client.println(F("  var dns = document.getElementById('dns').value;"));
  
  client.println(F("  var xhr = new XMLHttpRequest();"));
  client.println(F("  xhr.open('POST', '/api/network', true);"));
  client.println(F("  xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');"));
  client.println(F("  xhr.onreadystatechange = function() {"));
  client.println(F("    if (xhr.readyState === 4 && xhr.status === 200) {"));
  client.println(F("      alert('Network settings saved. Changes will take effect after restart.');"));
  client.println(F("    }"));
  client.println(F("  };"));
  client.println(F("  xhr.send('ipType=' + ipType + '&ip=' + ip + '&subnet=' + subnet + '&gateway=' + gateway + '&dns=' + dns);"));
  client.println(F("});"));
  
  client.println(F("</script>"));
  
  client.println(F("</body>"));
  client.println(F("</html>"));
}
