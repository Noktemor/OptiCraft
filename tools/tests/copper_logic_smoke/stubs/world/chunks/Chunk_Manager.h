#pragma once
#include "world/blocks/Block_Types.h"
class Chunk_Manager {public: Block_Types get_block_world(int,int,int)const{return Block_Types::Air;}
uint8_t get_block_meta_world(int,int,int)const{return 0;} bool set_block_world(int,int,int,Block_Types,uint8_t=0){return true;}
bool is_chunk_loaded(int,int,int)const{return true;}};
