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
