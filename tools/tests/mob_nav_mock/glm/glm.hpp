#pragma once
#include <cmath>
namespace glm {
struct vec2 {float x=0,y=0; vec2()=default; vec2(float X,float Y):x(X),y(Y){} };
struct vec3 {float x=0,y=0,z=0;vec3()=default;explicit vec3(float v):x(v),y(v),z(v){}vec3(float X,float Y,float Z):x(X),y(Y),z(Z){} };
inline vec3 operator+(vec3 a,vec3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
inline vec3 operator-(vec3 a,vec3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
inline vec3 operator*(vec3 a,float b){return {a.x*b,a.y*b,a.z*b};}
inline vec3 operator*(float b,vec3 a){return a*b;}
inline vec3 operator/(vec3 a,float b){return {a.x/b,a.y/b,a.z/b};}
inline float length(vec2 v){return std::sqrt(v.x*v.x+v.y*v.y);}
inline float length(vec3 v){return std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z);}
}
