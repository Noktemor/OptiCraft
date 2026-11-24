#pragma once
namespace Math_Helpers {
inline int floor_div(int a,int b){return a/b-((a%b !=0 && a<0)?1:0);}
}
