#include "common_structs.h"

Material::Material()
    : hasDiffuse(1),
      hasSpecularReflection(0),
      baseColor{ 1, 1, 1 },
      baseColorTextureId(TEXTURE_ID_INVALID),
      specularColor{1, 1, 1},
      ior(1.5f),
      emissiveStrength(0),
      emissiveColor{ 1, 1, 1 }
{}
