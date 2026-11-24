#pragma once
#include <cstdint>
enum class Block_Types : uint16_t { Air=0,Stone=5,Copper_Wire=156,Lever=157,Lever_On=158,Button=159,Button_Pressed=160,Signal_Clamp=161,Signal_Clamp_Off=162,Repeater=163,Repeater_On=164,Observer=511,Piston=512,Sticky_Piston=513,Piston_Head=514,Copper_Lamp=515,Copper_Lamp_On=516, Dynamite=8,Door=999 };
enum class Block_Shape : uint8_t {None=0,Slab=1,Stairs=2,Plate=3,Button=4,Wire=5,Fence=6,Wall=7,FenceGate=8,Door=9,Trapdoor=10,PressurePlate=11,Pane=12,Ladder=13,Carpet=14,SnowLayer=15,Sign=16,HangingSign=17,Rail=18,Chain=19,Lantern=20,Cauldron=21,Hopper=22,Bed=23,Candle=24,Rod=25,FlowerPot=26,Bell=27,Grindstone=28,Lectern=29,TorchPost=30,PistonHead=31,LeverPost=32,Count=33};
enum class Block_State : uint8_t { Solid=0,Liquid=1,Gas=2 };
struct Block_Properties {Block_Shape shape=Block_Shape::None; bool is_solid=false; Block_State state=Block_State::Solid; bool unbreakable()const {return false;}};
inline const Block_Properties& get_block_props(Block_Types t){static Block_Properties air{},solid{Block_Shape::None,true},wire{Block_Shape::Wire,false},door{Block_Shape::Door,true}; if(t==Block_Types::Air) return air; if(t==Block_Types::Copper_Wire)return wire; if(t==Block_Types::Door)return door; return solid;}
namespace Block_Meta {constexpr uint8_t active_bit=8,direction_mask=3,facing_north=0,facing_east=1,facing_south=2,facing_west=3;}
