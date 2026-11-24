#pragma once
namespace glm {
struct ivec3 {
 int x,y,z;
 constexpr ivec3(int a=0,int b=0,int c=0):x(a),y(b),z(c){}
 constexpr ivec3 operator+(ivec3 b)const{return {x+b.x,y+b.y,z+b.z};}
 constexpr bool operator==(ivec3 b)const{return x==b.x&&y==b.y&&z==b.z;}
};
struct vec3 {
 float x,y,z;
 constexpr vec3(float v=0):x(v),y(v),z(v){}
 constexpr vec3(float a,float b,float c):x(a),y(b),z(c){}
 constexpr vec3 operator-(vec3 other)const{return {x-other.x,y-other.y,z-other.z};}
};
inline constexpr float dot(vec3 a,vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
}
