using System;

namespace PixelVR
{
    /// <summary>
    /// Procesa los datos que llegan desde main_hmd.cpp para mostrarlos en la UI.
    ///
    /// IMPORTANTE:
    /// - El microcontrolador YA ejecuta Madgwick para HMD y mandos.
    /// - El cuaternión que se envía al driver SteamVR va sin procesar (passthrough).
    /// - Las funciones de esta clase solo se usan para los valores pitch/yaw/roll
    ///   que se muestran en pantalla, no afectan lo que recibe el driver.
    /// </summary>
    public static class SensorDataProcessor
    {
        public struct Quaternion
        {
            public float W, X, Y, Z;

            public override readonly string ToString() => $"Q({W:F4}, {X:F4}, {Y:F4}, {Z:F4})";
        }

        public struct EulerAngles
        {
            public float pitch;
            public float yaw;
            public float roll;
        }

        /// <summary>
        /// Normaliza un cuaternión recibido desde firmware.
        /// Si el valor es inválido, devuelve identidad.
        /// </summary>
        public static Quaternion NormalizeQuaternion(float w, float x, float y, float z)
        {
            float norm = (float)Math.Sqrt(w * w + x * x + y * y + z * z);
            if (norm <= 1e-6f)
            {
                return new Quaternion { W = 1f, X = 0f, Y = 0f, Z = 0f };
            }

            return new Quaternion
            {
                W = w / norm,
                X = x / norm,
                Y = y / norm,
                Z = z / norm
            };
        }

        public static EulerAngles QuaternionToEuler(float w, float x, float y, float z)
        {
            float norm = (float)Math.Sqrt(w * w + x * x + y * y + z * z);
            if (norm > 1e-6f)
            {
                w /= norm;
                x /= norm;
                y /= norm;
                z /= norm;
            }
            else
            {
                w = 1f;
                x = y = z = 0f;
            }

            float sinp = 2.0f * (w * x - y * z);
            float pitch = (float)Math.Asin(Math.Clamp(sinp, -1.0f, 1.0f));

            float siny_cosp = 2.0f * (w * y + x * z);
            float cosy_cosp = 1.0f - 2.0f * (x * x + y * y);
            float yaw = (float)Math.Atan2(siny_cosp, cosy_cosp);

            float sinr_cosp = 2.0f * (w * z + x * y);
            float cosr_cosp = 1.0f - 2.0f * (y * y + z * z);
            float roll = (float)Math.Atan2(sinr_cosp, cosr_cosp);

            return new EulerAngles { pitch = pitch, yaw = yaw, roll = roll };
        }

        // SteamVR define su mundo como X=derecha, Y=arriba, Z=atrás. Bajo esos
        // ejes: rotar en X=PITCH, rotar en Y=YAW, rotar en Z=ROLL. Por eso pitch
        // alimenta el componente X del cuaternión, yaw el Y, roll el Z.
        public static Quaternion EulerToQuaternion(float pitchDeg, float yawDeg, float rollDeg)
        {
            float pitch = pitchDeg * (float)Math.PI / 180.0f;
            float yaw = yawDeg * (float)Math.PI / 180.0f;
            float roll = rollDeg * (float)Math.PI / 180.0f;

            float cp = (float)Math.Cos(pitch / 2.0f);
            float sp = (float)Math.Sin(pitch / 2.0f);
            float cy = (float)Math.Cos(yaw / 2.0f);
            float sy = (float)Math.Sin(yaw / 2.0f);
            float cr = (float)Math.Cos(roll / 2.0f);
            float sr = (float)Math.Sin(roll / 2.0f);

            return NormalizeQuaternion(
                cp * cy * cr + sp * sy * sr,
                sp * cy * cr - cp * sy * sr,
                cp * sy * cr + sp * cy * sr,
                cp * cy * sr - sp * sy * cr);
        }
    }
}