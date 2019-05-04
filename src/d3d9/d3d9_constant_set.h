#pragma once

#include "d3d9_caps.h"

#include "../util/util_math.h"

#include <cstdint>

namespace dxvk {

  class Direct3DCommonBuffer9;

  enum class D3D9ConstantType {
    Float,
    Int,
    Bool
  };

  // We make an assumption later based on the packing of this struct for copying.
#pragma pack(push, 1)
  struct D3D9ShaderConstants {
    using vec4  = std::array<float, 4>;
    using ivec4 = std::array<int,   4>;

    union {
      std::array<vec4,  caps::MaxFloatConstants> fConsts;
      std::array<ivec4, caps::MaxOtherConstants> iConsts;
      uint32_t boolBitfield = 0;
    } hardware;
  };
#pragma pack(pop)

  struct D3D9ConstantSets {
    constexpr static uint32_t SetSize    = sizeof(D3D9ShaderConstants);
    Rc<DxvkBuffer> buffer;
    bool           dirty = true;
  };

}