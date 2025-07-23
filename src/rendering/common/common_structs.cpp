#include "common_structs.h"

Material::Material()
    : diffuseWeight(1),
      diffuseColor{ 1, 1, 1 },
      diffuseTextureId(TEXTURE_ID_INVALID),
      emissiveStrength(0),
      emissiveColor{ 1, 1, 1 }
{}
