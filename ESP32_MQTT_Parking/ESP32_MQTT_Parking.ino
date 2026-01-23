/**
 * ============================================================================
 * ESP32 MQTT Client - Sistema de Monitoreo de Estacionamientos
 * ============================================================================
 * 
 * Reto Técnico - Nuclea Solutions
 * 
 * Descripción:
 * Este programa implementa un cliente MQTT en ESP32 que se conecta a un broker
 * HiveMQ Cloud mediante TLS/SSL para recibir notificaciones en tiempo real
 * sobre la ocupación de estacionamientos. El sistema se suscribe al topic
 * "parking/tour-completed" y procesa mensajes JSON con información de las
 * zonas de estacionamiento.
 * 
 * Autor: Alfonso
 * Fecha: Enero 2026
 * Versión: 1.0
 * 
 * Librerías requeridas:
 * - WiFi.h (incluida en ESP32 core)
 * - WiFiClientSecure.h (incluida en ESP32 core)
 * - PubSubClient (instalar desde Library Manager)
 * - ArduinoJson (instalar desde Library Manager, versión 6.x recomendada)
 * 
 * ============================================================================
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ============================================================================
// CONFIGURACIÓN DE PANTALLA OLED
// ============================================================================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1  // No usamos pin de reset
#define SCREEN_ADDRESS 0x3C  // Dirección I2C típica para SSD1306

// Crear objeto de pantalla
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Variables globales para mostrar en OLED
int g_totalOcupados = 0;
int g_totalCapacidad = 0;
int g_totalDisponibles = 0;
float g_porcentajeOcupacion = 0;
int g_numZonas = 0;
bool g_mqttConnected = false;
bool g_wifiConnected = false;

// ============================================================================
// CONFIGURACIÓN DE RED WiFi
// ============================================================================
// IMPORTANTE: Modificar estos valores con las credenciales de tu red WiFi
const char* WIFI_SSID     = "TU_WIFI_SSID";      // Nombre de la red WiFi
const char* WIFI_PASSWORD = "TU_WIFI_PASSWORD";  // Contraseña WiFi REAL (NO subir)

// ============================================================================
// CONFIGURACIÓN DEL BROKER MQTT (HiveMQ Cloud)
// ============================================================================
const char* MQTT_HOST     = "TU_CLUSTER.hivemq.cloud"; // Endpoint de HiveMQ
const int   MQTT_PORT     = 8883;                      // Puerto TLS/SSL
const char* MQTT_USER     = "TU_USUARIO_MQTT";         // Usuario MQTT REAL
const char* MQTT_PASSWORD = "TU_PASSWORD_MQTT";        // Password MQTT REAL
const char* MQTT_TOPIC    = "parking/tour-completed";  // Topic (no sensible)
const int   MQTT_QOS      = 0;                         // QoS (no sensible)

// Identificador único del cliente MQTT (se genera con MAC address)
String MQTT_CLIENT_ID = "ESP32_Parking_";

// ============================================================================
// CAPACIDAD TOTAL DE ESTACIONAMIENTOS (Datos reales del Tec)
// ============================================================================
// Estructura para almacenar capacidad por zona
struct ZoneCapacity {
  const char* zoneName;
  int totalSpaces;
};

// Tabla de capacidades por estacionamiento
const ZoneCapacity ZONE_CAPACITIES[] = {
  {"EstacionamientoA", 250},
  {"EstacionamientoB", 249},
  {"EstacionamientoC", 366},
  {"EstacionamientoD", 485},
  {"EstacionamientoE", 220},
  {"EstacionamientoF", 210},   // Zona F
  {"EstacionamientoG", 267},   // Zona G
  {"EstacionamientoH", 289},   // Zona H
  {"EstacionamientoI", 270},   // Zona I
  {"EstacionamientoJ", 400}    // Zona J
};
const int NUM_ZONES = sizeof(ZONE_CAPACITIES) / sizeof(ZONE_CAPACITIES[0]);

// Función para obtener capacidad total de una zona
int getZoneCapacity(const char* zoneName) {
  for (int i = 0; i < NUM_ZONES; i++) {
    if (strcmp(ZONE_CAPACITIES[i].zoneName, zoneName) == 0) {
      return ZONE_CAPACITIES[i].totalSpaces;
    }
  }
  return -1;  // Zona no encontrada
}

// ============================================================================
// CONFIGURACIÓN DE TIEMPOS Y REINTENTOS
// ============================================================================
const unsigned long WIFI_TIMEOUT_MS = 20000;    // Timeout conexión WiFi (20s)
const unsigned long MQTT_RECONNECT_DELAY = 5000; // Delay entre reintentos MQTT (5s)
const unsigned long WIFI_RECONNECT_DELAY = 5000; // Delay entre reintentos WiFi (5s)

// ============================================================================
// OBJETOS GLOBALES
// ============================================================================
WiFiClientSecure wifiClient;      // Cliente WiFi con soporte TLS
PubSubClient mqttClient(wifiClient); // Cliente MQTT

// Variables de control de tiempo
unsigned long lastReconnectAttempt = 0;

// ============================================================================
// PROTOTIPO DE FUNCIONES
// ============================================================================
void setupWiFi();
void setupMQTT();
void setupOLED();
bool connectToMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void processJSONMessage(const char* jsonPayload);
void printSeparator();
void printHeader(const char* title);
void updateOLED();
void showOLEDMessage(const char* line1, const char* line2 = "", const char* line3 = "");

// ============================================================================
// FUNCIÓN SETUP - Inicialización del sistema
// ============================================================================
void setup() {
  // Inicializar comunicación serial para debug
  Serial.begin(115200);
  delay(1000); // Esperar a que se estabilice el puerto serial
  
  // Inicializar pantalla OLED
  setupOLED();
  showOLEDMessage("PARKING TEC", "Iniciando...", "");
  
  // Mostrar información de inicio
  printSeparator();
  Serial.println("ESP32 MQTT Client - Sistema de Monitoreo de Estacionamientos");
  Serial.println("Reto Técnico - Nuclea Solutions");
  printSeparator();
  Serial.println();
  
  // Generar Client ID único usando la dirección MAC
  MQTT_CLIENT_ID += String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.print("Client ID: ");
  Serial.println(MQTT_CLIENT_ID);
  Serial.println();
  
  // Configurar e iniciar conexión WiFi
  setupWiFi();
  
  // Sincronizar tiempo (necesario para TLS)
  Serial.println("Sincronizando tiempo con servidor NTP...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  
  // Esperar a que se sincronice el tiempo
  time_t now = time(nullptr);
  int attempts = 0;
  while (now < 8 * 3600 * 2 && attempts < 20) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
    attempts++;
  }
  Serial.println();
  
  if (now > 8 * 3600 * 2) {
    Serial.println("[NTP] Tiempo sincronizado correctamente");
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    Serial.print("[NTP] Fecha/Hora UTC: ");
    Serial.println(asctime(&timeinfo));
  } else {
    Serial.println("[NTP] Advertencia: No se pudo sincronizar el tiempo");
  }
  
  // Configurar cliente MQTT
  setupMQTT();
  
  // Conectar al broker MQTT
  connectToMQTT();
}

// ============================================================================
// FUNCIÓN LOOP - Bucle principal
// ============================================================================
void loop() {
  // Verificar conexión WiFi
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Conexión perdida. Reconectando...");
    setupWiFi();
  }
  
  // Verificar y mantener conexión MQTT
  if (!mqttClient.connected()) {
    unsigned long now = millis();
    // Intentar reconexión cada MQTT_RECONNECT_DELAY milisegundos
    if (now - lastReconnectAttempt > MQTT_RECONNECT_DELAY) {
      lastReconnectAttempt = now;
      Serial.println("[MQTT] Conexión perdida. Intentando reconexión...");
      if (connectToMQTT()) {
        lastReconnectAttempt = 0;
      }
    }
  } else {
    // Mantener conexión MQTT activa y procesar mensajes entrantes
    mqttClient.loop();
  }
}

// ============================================================================
// CONFIGURACIÓN Y CONEXIÓN WiFi
// ============================================================================
void setupWiFi() {
  printHeader("CONEXION WiFi");
  
  Serial.print("Conectando a: ");
  Serial.println(WIFI_SSID);
  
  // Configurar modo Station (cliente)
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  // Indicador visual de conexión
  unsigned long startTime = millis();
  int dots = 0;
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    dots++;
    
    // Salto de línea cada 20 puntos para mejor visualización
    if (dots % 20 == 0) {
      Serial.println();
    }
    
    // Verificar timeout
    if (millis() - startTime > WIFI_TIMEOUT_MS) {
      Serial.println();
      Serial.println("[WiFi] ERROR: Timeout de conexión");
      Serial.println("[WiFi] Reiniciando ESP32...");
      delay(2000);
      ESP.restart();
    }
  }
  
  Serial.println();
  Serial.println("[WiFi] ¡Conexión exitosa!");
  Serial.print("[WiFi] Dirección IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("[WiFi] Intensidad de señal (RSSI): ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");
  Serial.println();
  
  // Actualizar estado para OLED
  g_wifiConnected = true;
  showOLEDMessage("PARKING TEC", "WiFi: OK", WiFi.localIP().toString().c_str());
  delay(1000);
}

// ============================================================================
// CONFIGURACIÓN DEL CLIENTE MQTT
// ============================================================================
void setupMQTT() {
  printHeader("CONFIGURACION MQTT");
  
  // Configurar conexión TLS sin verificación de certificado
  // NOTA: En producción, se recomienda usar setCACert() con el certificado CA
  wifiClient.setInsecure();
  
  // Configurar timeouts para la conexión TLS
  wifiClient.setTimeout(15);  // Timeout de 15 segundos
  
  // Configurar servidor MQTT
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  
  // Configurar callback para mensajes entrantes
  mqttClient.setCallback(mqttCallback);
  
  // Configurar tamaño del buffer para mensajes grandes (JSON)
  mqttClient.setBufferSize(1024);
  
  // Configurar keep alive más largo
  mqttClient.setKeepAlive(60);
  
  // Configurar timeout de socket
  mqttClient.setSocketTimeout(15);
  
  Serial.print("Broker: ");
  Serial.println(MQTT_HOST);
  Serial.print("Puerto: ");
  Serial.println(MQTT_PORT);
  Serial.print("Topic: ");
  Serial.println(MQTT_TOPIC);
  Serial.println("TLS/SSL: Habilitado (setInsecure)");
  Serial.println();
}

// ============================================================================
// CONEXIÓN AL BROKER MQTT
// ============================================================================
bool connectToMQTT() {
  printHeader("CONEXION MQTT");
  
  Serial.println("Intentando conexión al broker MQTT...");
  Serial.print("Usuario: ");
  Serial.println(MQTT_USER);
  
  // Intentar conexión con credenciales
  if (mqttClient.connect(MQTT_CLIENT_ID.c_str(), MQTT_USER, MQTT_PASSWORD)) {
    Serial.println("[MQTT] ¡Conexión exitosa!");
    
    // Suscribirse al topic
    if (mqttClient.subscribe(MQTT_TOPIC, MQTT_QOS)) {
      Serial.print("[MQTT] Suscripción exitosa al topic: ");
      Serial.println(MQTT_TOPIC);
      Serial.print("[MQTT] QoS: ");
      Serial.println(MQTT_QOS);
    } else {
      Serial.println("[MQTT] ERROR: Falló la suscripción al topic");
    }
    
    Serial.println();
    printSeparator();
    Serial.println("SISTEMA LISTO - Esperando mensajes...");
    printSeparator();
    Serial.println();
    
    // Actualizar estado para OLED
    g_mqttConnected = true;
    showWaitingForMessages();
    
    return true;
  } else {
    // Mostrar código de error
    int state = mqttClient.state();
    Serial.print("[MQTT] ERROR: Falló la conexión. Código: ");
    Serial.println(state);
    
    // Interpretar código de error
    switch (state) {
      case -4: Serial.println("       -> MQTT_CONNECTION_TIMEOUT"); break;
      case -3: Serial.println("       -> MQTT_CONNECTION_LOST"); break;
      case -2: Serial.println("       -> MQTT_CONNECT_FAILED"); break;
      case -1: Serial.println("       -> MQTT_DISCONNECTED"); break;
      case 1:  Serial.println("       -> MQTT_CONNECT_BAD_PROTOCOL"); break;
      case 2:  Serial.println("       -> MQTT_CONNECT_BAD_CLIENT_ID"); break;
      case 3:  Serial.println("       -> MQTT_CONNECT_UNAVAILABLE"); break;
      case 4:  Serial.println("       -> MQTT_CONNECT_BAD_CREDENTIALS"); break;
      case 5:  Serial.println("       -> MQTT_CONNECT_UNAUTHORIZED"); break;
      default: Serial.println("       -> Error desconocido"); break;
    }
    
    return false;
  }
}

// ============================================================================
// CALLBACK - Procesamiento de mensajes MQTT recibidos
// ============================================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.println();
  printSeparator();
  Serial.println("¡NUEVO MENSAJE RECIBIDO!");
  printSeparator();
  
  // Mostrar información del mensaje
  Serial.print("Topic: ");
  Serial.println(topic);
  Serial.print("Longitud: ");
  Serial.print(length);
  Serial.println(" bytes");
  Serial.println();
  
  // Convertir payload a string
  char jsonPayload[length + 1];
  memcpy(jsonPayload, payload, length);
  jsonPayload[length] = '\0';
  
  // Mostrar payload raw para debug
  Serial.println("Payload JSON recibido:");
  Serial.println(jsonPayload);
  Serial.println();
  
  // Procesar el mensaje JSON
  processJSONMessage(jsonPayload);
}

// ============================================================================
// PROCESAMIENTO DEL MENSAJE JSON
// ============================================================================
void processJSONMessage(const char* jsonPayload) {
  // Crear documento JSON con capacidad suficiente
  StaticJsonDocument<1024> doc;
  
  // Parsear el JSON
  DeserializationError error = deserializeJson(doc, jsonPayload);
  
  // Verificar errores de parsing
  if (error) {
    Serial.print("[JSON] ERROR de parsing: ");
    Serial.println(error.c_str());
    return;
  }
  
  Serial.println("═══════════════════════════════════════════════════════════════════════════");
  Serial.println("              SISTEMA DE MONITOREO DE ESTACIONAMIENTOS - TEC              ");
  Serial.println("═══════════════════════════════════════════════════════════════════════════");
  Serial.println();
  
  // Extraer y mostrar timestamp
  const char* sentAt = doc["sentAt"];
  if (sentAt) {
    Serial.print("📅 Fecha/Hora del reporte: ");
    Serial.println(sentAt);
    Serial.println();
  }
  
  // Extraer y procesar array de zonas
  JsonArray zones = doc["zones"];
  
  if (zones.isNull()) {
    Serial.println("⚠️  Array 'zones' no encontrado o vacío");
    return;
  }
  
  int totalCars = 0;
  int totalCapacity = 0;
  int totalAvailable = 0;
  int zoneCount = zones.size();
  
  // Encabezado de la tabla
  Serial.println("┌──────────────────────────────────────────────────────────────────────────┐");
  Serial.println("│  ZONA              │ OCUPADOS │  TOTAL  │ DISPONIBLES │  OCUPACIÓN      │");
  Serial.println("├──────────────────────────────────────────────────────────────────────────┤");
  
  // Iterar sobre cada zona del array
  for (JsonObject zone : zones) {
    const char* zoneName = zone["zoneName"];
    int carsDetected = zone["carsDetected"];
    
    // Obtener capacidad total de la zona
    int capacity = getZoneCapacity(zoneName);
    int available = 0;
    float occupancyPercent = 0;
    
    if (capacity > 0) {
      available = capacity - carsDetected;
      if (available < 0) available = 0;  // Por si hay más autos que capacidad
      occupancyPercent = (float)carsDetected / capacity * 100.0;
      totalCapacity += capacity;
      totalAvailable += available;
    }
    
    // Mostrar información de la zona
    Serial.print("│  🅿️  ");
    
    // Nombre de zona (16 chars)
    String zoneStr = String(zoneName);
    Serial.print(zoneStr);
    for (int i = zoneStr.length(); i < 14; i++) Serial.print(" ");
    
    Serial.print("│   ");
    
    // Ocupados (4 chars)
    if (carsDetected < 10) Serial.print(" ");
    if (carsDetected < 100) Serial.print(" ");
    Serial.print(carsDetected);
    Serial.print("    │   ");
    
    // Total (4 chars)
    if (capacity > 0) {
      if (capacity < 10) Serial.print(" ");
      if (capacity < 100) Serial.print(" ");
      Serial.print(capacity);
    } else {
      Serial.print(" N/A");
    }
    Serial.print("   │     ");
    
    // Disponibles (4 chars)
    if (capacity > 0) {
      if (available < 10) Serial.print(" ");
      if (available < 100) Serial.print(" ");
      Serial.print(available);
    } else {
      Serial.print("N/A");
    }
    Serial.print("      │  ");
    
    // Porcentaje de ocupación con indicador visual
    if (capacity > 0) {
      if (occupancyPercent < 10) Serial.print(" ");
      Serial.print(occupancyPercent, 1);
      Serial.print("% ");
      
      // Indicador de estado
      if (occupancyPercent >= 90) {
        Serial.print("🔴");  // Crítico
      } else if (occupancyPercent >= 70) {
        Serial.print("🟡");  // Moderado
      } else {
        Serial.print("🟢");  // Disponible
      }
    } else {
      Serial.print("  N/A  ");
    }
    Serial.println("      │");
    
    totalCars += carsDetected;
  }
  
  // Línea de totales
  Serial.println("├──────────────────────────────────────────────────────────────────────────┤");
  Serial.print("│  📊 TOTALES        │   ");
  if (totalCars < 10) Serial.print(" ");
  if (totalCars < 100) Serial.print(" ");
  Serial.print(totalCars);
  Serial.print("    │  ");
  if (totalCapacity < 10) Serial.print(" ");
  if (totalCapacity < 100) Serial.print(" ");
  if (totalCapacity < 1000) Serial.print(" ");
  Serial.print(totalCapacity);
  Serial.print("  │     ");
  if (totalAvailable < 10) Serial.print(" ");
  if (totalAvailable < 100) Serial.print(" ");
  if (totalAvailable < 1000) Serial.print(" ");
  Serial.print(totalAvailable);
  Serial.print("     │  ");
  
  // Porcentaje total
  float totalOccupancy = 0;
  if (totalCapacity > 0) {
    totalOccupancy = (float)totalCars / totalCapacity * 100.0;
    if (totalOccupancy < 10) Serial.print(" ");
    Serial.print(totalOccupancy, 1);
    Serial.print("% ");
    if (totalOccupancy >= 90) {
      Serial.print("🔴");
    } else if (totalOccupancy >= 70) {
      Serial.print("🟡");
    } else {
      Serial.print("🟢");
    }
  }
  Serial.println("      │");
  Serial.println("└──────────────────────────────────────────────────────────────────────────┘");
  Serial.println();
  
  // Resumen estadístico mejorado
  Serial.println("📈 RESUMEN EJECUTIVO:");
  Serial.println("   ─────────────────────────────────────────");
  Serial.print("   • Zonas monitoreadas:      ");
  Serial.println(zoneCount);
  Serial.print("   • Espacios ocupados:       ");
  Serial.print(totalCars);
  Serial.print(" / ");
  Serial.println(totalCapacity);
  Serial.print("   • Espacios disponibles:    ");
  Serial.println(totalAvailable);
  Serial.print("   • Ocupación promedio:      ");
  Serial.print(totalOccupancy, 1);
  Serial.println("%");
  Serial.println();
  
  // Leyenda de colores
  Serial.println("   📍 Leyenda: 🟢 <70% | 🟡 70-90% | 🔴 >90%");
  Serial.println();
  
  // ===== ACTUALIZAR VARIABLES GLOBALES PARA OLED =====
  g_totalOcupados = totalCars;
  g_totalCapacidad = totalCapacity;
  g_totalDisponibles = totalAvailable;
  g_porcentajeOcupacion = totalOccupancy;
  g_numZonas = zoneCount;
  
  // Actualizar pantalla OLED
  updateOLED();
  
  printSeparator();
  Serial.println("Esperando siguiente mensaje...");
  printSeparator();
  Serial.println();
}

// ============================================================================
// FUNCIONES AUXILIARES DE FORMATO
// ============================================================================

/**
 * Imprime una línea separadora
 */
void printSeparator() {
  Serial.println("============================================================");
}

/**
 * Imprime un encabezado formateado
 */
void printHeader(const char* title) {
  Serial.println();
  printSeparator();
  Serial.print(">> ");
  Serial.println(title);
  printSeparator();
}

// ============================================================================
// FUNCIONES DE PANTALLA OLED
// ============================================================================

/**
 * Inicializa la pantalla OLED
 */
void setupOLED() {
  Serial.println("[OLED] Inicializando pantalla...");
  
  // Inicializar I2C con pines específicos (SDA=21, SCL=22)
  Wire.begin(21, 22);
  
  // Inicializar pantalla
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("[OLED] ERROR: No se pudo inicializar la pantalla");
    return;
  }
  
  Serial.println("[OLED] Pantalla inicializada correctamente");
  
  // Configuración inicial
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.display();
}

/**
 * Muestra un mensaje simple en la OLED
 */
void showOLEDMessage(const char* line1, const char* line2, const char* line3) {
  display.clearDisplay();
  
  // Título centrado
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("================");
  
  display.setTextSize(1);
  display.setCursor(20, 12);
  display.println(line1);
  
  display.setCursor(20, 28);
  display.println(line2);
  
  display.setCursor(20, 44);
  display.println(line3);
  
  display.setCursor(0, 56);
  display.println("================");
  
  display.display();
}

/**
 * Actualiza la pantalla OLED con datos de estacionamiento
 */
void updateOLED() {
  display.clearDisplay();
  
  // ===== ENCABEZADO =====
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("PARKING TEC  ");
  
  // Indicadores de conexión
  if (g_wifiConnected) {
    display.print("W");
  } else {
    display.print("-");
  }
  display.print("/");
  if (g_mqttConnected) {
    display.print("M");
  } else {
    display.print("-");
  }
  
  // ===== LÍNEA SEPARADORA =====
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);
  
  // ===== DATOS PRINCIPALES =====
  display.setTextSize(1);
  
  // Ocupados / Total
  display.setCursor(0, 14);
  display.print("Ocupados: ");
  display.print(g_totalOcupados);
  display.print("/");
  display.print(g_totalCapacidad);
  
  // Disponibles
  display.setCursor(0, 26);
  display.print("Disponibles: ");
  display.setTextSize(2);
  display.print(g_totalDisponibles);
  
  // ===== PORCENTAJE DE OCUPACIÓN =====
  display.setTextSize(1);
  display.setCursor(0, 46);
  display.print("Ocupacion: ");
  display.print(g_porcentajeOcupacion, 1);
  display.print("%");
  
  // Indicador visual de estado
  display.setCursor(100, 46);
  if (g_porcentajeOcupacion >= 90) {
    display.print("[!!]");  // Crítico
  } else if (g_porcentajeOcupacion >= 70) {
    display.print("[! ]");  // Moderado
  } else {
    display.print("[OK]");  // Disponible
  }
  
  // ===== LÍNEA INFERIOR =====
  display.drawLine(0, 56, 127, 56, SSD1306_WHITE);
  
  // Número de zonas
  display.setCursor(0, 58);
  display.print("Zonas: ");
  display.print(g_numZonas);
  
  display.display();
}

/**
 * Muestra animación de conexión WiFi
 */
void showWiFiConnecting() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(20, 20);
  display.println("Conectando");
  display.setCursor(30, 32);
  display.println("WiFi...");
  display.display();
}

/**
 * Muestra estado de conexión MQTT
 */
void showMQTTConnecting() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(20, 20);
  display.println("Conectando");
  display.setCursor(30, 32);
  display.println("MQTT...");
  display.display();
}

/**
 * Muestra pantalla de espera de mensajes
 */
void showWaitingForMessages() {
  display.clearDisplay();
  
  display.setTextSize(1);
  display.setCursor(10, 8);
  display.println("PARKING TEC");
  
  display.drawLine(0, 18, 127, 18, SSD1306_WHITE);
  
  display.setCursor(15, 28);
  display.println("Esperando");
  display.setCursor(20, 40);
  display.println("mensajes...");
  
  // Indicadores
  display.setCursor(0, 56);
  display.print("WiFi:");
  display.print(g_wifiConnected ? "OK" : "--");
  display.print(" MQTT:");
  display.print(g_mqttConnected ? "OK" : "--");
  
  display.display();
}
