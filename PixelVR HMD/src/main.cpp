#include <Arduino.h>
#include "EEPROM.h"
#include "WiFi.h"
#include "esp_now.h"
#include "Wire.h"
#include "FastIMU.h"
#include "MadgwickAHRS.h"

uint8_t mac[6] = {0xAC, 0xA7, 0x04, 0x27, 0xAE, 0x90};// Dirección MAC física del ESP32-S3 receptor; destino al que este HMD transmitirá por ESP-NOW

// ─── Identificación de tipo de paquete ESP-NOW ────────────────────────────────
// El receptor recibe paquetes de tres orígenes (control izquierdo, control
// derecho, HMD), así que cada paquete lleva un byte de tipo al inicio para que
// el receptor sepa cómo interpretarlo sin depender solo del tamaño. DEBE
// coincidir con el valor usado en el firmware de los controles y del receptor.
#define PACKET_TYPE_CONTROLLER 0x01
#define PACKET_TYPE_HMD        0x02

typedef struct __attribute__((packed)) HmdData
{
  uint8_t packetType;   // Siempre PACKET_TYPE_HMD en este firmware
  float qw, qx, qy, qz;
  float error;
} HmdData;

HmdData hmddata;

// USB-Serial (solo para logs/comandos; el dato ya no sale por acá)
static const uint32_t USB_BAUD_RATE = 115200;

// Terminales
#define SCL 21
#define SDA 22
#define led_r   4
#define led_g   32
#define led_b   35

// ─── IMU ─────────────────────────────────────────────────────────────────────
#define MPU_ADDRESS  0x68
#define I2C_CLOCK    400000
int currentImuGeometry = 2;   // Mutable para poder probar geometrías sin recompilar (ver comando "geo")
MPU6050 IMU;
calData calibIMU = { 0 };
AccelData IMUAccel;
GyroData IMUGyro;

// ─── Filtro Madgwick igual al de PixelVR Controllers ─────────────────────────
#define GYRO_DEADZONE_DPS    1.5f
#define QUAT_SMOOTH_ALPHA    0.08f
#define MADGWICK_BETA_MIN    0.02f
#define MADGWICK_BETA_MAX    0.08f
#define ACCEL_STILL_TOL_G    0.15f
#define GYRO_STILL_TOL_DPS   15.0f
#define BETA_SMOOTH_ALPHA    0.10f

struct QuatState {
  float w, x, y, z;
} quatFiltered;

bool quatFilterInit = false;
float currentBeta = MADGWICK_BETA_MIN;
Madgwick filter;

static inline float applyDeadzone(float value, float threshold) {
  return (fabsf(value) < threshold) ? 0.0f : value;
}

static QuatState nlerpQuat(const QuatState& prev,
                           float nw, float nx, float ny, float nz,
                           float alpha) {
  float dot = prev.w*nw + prev.x*nx + prev.y*ny + prev.z*nz;
  if (dot < 0.0f) { nw = -nw; nx = -nx; ny = -ny; nz = -nz; }

  QuatState result;
  result.w = prev.w + alpha * (nw - prev.w);
  result.x = prev.x + alpha * (nx - prev.x);
  result.y = prev.y + alpha * (ny - prev.y);
  result.z = prev.z + alpha * (nz - prev.z);

  const float invNorm = 1.0f / sqrtf(result.w*result.w + result.x*result.x +
                                    result.y*result.y + result.z*result.z);
  result.w *= invNorm; result.x *= invNorm;
  result.y *= invNorm; result.z *= invNorm;
  return result;
}

// ─── Offset de recentrado ─────────────────────────────────────────────────────
float yawOffset   = 0.0f;
float pitchOffset = 0.0f;
float rollOffset  = 0.0f;

// ─── EEPROM ───────────────────────────────────────────────────────────────────
#define EEPROM_SIZE    256
#define ADDR_CALIB_IMU 0

// ─── Timing ───────────────────────────────────────────────────────────────────
#define UPDATE_RATE_HZ  200.0f
const unsigned long INTERVALO_US = 1000000UL / (unsigned long)UPDATE_RATE_HZ;
unsigned long ultimoTiempoUS = 0;

static void resetOrientationFilter() {
  filter = Madgwick();
  filter.begin(UPDATE_RATE_HZ);
  currentBeta = MADGWICK_BETA_MIN;
  filter.beta = currentBeta;
  quatFiltered = {1.0f, 0.0f, 0.0f, 0.0f};
  quatFilterInit = false;
}

float        errorCode   = 0.0f;
volatile bool espNowSendPending = false;

// Pausa el envío ESP-NOW mientras dura un comando serial bloqueante o que no
// tiene sentido transmitir (calibrate, recenter), para no seguir imprimiendo
// "Enviado, OK." u otros logs de envío en medio de la operación.
bool espNowPaused = false;

void OnDataSent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) // Callback ESP-NOW invocado al confirmarse el envío de un paquete
{
  espNowSendPending = false;
  (void)tx_info;
  (void)status;
}

// ─── Helpers ──────────────────────────────────────────────────────────────────
static void printCalibration() {
  Serial.println(F("Log: --- Sesgo de Calibración Actual ---"));
  Serial.print(F("Accel Bias: "));
  Serial.print(calibIMU.accelBias[0]); Serial.print(", ");
  Serial.print(calibIMU.accelBias[1]); Serial.print(", ");
  Serial.println(calibIMU.accelBias[2]);
  Serial.print(F("Gyro Bias: "));
  Serial.print(calibIMU.gyroBias[0]); Serial.print(", ");
  Serial.print(calibIMU.gyroBias[1]); Serial.print(", ");
  Serial.println(calibIMU.gyroBias[2]);
}

static void saveCalibrationToEEPROM() {
  Serial.println(F("Log: Guardando calibración en memoria..."));
  calibIMU.valid = true;
  EEPROM.put(ADDR_CALIB_IMU, calibIMU);
  EEPROM.commit();
  Serial.println(F("Log: ¡Datos guardados exitosamente!"));
}

static bool loadCalibrationFromEEPROM() {
  Serial.println(F("Log: Buscando calibración previa..."));
  calData temp;
  EEPROM.get(ADDR_CALIB_IMU, temp);
  if (temp.valid) {
    calibIMU = temp;
    Serial.println(F("Log: Calibración importada con éxito."));
    return true;
  }
  Serial.println(F("Log: No se encontró calibración válida."));
  return false;
}

static void calibrateSensors() {
  calibIMU = { 0 };

  IMU.init(calibIMU, MPU_ADDRESS);
  IMU.setIMUGeometry(currentImuGeometry);

  Serial.println(F("Log: MANTENER EL VISOR COMPLETAMENTE QUIETO Y NIVELADO..."));
  delay(3000);
  IMU.calibrateAccelGyro(&calibIMU);
  IMU.init(calibIMU, MPU_ADDRESS);
  Serial.println(F("Log: MPU6050 Calibrado."));

  saveCalibrationToEEPROM();
}

// Animación de encendido con la misma curva y tiempos que PixelVR Controllers.
// En el HMD se usa el canal rojo como color de identidad.
static void setHmdLedBrightness(uint8_t brightness) {
  analogWrite(led_r, brightness);
  analogWrite(led_g, 0);
  analogWrite(led_b, 0);
}

static void fadeHmdLed(uint8_t from, uint8_t to, uint16_t durationMs) {
  const uint8_t steps = 48;
  for (uint8_t step = 0; step <= steps; ++step) {
    const float t = step / (float)steps;
    const float eased = t * t * (3.0f - 2.0f * t);
    const uint8_t brightness = (uint8_t)(from + (to - from) * eased);
    setHmdLedBrightness(brightness);
    delay(durationMs / steps);
  }
}

static void startupLedAnimation() {
  analogWriteResolution(led_r, 8);
  analogWriteResolution(led_g, 8);
  analogWriteResolution(led_b, 8);
  setHmdLedBrightness(0);
  fadeHmdLed(0, 125, 350);
  fadeHmdLed(125, 18, 420);
  fadeHmdLed(18, 205, 450);
  fadeHmdLed(205, 28, 420);
  fadeHmdLed(28, 255, 650);
}

void setup()
{
  Serial.begin(USB_BAUD_RATE);
  while (!Serial && millis() < 3000);

  if (!EEPROM.begin(EEPROM_SIZE)) {
    Serial.println(F("Log: Error al inicializar EEPROM."));
  }

  pinMode(led_r, OUTPUT); 
  pinMode(led_g, OUTPUT); 
  pinMode(led_b, OUTPUT);
  startupLedAnimation();

  Wire.begin(SDA, SCL);
  Wire.setClock(I2C_CLOCK);

  if (loadCalibrationFromEEPROM()) 
  {
    IMU.init(calibIMU, MPU_ADDRESS);
    IMU.setIMUGeometry(currentImuGeometry);
  } 
  else 
  {
    calibrateSensors();
  }

  // DLPF del MPU6050: 0x03 → ~44 Hz BW
  Wire.beginTransmission(MPU_ADDRESS);
  Wire.write(0x1A);
  Wire.write(0x03);
  Wire.endTransmission();

  printCalibration();
  resetOrientationFilter();
  
  // Inicializar paquete en identidad
  memset(&hmddata, 0, sizeof(hmddata));               // Limpia primero (padding/basura), DEBE ir antes de setear los valores reales
  hmddata.packetType = PACKET_TYPE_HMD;                // Identifica este paquete como del HMD ante el receptor
  hmddata.qw = 1.0f; 
  hmddata.qx = 0.0f; 
  hmddata.qy = 0.0f; 
  hmddata.qz = 0.0f;
  hmddata.error = 0.0f;

  // WiFi
  WiFi.mode(WIFI_STA);                                // Pone el ESP32 en modo Station; ESP-NOW requiere este modo
  Serial.println("Log: La dirección MAC de este HMD es...");
  Serial.println(WiFi.macAddress());
  Serial.printf("Log: MAC receptor: %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]); // Imprime la MAC del receptor para verificación

  // ESP-NOW
  Serial.println("Log: Inicializando ESP-NOW para transmitir datos del HMD...");
  if (esp_now_init() != ESP_OK)                       
  {
    Serial.println("Error: ESP-NOW init fallido");
    errorCode = 4;                                    
    digitalWrite(led_r, HIGH);                        
    return;                                           
  }
  esp_now_register_send_cb(OnDataSent);               // Registra el callback que se invocará al enviar paquetes

  esp_now_peer_info_t peerInfo;                       // Estructura que describe al peer (receptor) ESP-NOW
  memset(&peerInfo, 0, sizeof(peerInfo));             // Limpia la estructura para evitar campos basura
  memcpy(peerInfo.peer_addr, mac, 6);                 // Copia la MAC del receptor en la configuración del peer
  peerInfo.channel = 0;                               // Canal 0 = automático (sigue el canal del WiFi STA actual)
  peerInfo.encrypt = false;                           // Sin cifrado (prioridad: latencia baja para VR)
  peerInfo.ifidx   = WIFI_IF_STA;                     // Usa la interfaz Station

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {        // Registra al receptor como peer válido para envíos
    Serial.println("Error: No se pudo añadir peer ESP-NOW");
    digitalWrite(led_r, HIGH); digitalWrite(led_b, HIGH); // LED morado = error de peer
    return;
  }

  Serial.println("Log: ESP-NOW listo");

  ultimoTiempoUS = micros();
}

void loop()                                                                
{
  // ── Comandos serial ────────────────────────────────────────────────────────
  static String serialCommand = "";
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      serialCommand.trim();
      serialCommand.toLowerCase();
      if (serialCommand == "calibrate") {
        espNowPaused = true;               // Corta el envío mientras dura la calibración (bloqueante, ~3s)
        calibrateSensors();
        printCalibration();
        espNowPaused = false;
      } else if (serialCommand == "show") {
        printCalibration();
      } else if (serialCommand == "recenter") {
        espNowPaused = true;               // Corta el envío mientras se procesa el recentrado
        Serial.println(F("Log: Sin magnetómetro, el yaw no tiene referencia absoluta."));
        espNowPaused = false;
      } else if (serialCommand.startsWith("geo ")) {
        int idx = serialCommand.substring(4).toInt();
        if (idx >= 0 && idx <= 7) {
          currentImuGeometry = idx;
          IMU.setIMUGeometry(currentImuGeometry);
          yawOffset = pitchOffset = rollOffset = 0.0f;
          resetOrientationFilter();
          Serial.print(F("Log: IMU_GEOMETRY cambiada a "));
          Serial.println(currentImuGeometry);
        } else {
          Serial.println(F("Log: Indice invalido. Usar 'geo 0' a 'geo 7'."));
        }
      }
      serialCommand = "";
    } else {
      serialCommand += c;
    }
  }

  if (errorCode != 0.0f) {                                // Si hay un error activo
    hmddata.error = errorCode;                            // Lo propaga al paquete para que el receptor lo conozca
  }

  digitalWrite(led_r, HIGH);

  // ── Control de tiempo ──────────────────────────────────────────────────────
  unsigned long ahora = micros();
  if (ahora - ultimoTiempoUS < INTERVALO_US) return;
  ultimoTiempoUS = ahora;


  // ── Leer sensores ──────────────────────────────────────────────────────────
  IMU.update();
  IMU.getAccel(&IMUAccel);
  IMU.getGyro(&IMUGyro);


  const float gx = applyDeadzone(IMUGyro.gyroX, GYRO_DEADZONE_DPS);
  const float gy = applyDeadzone(IMUGyro.gyroY, GYRO_DEADZONE_DPS);
  const float gz = applyDeadzone(IMUGyro.gyroZ, GYRO_DEADZONE_DPS);

  // Beta adaptativo: mayor corrección de gravedad cuando el visor está quieto.
  const float accelMagG = sqrtf(IMUAccel.accelX * IMUAccel.accelX +
                                IMUAccel.accelY * IMUAccel.accelY +
                                IMUAccel.accelZ * IMUAccel.accelZ);
  const float accelDeviation = fabsf(accelMagG - 1.0f);
  const float gyroMagDps = sqrtf(gx * gx + gy * gy + gz * gz);
  const bool isStill = (accelDeviation < ACCEL_STILL_TOL_G) &&
                       (gyroMagDps < GYRO_STILL_TOL_DPS);
  const float targetBeta = isStill ? MADGWICK_BETA_MAX : MADGWICK_BETA_MIN;
  currentBeta = BETA_SMOOTH_ALPHA * targetBeta +
                (1.0f - BETA_SMOOTH_ALPHA) * currentBeta;
  filter.beta = currentBeta;

  filter.updateIMU(gx, gy, gz, IMUAccel.accelX, IMUAccel.accelY, IMUAccel.accelZ);

  Madgwick::Quat qIMU = filter.getQuaternion();
  if (!quatFilterInit) {
    quatFiltered = {qIMU.w, qIMU.x, qIMU.y, qIMU.z};
    quatFilterInit = true;
  } else {
    quatFiltered = nlerpQuat(quatFiltered, qIMU.w, qIMU.x, qIMU.y, qIMU.z,
                             QUAT_SMOOTH_ALPHA);
  }

  // ── Transformación de ejes a SteamVR (igual que PixelVR Controllers) ──────
  hmddata.qw = quatFiltered.w;
  hmddata.qx = quatFiltered.y;
  hmddata.qy = quatFiltered.z;
  hmddata.qz = quatFiltered.x;


  digitalWrite(led_r, HIGH);

  if (!espNowPaused && !espNowSendPending) {
    espNowSendPending = true;
    esp_err_t result = esp_now_send(mac, (uint8_t*)&hmddata, sizeof(hmddata));
    if (result != ESP_OK) {
      espNowSendPending = false;
      Serial.printf("Error: Envio fallido: %d\n", result);
    }
  }

  delay(1);
}
