#include "Arduino.h"
#include "EEPROM.h"
#include "WiFi.h"
#include "esp_now.h"
#include "Wire.h"
#include "FastIMU.h"
#include "MadgwickAHRS.h"

uint8_t mac[6] = {0xAC, 0xA7, 0x04, 0x27, 0xAE, 0x90}; // MAC física del receptor ESP-NOW

int lado = 0;   // 1 = izquierdo, 0 = derecho

// USB-Serial
static const uint32_t USB_BAUD_RATE = 115200;

// ─── Identificación de tipo de paquete ESP-NOW ────────────────────────────────
#define PACKET_TYPE_CONTROLLER 0x01
#define PACKET_TYPE_HMD        0x02

typedef struct __attribute__((packed)) ControllerData
{
  uint8_t packetType;
  int   side;
  float qw, qx, qy, qz;
  bool  a, b, system, grip, trigger, stick_click;
  float stick_x, stick_y;
  bool  status;
  float error;
} ControllerData;

ControllerData ctrldata;

// Terminales
// I²C
#define SCL 34
#define SDA 36
// LED RGB
#define LED_R 8
#define LED_B 10
#define LED_G 13
// Botones
#define button_a 39
#define button_b 3
#define button_system 37
#define button_trigger 35
#define button_grip 5
// Joystick
#define button_stick_click 16
#define thumbstick_x 2
#define thumbstick_y 4
// Háptico
#define HAPTIC_PIN 40

// ─── ADC / Joystick ───────────────────────────────────────────────────────────
// El ESP32-S2 usa 13 bits por defecto; se fija a 12 bits para que la
// normalización con ADC_MAX sea coherente.
#define ADC_BITS        12
#define ADC_MAX         4095.0f
#define STICK_DEADZONE  0.10f   // Zona muerta, compensada y normalizada
#define STICK_SAMPLES   64      // Muestras para calibrar el centro al arrancar
float stickCenterX = ADC_MAX / 2.0f;
float stickCenterY = ADC_MAX / 2.0f;
bool stickRawDebug = false;
unsigned long lastStickDebugMs = 0;

// ─── IMU ─────────────────────────────────────────────────────────────────────
#define MPU_ADDRESS  0x68
#define I2C_CLOCK    400000
int currentImuGeometry = 2;
#define GYRO_DEADZONE_DPS 1.5f
MPU6050 IMU;
calData calibIMU = { 0 };
AccelData IMUAccel;
GyroData IMUGyro;

// ─── Suavizado NLERP de cuaternión de salida ──────────────────────────────────
#define QUAT_SMOOTH_ALPHA 0.08f

struct QuatState {
  float w, x, y, z;
} quatFiltered;

bool quatFilterInit = false;

// ─── Madgwick beta adaptativo ─────────────────────────────────────────────────
#define MADGWICK_BETA_MIN     0.02f
#define MADGWICK_BETA_MAX     0.08f
#define ACCEL_STILL_TOL_G     0.15f
#define GYRO_STILL_TOL_DPS    15.0f
#define BETA_SMOOTH_ALPHA     0.10f
float currentBeta = MADGWICK_BETA_MIN;
Madgwick filter;

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

float errorCode = 0.0f;
bool espNowPaused = false;
volatile bool espNowSendPending = false;

void OnDataSent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
  espNowSendPending = false;
  if (status == ESP_NOW_SEND_SUCCESS) {
    ctrldata.status = true;
  } else {
    ctrldata.status = false;
  }
}

static inline float applyDeadzone(float value, float threshold) {
  return (fabsf(value) < threshold) ? 0.0f : value;
}

static inline float wrapAngle180(float angle) {
  angle = fmodf(angle + 180.0f, 360.0f);
  if (angle < 0.0f) angle += 360.0f;
  return angle - 180.0f;
}

static inline float shortestAngleDelta(float target, float current) {
  return wrapAngle180(target - current);
}

// Normaliza el ADC alrededor del centro calibrado, invierte el sentido de
// ambos ejes y aplica una curva progresiva fuerte para reducir la sensibilidad
// cerca del centro. La curva conserva el valor máximo en los extremos.
static inline float stickNorm(int raw, float center) {
  float v = (raw >= center) ? (raw - center) / (ADC_MAX - center)
                            : (raw - center) / center;
  v = constrain(v, -1.0f, 1.0f);
  v = -v;  // El montaje actual de los dos módulos entrega ambos ejes al revés.

  const float magnitude = fabsf(v);
  if (magnitude <= STICK_DEADZONE) return 0.0f;

  // Reescala la zona útil a [0, 1]; gamma 3.0 reduce mucho la ganancia intermedia.
  const float normalized = (magnitude - STICK_DEADZONE) / (1.0f - STICK_DEADZONE);
  const float shaped = powf(normalized, 3.0f);
  return copysignf(shaped, v);
}

// Promedia varias lecturas para hallar el centro del stick (suelto, en reposo)
static void calibrateStickCenter() {
  long sx = 0, sy = 0;
  for (int i = 0; i < STICK_SAMPLES; i++) {
    sx += analogRead(thumbstick_x);
    sy += analogRead(thumbstick_y);
    delay(2);
  }
  stickCenterX = sx / (float)STICK_SAMPLES;
  stickCenterY = sy / (float)STICK_SAMPLES;
  Serial.printf("Log: Centro stick X=%.0f Y=%.0f\n", stickCenterX, stickCenterY);
}

// ─── NLERP de cuaterniones ────────────────────────────────────────────────────
static QuatState nlerpQuat(const QuatState& prev,
                            float nw, float nx, float ny, float nz,
                            float alpha)
{
  float dot = prev.w*nw + prev.x*nx + prev.y*ny + prev.z*nz;
  if (dot < 0.f) { nw = -nw; nx = -nx; ny = -ny; nz = -nz; }

  QuatState r;
  r.w = prev.w + alpha * (nw - prev.w);
  r.x = prev.x + alpha * (nx - prev.x);
  r.y = prev.y + alpha * (ny - prev.y);
  r.z = prev.z + alpha * (nz - prev.z);

  float inv = 1.f / sqrtf(r.w*r.w + r.x*r.x + r.y*r.y + r.z*r.z);
  r.w *= inv; r.x *= inv; r.y *= inv; r.z *= inv;
  return r;
}

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

  Serial.println(F("Log: MANTENER EL CONTROL COMPLETAMENTE QUIETO Y NIVELADO..."));
  delay(3000);
  IMU.calibrateAccelGyro(&calibIMU);
  IMU.init(calibIMU, MPU_ADDRESS);
  Serial.println(F("Log: MPU6050 Calibrado."));

  saveCalibrationToEEPROM();
}

// Controla el brillo del LED de identidad sin cambiar su color.
static void setSideLedBrightness(uint8_t brightness) {
  analogWrite(LED_R, 0);
  analogWrite(LED_G, lado == 1 ? brightness : 0);
  analogWrite(LED_B, lado == 0 ? brightness : 0);
}

// Fundido con curva smoothstep para entradas y salidas suaves.
static void fadeSideLed(uint8_t from, uint8_t to, uint16_t durationMs) {
  const uint8_t steps = 48;
  for (uint8_t step = 0; step <= steps; ++step) {
    const float t = step / (float)steps;
    const float eased = t * t * (3.0f - 2.0f * t);
    const uint8_t brightness = (uint8_t)(from + (to - from) * eased);
    setSideLedBrightness(brightness);
    delay(durationMs / steps);
  }
}

// Doble pulso: dos subidas suaves; la segunda es más intensa.
static void startupLedAnimation() {
  analogWriteResolution(LED_R, 8);
  analogWriteResolution(LED_G, 8);
  analogWriteResolution(LED_B, 8);
  setSideLedBrightness(0);
  fadeSideLed(0, 125, 350);
  fadeSideLed(125, 18, 420);
  fadeSideLed(18, 205, 450);
  fadeSideLed(205, 28, 420);
  fadeSideLed(28, 255, 650);
}

void setup()
{
  Serial.begin(USB_BAUD_RATE);

  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  startupLedAnimation();

  while (!Serial && millis() < 3000);

  if (!EEPROM.begin(EEPROM_SIZE)) {
    Serial.println(F("Log: Error al inicializar EEPROM."));
  }

  pinMode(HAPTIC_PIN, OUTPUT);
  digitalWrite(HAPTIC_PIN, LOW);                      // Motor háptico apagado

  pinMode(button_a,           INPUT_PULLUP);
  pinMode(button_b,           INPUT_PULLUP);
  pinMode(button_system,      INPUT_PULLUP);
  pinMode(button_grip,        INPUT_PULLUP);
  pinMode(button_trigger,     INPUT_PULLUP);
  pinMode(button_stick_click, INPUT_PULLUP);

  // ADC del joystick: 12 bits y rango completo (~0-3.1 V)
  analogReadResolution(ADC_BITS);
  analogSetAttenuation(ADC_11db);
  pinMode(thumbstick_x, INPUT);
  pinMode(thumbstick_y, INPUT);
  calibrateStickCenter();                             // Mantener el stick suelto al encender

  // WiFi
  WiFi.mode(WIFI_STA);
  Serial.printf("Log: MAC receptor: %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.printf("Log: Lado: %s\n", lado == 1 ? "Izquierdo" : "Derecho");

  // ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error: ESP-NOW init fallido");
    errorCode = 4;
    digitalWrite(LED_R, HIGH);
    return;
  }
  esp_now_register_send_cb(OnDataSent);

  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  peerInfo.ifidx   = WIFI_IF_STA;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Error: No se pudo añadir peer ESP-NOW");
    digitalWrite(LED_R, HIGH); digitalWrite(LED_B, HIGH);
    return;
  }

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

  filter.begin(UPDATE_RATE_HZ);
  filter.beta = currentBeta;

  // Inicializar paquete
  memset(&ctrldata, 0, sizeof(ctrldata));
  ctrldata.packetType = PACKET_TYPE_CONTROLLER;
  ctrldata.qw = 1.0f;
  ctrldata.qx = 0.0f;
  ctrldata.qy = 0.0f;
  ctrldata.qz = 0.0f;
  ctrldata.side = lado;
  ctrldata.error = 0.0f;

  quatFiltered.w = 1.f; quatFiltered.x = 0.f;
  quatFiltered.y = 0.f; quatFiltered.z = 0.f;
  quatFilterInit = false;
}

void loop()
{
  static String serialCommand = "";
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      serialCommand.trim();
      serialCommand.toLowerCase();
      if (serialCommand == "calibrate") {
        espNowPaused = true;
        calibrateSensors();
        printCalibration();
        espNowPaused = false;
      } else if (serialCommand == "show") {
        printCalibration();
      } else if (serialCommand == "recenter") {
        espNowPaused = true;
        yawOffset = filter.getYaw();
        Serial.println(F("Log: Recentrado en YAW."));
        espNowPaused = false;
      } else if (serialCommand == "stick") {
        calibrateStickCenter();            // Recalibra el centro del stick (soltarlo antes)
      } else if (serialCommand == "stickraw") {
        stickRawDebug = !stickRawDebug;
        Serial.printf("Log: Diagnostico raw del stick %s\n", stickRawDebug ? "activado" : "desactivado");
      } else if (serialCommand.startsWith("geo ")) {
        int idx = serialCommand.substring(4).toInt();
        if (idx >= 0 && idx <= 7) {
          currentImuGeometry = idx;
          IMU.setIMUGeometry(currentImuGeometry);
          yawOffset = pitchOffset = rollOffset = 0.0f;
          quatFilterInit = false;
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

  if (errorCode != 0.0f) {
    ctrldata.error = errorCode;
  }

  if (lado == 1) { digitalWrite(LED_G, HIGH); }       // Verde fijo = izquierdo
  else           { digitalWrite(LED_B, HIGH); }       // Azul fijo = derecho

  // ── Control de tiempo ──────────────────────────────────────────────────────
  unsigned long ahora = micros();
  if (ahora - ultimoTiempoUS < INTERVALO_US) return;
  ultimoTiempoUS = ahora;

  // ── Leer sensores ──────────────────────────────────────────────────────────
  IMU.update();
  IMU.getAccel(&IMUAccel);
  IMU.getGyro(&IMUGyro);

  float gx = applyDeadzone(IMUGyro.gyroX, GYRO_DEADZONE_DPS);
  float gy = applyDeadzone(IMUGyro.gyroY, GYRO_DEADZONE_DPS);
  float gz = applyDeadzone(IMUGyro.gyroZ, GYRO_DEADZONE_DPS);

  // ── Beta adaptativo ────────────────────────────────────────────────────────
  float accelMagG = sqrtf(IMUAccel.accelX * IMUAccel.accelX +
                          IMUAccel.accelY * IMUAccel.accelY +
                          IMUAccel.accelZ * IMUAccel.accelZ);
  float accelDeviation = fabsf(accelMagG - 1.0f);
  float gyroMagDps = sqrtf(gx * gx + gy * gy + gz * gz);

  bool isStill = (accelDeviation < ACCEL_STILL_TOL_G) && (gyroMagDps < GYRO_STILL_TOL_DPS);
  float targetBeta = isStill ? MADGWICK_BETA_MAX : MADGWICK_BETA_MIN;
  currentBeta = BETA_SMOOTH_ALPHA * targetBeta + (1.0f - BETA_SMOOTH_ALPHA) * currentBeta;
  filter.beta = currentBeta;

  // ── Actualizar filtro ──────────────────────────────────────────────────────
  filter.updateIMU(gx, gy, gz, IMUAccel.accelX, IMUAccel.accelY, IMUAccel.accelZ);

  // ── Cuaternión + NLERP ─────────────────────────────────────────────────────
  Madgwick::Quat qIMU = filter.getQuaternion();

  if (!quatFilterInit) {
    quatFiltered.w = qIMU.w; quatFiltered.x = qIMU.x;
    quatFiltered.y = qIMU.y; quatFiltered.z = qIMU.z;
    quatFilterInit = true;
  } else {
    quatFiltered = nlerpQuat(quatFiltered,
                             qIMU.w, qIMU.x, qIMU.y, qIMU.z,
                             QUAT_SMOOTH_ALPHA);
  }

  // ── Transformación de ejes a SteamVR ───────────────────────────────────────
  ctrldata.qw = quatFiltered.w;
  ctrldata.qx = quatFiltered.y;
  ctrldata.qy = quatFiltered.z;
  ctrldata.qz = quatFiltered.x;

  ctrldata.a           = !digitalRead(button_a);
  ctrldata.b           = !digitalRead(button_b);
  ctrldata.system      = !digitalRead(button_system);
  ctrldata.grip        = !digitalRead(button_grip);
  ctrldata.trigger     = !digitalRead(button_trigger);
  ctrldata.stick_click = !digitalRead(button_stick_click);

  const int stickRawX = analogRead(thumbstick_x);
  const int stickRawY = analogRead(thumbstick_y);
  ctrldata.stick_x = stickNorm(stickRawX, stickCenterX);
  ctrldata.stick_y = stickNorm(stickRawY, stickCenterY);

  // Diagnostico opcional por USB, limitado a 10 Hz para no afectar el muestreo.
  const unsigned long nowMs = millis();
  if (stickRawDebug && nowMs - lastStickDebugMs >= 100) {
    lastStickDebugMs = nowMs;
    Serial.printf("Stick raw X=%d Y=%d | salida X=%.3f Y=%.3f | centro X=%.0f Y=%.0f\n",
                  stickRawX, stickRawY, ctrldata.stick_x, ctrldata.stick_y,
                  stickCenterX, stickCenterY);
  }

  if (!espNowPaused && !espNowSendPending) {
    espNowSendPending = true;
    esp_err_t result = esp_now_send(mac, (uint8_t*)&ctrldata, sizeof(ctrldata));
    if (result != ESP_OK) {
      espNowSendPending = false;
      Serial.printf("Error: Envio fallido: %d\n", result);
      ctrldata.status = false;
    }
  }

  delay(1);
}
