#include "common_structs.h"

Material::Material()
    : diffWeight(1),
      diffCol{ 1, 1, 1 },
      specWeight(0),
      specCol{ 1, 1, 1 },
      emissiveStrength(0),
      emissiveCol{ 1, 1, 1 }
{}
