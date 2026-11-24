#pragma once
#include "world/blocks/Block_Types.h"
struct Shape_Box {glm::vec3 lo,hi;};
inline int shape_boxes(Block_Shape shape,uint8_t, Shape_Box out[16]){
 if(shape==Block_Shape::None)return 0;
 out[0]={{0,0,0},{1,0.5f,1}};return 1;
}
