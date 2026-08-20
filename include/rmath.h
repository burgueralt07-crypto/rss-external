#pragma once
#include <cmath>

// --------------------------------------------------------------------------
// Tipos matemáticos mínimos para o ESP
// --------------------------------------------------------------------------

struct Vector2 {
    float x = 0.f, y = 0.f;
};

struct Vector3 {
    float x = 0.f, y = 0.f, z = 0.f;

    Vector3 operator-(const Vector3& o) const { return { x - o.x, y - o.y, z - o.z }; }
    Vector3 operator+(const Vector3& o) const { return { x + o.x, y + o.y, z + o.z }; }
    Vector3 operator*(float s)          const { return { x * s,   y * s,   z * s   }; }

    float Length() const { return std::sqrtf(x*x + y*y + z*z); }

    float Dot(const Vector3& o) const { return x*o.x + y*o.y + z*o.z; }
};

// Matriz 4x4 em row-major (como o Roblox armazena ViewMatrix)
struct Matrix4x4 {
    float m[4][4] = {};
};

// --------------------------------------------------------------------------
// WorldToScreen
//
// Transforma uma posição 3D (mundo) em coordenadas 2D de tela.
// Retorna false se o ponto está atrás da câmera.
//
// viewMatrix : Camera::ViewMatrix lida da memória (4x4 row-major)
// worldPos   : posição 3D do alvo
// screenSize : tamanho da viewport (Camera::ViewportSize)
// outScreen  : coordenada 2D resultante
// --------------------------------------------------------------------------
inline bool WorldToScreen(const Matrix4x4& viewMatrix,
                           const Vector3&   worldPos,
                           const Vector2&   screenSize,
                           Vector2&         outScreen)
{
    // Multiplica worldPos pelo ViewProjection
    // O Roblox usa uma view matrix que já contém a projeção perspectiva
    const auto& m = viewMatrix.m;

    float w = m[3][0] * worldPos.x
            + m[3][1] * worldPos.y
            + m[3][2] * worldPos.z
            + m[3][3];

    // Ponto atrás da câmera
    if (w < 0.001f)
        return false;

    float x = m[0][0] * worldPos.x
            + m[0][1] * worldPos.y
            + m[0][2] * worldPos.z
            + m[0][3];

    float y = m[1][0] * worldPos.x
            + m[1][1] * worldPos.y
            + m[1][2] * worldPos.z
            + m[1][3];

    // NDC → pixel
    outScreen.x = (screenSize.x * 0.5f) + (x / w) * (screenSize.x * 0.5f);
    outScreen.y = (screenSize.y * 0.5f) - (y / w) * (screenSize.y * 0.5f);

    return true;
}
