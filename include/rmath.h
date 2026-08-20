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

// Matriz 4x4 flat — mesmo layout do base de referência (data[0..15])
struct Matrix4x4 {
    float data[16] = {};
};

// Matriz 3x3 — layout da rotação do Primitive do Roblox (Primitive::Rotation)
// Colunas: [Right | Up | -Look] em coluna maior (column-major)
// right = col[0] = {m[0], m[3], m[6]}
// up    = col[1] = {m[1], m[4], m[7]}
// look  = col[2] = {m[2], m[5], m[8]}  (LookVector = -col[2] no Roblox)
struct Matrix3x3 {
    float m[9] = {}; // row-major conforme dump: m[0..2]=row0, m[3..5]=row1, m[6..8]=row2

    // right  = primeira coluna (X do objeto no mundo)
    Vector3 Right() const { return { m[0], m[3], m[6] }; }
    // up     = segunda coluna (Y do objeto no mundo)
    Vector3 Up()    const { return { m[1], m[4], m[7] }; }
    // look   = -terceira coluna (LookVector do Roblox aponta -Z local)
    Vector3 Look()  const { return { -m[2], -m[5], -m[8] }; }
};

// --------------------------------------------------------------------------
// WorldToScreen — baseado em RenderEngine::WorldToViewport do base
// --------------------------------------------------------------------------
inline bool WorldToScreen(const Matrix4x4& vm,
                           const Vector3&   worldPos,
                           const Vector2&   screenSize,
                           Vector2&         outScreen)
{
    float x = (worldPos.x * vm.data[0]) + (worldPos.y * vm.data[1])
            + (worldPos.z * vm.data[2])  + vm.data[3];

    float y = (worldPos.x * vm.data[4]) + (worldPos.y * vm.data[5])
            + (worldPos.z * vm.data[6])  + vm.data[7];

    float w = (worldPos.x * vm.data[12]) + (worldPos.y * vm.data[13])
            + (worldPos.z * vm.data[14]) + vm.data[15];

    if (w < 0.1f) return false;

    outScreen.x =  (screenSize.x / 2.f) * (x / w) + (screenSize.x / 2.f);
    outScreen.y = -(screenSize.y / 2.f) * (y / w) + (screenSize.y / 2.f);

    return true;
}
