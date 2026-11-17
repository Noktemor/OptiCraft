#pragma once
namespace glm {
struct ivec3 { int x,y,z; constexpr ivec3(int a=0,int b=0,int c=0):x(a),y(b),z(c){};
 constexpr ivec3 operator+(ivec3 b)const{return {x+b.x,y+b.y,z+b.z};}
 constexpr ivec3 operator-(ivec3 b)const{return {x-b.x,y-b.y,z-b.z};}
 constexpr ivec3 operator-()const{return {-x,-y,-z};}
 ivec3& operator+=(ivec3 b){x+=b.x;y+=b.y;z+=b.z;return *this;}
 constexpr ivec3 operator*(int a)const{return {x*a,y*a,z*a};}
 constexpr bool operator==(ivec3 b)const{return x==b.x&&y==b.y&&z==b.z;}
 constexpr bool operator!=(ivec3 b)const{return !(*this==b);}
};
struct vec3{float x,y,z; constexpr vec3(float a=0):x(a),y(a),z(a){}; constexpr vec3(float a,float b,float c):x(a),y(b),z(c){}};
}
