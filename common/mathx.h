// Petites fonctions de maths (vecteurs, quaternions, matrices colonne-major) partagees.
#pragma once

#include <cmath>

namespace ovm {

constexpr float kPi = 3.14159265358979f;

struct V3 {
    float x = 0, y = 0, z = 0;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float length(V3 a) { return std::sqrt(dot(a, a)); }

struct Q {
    float x = 0, y = 0, z = 0, w = 1;
};

inline Q qmul(Q a, Q b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Q qconj(Q q) { return {-q.x, -q.y, -q.z, q.w}; }
inline Q qaxis(char axis, float angle) {
    float s = std::sin(angle * 0.5f), c = std::cos(angle * 0.5f);
    return axis == 'x' ? Q{s, 0, 0, c} : axis == 'y' ? Q{0, s, 0, c} : Q{0, 0, s, c};
}
inline V3 qrot(Q q, V3 v) {
    Q p = qmul(qmul(q, Q{v.x, v.y, v.z, 0}), qconj(q));
    return {p.x, p.y, p.z};
}

// Matrice 4x4 colonne-major (m[col*4+row]), convention colonne : v' = M * v.
struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float& at(int row, int col) { return m[col * 4 + row]; }
    float at(int row, int col) const { return m[col * 4 + row]; }
};

inline Mat4 mmul(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int c = 0; c < 4; c++)
        for (int rw = 0; rw < 4; rw++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.at(rw, k) * b.at(k, c);
            r.at(rw, c) = s;
        }
    return r;
}
inline Mat4 mTranslate(V3 t) {
    Mat4 r;
    r.at(0, 3) = t.x;
    r.at(1, 3) = t.y;
    r.at(2, 3) = t.z;
    return r;
}
inline Mat4 mScale(V3 s) {
    Mat4 r;
    r.at(0, 0) = s.x;
    r.at(1, 1) = s.y;
    r.at(2, 2) = s.z;
    return r;
}
inline Mat4 mRotate(Q q) {
    Mat4 r;
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    r.at(0, 0) = 1 - 2 * (yy + zz); r.at(0, 1) = 2 * (xy - wz);     r.at(0, 2) = 2 * (xz + wy);
    r.at(1, 0) = 2 * (xy + wz);     r.at(1, 1) = 1 - 2 * (xx + zz); r.at(1, 2) = 2 * (yz - wx);
    r.at(2, 0) = 2 * (xz - wy);     r.at(2, 1) = 2 * (yz + wx);     r.at(2, 2) = 1 - 2 * (xx + yy);
    return r;
}
inline Mat4 mFromPose(V3 pos, Q q) { return mmul(mTranslate(pos), mRotate(q)); }
inline Mat4 mInverseRigid(V3 pos, Q q) {
    return mmul(mRotate(qconj(q)), mTranslate(pos * -1.0f));
}
// Projection D3D (profondeur 0..1) depuis les tangentes des demi-angles de champ de vision OpenXR.
inline Mat4 mProjection(float angleLeft, float angleRight, float angleUp, float angleDown,
                        float nearZ, float farZ) {
    float l = std::tan(angleLeft), r = std::tan(angleRight);
    float u = std::tan(angleUp), d = std::tan(angleDown);
    float w = r - l, h = u - d;
    Mat4 p;
    for (float& v : p.m) v = 0;
    p.at(0, 0) = 2 / w;
    p.at(1, 1) = 2 / h;
    p.at(0, 2) = (r + l) / w;
    p.at(1, 2) = (u + d) / h;
    p.at(2, 2) = -farZ / (farZ - nearZ);
    p.at(2, 3) = -farZ * nearZ / (farZ - nearZ);
    p.at(3, 2) = -1;
    return p;
}

}  // namespace ovm
