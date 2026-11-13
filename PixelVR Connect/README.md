# PixelVR Connect

PixelVR Connect configura `PixelVR Driver` y conserva el modo **Driver Test** para probar poses sintéticas.

## Seguimiento con el receptor ESP32-S3

1. En la sección USB-CDC, selecciona el puerto del receptor y pulsa **Configurar driver**.
2. Connect guarda el puerto en `driver_pixelvr.comPort` dentro de `default.vrsettings` y deja `driverMode` en `driverNormal`.
3. Inicia SteamVR. El driver abre el puerto a 460800 baudios y lee directamente los paquetes binarios del receptor.

Connect no necesita estar abierto para que el seguimiento funcione. El driver interpreta el encuadre `AA 55` + estructura binaria `allData` + `FF FE` emitido por el firmware del receptor.

## Driver Test

Al iniciar **Driver Test**, Connect cambia temporalmente `driverMode` a `driverTest` y envía las poses sintéticas del HMD y los dos controles por el named pipe `PixelVR_Pipe`. Al detener la prueba, restaura `driverNormal` para el siguiente inicio de SteamVR.

## Meta Quest 2 con ALVR

Selecciona **Meta Quest 2 (ALVR) + tracking PixelVR** para configurar ese modo del driver. En ALVR, activa **Headset → Use external HMD tracking**. El driver envía la pose del HMD de SteamVR a ALVR mediante UDP en `127.0.0.1:9945`; los controladores PixelVR siguen siendo publicados por el driver.
