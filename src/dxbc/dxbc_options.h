#pragma once

#include "../dxvk/dxvk_device.h"

namespace dxvk {

  struct D3D11Options;
  
  struct DxbcOptions {
    DxbcOptions();
    DxbcOptions(const Rc<DxvkDevice>& device, const D3D11Options& options);

    // Clamp oDepth in fragment shaders if the depth
    // clip device feature is not supported
    bool useDepthClipWorkaround = false;

    /// Use the ShaderImageReadWithoutFormat capability.
    bool useStorageImageReadWithoutFormat = false;

    /// Use subgroup operations to reduce the number of
    /// atomic operations for append/consume buffers.
    bool useSubgroupOpsForAtomicCounters = false;

    /// Use a SPIR-V extension to implement D3D-style discards
    bool useDemoteToHelperInvocation = false;

    /// Use subgroup operations to discard fragment
    /// shader invocations if derivatives remain valid.
    bool useSubgroupOpsForEarlyDiscard = false;

    /// Use SDiv instead of SHR to converte byte offsets to
    /// dword offsets. Fixes RE2 and DMC5 on Nvidia drivers.
    bool useSdivForBufferIndex = false;

    /// Enables sm4-compliant division-by-zero behaviour
    bool strictDivision = false;

    // Enables range checking for constant buffers.
    // Fixes issues in some games, breaks others.
    bool constantBufferRangeCheck = false;

    /// Clear thread-group shared memory to zero
    bool zeroInitWorkgroupMemory = false;

    /// Minimum storage buffer alignment
    VkDeviceSize minSsboAlignment = 0;
  };
  
}