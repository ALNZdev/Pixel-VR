#include <Arduino.h>
#include "EEPROM.h"
#include "WiFi.h"
#include "esp_now.h"
#include "Wire.h"
#include "FastIMU.h"

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
#define SCL 27
#define SDA 26
#define led_r   33
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

// ─── Filtro Mahony 6D autocontenido ──────────────────────────────────────────
// Fusión giroscopio + acelerómetro, sin dependencia AHRS externa.
// La corrección del acelerómetro pierde peso durante giros rápidos o cuando
// su magnitud se aleja de 1 g. La integral estima sesgo cuando el HMD está quieto.

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

struct Mahony6D {
  float q0 = 1.0f, q1 = 0.0f, q2 = 0.0f, q3 = 0.0f; // w, x, y, z
  float integralFBx = 0.0f, integralFBy = 0.0f, integralFBz = 0.0f;
  float accelLPFx = 0.0f, accelLPFy = 0.0f, accelLPFz = 1.0f;
  bool accelLPFInitialized = false;

  void reset() {
    q0 = 1.0f; q1 = q2 = q3 = 0.0f;
    integralFBx = integralFBy = integralFBz = 0.0f;
    accelLPFx = accelLPFy = 0.0f; accelLPFz = 1.0f;
    accelLPFInitialized = false;
  }

  void update(float gxDps, float gyDps, float gzDps,
              float axG, float ayG, float azG, float dt) {
    const float degToRad = 0.01745329252f;
    float gx = gxDps * degToRad;
    float gy = gyDps * degToRad;
    float gz = gzDps * degToRad;

    const float accelMag = sqrtf(axG * axG + ayG * ayG + azG * azG);
    if (accelMag > 0.0001f) {
      const float ax = axG / accelMag;
      const float ay = ayG / accelMag;
      const float az = azG / accelMag;

      // LPF solo en la referencia de gravedad; el giro sigue integrado en crudo.
      const float alpha = dt / (0.06f + dt);
      if (!accelLPFInitialized) {
        accelLPFx = ax; accelLPFy = ay; accelLPFz = az;
        accelLPFInitialized = true;
      } else {
        accelLPFx += alpha * (ax - accelLPFx);
        accelLPFy += alpha * (ay - accelLPFy);
        accelLPFz += alpha * (az - accelLPFz);
      }

      const float lpfNorm = sqrtf(accelLPFx * accelLPFx +
                                  accelLPFy * accelLPFy +
                                  accelLPFz * accelLPFz);
      if (lpfNorm > 0.0001f) {
        const float mx = accelLPFx / lpfNorm;
        const float my = accelLPFy / lpfNorm;
        const float mz = accelLPFz / lpfNorm;

        // Gravedad estimada por la orientación actual (medio vector).
        const float vx = q1 * q3 - q0 * q2;
        const float vy = q0 * q1 + q2 * q3;
        const float vz = q0 * q0 - 0.5f + q3 * q3;

        const float ex = my * vz - mz * vy;
        const float ey = mz * vx - mx * vz;
        const float ez = mx * vy - my * vx;

        const float gyroMagDps = sqrtf(gxDps * gxDps +
                                      gyDps * gyDps +
                                      gzDps * gzDps);
        const float magnitudeError = fabsf(accelMag - 1.0f);
        float accelWeight = 1.0f - magnitudeError / 0.25f;
        if (accelWeight < 0.0f) accelWeight = 0.0f;
        if (accelWeight > 1.0f) accelWeight = 1.0f;
        const float turnWeight = 1.0f / (1.0f +
                                  (gyroMagDps / 90.0f) * (gyroMagDps / 90.0f));
        accelWeight *= turnWeight;

        // Integral feedback se actualiza solo en reposo, para no confundir
        // aceleración lineal con sesgo del giroscopio.
        if (gyroMagDps < 4.0f && magnitudeError < 0.12f && accelWeight > 0.5f) {
          const float twoKi = 0.10f;
          integralFBx += twoKi * ex * dt;
          integralFBy += twoKi * ey * dt;
          integralFBz += twoKi * ez * dt;
        }

        const float twoKp = 2.0f;
        gx += integralFBx + twoKp * accelWeight * ex;
        gy += integralFBy + twoKp * accelWeight * ey;
        gz += integralFBz + twoKp * accelWeight * ez;
      }
    }

    // Integración del cuaternión con el tiempo real entre muestras.
    const float halfDt = 0.5f * dt;
    const float q0 = this->q0, q1 = this->q1;
    const float q2 = this->q2, q3 = this->q3;
    this->q0 += (-q1 * gx - q2 * gy - q3 * gz) * halfDt;
    this->q1 += ( q0 * gx + q2 * gz - q3 * gy) * halfDt;
    this->q2 += ( q0 * gy - q1 * gz + q3 * gx) * halfDt;
    this->q3 += ( q0 * gz + q1 * gy - q2 * gx) * halfDt;

    const float qNorm = sqrtf(this->q0 * this->q0 + this->q1 * this->q1 +
                              this->q2 * this->q2 + this->q3 * this->q3);
    if (qNorm > 0.0001f) {
      this->q0 /= qNorm; this->q1 /= qNorm;
      this->q2 /= qNorm; this->q3 /= qNorm;
    } else {
      reset();
    }
  }
} orientationFilter;

float        errorCode   = 0.0f;

// Pausa el envío ESP-NOW mientras dura un comando serial bloqueante o que no
// tiene sentido transmitir (calibrate, recenter), para no seguir imprimiendo
// "Enviado, OK." u otros logs de envío en medio de la operación.
bool espNowPaused = false;

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) // Callback ESP-NOW invocado al confirmarse el envío de un paquete
{
  if (status == ESP_NOW_SEND_SUCCESS) {
    Serial.println("Log: Enviado, OK.");
  }
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
          orientationFilter.reset();
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
  const unsigned long elapsedUS = ahora - ultimoTiempoUS;
  ultimoTiempoUS = ahora;


  // ── Leer sensores ──────────────────────────────────────────────────────────
  IMU.update();
  IMU.getAccel(&IMUAccel);
  IMU.getGyro(&IMUGyro);


  const float dt = constrain(elapsedUS * 1.0e-6f,
                             0.001f, 0.020f);
  orientationFilter.update(IMUGyro.gyroX, IMUGyro.gyroY, IMUGyro.gyroZ,
                           IMUAccel.accelX, IMUAccel.accelY, IMUAccel.accelZ,
                           dt);

  // ── Transformación de ejes a SteamVR (sin cambios respecto al original) ───
  hmddata.qw = orientationFilter.q0;
  hmddata.qx = orientationFilter.q2;
  hmddata.qy = orientationFilter.q3;
  hmddata.qz = orientationFilter.q1;


  digitalWrite(led_r, HIGH);

  if (!espNowPaused) {
    esp_err_t result = esp_now_send(mac, (uint8_t*)&hmddata, sizeof(hmddata));
    if (result != ESP_OK) {
      Serial.printf("Error: Envio fallido: %d\n", result);
    }
  }

  delay(1);
}
