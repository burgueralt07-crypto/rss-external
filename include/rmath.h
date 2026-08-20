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

// Matriz 4x4 flat (16 floats) — mesmo layout do base de referência
struct Matrix4x4 {
    float m[4][4] = {};

    // Acesso flat para compatibilidade
    float& at(int row, int col) { return m[row][col]; }
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
    const auto& m = viewMatrix.m;

    // Mesmo cálculo do RenderEngine::WorldToViewport do base de referência
    float x = (worldPos.x * m[0][0]) + (worldPos.y * m[0][1]) + (worldPos.z * m[0][2]) + m[0][3];
    float y = (worldPos.x * m[1][0]) + (worldPos.y * m[1][1]) + (worldPos.z * m[1][2]) + m[1][3];
    float w = (worldPos.x * m[3][0]) + (worldPos.y * m[3][1]) + (worldPos.z * m[3][2]) + m[3][3];

    if (w < 0.1f) return false;

    outScreen.x = (screenSize.x / 2.f * (x / w)) + (screenSize.x / 2.f);
    outScreen.y = -(screenSize.y / 2.f * (y / w)) + (screenSize.y / 2.f);

    return true;
}
