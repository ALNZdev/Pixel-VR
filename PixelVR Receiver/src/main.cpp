#include <Arduino.h>
#include "WiFi.h"
#include "esp_now.h"

// ─── Identificación de tipo de paquete ESP-NOW ────────────────────────────────
// Este receptor recibe paquetes de tres orígenes (control izquierdo, control
// derecho, HMD). Cada paquete lleva un byte de tipo al inicio para saber cómo
// interpretarlo, en vez de depender solo del tamaño. DEBE coincidir con el
// valor usado en el firmware de los controles y del HMD.
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

typedef struct __attribute__((packed)) HmdData
{
  uint8_t packetType;
  float qw, qx, qy, qz;
  float error;
} HmdData;

typedef struct __attribute__((packed)) allData   
{
  float hmd_qw, hmd_qx, hmd_qy, hmd_qz;          
  float hmd_error;                               

  float left_qw, left_qx, left_qy, left_qz;
  bool  left_a, left_b, left_system, left_grip, left_trigger, left_stick_click;
  float left_stick_x, left_stick_y;
  bool  left_status;
  float left_error;

  float right_qw, right_qx, right_qy, right_qz;
  bool  right_a, right_b, right_system, right_grip, right_trigger, right_stick_click; 
  float right_stick_x, right_stick_y;          
  bool  right_status;                                           
  float right_error;                           
} allData;

ControllerData ctrldata;                       
HmdData        hmddata;
allData        alldata;

// USB-CDC
static const uint8_t USB_SYNC1  = 0xAA;        
static const uint8_t USB_SYNC2  = 0x55;        
static const uint8_t USB_END1   = 0xFF;        
static const uint8_t USB_END2   = 0xFE;
static const uint32_t USB_BAUD_RATE  = 115200; 

// Terminales (LEDs de estado del receptor)
#define led_r   16                             
#define led_g   17                             
#define led_b   18                             

// ─── Timing ───────────────────────────────────────────────────────────────────
// Este receptor ya no lee un IMU propio: la tasa de sampling la marcan los tres
// emisores (HMD + 2 controles). Este limitador ahora solo regula la tasa de
// salida por USB, para entregarle a SteamVR un framerate estable e
// independiente de cuándo lleguen los paquetes ESP-NOW.
#define UPDATE_RATE_HZ  200.0f
const unsigned long INTERVALO_US = 1000000UL / (unsigned long)UPDATE_RATE_HZ;
unsigned long ultimoTiempoUS = 0;

float errorCode = 0.0f;

// Callback único de recepción ESP-NOW: ESP-NOW solo permite registrar uno, así
// que acá se distingue el origen del paquete por su primer byte (packetType)
// y se valida además el tamaño esperado para ese tipo antes de copiarlo.
void OnDataRecv(const uint8_t *mac, const uint8_t *data, int len)
{
  if (len < 1) return;
  uint8_t type = data[0];

  if (type == PACKET_TYPE_CONTROLLER)
  {
    if (len != sizeof(ControllerData)) return;
    memcpy(&ctrldata, data, sizeof(ctrldata));

    if (ctrldata.side == 1)
    {
      alldata.left_error       = ctrldata.error;
      alldata.left_qw = ctrldata.qw;
      alldata.left_qx = ctrldata.qx;
      alldata.left_qy = ctrldata.qy;
      alldata.left_qz = ctrldata.qz;
      alldata.left_a           = ctrldata.a;
      alldata.left_b           = ctrldata.b;
      alldata.left_system      = ctrldata.system;
      alldata.left_grip        = ctrldata.grip;
      alldata.left_trigger     = ctrldata.trigger;
      alldata.left_stick_click = ctrldata.stick_click;
      alldata.left_stick_x     = ctrldata.stick_x;
      alldata.left_stick_y     = ctrldata.stick_y;
      alldata.left_status      = ctrldata.status;
    }
    else if (ctrldata.side == 0)
    {
      alldata.right_error       = ctrldata.error;
      alldata.right_qw = ctrldata.qw;
      alldata.right_qx = ctrldata.qx;
      alldata.right_qy = ctrldata.qy;
      alldata.right_qz = ctrldata.qz;
      alldata.right_a           = ctrldata.a;
      alldata.right_b           = ctrldata.b;
      alldata.right_system      = ctrldata.system;
      alldata.right_grip        = ctrldata.grip;
      alldata.right_trigger     = ctrldata.trigger;
      alldata.right_stick_click = ctrldata.stick_click;
      alldata.right_stick_x     = ctrldata.stick_x;
      alldata.right_stick_y     = ctrldata.stick_y;
      alldata.right_status      = ctrldata.status;
    }
  }
  else if (type == PACKET_TYPE_HMD)
  {
    if (len != sizeof(HmdData)) return;
    memcpy(&hmddata, data, sizeof(hmddata));

    alldata.hmd_qw    = hmddata.qw;
    alldata.hmd_qx    = hmddata.qx;
    alldata.hmd_qy    = hmddata.qy;
    alldata.hmd_qz    = hmddata.qz;
    alldata.hmd_error = hmddata.error;
  }
  // Tipo desconocido: se ignora silenciosamente (paquete corrupto o de otro origen)
}

static void sendUSBPacket(const allData &p) {
  Serial.write(USB_SYNC1);
  Serial.write(USB_SYNC2);
  Serial.write(reinterpret_cast<const uint8_t*>(&p), sizeof(p));
  Serial.write(USB_END1);
  Serial.write(USB_END2);
}

void setup()
{
  Serial.begin(USB_BAUD_RATE);
  while (!Serial && millis() < 3000);

  pinMode(led_r, OUTPUT); 
  pinMode(led_g, OUTPUT); 
  pinMode(led_b, OUTPUT);

  // Inicializar paquete en identidad
  memset(&alldata, 0, sizeof(alldata));
  alldata.hmd_qw = 1.0f; 
  alldata.hmd_qx = 0.0f; 
  alldata.hmd_qy = 0.0f; 
  alldata.hmd_qz = 0.0f;

  WiFi.mode(WIFI_STA);
  Serial.println("Log: La dirección MAC de este receptor es...");
  Serial.println(WiFi.macAddress());
  Serial.println("Log: Inicializando ESP-NOW para recibir datos de HMD y mandos...");
  if (esp_now_init() != ESP_OK)                       
  {
    Serial.println("Error: ESP-NOW init fallido");
    errorCode = 4;                                    
    digitalWrite(led_r, HIGH);                        
    return;                                           
  }
  esp_now_register_recv_cb(OnDataRecv);
  Serial.println("Log: ESP-NOW listo");

  ultimoTiempoUS = micros();
}

void loop()                                                                
{
  digitalWrite(led_r, HIGH);

  // ── Control de tiempo ──────────────────────────────────────────────────────
  // Regula la tasa de salida por USB a 200 Hz, desacoplada de cuándo llegan
  // los paquetes ESP-NOW individuales de cada emisor.
  unsigned long ahora = micros();
  if (ahora - ultimoTiempoUS < INTERVALO_US) return;
  ultimoTiempoUS = ahora;

  digitalWrite(led_r, HIGH);
  sendUSBPacket(alldata);
  delay(1);
}