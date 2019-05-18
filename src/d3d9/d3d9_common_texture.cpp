#include "d3d9_common_texture.h"

#include "d3d9_caps.h"
#include "d3d9_util.h"

namespace dxvk {

  D3D9CommonTexture::D3D9CommonTexture(
          D3D9DeviceEx*           pDevice,
    const D3D9TextureDesc*        pDesc)
    : m_device( pDevice ), m_desc( *pDesc ) {

    if (m_desc.Format == D3D9Format::NULL_FORMAT)
      return;

    bool unknown      = m_desc.Format == D3D9Format::Unknown;
    bool depthStencil = m_desc.Usage & D3DUSAGE_DEPTHSTENCIL;
    if (unknown) {
      if (!depthStencil)
        m_desc.Format = D3D9Format::X8R8G8B8;
      else
        m_desc.Format = D3D9Format::D32;
    }

    D3D9_VK_FORMAT_MAPPING formatInfo = m_device->LookupFormat(m_desc.Format);

    DxvkImageCreateInfo imageInfo;
    imageInfo.type          = GetImageTypeFromResourceType(m_desc.Type);
    imageInfo.format        = formatInfo.Format;
    imageInfo.flags         = 0;
    imageInfo.sampleCount   = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.extent.width  = m_desc.Width;
    imageInfo.extent.height = m_desc.Height;
    imageInfo.extent.depth  = m_desc.Depth;
    imageInfo.numLayers     = GetLayerCount();
    imageInfo.mipLevels     = m_desc.MipLevels;
    imageInfo.usage         = VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                            | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.stages        = VK_PIPELINE_STAGE_TRANSFER_BIT;
    imageInfo.access        = VK_ACCESS_TRANSFER_READ_BIT
                            | VK_ACCESS_TRANSFER_WRITE_BIT;
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.layout        = VK_IMAGE_LAYOUT_GENERAL;

    DecodeMultiSampleType(m_desc.MultiSample, &imageInfo.sampleCount);

    // The image must be marked as mutable if it can be reinterpreted
    // by a view with a different format. Depth-stencil formats cannot
    // be reinterpreted in Vulkan, so we'll ignore those.
    auto formatProperties = imageFormatInfo(formatInfo.Format);

    bool isMutable     = formatInfo.FormatSrgb != VK_FORMAT_UNDEFINED;
    bool isColorFormat = (formatProperties->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) != 0;

    if (isMutable && isColorFormat) {
      imageInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
      imageInfo.viewFormatCount = 2;
      imageInfo.viewFormats = reinterpret_cast<VkFormat*>(&formatInfo); // Starts with VkFormat, VkFormat
    }

    // Adjust image flags based on the corresponding D3D flags
    if (!m_desc.Offscreen) {
      imageInfo.usage  |= VK_IMAGE_USAGE_SAMPLED_BIT;
      imageInfo.stages |= m_device->GetEnabledShaderStages();
      imageInfo.access |= VK_ACCESS_SHADER_READ_BIT;
    }

    bool possibleRT = m_desc.Usage & D3DUSAGE_RENDERTARGET
                   || m_desc.Usage & D3DUSAGE_AUTOGENMIPMAP;

    if (possibleRT) {
      imageInfo.usage  |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      imageInfo.stages |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      imageInfo.access |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                       |  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }

    if (depthStencil) {
      imageInfo.usage  |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
      imageInfo.stages |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                       |  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
      imageInfo.access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                       | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }

    // Access pattern for meta-resolve operations
    if (imageInfo.sampleCount != VK_SAMPLE_COUNT_1_BIT && isColorFormat) {
      imageInfo.usage  |= VK_IMAGE_USAGE_SAMPLED_BIT;
      imageInfo.stages |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      imageInfo.access |= VK_ACCESS_SHADER_READ_BIT;
    }

    if (m_desc.Type == D3DRTYPE_CUBETEXTURE)
      imageInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    // Some image formats (i.e. the R32G32B32 ones) are
    // only supported with linear tiling on most GPUs
    if (!CheckImageSupport(&imageInfo, VK_IMAGE_TILING_OPTIMAL))
      imageInfo.tiling = VK_IMAGE_TILING_LINEAR;

    // Determine map mode based on our findings
    m_mapMode = DetermineMapMode(&imageInfo);

    // If the image is mapped directly to host memory, we need
    // to enable linear tiling, and DXVK needs to be aware that
    // the image can be accessed by the host.
    if (m_mapMode == D3D9_COMMON_TEXTURE_MAP_MODE_DIRECT) {
      imageInfo.stages |= VK_PIPELINE_STAGE_HOST_BIT;
      imageInfo.tiling  = VK_IMAGE_TILING_LINEAR;
      imageInfo.access |= VK_ACCESS_HOST_WRITE_BIT;

      if (!(m_desc.Usage & D3DUSAGE_WRITEONLY))
        imageInfo.access |= VK_ACCESS_HOST_READ_BIT;
    }

    // We must keep LINEAR images in GENERAL layout, but we
    // can choose a better layout for the image based on how
    // it is going to be used by the game.
    if (imageInfo.tiling == VK_IMAGE_TILING_OPTIMAL)
      imageInfo.layout = OptimizeLayout(imageInfo.usage);

    // For some formats, we need to enable sampled and/or
    // render target capabilities if available, but these
    // should in no way affect the default image layout
    imageInfo.usage |= EnableMetaCopyUsage(imageInfo.format, imageInfo.tiling);
    imageInfo.usage |= EnableMetaPackUsage(imageInfo.format, m_desc.Usage & D3DUSAGE_WRITEONLY);

    // Check if we can actually create the image
    if (!CheckImageSupport(&imageInfo, imageInfo.tiling)) {
      throw DxvkError(str::format(
        "D3D9: Cannot create texture:",
        "\n  Format:  ", m_desc.Format,
        "\n  Extent:  ", m_desc.Width,
                    "x", m_desc.Height,
                    "x", m_desc.Depth,
        "\n  Samples: ", m_desc.MultiSample,
        "\n  Layers:  ", GetLayerCount(),
        "\n  Levels:  ", m_desc.MipLevels,
        "\n  Usage:   ", std::hex, m_desc.Usage));
    }

    // Create the image on a host-visible memory type
    // in case it is going to be mapped directly.
    VkMemoryPropertyFlags memoryProperties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    if (m_mapMode == D3D9_COMMON_TEXTURE_MAP_MODE_DIRECT) {
      memoryProperties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                       | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

      if (!IsWriteOnly())
        memoryProperties |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    }

    m_image = m_device->GetDXVKDevice()->createImage(imageInfo, memoryProperties);

    m_size = CalcMemoryConsumption();
    if (!m_device->ChangeReportedMemory(-m_size))
      throw DxvkError("D3D9: Reporting out of memory from tracking.");

    if (!m_desc.Offscreen)
      RecreateImageViews(0);

    if (m_desc.Usage & D3DUSAGE_DEPTHSTENCIL)
      CreateDepthStencilViews();

    if (possibleRT)
      CreateRenderTargetViews();

    m_mappingBuffers.resize(GetSubresourceCount());
    m_fixupBuffers.resize(GetSubresourceCount());

    DeallocMappingBuffers();
    DeallocFixupBuffers();

    m_shadow = CalcShadowState();
  }

  D3D9CommonTexture::D3D9CommonTexture(
          D3D9DeviceEx*           pDevice,
          Rc<DxvkImage>           Image,
          Rc<DxvkImageView>       ImageView,
          Rc<DxvkImageView>       ImageViewSrgb,
    const D3D9TextureDesc*        pDesc)
    : m_device        ( pDevice )
    , m_desc          ( *pDesc )
    , m_image         ( Image )
    , m_imageView     ( ImageView )
    , m_imageViewSrgb ( ImageViewSrgb ) {
    m_mapMode = m_image->info().tiling == VK_IMAGE_TILING_LINEAR
      ? D3D9_COMMON_TEXTURE_MAP_MODE_DIRECT
      : D3D9_COMMON_TEXTURE_MAP_MODE_BUFFER;

    m_mappingBuffers.resize(GetSubresourceCount());
    m_fixupBuffers.resize(GetSubresourceCount());

    DeallocMappingBuffers();
    DeallocFixupBuffers();

    m_shadow = CalcShadowState();
    m_size   = CalcMemoryConsumption();

    if (!m_device->ChangeReportedMemory(-m_size))
      throw DxvkError("D3D9: Reporting out of memory from tracking.");
  }

  D3D9CommonTexture::~D3D9CommonTexture() {
    m_device->ChangeReportedMemory(m_size);
  }

  Rc<DxvkImage> D3D9CommonTexture::GetResolveImage() {
    if (m_resolveImage != nullptr)
      return m_resolveImage;
    
    DxvkImageCreateInfo imageInfo = m_image->info();
    imageInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    m_resolveImage = m_device->GetDXVKDevice()->createImage(imageInfo, m_image->memFlags());

    return m_resolveImage;
  }

  VkImageSubresource D3D9CommonTexture::GetSubresourceFromIndex(
    VkImageAspectFlags    Aspect,
    UINT                  Subresource) const {
    VkImageSubresource result;
    result.aspectMask = Aspect;
    result.mipLevel = Subresource % m_desc.MipLevels;
    result.arrayLayer = Subresource / m_desc.MipLevels;
    return result;
  }

  bool D3D9CommonTexture::CheckViewCompatibility(DWORD Usage, D3D9Format Format, bool srgb) const {
    const DxvkImageCreateInfo& imageInfo = m_image->info();

    // Check whether the given bind flags are supported
    VkImageUsageFlags usage = GetImageUsageFlags(Usage);

    if ((imageInfo.usage & usage) != usage)
      return false;

    // Check whether the view format is compatible
    D3D9_VK_FORMAT_MAPPING viewFormat = m_device->LookupFormat(Format);
    D3D9_VK_FORMAT_MAPPING baseFormat = m_device->LookupFormat(m_desc.Format);

    if (imageInfo.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) {
      // Check whether the given combination of image
      // view type and view format is actually supported
      VkFormatFeatureFlags features = GetImageFormatFeatures(Usage);

      VkFormat srgbCorrectedViewFormat = PickSRGB(viewFormat.Format, viewFormat.FormatSrgb, srgb);

      if (!CheckFormatFeatureSupport(viewFormat.Format, features))
        return false;

      // Using the image format itself is always legal
      if (viewFormat.Format == baseFormat.Format)
        return true;

      // If there is a list of compatible formats, the
      // view format must be included in that list.
      for (size_t i = 0; i < imageInfo.viewFormatCount; i++) {
        if (imageInfo.viewFormats[i] == srgbCorrectedViewFormat)
          return true;
      }

      // Otherwise, all bit-compatible formats can be used.
      if (imageInfo.viewFormatCount == 0) {
        auto baseFormatInfo = imageFormatInfo(baseFormat.Format);
        auto viewFormatInfo = imageFormatInfo(srgbCorrectedViewFormat);

        return baseFormatInfo->aspectMask  == viewFormatInfo->aspectMask
            && baseFormatInfo->elementSize == viewFormatInfo->elementSize;
      }

      return false;
    }
    else {
      // For non-mutable images, the view format
      // must be identical to the image format.
      return viewFormat.Format == baseFormat.Format;
    }
  }


  HRESULT D3D9CommonTexture::NormalizeTextureProperties(D3D9TextureDesc* pDesc) {
    if (pDesc->Width == 0 || pDesc->Height == 0 || pDesc->Depth == 0)
      return D3DERR_INVALIDCALL;

    if (FAILED(DecodeMultiSampleType(pDesc->MultiSample, nullptr)))
      return D3DERR_INVALIDCALL;

    // Use the maximum possible mip level count if the supplied
    // mip level count is either unspecified (0) or invalid
    const uint32_t maxMipLevelCount = pDesc->MultiSample <= 1
      ? util::computeMipLevelCount({ pDesc->Width, pDesc->Height, pDesc->Depth })
      : 1u;

    if (pDesc->MipLevels == 0 || pDesc->MipLevels > maxMipLevelCount)
      pDesc->MipLevels = maxMipLevelCount;

    return D3D_OK;
  }


  BOOL D3D9CommonTexture::CheckImageSupport(
    const DxvkImageCreateInfo*  pImageInfo,
    VkImageTiling         Tiling) const {
    const Rc<DxvkAdapter> adapter = m_device->GetDXVKDevice()->adapter();

    VkImageFormatProperties formatProps = { };

    VkResult status = adapter->imageFormatProperties(
      pImageInfo->format, pImageInfo->type, Tiling,
      pImageInfo->usage, pImageInfo->flags, formatProps);

    if (status != VK_SUCCESS)
      return FALSE;

    return (pImageInfo->extent.width  <= formatProps.maxExtent.width)
        && (pImageInfo->extent.height <= formatProps.maxExtent.height)
        && (pImageInfo->extent.depth  <= formatProps.maxExtent.depth)
        && (pImageInfo->numLayers     <= formatProps.maxArrayLayers)
        && (pImageInfo->mipLevels     <= formatProps.maxMipLevels)
        && (pImageInfo->sampleCount     & formatProps.sampleCounts);
  }


  BOOL D3D9CommonTexture::CalcShadowState() const {
    static std::array<D3D9Format, 3> shadowBlacklist = {
      D3D9Format::INTZ, D3D9Format::DF16, D3D9Format::DF24
    };

    bool shadow = caps::IsDepthFormat(m_desc.Format);

    for (uint32_t i = 0; i < shadowBlacklist.size(); i++) {
      if (shadowBlacklist[i] == m_desc.Format)
        shadow = false;
    }

    return shadow;
  }

  int64_t D3D9CommonTexture::CalcMemoryConsumption() const {
    // This is not accurate. It's not meant to be.
    // We're just trying to persuade some applications
    // to not infinitely allocate resources.
    int64_t faceSize = 0;

    for (uint32_t i = 0; i < m_desc.MipLevels; i++)
      faceSize += int64_t(align(GetMipLength(i), 256));
   
    return faceSize * int64_t(GetLayerCount());
  }


  BOOL D3D9CommonTexture::CheckFormatFeatureSupport(
    VkFormat              Format,
    VkFormatFeatureFlags  Features) const {
    VkFormatProperties properties = m_device->GetDXVKDevice()->adapter()->formatProperties(Format);

    return (properties.linearTilingFeatures  & Features) == Features
      || (properties.optimalTilingFeatures & Features) == Features;
  }


  VkImageUsageFlags D3D9CommonTexture::EnableMetaCopyUsage(
    VkFormat              Format,
    VkImageTiling         Tiling) const {
    VkFormatFeatureFlags requestedFeatures = 0;

    if (Format == VK_FORMAT_D16_UNORM || Format == VK_FORMAT_D32_SFLOAT) {
      requestedFeatures |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
                        |  VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }

    if (Format == VK_FORMAT_R16_UNORM || Format == VK_FORMAT_R32_SFLOAT) {
      requestedFeatures |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
                        |  VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    }

    if (requestedFeatures == 0)
      return 0;

    // Enable usage flags for all supported and requested features
    VkFormatProperties properties = m_device->GetDXVKDevice()->adapter()->formatProperties(Format);

    requestedFeatures &= Tiling == VK_IMAGE_TILING_OPTIMAL
      ? properties.optimalTilingFeatures
      : properties.linearTilingFeatures;

    VkImageUsageFlags requestedUsage = 0;

    if (requestedFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
      requestedUsage |= VK_IMAGE_USAGE_SAMPLED_BIT;

    if (requestedFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
      requestedUsage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

    if (requestedFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
      requestedUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    return requestedUsage;
  }


  VkImageUsageFlags D3D9CommonTexture::EnableMetaPackUsage(
    VkFormat              Format,
    BOOL                  WriteOnly) const {
    if (WriteOnly)
      return 0;

    const auto dsMask = VK_IMAGE_ASPECT_DEPTH_BIT
      | VK_IMAGE_ASPECT_STENCIL_BIT;

    auto formatInfo = imageFormatInfo(Format);

    return formatInfo->aspectMask == dsMask
      ? VK_IMAGE_USAGE_SAMPLED_BIT
      : 0;
  }


  D3D9_COMMON_TEXTURE_MAP_MODE D3D9CommonTexture::DetermineMapMode(
    const DxvkImageCreateInfo*  pImageInfo) const {
    // Write-only images should go through a buffer for multiple reasons:
    // 1. Some games do not respect the row and depth pitch that is returned
    //    by the Map() method, which leads to incorrect rendering (e.g. Nier)
    // 2. Since the image will most likely be read for rendering by the GPU,
    //    writing the image to device-local image may be more efficient than
    //    reading its contents from host-visible memory.
    if (m_desc.Usage & D3DUSAGE_DYNAMIC)
      return D3D9_COMMON_TEXTURE_MAP_MODE_BUFFER;

    // These format requires fixup to an 8888.
    if (this->RequiresFixup())
      return D3D9_COMMON_TEXTURE_MAP_MODE_BUFFER;

    // Depth-stencil formats in D3D9 can be mapped and follow special
    // packing rules, so we need to copy that data into a buffer first
    if (GetPackedDepthStencilFormat(m_desc.Format))
      return D3D9_COMMON_TEXTURE_MAP_MODE_BUFFER;

    // We want to use a buffer for anything on-screen to get optimal
    // We want to use a linear mapping for anything staging.
    if (m_desc.Pool != D3DPOOL_SYSTEMMEM && m_desc.Pool != D3DPOOL_SCRATCH)
      return D3D9_COMMON_TEXTURE_MAP_MODE_BUFFER;

    // Images that can be read by the host should be mapped directly in
    // order to avoid expensive synchronization with the GPU. This does
    // however require linear tiling, which may not be supported for all
    // combinations of image parameters.
    return this->CheckImageSupport(pImageInfo, VK_IMAGE_TILING_LINEAR)
      ? D3D9_COMMON_TEXTURE_MAP_MODE_DIRECT
      : D3D9_COMMON_TEXTURE_MAP_MODE_BUFFER;
  }

  VkDeviceSize D3D9CommonTexture::GetMipLength(UINT MipLevel) const {
    const DxvkFormatInfo* formatInfo = imageFormatInfo(
      m_device->LookupFormat(m_desc.Format).Format);

    const VkExtent3D levelExtent = m_image->mipLevelExtent(MipLevel);
    const VkExtent3D blockCount  = util::computeBlockCount(levelExtent, formatInfo->blockSize);

    return formatInfo->elementSize
      * blockCount.width
      * blockCount.height
      * blockCount.depth;
  }

  bool D3D9CommonTexture::AllocBuffers(UINT Face, UINT MipLevel) {
    UINT Subresource = CalcSubresource(Face, MipLevel);

    if (m_mappingBuffers.at(Subresource) != nullptr)
      return false;

    DxvkBufferCreateInfo info;
    info.size  = GetMipLength(MipLevel);
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT
               | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    info.access = VK_ACCESS_TRANSFER_READ_BIT
                | VK_ACCESS_TRANSFER_WRITE_BIT;

    auto memoryType = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                    | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    if (!IsWriteOnly())
      memoryType |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT;

    m_mappingBuffers.at(Subresource) = m_device->GetDXVKDevice()->createBuffer(info, memoryType);

    if (this->RequiresFixup()) {
      m_fixupBuffers.at(Subresource) =
        m_device->GetDXVKDevice()->createBuffer(info, memoryType);
    }

    return true;
  }

  void D3D9CommonTexture::DeallocMappingBuffers() {
    for (auto& buf : m_mappingBuffers)
      buf = nullptr;
  }

  void D3D9CommonTexture::DeallocFixupBuffers() {
    for (auto& buf : m_fixupBuffers)
      buf = nullptr;
  }

  void D3D9CommonTexture::DeallocMappingBuffer(UINT Subresource) {
    m_mappingBuffers.at(Subresource) = nullptr;
  }

  void D3D9CommonTexture::DeallocFixupBuffer(UINT Subresource) {
    m_fixupBuffers.at(Subresource) = nullptr;
  }

  VkImageType D3D9CommonTexture::GetImageTypeFromResourceType(D3DRESOURCETYPE Type) {
    switch (Type) {
    case D3DRTYPE_CUBETEXTURE:
    case D3DRTYPE_TEXTURE:
    case D3DRTYPE_SURFACE:
      return VK_IMAGE_TYPE_2D;

    case D3DRTYPE_VOLUME:
    case D3DRTYPE_VOLUMETEXTURE:
      return VK_IMAGE_TYPE_3D;

    default: throw DxvkError("D3D9CommonTexture: Unhandled resource type");
    }
  }


  VkImageLayout D3D9CommonTexture::OptimizeLayout(VkImageUsageFlags Usage) {
    const VkImageUsageFlags usageFlags = Usage;

    // Filter out unnecessary flags. Transfer operations
    // are handled by the backend in a transparent manner.
    Usage &= ~(VK_IMAGE_USAGE_TRANSFER_DST_BIT
             | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

    // If the image is used only as an attachment, we never
    // have to transform the image back to a different layout
    if (Usage == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
      return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    if (Usage == VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
      return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    Usage &= ~(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
             | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);

    // If the image is used for reading but not as a storage
    // image, we can optimize the image for texture access
    if (Usage == VK_IMAGE_USAGE_SAMPLED_BIT) {
      return usageFlags & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
        ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    // Otherwise, we have to stick with the default layout
    return VK_IMAGE_LAYOUT_GENERAL;
  }

  HRESULT D3D9CommonTexture::Lock(
          UINT            Face,
          UINT            MipLevel,
          D3DLOCKED_BOX*  pLockedBox,
    const D3DBOX*         pBox,
          DWORD           Flags) {
    return m_device->LockImage(
      this,
      Face,
      MipLevel,
      pLockedBox,
      pBox,
      Flags);
  }

  HRESULT D3D9CommonTexture::Unlock(
          UINT Face,
          UINT MipLevel) {
    return m_device->UnlockImage(
      this,
      Face,
      MipLevel);
  }

  VkImageViewType D3D9CommonTexture::GetImageViewType() const {
    switch (m_desc.Type) {
    default:
    case D3DRTYPE_SURFACE:
    case D3DRTYPE_TEXTURE:
      return VK_IMAGE_VIEW_TYPE_2D;

    case D3DRTYPE_VOLUME:
    case D3DRTYPE_VOLUMETEXTURE:
      return VK_IMAGE_VIEW_TYPE_3D;

    case D3DRTYPE_CUBETEXTURE:
      return VK_IMAGE_VIEW_TYPE_CUBE;
    }
  }

  Rc<DxvkImageView> D3D9CommonTexture::CreateView(
    int32_t           Index,
    VkImageUsageFlags UsageFlags,
    bool              srgb,
    UINT              Lod) {
    const D3D9_VK_FORMAT_MAPPING formatInfo = m_device->LookupFormat(m_desc.Format);

    DxvkImageViewCreateInfo viewInfo;
    viewInfo.format  = PickSRGB(formatInfo.Format, formatInfo.FormatSrgb, srgb);
    viewInfo.aspect  = formatInfo.Aspect;
    viewInfo.usage   = UsageFlags;

    // Remove the stencil aspect if we are trying to create an image view of a depth stencil format 
    if (UsageFlags & VK_IMAGE_USAGE_SAMPLED_BIT
     && viewInfo.aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
      viewInfo.aspect &= ~VK_IMAGE_ASPECT_STENCIL_BIT;
    }

    if (!(UsageFlags & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
      viewInfo.swizzle = formatInfo.Swizzle;
      // Shaders expect the stencil value in the G component
      if (viewInfo.aspect == VK_IMAGE_ASPECT_STENCIL_BIT) {
        viewInfo.swizzle = VkComponentMapping{
          VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R,
          VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO };
      }
    }

    viewInfo.type = GetImageViewType();

    if (Index != -1 && viewInfo.type == VK_IMAGE_VIEW_TYPE_CUBE)
      viewInfo.type = VK_IMAGE_VIEW_TYPE_2D;

    viewInfo.minLevel  = Lod;
    viewInfo.numLevels = m_desc.MipLevels - Lod;
    viewInfo.minLayer  = Index == -1 ? 0 : Index;
    viewInfo.numLayers = Index == -1 ? GetLayerCount() : 1;

    // Create the underlying image view object
    return m_device->GetDXVKDevice()->createImageView(GetImage(), viewInfo);
  }

  void D3D9CommonTexture::RecreateImageViews(UINT Lod) {
    // TODO: Signal to device that this resource is dirty and needs to be rebound.

    m_imageView     = CreateView(-1, VK_IMAGE_USAGE_SAMPLED_BIT, false, Lod);
    m_imageViewSrgb = CreateView(-1, VK_IMAGE_USAGE_SAMPLED_BIT, true, Lod);

    uint32_t layerCount = GetLayerCount();
    for (uint32_t i = 0; i < layerCount; i++) {
      m_imageViewFaces[i]     = CreateView(i, VK_IMAGE_USAGE_SAMPLED_BIT, false, Lod);
      m_imageViewSrgbFaces[i] = CreateView(i, VK_IMAGE_USAGE_SAMPLED_BIT, true, Lod);
    }
  }

  void D3D9CommonTexture::CreateDepthStencilViews() {
    uint32_t layerCount = GetLayerCount();

    for (uint32_t i = 0; i < layerCount; i++)
      m_depthStencilView[i] = CreateView(i, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, false, 0);
  }

  void D3D9CommonTexture::CreateRenderTargetViews() {
    uint32_t layerCount = GetLayerCount();

    for (uint32_t i = 0; i < layerCount; i++) {
      m_renderTargetView[i]     = CreateView(i, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, false, 0);
      m_renderTargetViewSrgb[i] = CreateView(i, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, true, 0);
    }

    m_mipGenView = CreateView(-1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, false, 0);
  }

}