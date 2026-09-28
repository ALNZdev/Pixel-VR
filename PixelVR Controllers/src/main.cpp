#include "Arduino.h"
#include "EEPROM.h"
#include "WiFi.h"
#include "esp_now.h"
#include "Wire.h"
#include "FastIMU.h"
#include "MadgwickAHRS.h"

uint8_t mac[6] = {0xAC, 0xA7, 0x04, 0x27, 0xAE, 0x90};// Dirección MAC física del ESP32-S3 receptor; destino al que este mando transmitirá por ESP-NOW

int lado = 1;

// USB-Serial
static const uint32_t USB_BAUD_RATE = 115200;

// ─── Identificación de tipo de paquete ESP-NOW ────────────────────────────────
// El receptor ahora recibe paquetes de tres orígenes distintos (control
// izquierdo, control derecho, HMD), así que cada paquete lleva un byte de tipo
// al inicio para que el receptor sepa cómo interpretarlo sin depender solo del
// tamaño. DEBE coincidir con el valor usado en el firmware del HMD y del
// receptor.
#define PACKET_TYPE_CONTROLLER 0x01
#define PACKET_TYPE_HMD        0x02

typedef struct __attribute__((packed)) ControllerData 
{
  uint8_t packetType;   // Siempre PACKET_TYPE_CONTROLLER en este firmware
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
#define LED_G 10
#define LED_B 13
// Botones
#define button_a 39
#define button_b 37
#define button_system 35
#define button_trigger 3
#define button_grip 5
// Joystick
#define button_stick_click 33
#define thumbstick_x 18
#define thumbstick_y 16

// ─── IMU ─────────────────────────────────────────────────────────────────────
#define MPU_ADDRESS  0x68
#define I2C_CLOCK    400000
int currentImuGeometry = 0;   // Mutable para poder probar geometrías sin recompilar (ver comando "geo")
#define GYRO_DEADZONE_DPS 1.5f    // Sube si el giroscopio sigue inyectando ruido en reposo
MPU6050 IMU;
calData calibIMU = { 0 };
AccelData IMUAccel;
GyroData IMUGyro;

// ─── Suavizado NLERP de cuaternión de salida ──────────────────────────────────
// NLERP (Normalized Linear Interpolation) es el equivalente al EMA pero en el
// espacio esférico. Aplicar EMA componente a componente sin normalizar produce
// cuaterniones fuera de la esfera unitaria → rotaciones inválidas → más temblor.
// NLERP interpola linealmente y normaliza, manteniendo el cuaternión válido.
//
// Rango útil de QUAT_SMOOTH_ALPHA:
//   0.10 → muy suave, lag perceptible al mover rápido
//   0.25 → balance recomendado (punto de partida)
//   0.50 → casi sin lag, menos filtrado
#define QUAT_SMOOTH_ALPHA 0.08f   // ~2.5 Hz de corte a 200 Hz — subir si hay lag perceptible

struct QuatState {
  float w, x, y, z;
} quatFiltered;

bool quatFilterInit = false;

// ─── Madgwick beta adaptativo ─────────────────────────────────────────────────
// Idea: durante un movimiento brusco, el acelerómetro deja de medir solo
// gravedad (hay aceleración lineal mezclada) y si confiamos mucho en él el
// filtro se "ensucia" y queda desalineado en pitch/roll aunque después te
// quedes quieto. La solución es bajar beta (confiar más en el giroscopio)
// mientras hay movimiento, y subirlo automáticamente apenas detecta que el
// visor está quieto, para que la fusión se auto-corrija contra la gravedad
// sin necesidad de mandar "recenter" a mano. Esto SOLO corrige pitch/roll
// (referenciados a la gravedad); el yaw no tiene referencia absoluta sin
// magnetómetro, así que puede seguir derivando con el tiempo.
#define MADGWICK_BETA_MIN     0.02f   // Movimiento brusco -> confiar en giroscopio
#define MADGWICK_BETA_MAX     0.08f   // Quieto → menos corrección del accel = menos ruido
#define ACCEL_STILL_TOL_G     0.15f   // Tolerancia |accel|-1g para considerar "quieto"
#define GYRO_STILL_TOL_DPS    15.0f   // Velocidad angular máx. para considerar "quieto"
#define BETA_SMOOTH_ALPHA     0.10f   // Suaviza el propio cambio de beta (evita saltos)
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

float        errorCode   = 0.0f;

// Pausa el envío ESP-NOW mientras dura un comando serial bloqueante o que no
// tiene sentido transmitir (calibrate, recenter), para no seguir imprimiendo
// "Enviado, OK." u otros logs de envío en medio de la operación.
bool espNowPaused = false;

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) // Callback ESP-NOW invocado al confirmarse el envío de un paquete
{
  if (status == ESP_NOW_SEND_SUCCESS) {               // Si el receptor confirmó la recepción
    ctrldata.status = true;                           // Marca el estado como exitoso en la próxima transmisión
    Serial.println("Log: Enviado, OK.");
  }
}

static inline float applyDeadzone(float value, float threshold) {
  return (fabsf(value) < threshold) ? 0.0f : value;
}

// Normaliza un ángulo a [-180, 180). Necesario porque getYaw()/getRoll() (y, en
// casos extremos, getPitch()) pueden cruzar el límite ±180°, y comparar o
// promediar ángulos "crudos" ahí produce saltos falsos.
static inline float wrapAngle180(float angle) {
  angle = fmodf(angle + 180.0f, 360.0f);
  if (angle < 0.0f) angle += 360.0f;
  return angle - 180.0f;
}

// Diferencia angular más corta entre "target" y "current" (en [-180, 180]).
// Ej: shortestAngleDelta(-179, 179) = -2, no -358 ni +358.
static inline float shortestAngleDelta(float target, float current) {
  return wrapAngle180(target - current);
}

// ─── NLERP de cuaterniones ────────────────────────────────────────────────────
// Filtra el cuaternión raw del Madgwick para eliminar temblor de alta frecuencia.
// Garantiza shortest-path (dot < 0 → invertir signo del nuevo cuaternión para
// evitar que el filtro interpole "por el camino largo" y genere un salto brusco).
static QuatState nlerpQuat(const QuatState& prev,
                            float nw, float nx, float ny, float nz,
                            float alpha)
{
  // Shortest-path: si el punto más cercano en la esfera es el antipodal, invertir
  float dot = prev.w*nw + prev.x*nx + prev.y*ny + prev.z*nz;
  if (dot < 0.f) { nw = -nw; nx = -nx; ny = -ny; nz = -nz; }

  // Interpolación lineal componente a componente
  QuatState r; r.w = r.x = r.y = r.z = 0.f;
  r.w = prev.w + alpha * (nw - prev.w);
  r.x = prev.x + alpha * (nx - prev.x);
  r.y = prev.y + alpha * (ny - prev.y);
  r.z = prev.z + alpha * (nz - prev.z);

  // Normalización obligatoria para mantener el cuaternión en la esfera unitaria
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

void setup()
{
  Serial.begin(USB_BAUD_RATE);
  while (!Serial && millis() < 3000);

  if (!EEPROM.begin(EEPROM_SIZE)) {
    Serial.println(F("Log: Error al inicializar EEPROM."));
  }

  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);

  pinMode(button_a,          INPUT_PULLUP);
  pinMode(button_b,          INPUT_PULLUP);
  pinMode(button_system,     INPUT_PULLUP);
  pinMode(button_grip,       INPUT_PULLUP);
  pinMode(button_trigger,    INPUT_PULLUP);
  pinMode(button_stick_click,INPUT_PULLUP);

  pinMode(thumbstick_x,  INPUT);                      // Ejes del joystick como entradas analógicas (ADC)
  pinMode(thumbstick_y,  INPUT);

  // WiFi
  WiFi.mode(WIFI_STA);                                // Pone el ESP32 en modo Station; ESP-NOW requiere este modo
  Serial.printf("Log: MAC receptor: %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]); // Imprime la MAC del receptor para verificación
  Serial.printf("Log: Lado: %s\n", lado == 0 ? "Izquierdo" : "Derecho"); // Indica qué firmware/lado se está ejecutando
  
  // ESP-NOW
  if (esp_now_init() != ESP_OK) {                     // Intenta iniciar la pila ESP-NOW
    Serial.println("Error: ESP-NOW init fallido");
    errorCode = 4;                                        // Código de error 4 = fallo de inicialización ESP-NOW
    digitalWrite(LED_R, HIGH);                        // LED rojo encendido como indicador físico de error
    return;                                           // Sale del setup sin continuar
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
    digitalWrite(LED_R, HIGH); digitalWrite(LED_B, HIGH); // LED morado = error de peer
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

  // Inicializar paquete en identidad
  memset(&ctrldata, 0, sizeof(ctrldata));             // Limpia primero (padding/basura), DEBE ir antes de setear los valores reales
  ctrldata.packetType = PACKET_TYPE_CONTROLLER;       // Identifica este paquete como de control ante el receptor
  ctrldata.qw = 1.0f; 
  ctrldata.qx = 0.0f; 
  ctrldata.qy = 0.0f; 
  ctrldata.qz = 0.0f;
  ctrldata.side = lado;                               // Marca el lado del mando en el paquete
  ctrldata.error = 0.0f; 

  // Inicializar el estado del filtro NLERP en identidad
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
        espNowPaused = true;               // Corta el envío mientras dura la calibración (bloqueante, ~3s)
        calibrateSensors();
        printCalibration();
        espNowPaused = false;
      } else if (serialCommand == "show") {
        printCalibration();
      } else if (serialCommand == "recenter") {
        espNowPaused = true;               // Corta el envío mientras se procesa el recentrado
        yawOffset = filter.getYaw();  
        Serial.println(F("Log: Recentrado en YAW."));
        espNowPaused = false;
      } else if (serialCommand.startsWith("geo ")) {
        int idx = serialCommand.substring(4).toInt();
        if (idx >= 0 && idx <= 7) {
          currentImuGeometry = idx;
          IMU.setIMUGeometry(currentImuGeometry);
          yawOffset = pitchOffset = rollOffset = 0.0f;
          // Resetear el filtro NLERP al cambiar geometría para evitar
          // que interpole desde la orientación anterior a la nueva
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

  if (errorCode != 0.0f) {                                // Si hay un error activo
    ctrldata.error = errorCode;                           // Lo propaga al paquete para que el receptor lo conozca
  }

  if (lado == 0) { digitalWrite(LED_B, HIGH); }       // LED azul fijo = mando izquierdo operativo
  else           { digitalWrite(LED_G, HIGH); }        // LED verde fijo = mando derecho operativo


  // ── Control de tiempo ──────────────────────────────────────────────────────
  unsigned long ahora = micros();
  if (ahora - ultimoTiempoUS < INTERVALO_US) return;
  ultimoTiempoUS = ahora;


  // ── Leer sensores ──────────────────────────────────────────────────────────
  IMU.update();
  IMU.getAccel(&IMUAccel);
  IMU.getGyro(&IMUGyro);

  // ── Zona muerta en giroscopio ──────────────────────────────────────────────
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

  // ── Obtener cuaternión y aplicar NLERP ────────────────────────────────────
  // El cuaternión raw del Madgwick tiene ruido de alta frecuencia que a 200 Hz
  // se traduce en temblor visible en SteamVR. El NLERP actúa como un EMA pero
  // en el espacio esférico, manteniendo el cuaternión unitario en todo momento.
  Madgwick::Quat qIMU = filter.getQuaternion();

  if (!quatFilterInit) {
    // Primera muestra: inicializar el estado con el cuaternión actual sin filtrar
    quatFiltered.w = qIMU.w; quatFiltered.x = qIMU.x;
    quatFiltered.y = qIMU.y; quatFiltered.z = qIMU.z;
    quatFilterInit = true;
  } else {
    quatFiltered = nlerpQuat(quatFiltered,
                             qIMU.w, qIMU.x, qIMU.y, qIMU.z,
                             QUAT_SMOOTH_ALPHA);
  }

  // ── Transformación de ejes a SteamVR (sin cambios respecto al original) ───
  ctrldata.qw = quatFiltered.w;
  ctrldata.qx = -quatFiltered.y;
  ctrldata.qy =  quatFiltered.z;
  ctrldata.qz = -quatFiltered.x;  

  ctrldata.a           = !digitalRead(button_a);
  ctrldata.b           = !digitalRead(button_b);
  ctrldata.system      = !digitalRead(button_system);
  ctrldata.grip        = !digitalRead(button_grip);
  ctrldata.trigger     = !digitalRead(button_trigger);
  ctrldata.stick_click = !digitalRead(button_stick_click);

  ctrldata.stick_x      = ((analogRead(thumbstick_x) / 4095.0f) * 2.0f) - 1.0f;
  ctrldata.stick_y      = ((analogRead(thumbstick_y) / 4095.0f) * 2.0f) - 1.0f;

  if (!espNowPaused) {
    esp_err_t result = esp_now_send(mac, (uint8_t*)&ctrldata, sizeof(ctrldata));
    if (result != ESP_OK) {
      Serial.printf("Error: Envio fallido: %d\n", result);
      ctrldata.status = false;
    }
  }
  
  delay(1);
}