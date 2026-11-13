# PixelVR Android

Cliente Android para recibir el video estéreo SBS que produce PixelVR Driver. La conexión USB se hace con `adb reverse` sobre TCP; no depende de Wi‑Fi ni envía poses, sensores o entradas de mandos al driver. PixelVR Driver sigue siendo la fuente de tracking para SteamVR.

## Modo inmersivo OpenXR para Quest 2

La actividad se declara como una aplicación OpenXR inmersiva de Quest. El cliente crea un swapchain por ojo y presenta en cada uno la mitad correspondiente del video SBS decodificado por `MediaCodec`. La capa usa el espacio OpenXR `VIEW`, así que el video queda fijo al visor en vez de depender del tracking espacial de Quest; no muestra una ventana Android flotante.

PixelVR Driver sigue siendo la fuente del tracking que usa SteamVR. La app no crea acciones de mandos ni envía poses de Quest a PixelVR. El runtime de Quest puede usar su tracking internamente para presentar/reproyectar la capa OpenXR.

La integración usa el loader Android de Khronos 1.1.65; Meta requiere Horizon OS v62 o posterior para ese loader.

## Compilar

1. Abre esta carpeta `PixelVRAndroid` en Android Studio.
2. Usa el JDK integrado de Android Studio (JDK 17 o posterior).
3. Instala Android SDK Platform 34 y Android SDK Build-Tools 34.0.0.
4. En **SDK Tools**, instala Android NDK (Side by side) y CMake.
5. Sincroniza Gradle y elige **Build → Build APK(s)**.

Android Studio crea `local.properties` para el SDK local; el archivo no se versiona.

## Conectar por USB

1. Conecta Quest 2 por USB y habilita el modo desarrollador y la depuración USB.
2. Autoriza el PC y confirma que `adb devices` muestre el visor como `device`.
3. Crea el túnel con `adb reverse tcp:9944 tcp:9944` (PixelVR Connect también lo prepara al guardar/iniciar el modo Android USB).
4. Instala el APK en Quest 2 e inicia **PixelVR Android** desde la biblioteca de apps VR.
5. Activa el modo **Android Phone (USB)** en PixelVR Connect/PixelVR Driver.
6. El cliente se conecta a `127.0.0.1:9944`; al conectar, el driver fuerza un keyframe inicial.

## Ajustes para empezar

El stream contiene los dos ojos en un solo fotograma SBS, por lo que la resolución configurada es el ancho total. Como punto de partida, usa HEVC, 1920×1080 total, 60 FPS y 15–25 Mbps. Si la imagen se ve con bloques, sube el bitrate en pasos de 5 Mbps; si aparecen saltos o la latencia crece, vuelve a bajar bitrate o resolución antes de cambiar el transporte. 1280×720 a 3 Mbps es un perfil de baja calidad para un visor.

La tubería del driver ya usa una cola tipo mailbox para descartar fotogramas de render antiguos antes de codificarlos, encoder de baja latencia cuando está disponible, TCP_NODELAY y ADB reverse. En Android se habilita `MediaFormat.KEY_LOW_LATENCY` en API 30 o posterior. El receptor conserva el orden de los access units; si el decodificador se atasca, reconecta para que el driver envíe un nuevo keyframe.

## Protocolo

El receptor procesa el encabezado little-endian `PVRF` de 44 bytes, que incluye las dimensiones, códec, FOV configurado en `default.vrsettings` y el payload Annex-B H.264/HEVC. La app usa ese FOV de origen junto con el FOV real de cada ojo que reporta OpenXR para remapear los rayos de visión; las zonas sin cobertura de la imagen de origen quedan negras en lugar de estirarse. Así se conserva la perspectiva y se evitan deformaciones en los bordes. TCP puede dividir cada paquete en lecturas arbitrarias, así que el cliente reconstruye encabezado y payload completos antes de pasarlos al decodificador. El driver descarta imágenes renderizadas antiguas antes del encoder, manteniendo acotada la cola de video.
