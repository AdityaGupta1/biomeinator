#include "common_structs.h"

#if !__hlsl

Material::Material()
    : pad0(0), pad1(0), pad2(0), diffWeight(1), diffCol{ 1, 1, 1 }, specWeight(0), specCol{ 1, 1, 1 }
{}

#endif
