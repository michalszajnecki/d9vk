#include "d3d9_device.h"

#include "d3d9_interface.h"
#include "d3d9_swapchain.h"
#include "d3d9_caps.h"
#include "d3d9_util.h"
#include "d3d9_texture.h"
#include "d3d9_buffer.h"
#include "d3d9_vertex_declaration.h"
#include "d3d9_shader.h"
#include "d3d9_query.h"
#include "d3d9_stateblock.h"
#include "d3d9_monitor.h"
#include "d3d9_spec_constants.h"
#include "d3d9_names.h"

#include "../dxvk/dxvk_adapter.h"
#include "../dxvk/dxvk_instance.h"

#include "../util/util_bit.h"
#include "../util/util_math.h"

#include "d3d9_initializer.h"

#include <algorithm>
#include <cfloat>
#ifdef MSC_VER
#pragma fenv_access (on)
#endif

namespace dxvk {

  D3D9DeviceEx::D3D9DeviceEx(
          D3D9InterfaceEx*  pParent,
          D3D9Adapter*      pAdapter,
          D3DDEVTYPE        DeviceType,
          HWND              hFocusWindow,
          DWORD             BehaviorFlags,
          D3DDISPLAYMODEEX* pDisplayMode,
          Rc<DxvkDevice>    dxvkDevice)
    : m_adapter        ( pAdapter )
    , m_dxvkDevice     ( dxvkDevice )
    , m_csThread       ( dxvkDevice->createContext() )
    , m_frameLatency   ( DefaultFrameLatency )
    , m_csChunk        ( AllocCsChunk() )
    , m_parent         ( pParent )
    , m_deviceType     ( DeviceType )
    , m_window         ( hFocusWindow )
    , m_behaviorFlags  ( BehaviorFlags )
    , m_multithread    ( BehaviorFlags & D3DCREATE_MULTITHREADED )
    , m_shaderModules  ( new D3D9ShaderModuleSet )
    , m_d3d9Options    ( dxvkDevice, pAdapter->GetDXVKAdapter()->instance()->config() )
    , m_dxsoOptions    ( m_dxvkDevice, m_d3d9Options ) {
    m_initializer      = new D3D9Initializer(m_dxvkDevice);
    m_frameLatencyCap  = m_d3d9Options.maxFrameLatency;

    for (uint32_t i = 0; i < m_frameEvents.size(); i++)
      m_frameEvents[i] = new sync::Signal(true);

    EmitCs([
      cDevice = m_dxvkDevice
    ] (DxvkContext* ctx) {
      ctx->beginRecording(cDevice->createCommandList());

      DxvkLogicOpState loState;
      loState.enableLogicOp = VK_FALSE;
      loState.logicOp       = VK_LOGIC_OP_CLEAR;
      ctx->setLogicOpState(loState);
    });

    CreateConstantBuffers();

    if (!(BehaviorFlags & D3DCREATE_FPU_PRESERVE))
      SetupFPU();

    m_availableMemory = DetermineInitialTextureMemory();
  }


  D3D9DeviceEx::~D3D9DeviceEx() {
    Flush();
    SynchronizeCsThread();

    delete m_initializer;

    m_dxvkDevice->waitForIdle(); // Sync Device
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::QueryInterface(REFIID riid, void** ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    bool extended = m_parent->IsExtended()
                 && riid == __uuidof(IDirect3DDevice9Ex);

    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(IDirect3DDevice9)
     || extended) {
      *ppvObject = ref(this);
      return S_OK;
    }

    // We want to ignore this if the extended device is queried and we weren't made extended.
    if (riid == __uuidof(IDirect3DDevice9Ex))
      return E_NOINTERFACE;

    Logger::warn("D3D9DeviceEx::QueryInterface: Unknown interface query");
    Logger::warn(str::format(riid));
    return E_NOINTERFACE;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::TestCooperativeLevel() {
    // Equivelant of D3D11/DXGI present tests. We can always present.
    return D3D_OK;
  }


  UINT    STDMETHODCALLTYPE D3D9DeviceEx::GetAvailableTextureMem() {
    // This is not meant to be accurate.
    // The values are also wildly incorrect in d3d9... But some games rely
    // on this inaccurate value...

    // Clamp to megabyte range, as per spec.
    constexpr UINT range = 0xfff00000;

    UINT memory = UINT(m_availableMemory.load());
    return memory & range;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::EvictManagedResources() {
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetDirect3D(IDirect3D9** ppD3D9) {
    if (ppD3D9 == nullptr)
      return D3DERR_INVALIDCALL;

    *ppD3D9 = m_parent.ref();
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetDeviceCaps(D3DCAPS9* pCaps) {
    return m_adapter->GetDeviceCaps(m_deviceType, pCaps);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode) {
    D3D9DeviceLock lock = LockDevice();

    auto* swapchain = GetInternalSwapchain(iSwapChain);

    if (unlikely(swapchain == nullptr))
      return D3DERR_INVALIDCALL;

    return swapchain->GetDisplayMode(pMode);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters) {
    if (pParameters == nullptr)
      return D3DERR_INVALIDCALL;

    pParameters->AdapterOrdinal = m_adapter->GetOrdinal();
    pParameters->BehaviorFlags  = m_behaviorFlags;
    pParameters->DeviceType     = m_deviceType;
    pParameters->hFocusWindow   = m_window;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetCursorProperties(
          UINT               XHotSpot,
          UINT               YHotSpot,
          IDirect3DSurface9* pCursorBitmap) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pCursorBitmap == nullptr))
      return D3DERR_INVALIDCALL;

    auto* cursorTex = GetCommonTexture(pCursorBitmap);
    if (unlikely(cursorTex->Desc()->Format != D3D9Format::A8R8G8B8))
      return D3DERR_INVALIDCALL;

    uint32_t inputWidth  = cursorTex->Desc()->Width;
    uint32_t inputHeight = cursorTex->Desc()->Height;

    // Always use a hardware cursor when windowed.
    bool hwCursor  = m_presentParams.Windowed;

    // Always use a hardware cursor w/h <= 32 px
    hwCursor |= inputWidth  <= HardwareCursorWidth
             || inputHeight <= HardwareCursorHeight;

    if (hwCursor) {
      D3DLOCKED_BOX lockedBox;
      HRESULT hr = LockImage(cursorTex, 0, 0, &lockedBox, nullptr, D3DLOCK_READONLY);
      if (FAILED(hr))
        return hr;

      const uint8_t* data  = reinterpret_cast<const uint8_t*>(lockedBox.pBits);

      // Windows works with a stride of 128, lets respect that.
      // Copy data to the bitmap...
      CursorBitmap bitmap = { 0 };
      size_t copyPitch = std::min<size_t>(
        HardwareCursorPitch,
        inputWidth * inputHeight * HardwareCursorFormatSize);

      for (uint32_t h = 0; h < HardwareCursorHeight; h++)
        std::memcpy(&bitmap[h * HardwareCursorPitch], &data[h * lockedBox.RowPitch], copyPitch);

      UnlockImage(cursorTex, 0, 0);

      // Set this as our cursor.
      return m_cursor.SetHardwareCursor(XHotSpot, YHotSpot, bitmap);
    }

    // Software Cursor...
    Logger::warn("D3D9DeviceEx::SetCursorProperties: Software cursor not implemented.");
    return D3D_OK;
  }


  void    STDMETHODCALLTYPE D3D9DeviceEx::SetCursorPosition(int X, int Y, DWORD Flags) {
    D3D9DeviceLock lock = LockDevice();

    m_cursor.UpdateCursor(X, Y, Flags & D3DCURSOR_IMMEDIATE_UPDATE);
  }


  BOOL    STDMETHODCALLTYPE D3D9DeviceEx::ShowCursor(BOOL bShow) {
    D3D9DeviceLock lock = LockDevice();

    return m_cursor.ShowCursor(bShow);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateAdditionalSwapChain(
          D3DPRESENT_PARAMETERS* pPresentationParameters,
          IDirect3DSwapChain9**  ppSwapChain) {
    return CreateAdditionalSwapChainEx(pPresentationParameters, nullptr, ppSwapChain);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain) {
    D3D9DeviceLock lock = LockDevice();

    InitReturnPtr(pSwapChain);

    auto* swapchain = GetInternalSwapchain(iSwapChain);

    if (unlikely(swapchain == nullptr || pSwapChain == nullptr))
      return D3DERR_INVALIDCALL;

    *pSwapChain = static_cast<IDirect3DSwapChain9*>(ref(swapchain));

    return D3D_OK;
  }


  UINT    STDMETHODCALLTYPE D3D9DeviceEx::GetNumberOfSwapChains() {
    D3D9DeviceLock lock = LockDevice();

    return UINT(m_swapchains.size());
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::Reset(D3DPRESENT_PARAMETERS* pPresentationParameters) {
    return ResetEx(pPresentationParameters, nullptr);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::Present(
    const RECT*    pSourceRect,
    const RECT*    pDestRect,
          HWND     hDestWindowOverride,
    const RGNDATA* pDirtyRegion) {
    return PresentEx(
      pSourceRect,
      pDestRect,
      hDestWindowOverride,
      pDirtyRegion,
      0);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetBackBuffer(
          UINT                iSwapChain,
          UINT                iBackBuffer,
          D3DBACKBUFFER_TYPE  Type,
          IDirect3DSurface9** ppBackBuffer) {
    D3D9DeviceLock lock = LockDevice();

    InitReturnPtr(ppBackBuffer);

    auto* swapchain = GetInternalSwapchain(iSwapChain);

    if (unlikely(swapchain == nullptr))
      return D3DERR_INVALIDCALL;

    return swapchain->GetBackBuffer(iBackBuffer, Type, ppBackBuffer);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus) {
    D3D9DeviceLock lock = LockDevice();

    auto* swapchain = GetInternalSwapchain(iSwapChain);

    if (unlikely(swapchain == nullptr))
      return D3DERR_INVALIDCALL;

    return swapchain->GetRasterStatus(pRasterStatus);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetDialogBoxMode(BOOL bEnableDialogs) {
    Logger::warn("D3D9DeviceEx::SetDialogBoxMode: Stub");
    return D3D_OK;
  }


  void    STDMETHODCALLTYPE D3D9DeviceEx::SetGammaRamp(
          UINT          iSwapChain,
          DWORD         Flags,
    const D3DGAMMARAMP* pRamp) {
    D3D9DeviceLock lock = LockDevice();

    auto* swapchain = GetInternalSwapchain(iSwapChain);

    if (unlikely(swapchain == nullptr))
      return;

    swapchain->SetGammaRamp(Flags, pRamp);
  }


  void    STDMETHODCALLTYPE D3D9DeviceEx::GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP* pRamp) {
    D3D9DeviceLock lock = LockDevice();

    auto* swapchain = GetInternalSwapchain(iSwapChain);

    if (unlikely(swapchain == nullptr))
      return;

    swapchain->GetGammaRamp(pRamp);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateTexture(
          UINT                Width,
          UINT                Height,
          UINT                Levels,
          DWORD               Usage,
          D3DFORMAT           Format,
          D3DPOOL             Pool,
          IDirect3DTexture9** ppTexture,
          HANDLE*             pSharedHandle) {
    InitReturnPtr(ppTexture);

    if (unlikely(ppTexture == nullptr))
      return D3DERR_INVALIDCALL;

    D3D9_COMMON_TEXTURE_DESC desc;
    desc.Width              = Width;
    desc.Height             = Height;
    desc.Depth              = 1;
    desc.ArraySize          = 1;
    desc.MipLevels          = Levels;
    desc.Usage              = Usage;
    desc.Format             = EnumerateFormat(Format);
    desc.Pool               = Pool;
    desc.Discard            = FALSE;
    desc.MultiSample        = D3DMULTISAMPLE_NONE;
    desc.MultisampleQuality = 0;

    if (FAILED(D3D9CommonTexture::NormalizeTextureProperties(&desc)))
      return D3DERR_INVALIDCALL;

    try {
      const Com<D3D9Texture2D> texture = new D3D9Texture2D(this, &desc);

      void* initialData = nullptr;

      if (Pool == D3DPOOL_SYSTEMMEM && Levels == 1 && pSharedHandle != nullptr)
        initialData = *(reinterpret_cast<void**>(pSharedHandle));
      else // This must be a shared resource.
        InitReturnPtr(pSharedHandle);

      m_initializer->InitTexture(texture->GetCommonTexture(), initialData);
      *ppTexture = texture.ref();

      bool mipSuccess = (Usage & D3DUSAGE_AUTOGENMIPMAP) == (texture->GetCommonTexture()->Desc()->Usage & D3DUSAGE_AUTOGENMIPMAP);
      return mipSuccess ? D3D_OK : D3DOK_NOAUTOGEN;
    }
    catch (const DxvkError& e) {
      Logger::err(e.message());
      return D3DERR_OUTOFVIDEOMEMORY;
    }
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateVolumeTexture(
          UINT                      Width,
          UINT                      Height,
          UINT                      Depth,
          UINT                      Levels,
          DWORD                     Usage,
          D3DFORMAT                 Format,
          D3DPOOL                   Pool,
          IDirect3DVolumeTexture9** ppVolumeTexture,
          HANDLE*                   pSharedHandle) {
    InitReturnPtr(ppVolumeTexture);
    InitReturnPtr(pSharedHandle);

    if (unlikely(ppVolumeTexture == nullptr))
      return D3DERR_INVALIDCALL;

    D3D9_COMMON_TEXTURE_DESC desc;
    desc.Width              = Width;
    desc.Height             = Height;
    desc.Depth              = Depth;
    desc.ArraySize          = 1;
    desc.MipLevels          = Levels;
    desc.Usage              = Usage;
    desc.Format             = EnumerateFormat(Format);
    desc.Pool               = Pool;
    desc.Discard            = FALSE;
    desc.MultiSample        = D3DMULTISAMPLE_NONE;
    desc.MultisampleQuality = 0;

    if (FAILED(D3D9CommonTexture::NormalizeTextureProperties(&desc)))
      return D3DERR_INVALIDCALL;

    try {
      const Com<D3D9Texture3D> texture = new D3D9Texture3D(this, &desc);
      m_initializer->InitTexture(texture->GetCommonTexture());
      *ppVolumeTexture = texture.ref();

      bool mipSuccess = (Usage & D3DUSAGE_AUTOGENMIPMAP) == (texture->GetCommonTexture()->Desc()->Usage & D3DUSAGE_AUTOGENMIPMAP);
      return mipSuccess ? D3D_OK : D3DOK_NOAUTOGEN;
    }
    catch (const DxvkError& e) {
      Logger::err(e.message());
      return D3DERR_OUTOFVIDEOMEMORY;
    }
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateCubeTexture(
          UINT                    EdgeLength,
          UINT                    Levels,
          DWORD                   Usage,
          D3DFORMAT               Format,
          D3DPOOL                 Pool,
          IDirect3DCubeTexture9** ppCubeTexture,
          HANDLE*                 pSharedHandle) {
    InitReturnPtr(ppCubeTexture);
    InitReturnPtr(pSharedHandle);

    if (unlikely(ppCubeTexture == nullptr))
      return D3DERR_INVALIDCALL;

    D3D9_COMMON_TEXTURE_DESC desc;
    desc.Width              = EdgeLength;
    desc.Height             = EdgeLength;
    desc.Depth              = 1;
    desc.ArraySize          = 6; // A cube has 6 faces, wowwie!
    desc.MipLevels          = Levels;
    desc.Usage              = Usage;
    desc.Format             = EnumerateFormat(Format);
    desc.Pool               = Pool;
    desc.Discard            = FALSE;
    desc.MultiSample        = D3DMULTISAMPLE_NONE;
    desc.MultisampleQuality = 0;

    if (FAILED(D3D9CommonTexture::NormalizeTextureProperties(&desc)))
      return D3DERR_INVALIDCALL;

    try {
      const Com<D3D9TextureCube> texture = new D3D9TextureCube(this, &desc);
      m_initializer->InitTexture(texture->GetCommonTexture());
      *ppCubeTexture = texture.ref();

      bool mipSuccess = (Usage & D3DUSAGE_AUTOGENMIPMAP) == (texture->GetCommonTexture()->Desc()->Usage & D3DUSAGE_AUTOGENMIPMAP);
      return mipSuccess ? D3D_OK : D3DOK_NOAUTOGEN;
    }
    catch (const DxvkError& e) {
      Logger::err(e.message());
      return D3DERR_OUTOFVIDEOMEMORY;
    }
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateVertexBuffer(
          UINT                     Length,
          DWORD                    Usage,
          DWORD                    FVF,
          D3DPOOL                  Pool,
          IDirect3DVertexBuffer9** ppVertexBuffer,
          HANDLE*                  pSharedHandle) {
    InitReturnPtr(ppVertexBuffer);

    if (unlikely(ppVertexBuffer == nullptr))
      return D3DERR_INVALIDCALL;

    D3D9_BUFFER_DESC desc;
    desc.Format = D3D9Format::VERTEXDATA;
    desc.FVF    = FVF;
    desc.Pool   = Pool;
    desc.Size   = Length;
    desc.Type   = D3DRTYPE_VERTEXBUFFER;
    desc.Usage  = Usage;

    if (FAILED(D3D9CommonBuffer::ValidateBufferProperties(&desc)))
      return D3DERR_INVALIDCALL;

    try {
      const Com<D3D9VertexBuffer> buffer = new D3D9VertexBuffer(this, &desc);
      m_initializer->InitBuffer(buffer->GetCommonBuffer());
      *ppVertexBuffer = buffer.ref();
      return D3D_OK;
    }
    catch (const DxvkError & e) {
      Logger::err(e.message());
      return D3DERR_INVALIDCALL;
    }
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateIndexBuffer(
          UINT                    Length,
          DWORD                   Usage,
          D3DFORMAT               Format,
          D3DPOOL                 Pool,
          IDirect3DIndexBuffer9** ppIndexBuffer,
          HANDLE*                 pSharedHandle) {
    InitReturnPtr(ppIndexBuffer);

    if (unlikely(ppIndexBuffer == nullptr))
      return D3DERR_INVALIDCALL;

    D3D9_BUFFER_DESC desc;
    desc.Format = EnumerateFormat(Format);
    desc.Pool   = Pool;
    desc.Size   = Length;
    desc.Type   = D3DRTYPE_INDEXBUFFER;
    desc.Usage  = Usage;

    if (FAILED(D3D9CommonBuffer::ValidateBufferProperties(&desc)))
      return D3DERR_INVALIDCALL;

    try {
      const Com<D3D9IndexBuffer> buffer = new D3D9IndexBuffer(this, &desc);
      m_initializer->InitBuffer(buffer->GetCommonBuffer());
      *ppIndexBuffer = buffer.ref();
      return D3D_OK;
    }
    catch (const DxvkError & e) {
      Logger::err(e.message());
      return D3DERR_INVALIDCALL;
    }
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateRenderTarget(
          UINT                Width,
          UINT                Height,
          D3DFORMAT           Format,
          D3DMULTISAMPLE_TYPE MultiSample,
          DWORD               MultisampleQuality,
          BOOL                Lockable,
          IDirect3DSurface9** ppSurface,
          HANDLE*             pSharedHandle) {
    return CreateRenderTargetEx(
      Width,
      Height,
      Format,
      MultiSample,
      MultisampleQuality,
      Lockable,
      ppSurface,
      pSharedHandle,
      0);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateDepthStencilSurface(
          UINT                Width,
          UINT                Height,
          D3DFORMAT           Format,
          D3DMULTISAMPLE_TYPE MultiSample,
          DWORD               MultisampleQuality,
          BOOL                Discard,
          IDirect3DSurface9** ppSurface,
          HANDLE*             pSharedHandle) {
    return CreateDepthStencilSurfaceEx(
      Width,
      Height,
      Format,
      MultiSample,
      MultisampleQuality,
      Discard,
      ppSurface,
      pSharedHandle,
      0);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::UpdateSurface(
          IDirect3DSurface9* pSourceSurface,
    const RECT*              pSourceRect,
          IDirect3DSurface9* pDestinationSurface,
    const POINT*             pDestPoint) {
    D3D9DeviceLock lock = LockDevice();

    D3D9Surface* src = static_cast<D3D9Surface*>(pSourceSurface);
    D3D9Surface* dst = static_cast<D3D9Surface*>(pDestinationSurface);

    if (unlikely(src == nullptr || dst == nullptr))
      return D3DERR_INVALIDCALL;

    D3D9CommonTexture* srcTextureInfo = src->GetCommonTexture();
    D3D9CommonTexture* dstTextureInfo = dst->GetCommonTexture();

    if (unlikely(srcTextureInfo->Desc()->Pool != D3DPOOL_SYSTEMMEM || dstTextureInfo->Desc()->Pool != D3DPOOL_DEFAULT))
      return D3DERR_INVALIDCALL;

    Rc<DxvkBuffer> srcBuffer = srcTextureInfo->GetBuffer(src->GetSubresource());
    Rc<DxvkImage> dstImage   = dstTextureInfo->GetImage();

    const VkImageSubresource dstSubresource = dstTextureInfo->GetSubresourceFromIndex(VK_IMAGE_ASPECT_COLOR_BIT, dst->GetSubresource());

    const DxvkFormatInfo* dstFormatInfo = imageFormatInfo(dstImage->info().format);

    VkOffset3D srcOffset = { 0,0,0 };
    VkOffset3D dstOffset = { 0,0,0 };

    VkExtent3D srcExtent = srcTextureInfo->GetExtentMip(src->GetMipLevel());
    VkExtent3D regExtent = srcExtent;

    if (pDestPoint != nullptr) {
      dstOffset = { align(pDestPoint->x, dstFormatInfo->blockSize.width),
                    align(pDestPoint->y, dstFormatInfo->blockSize.height),
                    0 };
    }

    if (pSourceRect != nullptr) {
      srcOffset = { 
        align(pSourceRect->left, dstFormatInfo->blockSize.width),
        align(pSourceRect->top,  dstFormatInfo->blockSize.height),
        0 };

      regExtent = { 
        align(uint32_t(pSourceRect->right  - pSourceRect->left), dstFormatInfo->blockSize.width),
        align(uint32_t(pSourceRect->bottom - pSourceRect->top),  dstFormatInfo->blockSize.height),
        1 };
    }

    VkImageSubresourceLayers dstLayers = {
      dstSubresource.aspectMask,
      dstSubresource.mipLevel,
      dstSubresource.arrayLayer, 1 };

    VkExtent3D regBlockCount = util::computeBlockCount(regExtent, dstFormatInfo->blockSize);

    regExtent = util::minExtent3D(regExtent, util::computeBlockExtent(regBlockCount, dstFormatInfo->blockSize));

    VkDeviceSize srcOffsetBytes = srcOffset.z * srcExtent.height * srcExtent.width
                                + srcOffset.y * srcExtent.width
                                + srcOffset.x;

    EmitCs([
      cDstImage  = dstImage,
      cSrcBuffer = srcBuffer,
      cDstLayers = dstLayers,
      cDstOffset = dstOffset,
      cSrcOffset = srcOffsetBytes,
      cExtent    = regExtent,
      cSrcExtent = srcExtent
    ] (DxvkContext* ctx) {
      ctx->copyBufferToImage(
        cDstImage, cDstLayers, cDstOffset, cExtent,
        cSrcBuffer, cSrcOffset,
        VkExtent2D{ cSrcExtent.width, cSrcExtent.height });
    });

    if (dstTextureInfo->IsAutomaticMip())
      GenerateMips(dstTextureInfo);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::UpdateTexture(
          IDirect3DBaseTexture9* pSourceTexture,
          IDirect3DBaseTexture9* pDestinationTexture) {
    D3D9DeviceLock lock = LockDevice();

    if (!pDestinationTexture || !pSourceTexture)
      return D3DERR_INVALIDCALL;

    D3D9CommonTexture* dstTexInfo = GetCommonTexture(pDestinationTexture);
    D3D9CommonTexture* srcTexInfo = GetCommonTexture(pSourceTexture);

    if (unlikely(srcTexInfo->Desc()->Pool != D3DPOOL_SYSTEMMEM || dstTexInfo->Desc()->Pool != D3DPOOL_DEFAULT))
      return D3DERR_INVALIDCALL;

    const Rc<DxvkImage> dstImage  = dstTexInfo->GetImage();
      
    uint32_t mipLevels   = std::min(srcTexInfo->Desc()->MipLevels, dstTexInfo->Desc()->MipLevels);
    uint32_t arraySlices = std::min(srcTexInfo->Desc()->ArraySize, dstTexInfo->Desc()->ArraySize);
    for (uint32_t a = 0; a < arraySlices; a++) {
      for (uint32_t m = 0; m < mipLevels; m++) {
        Rc<DxvkBuffer> srcBuffer = srcTexInfo->GetBuffer(srcTexInfo->CalcSubresource(a, m));

        VkImageSubresourceLayers dstLayers = { VK_IMAGE_ASPECT_COLOR_BIT, m, a, 1 };
        
        VkExtent3D extent = dstImage->mipLevelExtent(m);
        
        EmitCs([
          cDstImage  = dstImage,
          cSrcBuffer = srcBuffer,
          cDstLayers = dstLayers,
          cExtent    = extent
        ] (DxvkContext* ctx) {
          ctx->copyBufferToImage(
            cDstImage,  cDstLayers, VkOffset3D { 0, 0, 0 },
            cExtent,    cSrcBuffer, 0,
            { cExtent.width, cExtent.height });
        });
      }
    }

    pDestinationTexture->GenerateMipSubLevels();

    FlushImplicit(false);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetRenderTargetData(
          IDirect3DSurface9* pRenderTarget,
          IDirect3DSurface9* pDestSurface) {
    D3D9DeviceLock lock = LockDevice();

    D3D9Surface* src = static_cast<D3D9Surface*>(pRenderTarget);
    D3D9Surface* dst = static_cast<D3D9Surface*>(pDestSurface);

    if (unlikely(src == nullptr || dst == nullptr))
      return D3DERR_INVALIDCALL;

    if (pRenderTarget == pDestSurface)
      return D3D_OK;

    D3D9CommonTexture* dstTexInfo = GetCommonTexture(dst);
    D3D9CommonTexture* srcTexInfo = GetCommonTexture(src);

    if (dstTexInfo->Desc()->Pool == D3DPOOL_DEFAULT)
      return this->StretchRect(pRenderTarget, nullptr, pDestSurface, nullptr, D3DTEXF_NONE);

    Rc<DxvkImage>  image  = srcTexInfo->GetImage();
    Rc<DxvkBuffer> buffer = dstTexInfo->GetBuffer(dst->GetSubresource());

    const DxvkFormatInfo* dstFormatInfo = imageFormatInfo(image->info().format);
    const VkImageSubresource dstSubresource = dstTexInfo->GetSubresourceFromIndex(dstFormatInfo->aspectMask, dst->GetSubresource());

    VkImageSubresourceLayers dstSubresourceLayers = {
      dstSubresource.aspectMask,
      dstSubresource.mipLevel,
      dstSubresource.arrayLayer, 1 };

    VkExtent3D levelExtent = image->mipLevelExtent(dstSubresource.mipLevel);

    dstTexInfo->MarkSystemMemGPUModified();

    EmitCs([
      cBuffer       = buffer,
      cImage        = image,
      cSubresources = dstSubresourceLayers,
      cLevelExtent  = levelExtent
    ] (DxvkContext* ctx) {
      ctx->copyImageToBuffer(
        cBuffer, 0, VkExtent2D { 0u, 0u },
        cImage, cSubresources, VkOffset3D { 0, 0, 0 },
        cLevelExtent);
    });

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9* pDestSurface) {
    D3D9DeviceLock lock = LockDevice();

    auto* swapchain = GetInternalSwapchain(iSwapChain);

    if (unlikely(swapchain == nullptr))
      return D3DERR_INVALIDCALL;

    return swapchain->GetFrontBufferData(pDestSurface);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::StretchRect(
          IDirect3DSurface9*   pSourceSurface,
    const RECT*                pSourceRect,
          IDirect3DSurface9*   pDestSurface,
    const RECT*                pDestRect,
          D3DTEXTUREFILTERTYPE Filter) {
    D3D9DeviceLock lock = LockDevice();

    D3D9Surface* dst = static_cast<D3D9Surface*>(pDestSurface);
    D3D9Surface* src = static_cast<D3D9Surface*>(pSourceSurface);

    if (unlikely(src == nullptr || dst == nullptr))
      return D3DERR_INVALIDCALL;

    bool fastPath         = true;
    bool needsCopyResolve = false;
    bool needsBlitResolve = false;

    D3D9CommonTexture* dstTextureInfo = dst->GetCommonTexture();
    D3D9CommonTexture* srcTextureInfo = src->GetCommonTexture();

    Rc<DxvkImage> dstImage = dstTextureInfo->GetImage();
    Rc<DxvkImage> srcImage = srcTextureInfo->GetImage();

    const DxvkFormatInfo* dstFormatInfo = imageFormatInfo(dstImage->info().format);
    const DxvkFormatInfo* srcFormatInfo = imageFormatInfo(srcImage->info().format);

    const VkImageSubresource dstSubresource = dstTextureInfo->GetSubresourceFromIndex(dstFormatInfo->aspectMask, dst->GetSubresource());
    const VkImageSubresource srcSubresource = srcTextureInfo->GetSubresourceFromIndex(srcFormatInfo->aspectMask, src->GetSubresource());

    VkExtent3D srcExtent = srcImage->mipLevelExtent(srcSubresource.mipLevel);
    VkExtent3D dstExtent = dstImage->mipLevelExtent(dstSubresource.mipLevel);

    D3D9Format srcFormat = srcTextureInfo->Desc()->Format;
    D3D9Format dstFormat = dstTextureInfo->Desc()->Format;

    // We may only fast path copy non identicals one way!
    // We don't know what garbage could be in the X8 data.
    bool similar = (srcFormat == dstFormat)
                || (srcFormat == D3D9Format::A8B8G8R8 && dstFormat == D3D9Format::X8B8G8R8)
                || (srcFormat == D3D9Format::A8R8G8B8 && dstFormat == D3D9Format::X8R8G8B8)
                || (srcFormat == D3D9Format::A1R5G5B5 && dstFormat == D3D9Format::X1R5G5B5)
                || (srcFormat == D3D9Format::A4R4G4B4 && dstFormat == D3D9Format::X4R4G4B4);

    // Copies are only supported on similar formats.
    fastPath &= similar;

    // Copies are only supported if the sample count matches,
    // otherwise we need to resolve.
    needsCopyResolve = dstImage->info().sampleCount != srcImage->info().sampleCount;
    needsBlitResolve = srcImage->info().sampleCount != VK_SAMPLE_COUNT_1_BIT;

    // Copies would only work if we are block aligned.
    if (pSourceRect != nullptr) {
      fastPath       &=  (pSourceRect->left   % srcFormatInfo->blockSize.width  == 0);
      fastPath       &=  (pSourceRect->right  % srcFormatInfo->blockSize.width  == 0);
      fastPath       &=  (pSourceRect->top    % srcFormatInfo->blockSize.height == 0);
      fastPath       &=  (pSourceRect->bottom % srcFormatInfo->blockSize.height == 0);
    }

    if (pDestRect != nullptr) {
      fastPath       &=  (pDestRect->left     % dstFormatInfo->blockSize.width  == 0);
      fastPath       &=  (pDestRect->top      % dstFormatInfo->blockSize.height == 0);
    }

    VkImageSubresourceLayers dstSubresourceLayers = {
      dstSubresource.aspectMask,
      dstSubresource.mipLevel,
      dstSubresource.arrayLayer, 1 };

    VkImageSubresourceLayers srcSubresourceLayers = {
      srcSubresource.aspectMask,
      srcSubresource.mipLevel,
      srcSubresource.arrayLayer, 1 };

    VkImageBlit blitInfo;
    blitInfo.dstSubresource = dstSubresourceLayers;
    blitInfo.srcSubresource = srcSubresourceLayers;

    blitInfo.dstOffsets[0] = pDestRect != nullptr
      ? VkOffset3D{ int32_t(pDestRect->left), int32_t(pDestRect->top), 0 }
      : VkOffset3D{ 0,                        0,                       0 };

    blitInfo.dstOffsets[1] = pDestRect != nullptr
      ? VkOffset3D{ int32_t(pDestRect->right), int32_t(pDestRect->bottom), 1 }
      : VkOffset3D{ int32_t(dstExtent.width),  int32_t(dstExtent.height),  1 };

    blitInfo.srcOffsets[0] = pSourceRect != nullptr
      ? VkOffset3D{ int32_t(pSourceRect->left), int32_t(pSourceRect->top), 0 }
      : VkOffset3D{ 0,                          0,                         0 };

    blitInfo.srcOffsets[1] = pSourceRect != nullptr
      ? VkOffset3D{ int32_t(pSourceRect->right), int32_t(pSourceRect->bottom), 1 }
      : VkOffset3D{ int32_t(srcExtent.width),    int32_t(srcExtent.height),    1 };
    
    VkExtent3D srcCopyExtent =
    { uint32_t(blitInfo.srcOffsets[1].x - blitInfo.srcOffsets[0].x),
      uint32_t(blitInfo.srcOffsets[1].y - blitInfo.srcOffsets[0].y),
      uint32_t(blitInfo.srcOffsets[1].z - blitInfo.srcOffsets[0].z) };

    VkExtent3D dstCopyExtent =
    { uint32_t(blitInfo.dstOffsets[1].x - blitInfo.dstOffsets[0].x),
      uint32_t(blitInfo.dstOffsets[1].y - blitInfo.dstOffsets[0].y),
      uint32_t(blitInfo.dstOffsets[1].z - blitInfo.dstOffsets[0].z) };

    // Copies would only work if the extents match. (ie. no stretching)
    bool stretch = srcCopyExtent != dstCopyExtent;
    fastPath &= !stretch;

    if (fastPath) {
      if (needsCopyResolve) {
        VkImageResolve region;
        region.srcSubresource = blitInfo.srcSubresource;
        region.srcOffset      = blitInfo.srcOffsets[0];
        region.dstSubresource = blitInfo.dstSubresource;
        region.dstOffset      = blitInfo.dstOffsets[0];
        region.extent         = srcCopyExtent;
        
        EmitCs([
          cDstImage = dstImage,
          cSrcImage = srcImage,
          cRegion   = region
        ] (DxvkContext* ctx) {
          ctx->resolveImage(
            cDstImage, cSrcImage, cRegion,
            VK_FORMAT_UNDEFINED);
        });
      } else {
        EmitCs([
          cDstImage  = dstImage,
          cSrcImage  = srcImage,
          cDstLayers = blitInfo.dstSubresource,
          cSrcLayers = blitInfo.srcSubresource,
          cDstOffset = blitInfo.dstOffsets[0],
          cSrcOffset = blitInfo.srcOffsets[0],
          cExtent    = srcCopyExtent
        ] (DxvkContext* ctx) {
          ctx->copyImage(
            cDstImage, cDstLayers, cDstOffset,
            cSrcImage, cSrcLayers, cSrcOffset,
            cExtent);
        });
      }
    }
    else {
      if (needsBlitResolve) {
        auto resolveSrc = srcTextureInfo->GetResolveImage();

        VkImageResolve region;
        region.srcSubresource = blitInfo.srcSubresource;
        region.srcOffset      = blitInfo.srcOffsets[0];
        region.dstSubresource = blitInfo.srcSubresource;
        region.dstOffset      = blitInfo.srcOffsets[0];
        region.extent         = srcCopyExtent;
        
        EmitCs([
          cDstImage = resolveSrc,
          cSrcImage = srcImage,
          cRegion   = region
        ] (DxvkContext* ctx) {
          ctx->resolveImage(
            cDstImage, cSrcImage, cRegion,
            VK_FORMAT_UNDEFINED);
        });

        srcImage = resolveSrc;
      }

      EmitCs([
        cDstImage = dstImage,
        cSrcImage = srcImage,
        cBlitInfo = blitInfo,
        cFilter   = stretch ? DecodeFilter(Filter) : VK_FILTER_NEAREST
      ] (DxvkContext* ctx) {
        ctx->blitImage(
          cDstImage,
          cSrcImage,
          cBlitInfo,
          cFilter);
      });
    }

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::ColorFill(
          IDirect3DSurface9* pSurface,
    const RECT*              pRect,
          D3DCOLOR           Color) {
    D3D9DeviceLock lock = LockDevice();

    D3D9Surface* dst = static_cast<D3D9Surface*>(pSurface);

    if (unlikely(dst == nullptr))
      return D3DERR_INVALIDCALL;

    D3D9CommonTexture* dstTextureInfo = dst->GetCommonTexture();

    VkOffset3D offset = VkOffset3D{ 0u, 0u, 0u };
    VkExtent3D extent = dstTextureInfo->GetExtent();

    bool fullExtent = true;
    if (pRect != nullptr) {
      ConvertRect(*pRect, offset, extent);

      fullExtent = offset == VkOffset3D{ 0u, 0u, 0u }
                && extent == dstTextureInfo->GetExtent();
    }

    Rc<DxvkImageView> imageView         = dst->GetImageView(false);
    Rc<DxvkImageView> renderTargetView  = dst->GetRenderTargetView(false);

    VkClearValue clearValue;
    DecodeD3DCOLOR(Color, clearValue.color.float32);

    // Fast path for games that may use this as an
    // alternative to Clear on render targets.
    if (fullExtent && renderTargetView != nullptr) {
      EmitCs([
        cImageView  = renderTargetView,
        cClearValue = clearValue
      ] (DxvkContext* ctx) {
        ctx->clearRenderTarget(
          cImageView,
          VK_IMAGE_ASPECT_COLOR_BIT,
          cClearValue);
      });
    } else {
      EmitCs([
        cImageView  = imageView,
        cOffset     = offset,
        cExtent     = extent,
        cClearValue = clearValue
      ] (DxvkContext* ctx) {
        ctx->clearImageView(
          cImageView,
          cOffset, cExtent,
          VK_IMAGE_ASPECT_COLOR_BIT,
          cClearValue);
      });
    }

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateOffscreenPlainSurface(
    UINT Width,
    UINT Height,
    D3DFORMAT Format,
    D3DPOOL Pool,
    IDirect3DSurface9** ppSurface,
    HANDLE* pSharedHandle) {
    return CreateOffscreenPlainSurfaceEx(
      Width,     Height,
      Format,    Pool,
      ppSurface, pSharedHandle,
      0);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetRenderTarget(
          DWORD              RenderTargetIndex,
          IDirect3DSurface9* pRenderTarget) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(RenderTargetIndex >= caps::MaxSimultaneousRenderTargets
     || (pRenderTarget == nullptr && RenderTargetIndex == 0)))
      return D3DERR_INVALIDCALL;

    D3D9Surface* rt = static_cast<D3D9Surface*>(pRenderTarget);

    if (unlikely(rt && !(rt->GetCommonTexture()->Desc()->Usage & D3DUSAGE_RENDERTARGET)))
      return D3DERR_INVALIDCALL;

    if (m_state.renderTargets[RenderTargetIndex] == rt)
      return D3D_OK;

    // Do a strong flush if the first render target is changed.
    FlushImplicit(RenderTargetIndex == 0 ? TRUE : FALSE);
    m_flags.set(D3D9DeviceFlag::DirtyFramebuffer);

    changePrivate(m_state.renderTargets[RenderTargetIndex], rt);

    if (RenderTargetIndex == 0) {
      const auto* desc = m_state.renderTargets[0]->GetCommonTexture()->Desc();

      bool validSampleMask = desc->MultiSample > D3DMULTISAMPLE_NONMASKABLE;

      if (validSampleMask != m_flags.test(D3D9DeviceFlag::ValidSampleMask)) {
        m_flags.clr(D3D9DeviceFlag::ValidSampleMask);
        if (validSampleMask)
          m_flags.set(D3D9DeviceFlag::ValidSampleMask);

        m_flags.set(D3D9DeviceFlag::DirtyMultiSampleState);
      }

      D3DVIEWPORT9 viewport;
      viewport.X       = 0;
      viewport.Y       = 0;
      viewport.Width   = desc->Width;
      viewport.Height  = desc->Height;
      viewport.MinZ    = 0.0f;
      viewport.MaxZ    = 1.0f;
      m_state.viewport = viewport;

      RECT scissorRect;
      scissorRect.left    = 0;
      scissorRect.top     = 0;
      scissorRect.right   = desc->Width;
      scissorRect.bottom  = desc->Height;
      m_state.scissorRect = scissorRect;

      m_flags.set(D3D9DeviceFlag::DirtyViewportScissor);
      m_flags.set(D3D9DeviceFlag::DirtyFFViewport);
    }

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetRenderTarget(
          DWORD               RenderTargetIndex,
          IDirect3DSurface9** ppRenderTarget) {
    D3D9DeviceLock lock = LockDevice();

    InitReturnPtr(ppRenderTarget);

    if (unlikely(ppRenderTarget == nullptr || RenderTargetIndex > caps::MaxSimultaneousRenderTargets))
      return D3DERR_INVALIDCALL;

    if (m_state.renderTargets[RenderTargetIndex] == nullptr)
      return D3DERR_NOTFOUND;

    *ppRenderTarget = ref(m_state.renderTargets[RenderTargetIndex]);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil) {
    D3D9DeviceLock lock = LockDevice();

    D3D9Surface* ds = static_cast<D3D9Surface*>(pNewZStencil);

    if (unlikely(ds && !(ds->GetCommonTexture()->Desc()->Usage & D3DUSAGE_DEPTHSTENCIL)))
      return D3DERR_INVALIDCALL;

    if (m_state.depthStencil == ds)
      return D3D_OK;

    FlushImplicit(FALSE);
    m_flags.set(D3D9DeviceFlag::DirtyFramebuffer);

    changePrivate(m_state.depthStencil, ds);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface) {
    D3D9DeviceLock lock = LockDevice();

    InitReturnPtr(ppZStencilSurface);

    if (unlikely(ppZStencilSurface == nullptr))
      return D3DERR_INVALIDCALL;

    if (m_state.depthStencil == nullptr)
      return D3DERR_NOTFOUND;

    *ppZStencilSurface = ref(m_state.depthStencil);

    return D3D_OK;
  }

  // The Begin/EndScene functions actually do nothing.
  // Some games don't even call them.

  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::BeginScene() {
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::EndScene() {
    FlushImplicit(true);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::Clear(
          DWORD    Count,
    const D3DRECT* pRects,
          DWORD    Flags,
          D3DCOLOR Color,
          float    Z,
          DWORD    Stencil) {
    D3D9DeviceLock lock = LockDevice();

    const auto& vp = m_state.viewport;
    const auto& sc = m_state.scissorRect;

    bool srgb      = m_state.renderStates[D3DRS_SRGBWRITEENABLE]   != FALSE;
    bool scissor   = m_state.renderStates[D3DRS_SCISSORTESTENABLE] != FALSE;

    VkOffset3D offset = { int32_t(vp.X),    int32_t(vp.Y),      0  };
    VkExtent3D extent = {         vp.Width,         vp.Height,  1u };

    if (scissor) {
      offset.x = std::max<int32_t> (offset.x, sc.left);
      offset.y = std::max<int32_t> (offset.y, sc.top);

      extent.width  = std::min<uint32_t>(extent.width,  sc.right  - offset.x);
      extent.height = std::min<uint32_t>(extent.height, sc.bottom - offset.y);
    }

    // This becomes pretty unreadable in one singular if statement...
    if (Count) {
      // If pRects is null, or our first rect encompasses the viewport:
      if (!pRects)
        Count = 0;
      else if (pRects[0].x1 <= offset.x                         && pRects[0].y1 <= offset.y
            && pRects[0].x2 >= offset.x + int32_t(extent.width) && pRects[0].y2 >= offset.y + int32_t(extent.height))
        Count = 0;
    }

    // Here, Count of 0 will denote whether or not to care about user rects.

    auto* rt0Desc = m_state.renderTargets[0]->GetCommonTexture()->Desc();

    VkClearValue clearValueDepth;
    clearValueDepth.depthStencil.depth   = Z;
    clearValueDepth.depthStencil.stencil = Stencil;

    VkClearValue clearValueColor;
    DecodeD3DCOLOR(Color, clearValueColor.color.float32);

    auto dsv = m_state.depthStencil != nullptr ? m_state.depthStencil->GetDepthStencilView() : nullptr;
    VkImageAspectFlags depthAspectMask = 0;
    if (dsv != nullptr) {
      if (Flags & D3DCLEAR_ZBUFFER)
        depthAspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;

      if (Flags & D3DCLEAR_STENCIL)
        depthAspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;

      depthAspectMask &= imageFormatInfo(dsv->info().format)->aspectMask;
    }

    auto ClearImageView = [this](
      bool               fullClear,
      VkOffset3D         offset,
      VkExtent3D         extent,
      Rc<DxvkImageView>  imageView,
      VkImageAspectFlags aspectMask,
      VkClearValue       clearValue) {
      if (fullClear) {
        EmitCs([
          cClearValue = clearValue,
          cAspectMask = aspectMask,
          cImageView  = imageView
        ] (DxvkContext* ctx) {
          ctx->clearRenderTarget(
            cImageView,
            cAspectMask,
            cClearValue);
        });
      }
      else {
        EmitCs([
          cClearValue = clearValue,
          cAspectMask = aspectMask,
          cImageView  = imageView,
          cOffset     = offset,
          cExtent     = extent
        ] (DxvkContext* ctx) {
          ctx->clearImageView(
            cImageView,
            cOffset, cExtent,
            cAspectMask,
            cClearValue);
        });
      }
    };

    auto ClearViewRect = [&](
      bool               fullClear,
      VkOffset3D         offset,
      VkExtent3D         extent) {
      // Clear depth if we need to.
      if (depthAspectMask != 0)
        ClearImageView(fullClear, offset, extent, dsv, depthAspectMask, clearValueDepth);

      // Clear render targets if we need to.
      if (Flags & D3DCLEAR_TARGET) {
        for (auto rt : m_state.renderTargets) {
          auto rtv = rt != nullptr ? rt->GetRenderTargetView(srgb) : nullptr;

          if (unlikely(rtv != nullptr))
            ClearImageView(fullClear, offset, extent, rtv, VK_IMAGE_ASPECT_COLOR_BIT, clearValueColor);
        }
      }
    };

    // A Hat in Time and other UE3 games only gets partial clears here
    // because of an oversized rt height due to their weird alignment...
    // This works around that.
    uint32_t alignment = m_d3d9Options.lenientClear ? 8 : 1;

    bool extentMatches = align(extent.width,  alignment) == align(rt0Desc->Width,  alignment)
                      && align(extent.height, alignment) == align(rt0Desc->Height, alignment);

    bool rtSizeMatchesClearSize = offset.x == 0 && offset.y == 0 && extentMatches;

    if (likely(!Count && rtSizeMatchesClearSize)) {
      // Fast path w/ ClearRenderTarget for when
      // our viewport and stencils match the RT size
      ClearViewRect(true, offset, extent);
    }
    else if (!Count) {
      // Clear our viewport & scissor minified region in this rendertarget.
      ClearViewRect(false, offset, extent);
    }
    else {
      // Clear the application provided rects.
      for (uint32_t i = 0; i < Count; i++) {
        VkOffset3D rectOffset = {
          std::max<int32_t>(pRects[i].x1, offset.x),
          std::max<int32_t>(pRects[i].y1, offset.y),
          0
        };

        VkExtent3D rectExtent = {
          std::min<uint32_t>(pRects[i].x2, offset.x + extent.width)  - rectOffset.x,
          std::min<uint32_t>(pRects[i].y2, offset.y + extent.height) - rectOffset.y,
          1u
        };

        ClearViewRect(false, rectOffset, rectExtent);
      }
    }

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) {
    return SetStateTransform(GetTransformIndex(State), pMatrix);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pMatrix == nullptr))
      return D3DERR_INVALIDCALL;

    *pMatrix = bit::cast<D3DMATRIX>(m_state.transforms[GetTransformIndex(State)]);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::MultiplyTransform(D3DTRANSFORMSTATETYPE TransformState, const D3DMATRIX* pMatrix) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(ShouldRecord()))
      return m_recorder->MultiplyStateTransform(TransformState, pMatrix);

    uint32_t idx = GetTransformIndex(TransformState);

    m_state.transforms[idx] = ConvertMatrix(pMatrix) * m_state.transforms[idx];

    m_flags.set(D3D9DeviceFlag::DirtyFFVertexData);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetViewport(const D3DVIEWPORT9* pViewport) {
    D3D9DeviceLock lock = LockDevice();

    D3DVIEWPORT9 viewport;
    if (pViewport == nullptr) {
      auto rtv = m_state.renderTargets[0]->GetRenderTargetView(false);

      viewport.X      = 0;
      viewport.Y      = 0;
      viewport.Width  = rtv->image()->info().extent.width;
      viewport.Height = rtv->image()->info().extent.height;
      viewport.MinZ   = 0.0f;
      viewport.MaxZ   = 1.0f;
    }
    else
      viewport = *pViewport;

    if (unlikely(ShouldRecord()))
      return m_recorder->SetViewport(&viewport);

    m_state.viewport = viewport;

    m_flags.set(D3D9DeviceFlag::DirtyViewportScissor);
    m_flags.set(D3D9DeviceFlag::DirtyFFViewport);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetViewport(D3DVIEWPORT9* pViewport) {
    D3D9DeviceLock lock = LockDevice();

    if (pViewport == nullptr)
      return D3DERR_INVALIDCALL;

    *pViewport = m_state.viewport;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetMaterial(const D3DMATERIAL9* pMaterial) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pMaterial == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(ShouldRecord()))
      return m_recorder->SetMaterial(pMaterial);

    m_state.material = *pMaterial;
    m_flags.set(D3D9DeviceFlag::DirtyFFVertexData);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetMaterial(D3DMATERIAL9* pMaterial) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pMaterial == nullptr))
      return D3DERR_INVALIDCALL;

    *pMaterial = m_state.material;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetLight(DWORD Index, const D3DLIGHT9* pLight) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pLight == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(ShouldRecord())) {
      Logger::warn("D3D9DeviceEx::SetLight: State block not implemented.");
      return D3D_OK;
    }

    if (Index >= m_state.lights.size())
      m_state.lights.resize(Index + 1);

    m_state.lights[Index] = *pLight;

    if (m_state.IsLightEnabled(Index))
      m_flags.set(D3D9DeviceFlag::DirtyFFVertexData);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetLight(DWORD Index, D3DLIGHT9* pLight) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pLight == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(Index >= m_state.lights.size() || !m_state.lights[Index]))
      return D3DERR_INVALIDCALL;

    *pLight = m_state.lights[Index].value();

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::LightEnable(DWORD Index, BOOL Enable) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(Index >= m_state.lights.size()))
      m_state.lights.resize(Index + 1);

    if (unlikely(!m_state.lights[Index]))
      m_state.lights[Index] = DefaultLight;

    uint32_t searchIndex = UINT32_MAX;
    uint32_t setIndex    = Index;

    if (!Enable)
      std::swap(searchIndex, setIndex);

    for (auto& idx : m_state.enabledLightIndices) {
      if (idx == searchIndex) {
        idx = setIndex;
        m_flags.set(D3D9DeviceFlag::DirtyFFVertexData);
        m_flags.set(D3D9DeviceFlag::DirtyFFVertexShader);
        break;
      }
    }

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetLightEnable(DWORD Index, BOOL* pEnable) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pEnable == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(Index >= m_state.lights.size() || !m_state.lights[Index]))
      return D3DERR_INVALIDCALL;

    *pEnable = m_state.IsLightEnabled(Index) ? 128 : 0; // Weird quirk but OK.

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetClipPlane(DWORD Index, const float* pPlane) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(Index >= caps::MaxClipPlanes || !pPlane))
      return D3DERR_INVALIDCALL;

    if (unlikely(ShouldRecord()))
      return m_recorder->SetClipPlane(Index, pPlane);
    
    bool dirty = false;
    
    for (uint32_t i = 0; i < 4; i++) {
      dirty |= m_state.clipPlanes[Index].coeff[i] != pPlane[i];
      m_state.clipPlanes[Index].coeff[i] = pPlane[i];
    }

    bool enabled = m_state.renderStates[D3DRS_CLIPPLANEENABLE] & (1u << Index);
    dirty &= enabled;
    
    if (dirty)
      m_flags.set(D3D9DeviceFlag::DirtyClipPlanes);
    
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetClipPlane(DWORD Index, float* pPlane) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(Index >= caps::MaxClipPlanes || !pPlane))
      return D3DERR_INVALIDCALL;
    
    for (uint32_t i = 0; i < 4; i++)
      pPlane[i] = m_state.clipPlanes[Index].coeff[i];
    
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) {
    D3D9DeviceLock lock = LockDevice();

    // D3D9 only allows reading for values 0 and 7-255 so we don't need to do anything but return OK
    if (unlikely(State > 255 || (State < D3DRS_ZENABLE && State != 0))) {
      return D3D_OK;
    }

    if (unlikely(ShouldRecord()))
      return m_recorder->SetRenderState(State, Value);

    auto& states = m_state.renderStates;

    bool changed = states[State] != Value;

    if (likely(changed)) {
      const bool oldATOC = IsAlphaToCoverageEnabled();

      // AMD's driver hack for ATOC and RESZ
      if (unlikely(State == D3DRS_POINTSIZE)) {
        // ATOC
        constexpr uint32_t AlphaToCoverageEnable  = MAKEFOURCC('A', '2', 'M', '1');
        constexpr uint32_t AlphaToCoverageDisable = MAKEFOURCC('A', '2', 'M', '0');

        if (Value == AlphaToCoverageEnable
         || Value == AlphaToCoverageDisable) {
          m_amdATOC = Value == AlphaToCoverageEnable;

          bool newATOC = IsAlphaToCoverageEnabled();

          if (oldATOC != newATOC)
            m_flags.set(D3D9DeviceFlag::DirtyMultiSampleState);

          return D3D_OK;
        }

        // RESZ
        constexpr uint32_t RESZ = 0x7fa05000;
        if (Value == RESZ) {
          ResolveZ();
          return D3D_OK;
        }
      }

      // NV's driver hack for ATOC.
      if (unlikely(State == D3DRS_ADAPTIVETESS_Y)) {
        constexpr uint32_t AlphaToCoverageEnable  = MAKEFOURCC('A', 'T', 'O', 'C');
        constexpr uint32_t AlphaToCoverageDisable = 0;

        if (Value == AlphaToCoverageEnable
         || Value == AlphaToCoverageDisable) {
          m_nvATOC = Value == AlphaToCoverageEnable;

          bool newATOC = IsAlphaToCoverageEnabled();
          
          if (oldATOC != newATOC)
            m_flags.set(D3D9DeviceFlag::DirtyMultiSampleState);

          return D3D_OK;
        }
      }

      states[State] = Value;

      switch (State) {
        case D3DRS_SEPARATEALPHABLENDENABLE:
        case D3DRS_ALPHABLENDENABLE:
        case D3DRS_BLENDOP:
        case D3DRS_BLENDOPALPHA:
        case D3DRS_DESTBLEND:
        case D3DRS_DESTBLENDALPHA:
        case D3DRS_COLORWRITEENABLE:
        case D3DRS_COLORWRITEENABLE1:
        case D3DRS_COLORWRITEENABLE2:
        case D3DRS_COLORWRITEENABLE3:
        case D3DRS_SRCBLEND:
        case D3DRS_SRCBLENDALPHA:
          m_flags.set(D3D9DeviceFlag::DirtyBlendState);
          break;
        
        case D3DRS_ALPHATESTENABLE: {
          bool newATOC = IsAlphaToCoverageEnabled();

          if (oldATOC != newATOC)
            m_flags.set(D3D9DeviceFlag::DirtyMultiSampleState);
        }
        case D3DRS_ALPHAFUNC:
          m_flags.set(D3D9DeviceFlag::DirtyAlphaTestState);
          break;

        case D3DRS_BLENDFACTOR:
          BindBlendFactor();
          break;

        case D3DRS_MULTISAMPLEMASK:
          if (m_flags.test(D3D9DeviceFlag::ValidSampleMask))
            m_flags.set(D3D9DeviceFlag::DirtyMultiSampleState);
          break;

        case D3DRS_ZENABLE:
        case D3DRS_ZFUNC:
        case D3DRS_TWOSIDEDSTENCILMODE:
        case D3DRS_ZWRITEENABLE:
        case D3DRS_STENCILENABLE:
        case D3DRS_STENCILFAIL:
        case D3DRS_STENCILZFAIL:
        case D3DRS_STENCILPASS:
        case D3DRS_STENCILFUNC:
        case D3DRS_CCW_STENCILFAIL:
        case D3DRS_CCW_STENCILZFAIL:
        case D3DRS_CCW_STENCILPASS:
        case D3DRS_CCW_STENCILFUNC:
        case D3DRS_STENCILMASK:
        case D3DRS_STENCILWRITEMASK:
          m_flags.set(D3D9DeviceFlag::DirtyDepthStencilState);
          break;

        case D3DRS_STENCILREF:
          BindDepthStencilRefrence();
          break;

        case D3DRS_SCISSORTESTENABLE:
          m_flags.set(D3D9DeviceFlag::DirtyViewportScissor);
          break;

        case D3DRS_SRGBWRITEENABLE:
          m_flags.set(D3D9DeviceFlag::DirtyFramebuffer);
          break;

        case D3DRS_DEPTHBIAS:
        case D3DRS_SLOPESCALEDEPTHBIAS:
        case D3DRS_CULLMODE:
        case D3DRS_FILLMODE:
          m_flags.set(D3D9DeviceFlag::DirtyRasterizerState);
          break;

        case D3DRS_CLIPPLANEENABLE:
          m_flags.set(D3D9DeviceFlag::DirtyClipPlanes);
          break;

        case D3DRS_ALPHAREF:
          UpdatePushConstant<D3D9RenderStateItem::AlphaRef>();
          break;

        case D3DRS_TEXTUREFACTOR:
          m_flags.set(D3D9DeviceFlag::DirtyFFPixelData);
          break;

        case D3DRS_DIFFUSEMATERIALSOURCE:
        case D3DRS_AMBIENTMATERIALSOURCE:
        case D3DRS_SPECULARMATERIALSOURCE:
        case D3DRS_EMISSIVEMATERIALSOURCE:
        case D3DRS_COLORVERTEX:
        case D3DRS_LIGHTING:
        case D3DRS_NORMALIZENORMALS:
          m_flags.set(D3D9DeviceFlag::DirtyFFVertexShader);
          break;

        case D3DRS_AMBIENT:
          m_flags.set(D3D9DeviceFlag::DirtyFFVertexData);
          break;

        default:
          static bool s_errorShown[256];

          if (!std::exchange(s_errorShown[State], true))
            Logger::warn(str::format("D3D9DeviceEx::SetRenderState: Unhandled render state ", State));
          break;
      }
    }

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pValue == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(State > 255 || (State < D3DRS_ZENABLE && State != 0))) {
      return D3DERR_INVALIDCALL;
    }

    if (State < D3DRS_ZENABLE || State > D3DRS_BLENDOPALPHA)
      *pValue = 0;
    else
      *pValue = m_state.renderStates[State];

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateStateBlock(
          D3DSTATEBLOCKTYPE      Type,
          IDirect3DStateBlock9** ppSB) {
    D3D9DeviceLock lock = LockDevice();

    InitReturnPtr(ppSB);

    if (unlikely(ppSB == nullptr))
      return D3DERR_INVALIDCALL;

    try {
      const Com<D3D9StateBlock> sb = new D3D9StateBlock(this, ConvertStateBlockType(Type));
      *ppSB = sb.ref();
      return D3D_OK;
    }
    catch (const DxvkError & e) {
      Logger::err(e.message());
      return D3DERR_INVALIDCALL;
    }
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::BeginStateBlock() {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(m_recorder != nullptr))
      return D3DERR_INVALIDCALL;

    m_recorder = new D3D9StateBlock(this, D3D9StateBlockType::None);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::EndStateBlock(IDirect3DStateBlock9** ppSB) {
    D3D9DeviceLock lock = LockDevice();

    InitReturnPtr(ppSB);

    if (unlikely(ppSB == nullptr || m_recorder == nullptr))
      return D3DERR_INVALIDCALL;

    *ppSB = m_recorder.ref();
    m_recorder = nullptr;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetClipStatus(const D3DCLIPSTATUS9* pClipStatus) {
    Logger::warn("D3D9DeviceEx::SetClipStatus: Stub");
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetClipStatus(D3DCLIPSTATUS9* pClipStatus) {
    Logger::warn("D3D9DeviceEx::GetClipStatus: Stub");
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture) {
    D3D9DeviceLock lock = LockDevice();

    if (ppTexture == nullptr)
      return D3DERR_INVALIDCALL;

    *ppTexture = nullptr;

    if (unlikely(InvalidSampler(Stage)))
      return D3D_OK;

    DWORD stateSampler = RemapSamplerState(Stage);

    *ppTexture = ref(m_state.textures[stateSampler]);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(InvalidSampler(Stage)))
      return D3D_OK;

    DWORD stateSampler = RemapSamplerState(Stage);

    return SetStateTexture(stateSampler, pTexture);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetTextureStageState(
          DWORD                    Stage,
          D3DTEXTURESTAGESTATETYPE Type,
          DWORD*                   pValue) {
    if (unlikely(pValue == nullptr))
      return D3DERR_INVALIDCALL;

    *pValue = 0;

    if (unlikely(Stage >= caps::TextureStageCount))
      return D3DERR_INVALIDCALL;

    if (unlikely(Type >= TextureStageStateCount))
      return D3DERR_INVALIDCALL;

    *pValue = m_state.textureStages[Stage][Type];

    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetTextureStageState(
          DWORD                    Stage,
          D3DTEXTURESTAGESTATETYPE Type,
          DWORD                    Value) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(Stage >= caps::TextureStageCount))
      return D3DERR_INVALIDCALL;

    if (unlikely(Type >= TextureStageStateCount))
      return D3DERR_INVALIDCALL;

    if (unlikely(ShouldRecord()))
      return m_recorder->SetTextureStageState(Stage, Type, Value);

    if (likely(m_state.textureStages[Stage][Type] != Value)) {
      if ((Type >= D3DTSS_BUMPENVMAT00  && Type <= D3DTSS_BUMPENVMAT11)
       || (Type == D3DTSS_BUMPENVLSCALE || Type == D3DTSS_BUMPENVLOFFSET))
        m_flags.set(D3D9DeviceFlag::DirtySharedPixelShaderData);
      else if (Type == D3DTSS_TEXTURETRANSFORMFLAGS) {
        // This state affects both!
        m_flags.set(D3D9DeviceFlag::DirtyFFPixelShader);
        m_flags.set(D3D9DeviceFlag::DirtyFFVertexShader);
      }
      else if (Type != D3DTSS_TEXCOORDINDEX)
        m_flags.set(D3D9DeviceFlag::DirtyFFPixelShader);
      else
        m_flags.set(D3D9DeviceFlag::DirtyFFVertexShader);
      m_state.textureStages[Stage][Type] = Value;
    }

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetSamplerState(
          DWORD               Sampler,
          D3DSAMPLERSTATETYPE Type,
          DWORD*              pValue) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pValue == nullptr))
      return D3DERR_INVALIDCALL;

    *pValue = 0;

    if (unlikely(InvalidSampler(Sampler)))
      return D3D_OK;

    Sampler = RemapSamplerState(Sampler);

    *pValue = m_state.samplerStates[Sampler][Type];

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetSamplerState(
          DWORD               Sampler,
          D3DSAMPLERSTATETYPE Type,
          DWORD               Value) {
    D3D9DeviceLock lock = LockDevice();
    if (unlikely(InvalidSampler(Sampler)))
      return D3D_OK;

    uint32_t stateSampler = RemapSamplerState(Sampler);

    return SetStateSamplerState(stateSampler, Type, Value);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::ValidateDevice(DWORD* pNumPasses) {
    if (pNumPasses != nullptr)
      *pNumPasses = 1;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetPaletteEntries(UINT PaletteNumber, const PALETTEENTRY* pEntries) {
    // This succeeds even though we don't advertise support.
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries) {
    // Don't advertise support for this...
    return D3DERR_INVALIDCALL;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetCurrentTexturePalette(UINT PaletteNumber) {
    // This succeeds even though we don't advertise support.
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetCurrentTexturePalette(UINT *PaletteNumber) {
    // Don't advertise support for this...
    return D3DERR_INVALIDCALL;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetScissorRect(const RECT* pRect) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pRect == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(ShouldRecord()))
      return m_recorder->SetScissorRect(pRect);

    m_state.scissorRect = *pRect;

    m_flags.set(D3D9DeviceFlag::DirtyViewportScissor);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetScissorRect(RECT* pRect) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pRect == nullptr))
      return D3DERR_INVALIDCALL;

    *pRect = m_state.scissorRect;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetSoftwareVertexProcessing(BOOL bSoftware) {
    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::warn("D3D9DeviceEx::SetSoftwareVertexProcessing: Stub");

    return D3D_OK;
  }


  BOOL    STDMETHODCALLTYPE D3D9DeviceEx::GetSoftwareVertexProcessing() {
    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::warn("D3D9DeviceEx::GetSoftwareVertexProcessing: Stub");

    return FALSE;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetNPatchMode(float nSegments) {
    return D3D_OK;
  }


  float   STDMETHODCALLTYPE D3D9DeviceEx::GetNPatchMode() {
    return 0.0f;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::DrawPrimitive(
          D3DPRIMITIVETYPE PrimitiveType,
          UINT             StartVertex,
          UINT             PrimitiveCount) {
    D3D9DeviceLock lock = LockDevice();

    PrepareDraw();

    EmitCs([this,
      cPrimType    = PrimitiveType,
      cPrimCount   = PrimitiveCount,
      cStartVertex = StartVertex,
      cInstanceCount = GetInstanceCount()
    ](DxvkContext* ctx) {
      auto drawInfo = GenerateDrawInfo(cPrimType, cPrimCount, cInstanceCount);

      ApplyPrimitiveType(ctx, cPrimType);

      ctx->draw(
        drawInfo.vertexCount, drawInfo.instanceCount,
        cStartVertex, 0);
    });

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::DrawIndexedPrimitive(
          D3DPRIMITIVETYPE PrimitiveType,
          INT              BaseVertexIndex,
          UINT             MinVertexIndex,
          UINT             NumVertices,
          UINT             StartIndex,
          UINT             PrimitiveCount) {
    D3D9DeviceLock lock = LockDevice();

    PrepareDraw();

    EmitCs([this,
      cPrimType        = PrimitiveType,
      cPrimCount       = PrimitiveCount,
      cStartIndex      = StartIndex,
      cBaseVertexIndex = BaseVertexIndex,
      cInstanceCount   = GetInstanceCount()
    ](DxvkContext* ctx) {
      auto drawInfo = GenerateDrawInfo(cPrimType, cPrimCount, cInstanceCount);

      ApplyPrimitiveType(ctx, cPrimType);

      ctx->drawIndexed(
        drawInfo.vertexCount, drawInfo.instanceCount,
        cStartIndex,
        cBaseVertexIndex, 0);
    });

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::DrawPrimitiveUP(
          D3DPRIMITIVETYPE PrimitiveType,
          UINT             PrimitiveCount,
    const void*            pVertexStreamZeroData,
          UINT             VertexStreamZeroStride) {
    D3D9DeviceLock lock = LockDevice();

    PrepareDraw(true);

    auto drawInfo = GenerateDrawInfo(PrimitiveType, PrimitiveCount, 0);

    const uint32_t upSize = drawInfo.vertexCount * VertexStreamZeroStride;

    auto upSlice = AllocUpBuffer(upSize);
    std::memcpy(upSlice.mapPtr, pVertexStreamZeroData, upSize);

    EmitCs([this,
      cBufferSlice  = std::move(upSlice.slice),
      cPrimType     = PrimitiveType,
      cPrimCount    = PrimitiveCount,
      cInstanceCount = GetInstanceCount(),
      cStride       = VertexStreamZeroStride
    ](DxvkContext* ctx) {
      auto drawInfo = GenerateDrawInfo(cPrimType, cPrimCount, cInstanceCount);

      ApplyPrimitiveType(ctx, cPrimType);

      ctx->bindVertexBuffer(0, cBufferSlice, cStride);
      ctx->draw(
        drawInfo.vertexCount, drawInfo.instanceCount,
        0, 0);
    });

    m_flags.set(D3D9DeviceFlag::UpDirtiedVertices);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::DrawIndexedPrimitiveUP(
          D3DPRIMITIVETYPE PrimitiveType,
          UINT             MinVertexIndex,
          UINT             NumVertices,
          UINT             PrimitiveCount,
    const void*            pIndexData,
          D3DFORMAT        IndexDataFormat,
    const void*            pVertexStreamZeroData,
          UINT             VertexStreamZeroStride) {
    D3D9DeviceLock lock = LockDevice();

    PrepareDraw(true);

    auto drawInfo = GenerateDrawInfo(PrimitiveType, PrimitiveCount, 0);

    const uint32_t vertexSize  = (MinVertexIndex + NumVertices) * VertexStreamZeroStride;

    const uint32_t indexSize = IndexDataFormat == D3DFMT_INDEX16 ? 2 : 4;
    const uint32_t indicesSize = drawInfo.vertexCount * indexSize;

    const uint32_t upSize = vertexSize + indicesSize;

    auto upSlice = AllocUpBuffer(upSize);
    uint8_t* data = reinterpret_cast<uint8_t*>(upSlice.mapPtr);

    std::memcpy(data, pVertexStreamZeroData, vertexSize);
    std::memcpy(data + vertexSize, pIndexData, indicesSize);

    EmitCs([this,
      cVertexSize   = vertexSize,
      cBufferSlice  = std::move(upSlice.slice),
      cPrimType     = PrimitiveType,
      cPrimCount    = PrimitiveCount,
      cStride       = VertexStreamZeroStride,
      cInstanceCount = GetInstanceCount(),
      cIndexType    = DecodeIndexType(
                        static_cast<D3D9Format>(IndexDataFormat))
    ](DxvkContext* ctx) {
      auto drawInfo = GenerateDrawInfo(cPrimType, cPrimCount, cInstanceCount);

      ApplyPrimitiveType(ctx, cPrimType);

      ctx->bindVertexBuffer(0, cBufferSlice.subSlice(0, cVertexSize), cStride);
      ctx->bindIndexBuffer(cBufferSlice.subSlice(cVertexSize, cBufferSlice.length() - cVertexSize), cIndexType);
      ctx->drawIndexed(
        drawInfo.vertexCount, drawInfo.instanceCount,
        0,
        0, 0);
    });

    m_flags.set(D3D9DeviceFlag::UpDirtiedVertices);
    m_flags.set(D3D9DeviceFlag::UpDirtiedIndices);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::ProcessVertices(
          UINT                         SrcStartIndex,
          UINT                         DestIndex,
          UINT                         VertexCount,
          IDirect3DVertexBuffer9*      pDestBuffer,
          IDirect3DVertexDeclaration9* pVertexDecl,
          DWORD                        Flags) {
    if (unlikely(pDestBuffer == nullptr || pVertexDecl == nullptr))
      return D3DERR_INVALIDCALL;

    D3D9CommonBuffer* dst  = static_cast<D3D9VertexBuffer*>(pDestBuffer)->GetCommonBuffer();
    D3D9VertexDecl*   decl = static_cast<D3D9VertexDecl*>  (pVertexDecl);

    PrepareDraw(false);

    if (decl == nullptr) {
      DWORD FVF = dst->Desc()->FVF;

      auto iter = m_fvfTable.find(FVF);

      if (iter == m_fvfTable.end()) {
        decl = new D3D9VertexDecl(this, FVF);
        m_fvfTable.insert(std::make_pair(FVF, decl));
      }
      else
        decl = iter->second.ptr();
    }

    uint32_t offset = DestIndex * decl->GetSize();

    auto slice = dst->GetBufferSlice<D3D9_COMMON_BUFFER_TYPE_REAL>();
         slice = slice.subSlice(offset, slice.length() - offset);

    EmitCs([this,
      cDecl          = ref(decl),
      cVertexCount   = VertexCount,
      cStartIndex    = SrcStartIndex,
      cInstanceCount = GetInstanceCount(),
      cBufferSlice   = slice,
      cIndexed       = m_state.indices != nullptr
    ](DxvkContext* ctx) {
      Rc<DxvkShader> shader = m_swvpEmulator.GetShaderModule(this, cDecl);

      auto drawInfo = GenerateDrawInfo(D3DPT_POINTLIST, cVertexCount, cInstanceCount);

      if (drawInfo.instanceCount != 1) {
        drawInfo.instanceCount = 1;

        Logger::warn("D3D9DeviceEx::ProcessVertices: instancing unsupported");
      }

      ApplyPrimitiveType(ctx, D3DPT_POINTLIST);

      ctx->bindShader(VK_SHADER_STAGE_GEOMETRY_BIT, shader);
      ctx->bindResourceBuffer(getSWVPBufferSlot(), cBufferSlice);
      ctx->draw(
        drawInfo.vertexCount, drawInfo.instanceCount,
        cStartIndex, 0);
      ctx->bindResourceBuffer(getSWVPBufferSlot(), DxvkBufferSlice());
      ctx->bindShader(VK_SHADER_STAGE_GEOMETRY_BIT, nullptr);
    });

    if (dst->GetMapMode() == D3D9_COMMON_BUFFER_MAP_MODE_BUFFER) {
      uint32_t copySize = VertexCount * decl->GetSize();

      EmitCs([
        cSrcBuffer = dst->GetBuffer<D3D9_COMMON_BUFFER_TYPE_REAL>(),
        cDstBuffer = dst->GetBuffer<D3D9_COMMON_BUFFER_TYPE_MAPPING>(),
        cOffset    = offset,
        cCopySize  = copySize
      ](DxvkContext* ctx) {
        ctx->copyBuffer(cDstBuffer, cOffset, cSrcBuffer, cOffset, cCopySize);
      });
    }

    dst->SetReadLocked(true);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateVertexDeclaration(
    const D3DVERTEXELEMENT9*            pVertexElements,
          IDirect3DVertexDeclaration9** ppDecl) {
    InitReturnPtr(ppDecl);

    if (unlikely(ppDecl == nullptr || pVertexElements == nullptr))
      return D3DERR_INVALIDCALL;

    const D3DVERTEXELEMENT9* counter = pVertexElements;
    while (counter->Stream != 0xFF)
      counter++;

    const uint32_t declCount = uint32_t(counter - pVertexElements);

    try {
      const Com<D3D9VertexDecl> decl = new D3D9VertexDecl(this, pVertexElements, declCount);
      *ppDecl = decl.ref();
      return D3D_OK;
    }
    catch (const DxvkError & e) {
      Logger::err(e.message());
      return D3DERR_INVALIDCALL;
    }
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl) {
    D3D9DeviceLock lock = LockDevice();

    D3D9VertexDecl* decl = static_cast<D3D9VertexDecl*>(pDecl);

    if (unlikely(ShouldRecord()))
      return m_recorder->SetVertexDeclaration(decl);

    if (decl == m_state.vertexDecl)
      return D3D_OK;

    bool dirtyFFShader = !decl || !m_state.vertexDecl;
    if (!dirtyFFShader)
      dirtyFFShader |= decl->TestFlag(D3D9VertexDeclFlag::HasPositionT)  != m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasPositionT)
                    || decl->TestFlag(D3D9VertexDeclFlag::HasColor0)     != m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasColor0)
                    || decl->TestFlag(D3D9VertexDeclFlag::HasColor1)     != m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasColor1);

    if (dirtyFFShader)
      m_flags.set(D3D9DeviceFlag::DirtyFFVertexShader);

    changePrivate(m_state.vertexDecl, decl);

    m_flags.set(D3D9DeviceFlag::DirtyInputLayout);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl) {
    D3D9DeviceLock lock = LockDevice();

    InitReturnPtr(ppDecl);

    if (ppDecl == nullptr)
      return D3D_OK;

    if (m_state.vertexDecl == nullptr)
      return D3DERR_NOTFOUND;

    *ppDecl = ref(m_state.vertexDecl);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetFVF(DWORD FVF) {
    D3D9DeviceLock lock = LockDevice();

    if (FVF == 0)
      return D3D_OK;

    D3D9VertexDecl* decl = nullptr;

    auto iter = m_fvfTable.find(FVF);

    if (iter == m_fvfTable.end()) {
      decl = new D3D9VertexDecl(this, FVF);
      m_fvfTable.insert(std::make_pair(FVF, decl));
    }
    else
      decl = iter->second.ptr();

    return this->SetVertexDeclaration(decl);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetFVF(DWORD* pFVF) {
    D3D9DeviceLock lock = LockDevice();

    if (pFVF == nullptr)
      return D3DERR_INVALIDCALL;

    *pFVF = m_state.vertexDecl != nullptr
      ? m_state.vertexDecl->GetFVF()
      : 0;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateVertexShader(
    const DWORD*                   pFunction,
          IDirect3DVertexShader9** ppShader) {
    InitReturnPtr(ppShader);

    if (unlikely(ppShader == nullptr))
      return D3DERR_INVALIDCALL;

    DxsoModuleInfo moduleInfo;
    moduleInfo.options = m_dxsoOptions;

    D3D9CommonShader module;

    if (FAILED(this->CreateShaderModule(&module,
      VK_SHADER_STAGE_VERTEX_BIT,
      pFunction,
      &moduleInfo)))
      return D3DERR_INVALIDCALL;

    *ppShader = ref(new D3D9VertexShader(this, module));

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetVertexShader(IDirect3DVertexShader9* pShader) {
    D3D9DeviceLock lock = LockDevice();

    D3D9VertexShader* shader = static_cast<D3D9VertexShader*>(pShader);

    if (unlikely(ShouldRecord()))
      return m_recorder->SetVertexShader(shader);

    if (shader == m_state.vertexShader)
      return D3D_OK;

    auto* oldShader = GetCommonShader(m_state.vertexShader);
    auto* newShader = GetCommonShader(shader);

    bool oldCopies = oldShader && oldShader->GetMeta().needsConstantCopies;
    bool newCopies = newShader && newShader->GetMeta().needsConstantCopies;

    m_consts[DxsoProgramTypes::VertexShader].dirty |= oldCopies || newCopies || !oldShader;
    m_consts[DxsoProgramTypes::VertexShader].meta  = newShader ? &newShader->GetMeta() : nullptr;

    if (newShader && oldShader) {
      m_consts[DxsoProgramTypes::VertexShader].dirty
        |= newShader->GetMeta().maxConstIndexF > oldShader->GetMeta().maxConstIndexF
        || newShader->GetMeta().maxConstIndexI > oldShader->GetMeta().maxConstIndexI
        || newShader->GetMeta().maxConstIndexB > oldShader->GetMeta().maxConstIndexB;
    }

    changePrivate(m_state.vertexShader, shader);

    if (shader != nullptr) {
      m_flags.clr(D3D9DeviceFlag::DirtyProgVertexShader);
      m_flags.set(D3D9DeviceFlag::DirtyFFVertexShader);

      BindShader(
        DxsoProgramTypes::VertexShader,
        GetCommonShader(shader));
    }

    m_flags.set(D3D9DeviceFlag::DirtyInputLayout);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetVertexShader(IDirect3DVertexShader9** ppShader) {
    D3D9DeviceLock lock = LockDevice();

    InitReturnPtr(ppShader);

    if (unlikely(ppShader == nullptr))
      return D3DERR_INVALIDCALL;

    *ppShader = ref(m_state.vertexShader);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetVertexShaderConstantF(
          UINT   StartRegister,
    const float* pConstantData,
          UINT   Vector4fCount) {
    D3D9DeviceLock lock = LockDevice();

    return SetShaderConstants<
      DxsoProgramTypes::VertexShader,
      D3D9ConstantType::Float>(
        StartRegister,
        pConstantData,
        Vector4fCount);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetVertexShaderConstantF(
          UINT   StartRegister,
          float* pConstantData,
          UINT   Vector4fCount) {
    D3D9DeviceLock lock = LockDevice();

    return GetShaderConstants<
      DxsoProgramTypes::VertexShader,
      D3D9ConstantType::Float>(
        StartRegister,
        pConstantData,
        Vector4fCount);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetVertexShaderConstantI(
          UINT StartRegister,
    const int* pConstantData,
          UINT Vector4iCount) {
    D3D9DeviceLock lock = LockDevice();

    return SetShaderConstants<
      DxsoProgramTypes::VertexShader,
      D3D9ConstantType::Int>(
        StartRegister,
        pConstantData,
        Vector4iCount);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetVertexShaderConstantI(
          UINT StartRegister,
          int* pConstantData,
          UINT Vector4iCount) {
    D3D9DeviceLock lock = LockDevice();

    return GetShaderConstants<
      DxsoProgramTypes::VertexShader,
      D3D9ConstantType::Int>(
        StartRegister,
        pConstantData,
        Vector4iCount);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetVertexShaderConstantB(
          UINT  StartRegister,
    const BOOL* pConstantData,
          UINT  BoolCount) {
    D3D9DeviceLock lock = LockDevice();

    return SetShaderConstants<
      DxsoProgramTypes::VertexShader,
      D3D9ConstantType::Bool>(
        StartRegister,
        pConstantData,
        BoolCount);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetVertexShaderConstantB(
          UINT  StartRegister,
          BOOL* pConstantData,
          UINT  BoolCount) {
    D3D9DeviceLock lock = LockDevice();

    return GetShaderConstants<
      DxsoProgramTypes::VertexShader,
      D3D9ConstantType::Bool>(
        StartRegister,
        pConstantData,
        BoolCount);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetStreamSource(
          UINT                    StreamNumber,
          IDirect3DVertexBuffer9* pStreamData,
          UINT                    OffsetInBytes,
          UINT                    Stride) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(StreamNumber >= caps::MaxStreams))
      return D3DERR_INVALIDCALL;

    D3D9VertexBuffer* buffer = static_cast<D3D9VertexBuffer*>(pStreamData);

    if (unlikely(ShouldRecord()))
      return m_recorder->SetStreamSource(
        StreamNumber,
        buffer,
        OffsetInBytes,
        Stride);

    auto& vbo = m_state.vertexBuffers[StreamNumber];
    bool needsUpdate = vbo.vertexBuffer != buffer;

    if (needsUpdate)
      changePrivate(vbo.vertexBuffer, buffer);

    needsUpdate |= vbo.offset != OffsetInBytes
                || vbo.stride != Stride;

    vbo.offset = OffsetInBytes;
    vbo.stride = Stride;

    if (needsUpdate)
      BindVertexBuffer(StreamNumber, buffer, OffsetInBytes, Stride);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetStreamSource(
          UINT                     StreamNumber,
          IDirect3DVertexBuffer9** ppStreamData,
          UINT*                    pOffsetInBytes,
          UINT*                    pStride) {
    D3D9DeviceLock lock = LockDevice();

    InitReturnPtr(ppStreamData);

    if (unlikely(pOffsetInBytes != nullptr))
      *pOffsetInBytes = 0;

    if (unlikely(pStride != nullptr))
      *pStride = 0;
    
    if (unlikely(ppStreamData == nullptr || pOffsetInBytes == nullptr || pStride == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(StreamNumber >= caps::MaxStreams))
      return D3DERR_INVALIDCALL;

    const auto& vbo = m_state.vertexBuffers[StreamNumber];

    *ppStreamData   = ref(vbo.vertexBuffer);
    *pOffsetInBytes = vbo.offset;
    *pStride        = vbo.stride;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetStreamSourceFreq(UINT StreamNumber, UINT Setting) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(StreamNumber >= caps::MaxStreams))
      return D3DERR_INVALIDCALL;

    const bool indexed  = Setting & D3DSTREAMSOURCE_INDEXEDDATA;
    const bool instanced = Setting & D3DSTREAMSOURCE_INSTANCEDATA;

    if (unlikely(StreamNumber == 0 && instanced))
      return D3DERR_INVALIDCALL;

    if (unlikely(instanced && indexed))
      return D3DERR_INVALIDCALL;

    if (unlikely(Setting == 0))
      return D3DERR_INVALIDCALL;

    if (unlikely(ShouldRecord()))
      return m_recorder->SetStreamSourceFreq(StreamNumber, Setting);

    if (m_state.streamFreq[StreamNumber] == Setting)
      return D3D_OK;

    m_state.streamFreq[StreamNumber] = Setting;

    if (instanced)
      m_instancedData |=   1u << StreamNumber;
    else
      m_instancedData &= ~(1u << StreamNumber);

    m_flags.set(D3D9DeviceFlag::DirtyInputLayout);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetStreamSourceFreq(UINT StreamNumber, UINT* pSetting) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(StreamNumber >= caps::MaxStreams))
      return D3DERR_INVALIDCALL;

    if (unlikely(pSetting == nullptr))
      return D3DERR_INVALIDCALL;

    *pSetting = m_state.streamFreq[StreamNumber];

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetIndices(IDirect3DIndexBuffer9* pIndexData) {
    D3D9DeviceLock lock = LockDevice();

    D3D9IndexBuffer* buffer = static_cast<D3D9IndexBuffer*>(pIndexData);

    if (unlikely(ShouldRecord()))
      return m_recorder->SetIndices(buffer);

    if (buffer == m_state.indices)
      return D3D_OK;

    changePrivate(m_state.indices, buffer);

    BindIndices();

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetIndices(IDirect3DIndexBuffer9** ppIndexData) {
    D3D9DeviceLock lock = LockDevice();
    InitReturnPtr(ppIndexData);

    if (unlikely(ppIndexData == nullptr))
      return D3DERR_INVALIDCALL;

    *ppIndexData = ref(m_state.indices);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreatePixelShader(
    const DWORD*                  pFunction,
          IDirect3DPixelShader9** ppShader) {
    InitReturnPtr(ppShader);

    if (unlikely(ppShader == nullptr))
      return D3DERR_INVALIDCALL;

    DxsoModuleInfo moduleInfo;
    moduleInfo.options = m_dxsoOptions;

    D3D9CommonShader module;

    if (FAILED(this->CreateShaderModule(&module,
      VK_SHADER_STAGE_FRAGMENT_BIT,
      pFunction,
      &moduleInfo)))
      return D3DERR_INVALIDCALL;

    *ppShader = ref(new D3D9PixelShader(this, module));

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetPixelShader(IDirect3DPixelShader9* pShader) {
    D3D9DeviceLock lock = LockDevice();

    D3D9PixelShader* shader = static_cast<D3D9PixelShader*>(pShader);

    if (unlikely(ShouldRecord()))
      return m_recorder->SetPixelShader(shader);

    if (shader == m_state.pixelShader)
      return D3D_OK;

    auto* oldShader = GetCommonShader(m_state.pixelShader);
    auto* newShader = GetCommonShader(shader);

    bool oldCopies = oldShader && oldShader->GetMeta().needsConstantCopies;
    bool newCopies = newShader && newShader->GetMeta().needsConstantCopies;

    m_consts[DxsoProgramTypes::PixelShader].dirty |= oldCopies || newCopies || !oldShader;
    m_consts[DxsoProgramTypes::PixelShader].meta  = newShader ? &newShader->GetMeta() : nullptr;

    if (newShader && oldShader) {
      m_consts[DxsoProgramTypes::PixelShader].dirty
        |= newShader->GetMeta().maxConstIndexF > oldShader->GetMeta().maxConstIndexF
        || newShader->GetMeta().maxConstIndexI > oldShader->GetMeta().maxConstIndexI
        || newShader->GetMeta().maxConstIndexB > oldShader->GetMeta().maxConstIndexB;
    }

    changePrivate(m_state.pixelShader, shader);

    if (shader != nullptr) {
      m_flags.set(D3D9DeviceFlag::DirtyFFPixelShader);

      BindShader(
        DxsoProgramTypes::PixelShader,
        GetCommonShader(shader));
    }

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetPixelShader(IDirect3DPixelShader9** ppShader) {
    D3D9DeviceLock lock = LockDevice();

    InitReturnPtr(ppShader);

    if (unlikely(ppShader == nullptr))
      return D3DERR_INVALIDCALL;

    *ppShader = ref(m_state.pixelShader);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetPixelShaderConstantF(
    UINT   StartRegister,
    const float* pConstantData,
    UINT   Vector4fCount) {
    D3D9DeviceLock lock = LockDevice();

    return SetShaderConstants <
      DxsoProgramTypes::PixelShader,
      D3D9ConstantType::Float>(
        StartRegister,
        pConstantData,
        Vector4fCount);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetPixelShaderConstantF(
    UINT   StartRegister,
    float* pConstantData,
    UINT   Vector4fCount) {
    D3D9DeviceLock lock = LockDevice();

    return GetShaderConstants<
      DxsoProgramTypes::PixelShader,
      D3D9ConstantType::Float>(
        StartRegister,
        pConstantData,
        Vector4fCount);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetPixelShaderConstantI(
    UINT StartRegister,
    const int* pConstantData,
    UINT Vector4iCount) {
    D3D9DeviceLock lock = LockDevice();

    return SetShaderConstants<
      DxsoProgramTypes::PixelShader,
      D3D9ConstantType::Int>(
        StartRegister,
        pConstantData,
        Vector4iCount);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetPixelShaderConstantI(
    UINT StartRegister,
    int* pConstantData,
    UINT Vector4iCount) {
    D3D9DeviceLock lock = LockDevice();

    return GetShaderConstants<
      DxsoProgramTypes::PixelShader,
      D3D9ConstantType::Int>(
        StartRegister,
        pConstantData,
        Vector4iCount);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetPixelShaderConstantB(
    UINT  StartRegister,
    const BOOL* pConstantData,
    UINT  BoolCount) {
    D3D9DeviceLock lock = LockDevice();

    return SetShaderConstants<
      DxsoProgramTypes::PixelShader,
      D3D9ConstantType::Bool>(
        StartRegister,
        pConstantData,
        BoolCount);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetPixelShaderConstantB(
    UINT  StartRegister,
    BOOL* pConstantData,
    UINT  BoolCount) {
    D3D9DeviceLock lock = LockDevice();

    return GetShaderConstants<
      DxsoProgramTypes::PixelShader,
      D3D9ConstantType::Bool>(
        StartRegister,
        pConstantData,
        BoolCount);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::DrawRectPatch(
          UINT               Handle,
    const float*             pNumSegs,
    const D3DRECTPATCH_INFO* pRectPatchInfo) {
    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::warn("D3D9DeviceEx::DrawRectPatch: Stub");
    return D3DERR_INVALIDCALL;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::DrawTriPatch(
          UINT              Handle,
    const float*            pNumSegs,
    const D3DTRIPATCH_INFO* pTriPatchInfo) {
    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::warn("D3D9DeviceEx::DrawTriPatch: Stub");
    return D3DERR_INVALIDCALL;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::DeletePatch(UINT Handle) {
    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::warn("D3D9DeviceEx::DeletePatch: Stub");
    return D3DERR_INVALIDCALL;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery) {
    InitReturnPtr(ppQuery);

    HRESULT hr = D3D9Query::QuerySupported(Type);

    if (ppQuery == nullptr || hr != D3D_OK)
      return hr;

    try {
      *ppQuery = ref(new D3D9Query(this, Type));
      return D3D_OK;
    }
    catch (const DxvkError & e) {
      Logger::err(e.message());
      return D3DERR_INVALIDCALL;
    }
  }


  // Ex Methods


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetConvolutionMonoKernel(
          UINT   width,
          UINT   height,
          float* rows,
          float* columns) {
    // We don't advertise support for this.
    return D3DERR_INVALIDCALL;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::ComposeRects(
          IDirect3DSurface9*      pSrc,
          IDirect3DSurface9*      pDst,
          IDirect3DVertexBuffer9* pSrcRectDescs,
          UINT                    NumRects,
          IDirect3DVertexBuffer9* pDstRectDescs,
          D3DCOMPOSERECTSOP       Operation,
          int                     Xoffset,
          int                     Yoffset) {
    Logger::warn("D3D9DeviceEx::ComposeRects: Stub");
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetGPUThreadPriority(INT* pPriority) {
    Logger::warn("D3D9DeviceEx::GetGPUThreadPriority: Stub");
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetGPUThreadPriority(INT Priority) {
    Logger::warn("D3D9DeviceEx::SetGPUThreadPriority: Stub");
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::WaitForVBlank(UINT iSwapChain) {
    D3D9DeviceLock lock = LockDevice();

    auto* swapchain = GetInternalSwapchain(iSwapChain);

    if (swapchain == nullptr)
      return D3DERR_INVALIDCALL;

    return swapchain->WaitForVBlank();
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CheckResourceResidency(IDirect3DResource9** pResourceArray, UINT32 NumResources) {
    Logger::warn("D3D9DeviceEx::CheckResourceResidency: Stub");
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::SetMaximumFrameLatency(UINT MaxLatency) {
    D3D9DeviceLock lock = LockDevice();

    if (MaxLatency == 0)
      MaxLatency = DefaultFrameLatency;

    if (MaxLatency > m_frameEvents.size())
      MaxLatency = m_frameEvents.size();

    m_frameLatency = MaxLatency;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetMaximumFrameLatency(UINT* pMaxLatency) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pMaxLatency == nullptr))
      return D3DERR_INVALIDCALL;

    *pMaxLatency = m_frameLatency;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CheckDeviceState(HWND hDestinationWindow) {
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::PresentEx(
    const RECT* pSourceRect,
    const RECT* pDestRect,
          HWND hDestWindowOverride,
    const RGNDATA* pDirtyRegion,
          DWORD dwFlags) {
    D3D9DeviceLock lock = LockDevice();

    auto* swapchain = GetInternalSwapchain(0);
    if (swapchain == nullptr)
      return D3DERR_INVALIDCALL;
    
    return swapchain->Present(
      pSourceRect,
      pDestRect,
      hDestWindowOverride,
      pDirtyRegion,
      dwFlags);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateRenderTargetEx(
          UINT                Width,
          UINT                Height,
          D3DFORMAT           Format,
          D3DMULTISAMPLE_TYPE MultiSample,
          DWORD               MultisampleQuality,
          BOOL                Lockable,
          IDirect3DSurface9** ppSurface,
          HANDLE*             pSharedHandle,
          DWORD               Usage) {
    InitReturnPtr(ppSurface);
    InitReturnPtr(pSharedHandle);

    if (unlikely(ppSurface == nullptr))
      return D3DERR_INVALIDCALL;

    D3D9_COMMON_TEXTURE_DESC desc;
    desc.Width              = Width;
    desc.Height             = Height;
    desc.Depth              = 1;
    desc.ArraySize          = 1;
    desc.MipLevels          = 1;
    desc.Usage              = Usage | D3DUSAGE_RENDERTARGET;
    desc.Format             = EnumerateFormat(Format);
    desc.Pool               = D3DPOOL_DEFAULT;
    desc.Discard            = FALSE;
    desc.MultiSample        = MultiSample;
    desc.MultisampleQuality = MultisampleQuality;

    if (FAILED(D3D9CommonTexture::NormalizeTextureProperties(&desc)))
      return D3DERR_INVALIDCALL;

    try {
      const Com<D3D9Surface> surface = new D3D9Surface(this, &desc);
      m_initializer->InitTexture(surface->GetCommonTexture());
      *ppSurface = surface.ref();
      return D3D_OK;
    }
    catch (const DxvkError& e) {
      Logger::err(e.message());
      return D3DERR_OUTOFVIDEOMEMORY;
    }
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateOffscreenPlainSurfaceEx(
          UINT                Width,
          UINT                Height,
          D3DFORMAT           Format,
          D3DPOOL             Pool,
          IDirect3DSurface9** ppSurface,
          HANDLE*             pSharedHandle,
          DWORD               Usage) {
    InitReturnPtr(ppSurface);
    InitReturnPtr(pSharedHandle);

    if (unlikely(ppSurface == nullptr))
      return D3DERR_INVALIDCALL;

    D3D9_COMMON_TEXTURE_DESC desc;
    desc.Width              = Width;
    desc.Height             = Height;
    desc.Depth              = 1;
    desc.ArraySize          = 1;
    desc.MipLevels          = 1;
    desc.Usage              = Usage;
    desc.Format             = EnumerateFormat(Format);
    desc.Pool               = Pool;
    desc.Discard            = FALSE;
    desc.MultiSample        = D3DMULTISAMPLE_NONE;
    desc.MultisampleQuality = 0;

    if (FAILED(D3D9CommonTexture::NormalizeTextureProperties(&desc)))
      return D3DERR_INVALIDCALL;

    try {
      const Com<D3D9Surface> surface = new D3D9Surface(this, &desc);
      m_initializer->InitTexture(surface->GetCommonTexture());
      *ppSurface = surface.ref();
      return D3D_OK;
    }
    catch (const DxvkError& e) {
      Logger::err(e.message());
      return D3DERR_OUTOFVIDEOMEMORY;
    }
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateDepthStencilSurfaceEx(
          UINT                Width,
          UINT                Height,
          D3DFORMAT           Format,
          D3DMULTISAMPLE_TYPE MultiSample,
          DWORD               MultisampleQuality,
          BOOL                Discard,
          IDirect3DSurface9** ppSurface,
          HANDLE*             pSharedHandle,
          DWORD               Usage) {
    InitReturnPtr(ppSurface);
    InitReturnPtr(pSharedHandle);

    if (unlikely(ppSurface == nullptr))
      return D3DERR_INVALIDCALL;

    D3D9_COMMON_TEXTURE_DESC desc;
    desc.Width              = Width;
    desc.Height             = Height;
    desc.Depth              = 1;
    desc.ArraySize          = 1;
    desc.MipLevels          = 1;
    desc.Usage              = Usage | D3DUSAGE_DEPTHSTENCIL;
    desc.Format             = EnumerateFormat(Format);
    desc.Pool               = D3DPOOL_DEFAULT;
    desc.Discard            = Discard;
    desc.MultiSample        = MultiSample;
    desc.MultisampleQuality = MultisampleQuality;

    if (FAILED(D3D9CommonTexture::NormalizeTextureProperties(&desc)))
      return D3DERR_INVALIDCALL;

    try {
      const Com<D3D9Surface> surface = new D3D9Surface(this, &desc);
      m_initializer->InitTexture(surface->GetCommonTexture());
      *ppSurface = surface.ref();
      return D3D_OK;
    }
    catch (const DxvkError& e) {
      Logger::err(e.message());
      return D3DERR_OUTOFVIDEOMEMORY;
    }
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::ResetEx(
          D3DPRESENT_PARAMETERS* pPresentationParameters,
          D3DDISPLAYMODEEX*      pFullscreenDisplayMode) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(pPresentationParameters == nullptr))
      return D3DERR_INVALIDCALL;

    D3D9Format backBufferFmt = EnumerateFormat(pPresentationParameters->BackBufferFormat);

    Logger::info(str::format(
      "D3D9DeviceEx::ResetEx:\n",
      "  Requested Presentation Parameters\n",
      "    - Width:              ", pPresentationParameters->BackBufferWidth, "\n",
      "    - Height:             ", pPresentationParameters->BackBufferHeight, "\n",
      "    - Format:             ", backBufferFmt, "\n"
      "    - Auto Depth Stencil: ", pPresentationParameters->EnableAutoDepthStencil ? "true" : "false", "\n",
      "                ^ Format: ", EnumerateFormat(pPresentationParameters->AutoDepthStencilFormat), "\n",
      "    - Windowed:           ", pPresentationParameters->Windowed ? "true" : "false", "\n"));

    if (backBufferFmt != D3D9Format::Unknown) {
      if (!IsSupportedBackBufferFormat(
        backBufferFmt,
        pPresentationParameters->Windowed))
        return D3DERR_INVALIDCALL;
    }

    SetDepthStencilSurface(nullptr);

    for (uint32_t i = 0; i < caps::MaxSimultaneousRenderTargets; i++)
      SetRenderTarget(0, nullptr);

    auto & rs = m_state.renderStates;

    rs[D3DRS_SEPARATEALPHABLENDENABLE] = FALSE;
    rs[D3DRS_ALPHABLENDENABLE]         = FALSE;
    rs[D3DRS_BLENDOP]                  = D3DBLENDOP_ADD;
    rs[D3DRS_BLENDOPALPHA]             = D3DBLENDOP_ADD;
    rs[D3DRS_DESTBLEND]                = D3DBLEND_ZERO;
    rs[D3DRS_DESTBLENDALPHA]           = D3DBLEND_ZERO;
    rs[D3DRS_COLORWRITEENABLE]         = 0x0000000f;
    rs[D3DRS_COLORWRITEENABLE1]        = 0x0000000f;
    rs[D3DRS_COLORWRITEENABLE2]        = 0x0000000f;
    rs[D3DRS_COLORWRITEENABLE3]        = 0x0000000f;
    rs[D3DRS_SRCBLEND]                 = D3DBLEND_ONE;
    rs[D3DRS_SRCBLENDALPHA]            = D3DBLEND_ONE;
    BindBlendState();

    rs[D3DRS_BLENDFACTOR]              = 0xffffffff;
    BindBlendFactor();

    rs[D3DRS_ZENABLE]                  = pPresentationParameters->EnableAutoDepthStencil != FALSE
                                       ? D3DZB_TRUE
                                       : D3DZB_FALSE;
    rs[D3DRS_ZFUNC]                    = D3DCMP_LESSEQUAL;
    rs[D3DRS_TWOSIDEDSTENCILMODE]      = FALSE;
    rs[D3DRS_ZWRITEENABLE]             = TRUE;
    rs[D3DRS_STENCILENABLE]            = FALSE;
    rs[D3DRS_STENCILFAIL]              = D3DSTENCILOP_KEEP;
    rs[D3DRS_STENCILZFAIL]             = D3DSTENCILOP_KEEP;
    rs[D3DRS_STENCILPASS]              = D3DSTENCILOP_KEEP;
    rs[D3DRS_STENCILFUNC]              = D3DCMP_ALWAYS;
    rs[D3DRS_CCW_STENCILFAIL]          = D3DSTENCILOP_KEEP;
    rs[D3DRS_CCW_STENCILZFAIL]         = D3DSTENCILOP_KEEP;
    rs[D3DRS_CCW_STENCILPASS]          = D3DSTENCILOP_KEEP;
    rs[D3DRS_CCW_STENCILFUNC]          = D3DCMP_ALWAYS;
    rs[D3DRS_STENCILMASK]              = 0xFFFFFFFF;
    rs[D3DRS_STENCILWRITEMASK]         = 0xFFFFFFFF;
    BindDepthStencilState();

    rs[D3DRS_STENCILREF] = 0;
    BindDepthStencilRefrence();

    rs[D3DRS_FILLMODE]            = D3DFILL_SOLID;
    rs[D3DRS_CULLMODE]            = D3DCULL_CCW;
    rs[D3DRS_DEPTHBIAS]           = bit::cast<DWORD>(0.0f);
    rs[D3DRS_SLOPESCALEDEPTHBIAS] = bit::cast<DWORD>(0.0f);
    BindRasterizerState();

    rs[D3DRS_SCISSORTESTENABLE]   = FALSE;

    rs[D3DRS_ALPHATESTENABLE]     = FALSE;
    rs[D3DRS_ALPHAFUNC]           = D3DCMP_ALWAYS;
    BindAlphaTestState();
    rs[D3DRS_ALPHAREF]            = 0;
    UpdatePushConstant<D3D9RenderStateItem::AlphaRef>();

    rs[D3DRS_MULTISAMPLEMASK]     = 0xffffffff;
    BindMultiSampleState();

    rs[D3DRS_TEXTUREFACTOR]       = 0xffffffff;
    m_flags.set(D3D9DeviceFlag::DirtyFFPixelData);
    
    rs[D3DRS_DIFFUSEMATERIALSOURCE]  = D3DMCS_COLOR1;
    rs[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
    rs[D3DRS_AMBIENTMATERIALSOURCE]  = D3DMCS_MATERIAL;
    rs[D3DRS_EMISSIVEMATERIALSOURCE] = D3DMCS_MATERIAL;
    rs[D3DRS_LIGHTING]               = TRUE;
    rs[D3DRS_COLORVERTEX]            = TRUE;
    m_flags.set(D3D9DeviceFlag::DirtyFFVertexShader);

    rs[D3DRS_AMBIENT]                = 0;
    m_flags.set(D3D9DeviceFlag::DirtyFFVertexData);

    // Render States not implemented beyond this point.
    rs[D3DRS_SHADEMODE]                  = D3DSHADE_GOURAUD;
    rs[D3DRS_LASTPIXEL]                  = TRUE;
    rs[D3DRS_DITHERENABLE]               = FALSE;
    rs[D3DRS_FOGENABLE]                  = FALSE;
    rs[D3DRS_SPECULARENABLE]             = FALSE;
    rs[D3DRS_FOGCOLOR]                   = 0;
    rs[D3DRS_FOGTABLEMODE]               = D3DFOG_NONE;
    rs[D3DRS_FOGSTART]                   = bit::cast<DWORD>(0.0f);
    rs[D3DRS_FOGEND]                     = bit::cast<DWORD>(1.0f);
    rs[D3DRS_FOGDENSITY]                 = bit::cast<DWORD>(1.0f);
    rs[D3DRS_RANGEFOGENABLE]             = FALSE;
    rs[D3DRS_WRAP0]                      = 0;
    rs[D3DRS_WRAP1]                      = 0;
    rs[D3DRS_WRAP2]                      = 0;
    rs[D3DRS_WRAP3]                      = 0;
    rs[D3DRS_WRAP4]                      = 0;
    rs[D3DRS_WRAP5]                      = 0;
    rs[D3DRS_WRAP6]                      = 0;
    rs[D3DRS_WRAP7]                      = 0;
    rs[D3DRS_CLIPPING]                   = TRUE;
    rs[D3DRS_FOGVERTEXMODE]              = D3DFOG_NONE;
    rs[D3DRS_LOCALVIEWER]                = TRUE;
    rs[D3DRS_NORMALIZENORMALS]           = FALSE;
    rs[D3DRS_VERTEXBLEND]                = D3DVBF_DISABLE;
    rs[D3DRS_CLIPPLANEENABLE]            = 0;
    rs[D3DRS_POINTSIZE]                  = bit::cast<DWORD>(1.0f);
    rs[D3DRS_POINTSIZE_MIN]              = bit::cast<DWORD>(1.0f);
    rs[D3DRS_POINTSPRITEENABLE]          = FALSE;
    rs[D3DRS_POINTSCALEENABLE]           = FALSE;
    rs[D3DRS_POINTSCALE_A]               = bit::cast<DWORD>(1.0f);
    rs[D3DRS_POINTSCALE_B]               = bit::cast<DWORD>(0.0f);
    rs[D3DRS_POINTSCALE_C]               = bit::cast<DWORD>(0.0f);
    rs[D3DRS_MULTISAMPLEANTIALIAS]       = TRUE;
    rs[D3DRS_PATCHEDGESTYLE]             = D3DPATCHEDGE_DISCRETE;
    rs[D3DRS_DEBUGMONITORTOKEN]          = D3DDMT_ENABLE;
    rs[D3DRS_POINTSIZE_MAX]              = bit::cast<DWORD>(64.0f);
    rs[D3DRS_INDEXEDVERTEXBLENDENABLE]   = FALSE;
    rs[D3DRS_TWEENFACTOR]                = bit::cast<DWORD>(0.0f);
    rs[D3DRS_POSITIONDEGREE]             = D3DDEGREE_CUBIC;
    rs[D3DRS_NORMALDEGREE]               = D3DDEGREE_LINEAR;
    rs[D3DRS_ANTIALIASEDLINEENABLE]      = FALSE;
    rs[D3DRS_MINTESSELLATIONLEVEL]       = bit::cast<DWORD>(1.0f);
    rs[D3DRS_MAXTESSELLATIONLEVEL]       = bit::cast<DWORD>(1.0f);
    rs[D3DRS_ADAPTIVETESS_X]             = bit::cast<DWORD>(0.0f);
    rs[D3DRS_ADAPTIVETESS_Y]             = bit::cast<DWORD>(0.0f);
    rs[D3DRS_ADAPTIVETESS_Z]             = bit::cast<DWORD>(1.0f);
    rs[D3DRS_ADAPTIVETESS_W]             = bit::cast<DWORD>(0.0f);
    rs[D3DRS_ENABLEADAPTIVETESSELLATION] = FALSE;
    rs[D3DRS_SRGBWRITEENABLE]            = 0;
    rs[D3DRS_WRAP8]                      = 0;
    rs[D3DRS_WRAP9]                      = 0;
    rs[D3DRS_WRAP10]                     = 0;
    rs[D3DRS_WRAP11]                     = 0;
    rs[D3DRS_WRAP12]                     = 0;
    rs[D3DRS_WRAP13]                     = 0;
    rs[D3DRS_WRAP14]                     = 0;
    rs[D3DRS_WRAP15]                     = 0;
    // End Unimplemented Render States

    for (uint32_t i = 0; i < caps::TextureStageCount; i++) {
      auto& stage = m_state.textureStages[i];

      stage[D3DTSS_COLOROP]               = i == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
      stage[D3DTSS_COLORARG1]             = D3DTA_TEXTURE;
      stage[D3DTSS_COLORARG2]             = D3DTA_CURRENT;
      stage[D3DTSS_ALPHAOP]               = i == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
      stage[D3DTSS_ALPHAARG1]             = D3DTA_TEXTURE;
      stage[D3DTSS_ALPHAARG2]             = D3DTA_CURRENT;
      stage[D3DTSS_BUMPENVMAT00]          = bit::cast<DWORD>(0.0f);
      stage[D3DTSS_BUMPENVMAT01]          = bit::cast<DWORD>(0.0f);
      stage[D3DTSS_BUMPENVMAT10]          = bit::cast<DWORD>(0.0f);
      stage[D3DTSS_BUMPENVMAT11]          = bit::cast<DWORD>(0.0f);
      stage[D3DTSS_TEXCOORDINDEX]         = i;
      stage[D3DTSS_BUMPENVLSCALE]         = bit::cast<DWORD>(0.0f);
      stage[D3DTSS_BUMPENVLOFFSET]        = bit::cast<DWORD>(0.0f);
      stage[D3DTSS_TEXTURETRANSFORMFLAGS] = D3DTTFF_DISABLE;
      stage[D3DTSS_COLORARG0]             = D3DTA_CURRENT;
      stage[D3DTSS_ALPHAARG0]             = D3DTA_CURRENT;
      stage[D3DTSS_RESULTARG]             = D3DTA_CURRENT;
      stage[D3DTSS_CONSTANT]              = 0x00000000;
    }
    m_flags.set(D3D9DeviceFlag::DirtySharedPixelShaderData);
    m_flags.set(D3D9DeviceFlag::DirtyFFPixelShader);

    for (uint32_t i = 0; i < caps::MaxStreams; i++)
      m_state.streamFreq[i] = 1;

    for (uint32_t i = 0; i < m_state.textures.size(); i++) {
      m_state.textures[i] = nullptr;

      DWORD sampler = i;
      auto samplerInfo = RemapStateSamplerShader(sampler);
      uint32_t slot = computeResourceSlotId(samplerInfo.first, DxsoBindingType::ColorImage, uint32_t(samplerInfo.second));

      EmitCs([
        cSlot = slot
      ](DxvkContext* ctx) {
        ctx->bindResourceView(cSlot, nullptr, nullptr);
      });
    }

    auto& ss = m_state.samplerStates;
    for (uint32_t i = 0; i < ss.size(); i++) {
      auto& state = ss[i];
      state[D3DSAMP_ADDRESSU]      = D3DTADDRESS_WRAP;
      state[D3DSAMP_ADDRESSV]      = D3DTADDRESS_WRAP;
      state[D3DSAMP_ADDRESSU]      = D3DTADDRESS_WRAP;
      state[D3DSAMP_ADDRESSW]      = D3DTADDRESS_WRAP;
      state[D3DSAMP_BORDERCOLOR]   = 0x00000000;
      state[D3DSAMP_MAGFILTER]     = D3DTEXF_POINT;
      state[D3DSAMP_MINFILTER]     = D3DTEXF_POINT;
      state[D3DSAMP_MIPFILTER]     = D3DTEXF_NONE;
      state[D3DSAMP_MIPMAPLODBIAS] = bit::cast<DWORD>(0.0f);
      state[D3DSAMP_MAXMIPLEVEL]   = 0;
      state[D3DSAMP_MAXANISOTROPY] = 1;
      state[D3DSAMP_SRGBTEXTURE]   = 0;
      state[D3DSAMP_ELEMENTINDEX]  = 0;
      state[D3DSAMP_DMAPOFFSET]    = 0;

      BindSampler(i);
    }

    m_dirtySamplerStates = 0;

    for (uint32_t i = 0; i < caps::MaxClipPlanes; i++) {
      float plane[4] = { 0, 0, 0, 0 };
      SetClipPlane(i, plane);
    }

    Flush();
    SynchronizeCsThread();

    HRESULT hr;
    auto* implicitSwapchain = GetInternalSwapchain(0);
    if (implicitSwapchain == nullptr) {
      Com<IDirect3DSwapChain9> swapchain;
      hr = CreateAdditionalSwapChainEx(pPresentationParameters, pFullscreenDisplayMode, &swapchain);
      if (FAILED(hr))
        throw DxvkError("Reset: failed to create implicit swapchain");
    }
    else {
      hr = implicitSwapchain->Reset(pPresentationParameters, pFullscreenDisplayMode);
      if (FAILED(hr))
        throw DxvkError("Reset: failed to reset swapchain");
    }

    Com<IDirect3DSurface9> backbuffer;
    hr = m_swapchains[0]->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
    if (FAILED(hr))
      throw DxvkError("Reset: failed to get implicit swapchain backbuffers");

    SetRenderTarget(0, backbuffer.ptr());

    if (pPresentationParameters->EnableAutoDepthStencil) {
      Com<D3D9Surface> autoDepthStencil;

      CreateDepthStencilSurface(
        pPresentationParameters->BackBufferWidth,
        pPresentationParameters->BackBufferHeight,
        pPresentationParameters->AutoDepthStencilFormat,
        pPresentationParameters->MultiSampleType,
        pPresentationParameters->MultiSampleQuality,
        FALSE,
        reinterpret_cast<IDirect3DSurface9**>(&autoDepthStencil),
        nullptr);

      m_autoDepthStencil = autoDepthStencil.ptr();

      SetDepthStencilSurface(m_autoDepthStencil.ptr());
    }

    // We should do this...
    m_flags.set(D3D9DeviceFlag::DirtyInputLayout);

    // Force this if we end up binding the same RT to make scissor change go into effect.
    BindViewportAndScissor();

    UpdateSamplerSpecConsant(0u);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::GetDisplayModeEx(
          UINT                iSwapChain,
          D3DDISPLAYMODEEX*   pMode,
          D3DDISPLAYROTATION* pRotation) {
    D3D9DeviceLock lock = LockDevice();

    auto* swapchain = GetInternalSwapchain(iSwapChain);

    if (unlikely(swapchain == nullptr))
      return D3DERR_INVALIDCALL;

    return swapchain->GetDisplayModeEx(pMode, pRotation);
  }


  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateAdditionalSwapChainEx(
          D3DPRESENT_PARAMETERS* pPresentationParameters,
    const D3DDISPLAYMODEEX*      pFullscreenDisplayMode,
          IDirect3DSwapChain9**  ppSwapChain) {
    D3D9DeviceLock lock = LockDevice();

    InitReturnPtr(ppSwapChain);

    if (ppSwapChain == nullptr || pPresentationParameters == nullptr)
      return D3DERR_INVALIDCALL;

    for (uint32_t i = 0; i < m_swapchains.size(); i++)
      GetInternalSwapchain(i)->Invalidate(pPresentationParameters->hDeviceWindow);

    auto* swapchain = new D3D9SwapChainEx(this, pPresentationParameters, pFullscreenDisplayMode);
    *ppSwapChain = ref(swapchain);

    m_swapchains.push_back(swapchain);
    swapchain->AddRefPrivate();

    return D3D_OK;
  }


  HRESULT D3D9DeviceEx::SetStateSamplerState(
    DWORD               StateSampler,
    D3DSAMPLERSTATETYPE Type,
    DWORD               Value) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(ShouldRecord()))
      return m_recorder->SetStateSamplerState(StateSampler, Type, Value);

    auto& state = m_state.samplerStates;

    bool changed = state[StateSampler][Type] != Value;

    if (likely(changed)) {
      state[StateSampler][Type] = Value;

      if (Type == D3DSAMP_ADDRESSU
       || Type == D3DSAMP_ADDRESSV
       || Type == D3DSAMP_ADDRESSW
       || Type == D3DSAMP_MAGFILTER
       || Type == D3DSAMP_MINFILTER
       || Type == D3DSAMP_MIPFILTER
       || Type == D3DSAMP_MAXANISOTROPY
       || Type == D3DSAMP_MIPMAPLODBIAS
       || Type == D3DSAMP_MAXMIPLEVEL
       || Type == D3DSAMP_BORDERCOLOR)
        m_dirtySamplerStates |= 1u << StateSampler;
      else if (Type == D3DSAMP_SRGBTEXTURE)
        BindTexture(StateSampler);
    }

    return D3D_OK;
  }


  HRESULT D3D9DeviceEx::SetStateTexture(DWORD StateSampler, IDirect3DBaseTexture9* pTexture) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(ShouldRecord()))
      return m_recorder->SetStateTexture(StateSampler, pTexture);

    if (m_state.textures[StateSampler] == pTexture)
      return D3D_OK;

    // We need to check our ops and disable respective stages.
    // Given we have transition from a null resource to
    // a valid resource or vice versa.
    if (pTexture == nullptr || m_state.textures[StateSampler] == nullptr)
      m_flags.set(D3D9DeviceFlag::DirtyFFPixelShader);
    
    TextureChangePrivate(m_state.textures[StateSampler], pTexture);

    BindTexture(StateSampler);

    return D3D_OK;
  }


  HRESULT D3D9DeviceEx::SetStateTransform(uint32_t idx, const D3DMATRIX* pMatrix) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(ShouldRecord()))
      return m_recorder->SetStateTransform(idx, pMatrix);

    m_state.transforms[idx] = ConvertMatrix(pMatrix);

    m_flags.set(D3D9DeviceFlag::DirtyFFVertexData);

    return D3D_OK;
  }


  bool D3D9DeviceEx::IsExtended() {
    return m_parent->IsExtended();
  }


  HWND D3D9DeviceEx::GetWindow() {
    return m_window;
  }


  Rc<sync::Signal> D3D9DeviceEx::GetFrameSyncEvent(UINT BufferCount) {
    uint32_t frameLatency = m_frameLatency;

    if (BufferCount != 0
     && BufferCount <= frameLatency)
      frameLatency = BufferCount;

    if (m_frameLatencyCap != 0
      && m_frameLatencyCap <= frameLatency)
      frameLatency = m_frameLatencyCap;

    uint32_t frameId = m_frameId++ % frameLatency;
    return m_frameEvents[frameId];
  }


  DxvkDeviceFeatures D3D9DeviceEx::GetDeviceFeatures(const Rc<DxvkAdapter>& adapter) {
    DxvkDeviceFeatures supported = adapter->features();
    DxvkDeviceFeatures enabled = {};

    // Geometry shaders are used for some meta ops
    enabled.core.features.geometryShader = VK_TRUE;
    enabled.core.features.robustBufferAccess = VK_TRUE;

    enabled.extMemoryPriority.memoryPriority = supported.extMemoryPriority.memoryPriority;

    enabled.extShaderDemoteToHelperInvocation.shaderDemoteToHelperInvocation = supported.extShaderDemoteToHelperInvocation.shaderDemoteToHelperInvocation;

    enabled.extVertexAttributeDivisor.vertexAttributeInstanceRateDivisor = supported.extVertexAttributeDivisor.vertexAttributeInstanceRateDivisor;
    enabled.extVertexAttributeDivisor.vertexAttributeInstanceRateZeroDivisor = supported.extVertexAttributeDivisor.vertexAttributeInstanceRateZeroDivisor;

    // ProcessVertices
    enabled.core.features.vertexPipelineStoresAndAtomics = VK_TRUE;

    // DXVK Meta
    enabled.core.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    enabled.core.features.shaderStorageImageExtendedFormats    = VK_TRUE;

    enabled.core.features.imageCubeArray = VK_TRUE;

    // SM1 level hardware
    enabled.core.features.depthClamp = VK_TRUE;
    enabled.core.features.depthBiasClamp = VK_TRUE;
    enabled.core.features.fillModeNonSolid = VK_TRUE;
    enabled.core.features.pipelineStatisticsQuery = supported.core.features.pipelineStatisticsQuery;
    enabled.core.features.sampleRateShading = VK_TRUE;
    enabled.core.features.samplerAnisotropy = VK_TRUE;
    enabled.core.features.shaderClipDistance = VK_TRUE;
    enabled.core.features.shaderCullDistance = VK_TRUE;

    // Ensure we support real BC formats and unofficial vendor ones.
    enabled.core.features.textureCompressionBC = VK_TRUE;

    enabled.extDepthClipEnable.depthClipEnable = supported.extDepthClipEnable.depthClipEnable;
    enabled.extHostQueryReset.hostQueryReset = supported.extHostQueryReset.hostQueryReset;

    // SM2 level hardware
    enabled.core.features.occlusionQueryPrecise = VK_TRUE;

    // SM3 level hardware
    enabled.core.features.multiViewport = VK_TRUE;
    enabled.core.features.independentBlend = VK_TRUE;

    // D3D10 level hardware supports this in D3D9 native.
    enabled.core.features.fullDrawIndexUint32 = VK_TRUE;

    return enabled;
  }


  D3D9UPBufferSlice D3D9DeviceEx::AllocUpBuffer(VkDeviceSize size) {
    constexpr VkDeviceSize DefaultSize = 1 << 20;

    constexpr VkMemoryPropertyFlags memoryFlags
      = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
      | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    if (size <= DefaultSize) {
      if (unlikely(!m_upBuffer.slice.defined())) {
        DxvkBufferCreateInfo info;
        info.size   = DefaultSize;
        info.usage  = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                    | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        info.access = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT
                    | VK_ACCESS_INDEX_READ_BIT;
        info.stages = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;

        m_upBuffer.slice  = DxvkBufferSlice(m_dxvkDevice->createBuffer(info, memoryFlags));
        m_upBuffer.mapPtr = m_upBuffer.slice.mapPtr(0);
      } else if (unlikely(m_upBuffer.slice.length() < size)) {
        auto physSlice = m_upBuffer.slice.buffer()->allocSlice();

        m_upBuffer.slice  = DxvkBufferSlice(m_upBuffer.slice.buffer());
        m_upBuffer.mapPtr = physSlice.mapPtr;

        EmitCs([
          cBuffer = m_upBuffer.slice.buffer(),
          cSlice  = physSlice
        ] (DxvkContext* ctx) {
          ctx->invalidateBuffer(cBuffer, cSlice);
        });
      }

      D3D9UPBufferSlice result;
      result.slice  = m_upBuffer.slice.subSlice(0, size);
      result.mapPtr = reinterpret_cast<char*>(m_upBuffer.mapPtr) + m_upBuffer.slice.offset();

      VkDeviceSize adjust = align(size, CACHE_LINE_SIZE);
      m_upBuffer.slice = m_upBuffer.slice.subSlice(adjust, m_upBuffer.slice.length() - adjust);
      return result;
    } else {
      // Create a temporary buffer for very large allocations
      DxvkBufferCreateInfo info;
      info.size   = size;
      info.usage  = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                  | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
      info.access = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT
                  | VK_ACCESS_INDEX_READ_BIT;
      info.stages = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;

      D3D9UPBufferSlice result;
      result.slice  = DxvkBufferSlice(m_dxvkDevice->createBuffer(info, memoryFlags));
      result.mapPtr = result.slice.mapPtr(0);
      return result;
    }
  }


  D3D9SwapChainEx* D3D9DeviceEx::GetInternalSwapchain(UINT index) {
    if (unlikely(index >= m_swapchains.size()))
      return nullptr;

    return static_cast<D3D9SwapChainEx*>(m_swapchains[index]);
  }


  bool D3D9DeviceEx::ShouldRecord() {
    return m_recorder != nullptr && !m_recorder->IsApplying();
  }


  D3D9_VK_FORMAT_MAPPING D3D9DeviceEx::LookupFormat(
    D3D9Format            Format) const {
    return m_adapter->GetFormatMapping(Format);
  }

  bool D3D9DeviceEx::WaitForResource(
  const Rc<DxvkResource>&                 Resource,
        DWORD                             MapFlags) {
    // Wait for the any pending D3D9 command to be executed
    // on the CS thread so that we can determine whether the
    // resource is currently in use or not.

    SynchronizeCsThread();

    if (Resource->isInUse()) {
      if (MapFlags & D3DLOCK_DONOTWAIT) {
        // We don't have to wait, but misbehaving games may
        // still try to spin on `Map` until the resource is
        // idle, so we should flush pending commands
        FlushImplicit(FALSE);
        return false;
      }
      else {
        // Make sure pending commands using the resource get
        // executed on the the GPU if we have to wait for it
        Flush();
        SynchronizeCsThread();

        while (Resource->isInUse())
          dxvk::this_thread::yield();
      }
    }

    return true;
  }


  uint32_t D3D9DeviceEx::CalcImageLockOffset(
            uint32_t                SlicePitch,
            uint32_t                RowPitch,
      const DxvkFormatInfo*         FormatInfo,
      const D3DBOX*                 pBox) {
    if (pBox == nullptr)
      return 0;

    uint32_t rowOffset;
    if (FormatInfo != nullptr) {
      uint32_t blockSize  = uint32_t(FormatInfo->blockSize.width);
      uint32_t blockCount = (pBox->Left + blockSize - 1) / blockSize;
      rowOffset = uint32_t(FormatInfo->elementSize) * blockCount;
    }
    else
      rowOffset = pBox->Left;

    return pBox->Front * SlicePitch +
           pBox->Top   * RowPitch   +
           rowOffset;
  }


  HRESULT D3D9DeviceEx::LockImage(
            D3D9CommonTexture*      pResource,
            UINT                    Face,
            UINT                    MipLevel,
            D3DLOCKED_BOX*          pLockedBox,
      const D3DBOX*                 pBox,
            DWORD                   Flags) {
    D3D9DeviceLock lock = LockDevice();

    UINT Subresource = pResource->CalcSubresource(Face, MipLevel);

    // Don't allow multiple lockings.
    if (unlikely(pResource->MarkLocked(Subresource, true)))
      return D3DERR_INVALIDCALL;

    if (unlikely((Flags & D3DLOCK_DISCARD) && (Flags & D3DLOCK_READONLY)))
      return D3DERR_INVALIDCALL;

    if (unlikely(!m_d3d9Options.allowLockFlagReadonly))
      Flags &= ~D3DLOCK_READONLY;

    if (unlikely((Flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) == (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)))
      Flags &= ~D3DLOCK_DISCARD;

    auto& desc = *(pResource->Desc());

    bool alloced = pResource->CreateBufferSubresource(Subresource);

    const Rc<DxvkBuffer> mappedBuffer = pResource->GetBuffer(Subresource);

    auto formatInfo = imageFormatInfo(pResource->Format());
    auto subresource = pResource->GetSubresourceFromIndex(
        formatInfo->aspectMask, Subresource);

    VkExtent3D levelExtent = pResource->GetExtentMip(MipLevel);
    VkExtent3D blockCount  = util::computeBlockCount(levelExtent, formatInfo->blockSize);
    
    const bool systemmem = desc.Pool == D3DPOOL_SYSTEMMEM;
    const bool managed   = IsPoolManaged(desc.Pool);
    const bool scratch   = desc.Pool == D3DPOOL_SCRATCH;

    bool modified = pResource->GetSystemMemGPUModified();

    bool fullResource = pBox == nullptr;
    if (unlikely(!fullResource)) {
      VkOffset3D lockOffset;
      VkExtent3D lockExtent;

      ConvertBox(*pBox, lockOffset, lockExtent);

      fullResource = lockOffset == VkOffset3D{ 0, 0, 0 }
                  && lockExtent.width  >= levelExtent.width
                  && lockExtent.height >= levelExtent.height
                  && lockExtent.depth  >= levelExtent.depth;
    }

    // Discard is ignored if the resource is not dynamic

    // If we are not locking the entire image
    // a partial discard is meant to occur.
    // We can't really implement that, so just ignore discard
    // if we are not locking the full resource

    if (!(desc.Usage & D3DUSAGE_DYNAMIC) || !fullResource)
      Flags &= ~D3DLOCK_DISCARD;

    pResource->SetLockFlags(Subresource, Flags);
      
    DxvkBufferSliceHandle physSlice;
      
    if (Flags & D3DLOCK_DISCARD) {
      // We do not have to preserve the contents of the
      // buffer if the entire image gets discarded.
      physSlice = pResource->DiscardMapSlice(Subresource);

      EmitCs([
        cImageBuffer = mappedBuffer,
        cBufferSlice = physSlice
      ] (DxvkContext* ctx) {
        ctx->invalidateBuffer(cImageBuffer, cBufferSlice);
      });
    }
    else if (managed || scratch || systemmem) {
      // Managed and scratch resources
      // are meant to be able to provide readback without waiting.
      // We always keep a copy of them in system memory for this reason.
      // No need to wait as its not in use.
      physSlice = pResource->GetMappedSlice(Subresource);

      // We do not need to wait for the resource in the event the
      // calling app promises not to overwrite data that is in use
      // or is reading. Remember! This will only trigger for MANAGED resources
      // that cannot get affected by GPU, therefore readonly is A-OK for NOT waiting.
      const bool noOverwrite  = Flags & D3DLOCK_NOOVERWRITE;
      const bool readOnly     = Flags & D3DLOCK_READONLY;
      const bool skipWait = (readOnly && managed) || scratch || (readOnly && systemmem && !modified);

      if (alloced)
        std::memset(physSlice.mapPtr, 0, physSlice.length);
      else if (!noOverwrite && !skipWait) {
        pResource->UnmarkSystemMemGPUModified();
        if (!WaitForResource(mappedBuffer, Flags))
          return D3DERR_WASSTILLDRAWING;
      }
    }
    else {
      const Rc<DxvkImage>  mappedImage = pResource->GetImage();

      // When using any map mode which requires the image contents
      // to be preserved, and if the GPU has write access to the
      // image, copy the current image contents into the buffer.
      auto subresourceLayers = vk::makeSubresourceLayers(subresource);

      EmitCs([
        cImageBuffer  = mappedBuffer,
        cImage        = mappedImage,
        cSubresources = subresourceLayers,
        cLevelExtent  = levelExtent
      ] (DxvkContext* ctx) {
        ctx->copyImageToBuffer(
          cImageBuffer, 0, VkExtent2D { 0u, 0u },
          cImage, cSubresources, VkOffset3D { 0, 0, 0 },
          cLevelExtent);
      });

      // We can't implement NOOVERWRITE for this path
      // because of the copyImageToBuffer above,
      // and the fact that we are a backed image.
      if (!WaitForResource(mappedBuffer, Flags))
        return D3DERR_WASSTILLDRAWING;
      physSlice = mappedBuffer->getSliceHandle();
    }

    const bool atiHack = desc.Format == D3D9Format::ATI1 || desc.Format == D3D9Format::ATI2;
    // Set up map pointer.
    if (atiHack) {
      // We need to lie here. The game is expected to use this info and do a workaround.
      // It's stupid. I know.
      pLockedBox->RowPitch   = std::max(desc.Width >> MipLevel, 1u);
      pLockedBox->SlicePitch = pLockedBox->RowPitch * std::max(desc.Height >> MipLevel, 1u);
    }
    else {
      // Data is tightly packed within the mapped buffer.
      pLockedBox->RowPitch   = formatInfo->elementSize * blockCount.width;
      pLockedBox->SlicePitch = formatInfo->elementSize * blockCount.width * blockCount.height;
    }

    const uint32_t offset = CalcImageLockOffset(
      pLockedBox->SlicePitch,
      pLockedBox->RowPitch,
      !atiHack ? formatInfo : nullptr,
      pBox);

    uint8_t* data = reinterpret_cast<uint8_t*>(physSlice.mapPtr);
    data += offset;
    pLockedBox->pBits = data;
    return D3D_OK;
  }


  HRESULT D3D9DeviceEx::UnlockImage(
        D3D9CommonTexture*      pResource,
        UINT                    Face,
        UINT                    MipLevel) {
    D3D9DeviceLock lock = LockDevice();

    UINT Subresource = pResource->CalcSubresource(Face, MipLevel);

    // We weren't locked anyway!
    if (unlikely(!pResource->MarkLocked(Subresource, false)))
      return D3DERR_INVALIDCALL;

    // Do we have a pending copy?
    if (!(pResource->GetLockFlags(Subresource) & D3DLOCK_READONLY)) {
      // Only flush buffer -> image if we actually have an image
      if (pResource->GetMapMode() == D3D9_COMMON_TEXTURE_MAP_MODE_BACKED)
        this->FlushImage(pResource, Subresource);
    }

    if (pResource->GetMapMode() == D3D9_COMMON_TEXTURE_MAP_MODE_BACKED
    && (!pResource->IsManaged() || m_d3d9Options.evictManagedOnUnlock))
      pResource->DestroyBufferSubresource(Subresource);

    if (pResource->IsAutomaticMip())
      GenerateMips(pResource);

    return D3D_OK;
  }


  HRESULT D3D9DeviceEx::FlushImage(
        D3D9CommonTexture*      pResource,
        UINT                    Subresource) {
    const Rc<DxvkImage>  image = pResource->GetImage();

    // Now that data has been written into the buffer,
    // we need to copy its contents into the image
    const Rc<DxvkBuffer> copyBuffer = pResource->GetBuffer(Subresource);

    auto formatInfo  = imageFormatInfo(image->info().format);
    auto subresource = pResource->GetSubresourceFromIndex(
      formatInfo->aspectMask, Subresource);

    VkExtent3D levelExtent = image
      ->mipLevelExtent(subresource.mipLevel);

    VkImageSubresourceLayers subresourceLayers = {
      subresource.aspectMask,
      subresource.mipLevel,
      subresource.arrayLayer, 1 };

    EmitCs([
      cSrcBuffer      = copyBuffer,
      cDstImage       = image,
      cDstLayers      = subresourceLayers,
      cDstLevelExtent = levelExtent
    ] (DxvkContext* ctx) {
      ctx->copyBufferToImage(cDstImage, cDstLayers,
        VkOffset3D{ 0, 0, 0 }, cDstLevelExtent,
        cSrcBuffer, 0, { 0u, 0u });
    });

    return D3D_OK;
  }


  void D3D9DeviceEx::GenerateMips(
    D3D9CommonTexture* pResource) {
    EmitCs([
      cImageView = pResource->GetViews().MipGenRT
    ] (DxvkContext* ctx) {
      ctx->generateMipmaps(cImageView);
    });
  }


  HRESULT D3D9DeviceEx::LockBuffer(
          D3D9CommonBuffer*       pResource,
          UINT                    OffsetToLock,
          UINT                    SizeToLock,
          void**                  ppbData,
          DWORD                   Flags) {
    D3D9DeviceLock lock = LockDevice();

    if (unlikely(ppbData == nullptr))
      return D3DERR_INVALIDCALL;

    if (!m_d3d9Options.allowLockFlagReadonly)
      Flags &= ~D3DLOCK_READONLY;

    auto& desc = *pResource->Desc();

    // Ignore DISCARD if NOOVERWRITE is set
    if ((Flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) == (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE))
      Flags &= ~D3DLOCK_DISCARD;

    // Ignore DISCARD/NOOVERWRITE if the buffer is non-dynamic.
    if (!(desc.Usage & D3DUSAGE_DYNAMIC))
      Flags &= ~(D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE);

    pResource->SetMapFlags(Flags);
    Rc<DxvkBuffer> mappingBuffer = pResource->GetBuffer<D3D9_COMMON_BUFFER_TYPE_MAPPING>();

    DxvkBufferSliceHandle physSlice;

    const UINT maxLockSize = desc.Size - OffsetToLock;

    // We can only respect this for these cases -- otherwise R/W OOB still get copied on native
    // and some stupid games depend on that.
    bool respectBounds = (desc.Usage & D3DUSAGE_DYNAMIC) || IsPoolManaged(desc.Pool);

    if (SizeToLock == 0 || !respectBounds)
      SizeToLock = maxLockSize;
    else
      SizeToLock = std::min(SizeToLock, maxLockSize);

    pResource->LockRange() = D3D9Range(OffsetToLock, OffsetToLock + SizeToLock);

    if (Flags & D3DLOCK_DISCARD) {
      // Allocate a new backing slice for the buffer and set
      // it as the 'new' mapped slice. This assumes that the
      // only way to invalidate a buffer is by mapping it.
      physSlice = pResource->DiscardMapSlice();

      EmitCs([
        cBuffer      = std::move(mappingBuffer),
        cBufferSlice = physSlice
      ] (DxvkContext* ctx) {
        ctx->invalidateBuffer(cBuffer, cBufferSlice);
      });
    }
    else {
      // NOOVERWRITE promises that they will not write in a currently used area.
      // READONLY promises they will only read -- and we never write to these buffers from the GPU
      // Therefore we can skip waiting for these two cases.
      bool dirtyRangeOverlap = true;

      if (respectBounds && pResource->GetMapMode() == D3D9_COMMON_BUFFER_MAP_MODE_BUFFER && !(Flags & D3DLOCK_READONLY))
        dirtyRangeOverlap = pResource->DirtyRange().overlap(pResource->LockRange());

      bool readLocked = pResource->SetReadLocked(false);

      bool skipWait = (Flags & D3DLOCK_NOOVERWRITE) || ( (Flags & D3DLOCK_READONLY) && !readLocked ) || !dirtyRangeOverlap;

      // Wait until the resource is no longer in use
      if (!skipWait) {
        if (!(Flags & D3DLOCK_DONOTWAIT))
          pResource->DirtyRange().clear();

        if (!WaitForResource(mappingBuffer, Flags))
          return D3DERR_WASSTILLDRAWING;
      }

      // Use map pointer from previous map operation. This
      // way we don't have to synchronize with the CS thread
      // if the map mode is D3DLOCK_NOOVERWRITE.
      physSlice = pResource->GetMappedSlice();
    }

    uint8_t* data  = reinterpret_cast<uint8_t*>(physSlice.mapPtr);
             data += OffsetToLock;

    *ppbData = reinterpret_cast<void*>(data);

    return D3D_OK;
  }


  HRESULT D3D9DeviceEx::UnlockBuffer(
        D3D9CommonBuffer*       pResource) {
    if (pResource->GetMapMode() != D3D9_COMMON_BUFFER_MAP_MODE_BUFFER)
      return D3D_OK;

    D3D9DeviceLock lock = LockDevice();

    if (pResource->SetMapFlags(0) & D3DLOCK_READONLY)
      return D3D_OK;

    FlushImplicit(FALSE);

    auto dstBuffer = pResource->GetBufferSlice<D3D9_COMMON_BUFFER_TYPE_REAL>();
    auto srcBuffer = pResource->GetBufferSlice<D3D9_COMMON_BUFFER_TYPE_STAGING>();

    EmitCs([
      cDstSlice = dstBuffer,
      cSrcSlice = srcBuffer,
      cRange    = pResource->LockRange()
    ] (DxvkContext* ctx) {
      ctx->copyBuffer(
        cDstSlice.buffer(),
        cDstSlice.offset() + cRange.min,
        cSrcSlice.buffer(),
        cSrcSlice.offset() + cRange.min,
        cRange.max - cRange.min);
    });

    return D3D_OK;
  }


  void D3D9DeviceEx::EmitCsChunk(DxvkCsChunkRef&& chunk) {
    m_csThread.dispatchChunk(std::move(chunk));
    m_csIsBusy = true;
  }


  void D3D9DeviceEx::FlushImplicit(BOOL StrongHint) {
    // Flush only if the GPU is about to go idle, in
    // order to keep the number of submissions low.
    uint32_t pending = m_dxvkDevice->pendingSubmissions();

    if (StrongHint || pending <= MaxPendingSubmits) {
      auto now = std::chrono::high_resolution_clock::now();

      uint32_t delay = MinFlushIntervalUs
                     + IncFlushIntervalUs * pending;

      // Prevent flushing too often in short intervals.
      if (now - m_lastFlush >= std::chrono::microseconds(delay))
        Flush();
    }
  }


  void D3D9DeviceEx::SynchronizeCsThread() {
    D3D9DeviceLock lock = LockDevice();

    // Dispatch current chunk so that all commands
    // recorded prior to this function will be run
    FlushCsChunk();

    if (m_csThread.isBusy())
      m_csThread.synchronize();
  }


  void D3D9DeviceEx::SetupFPU() {
    // Should match d3d9 float behaviour.

#if defined(_MSC_VER)
    // For MSVC we can use these cross arch and platform funcs to set the FPU.
    // This will work on any platform, x86, x64, ARM, etc.

    // Clear exceptions.
    _clearfp();

    // Disable exceptions
    _controlfp(_MCW_EM, _MCW_EM);

#ifndef _WIN64
    // Use 24 bit precision
    _controlfp(_PC_24, _MCW_PC);
#endif

    // Round to nearest
    _controlfp(_RC_NEAR, _MCW_RC);
#elif (defined(__GNUC__) || defined(__MINGW32__)) && (defined(__i386__) || defined(__x86_64__) || defined(__ia64))
    // For GCC/MinGW we can use inline asm to set it.
    // This only works for x86 and x64 processors however.

    uint16_t control;

    // Get current control word.
    __asm__ __volatile__("fnstcw %0" : "=m" (*&control));

    // Clear existing settings.
    control &= 0xF0C0;

    // Disable exceptions
    // Use 24 bit precision
    // Round to nearest
    control |= 0x003F;

    // Set new control word.
    __asm__ __volatile__("fldcw %0" : : "m" (*&control));
#else
    Logger::warn("D3D9DeviceEx::SetupFPU: not supported on this arch.");
#endif
  }


  int64_t D3D9DeviceEx::DetermineInitialTextureMemory() {
    auto memoryProp = m_adapter->GetDXVKAdapter()->memoryProperties();

    VkDeviceSize availableTextureMemory = 0;

    for (uint32_t i = 0; i < memoryProp.memoryHeapCount; i++) {
      VkMemoryHeap& heap = memoryProp.memoryHeaps[i];

      if (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        availableTextureMemory += memoryProp.memoryHeaps[i].size;
    }

    // The value returned is a 32-bit value, so we need to clamp it.
    VkDeviceSize maxMemory = m_d3d9Options.maxAvailableMemory;
    availableTextureMemory = std::min(availableTextureMemory, maxMemory);

    return int64_t(availableTextureMemory);
  }


  void D3D9DeviceEx::CreateConstantBuffers() {
    DxvkBufferCreateInfo info;
    info.usage  = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    info.access = VK_ACCESS_UNIFORM_READ_BIT;

    VkMemoryPropertyFlags memoryFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    info.stages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
    info.size   = sizeof(D3D9ShaderConstantsVS);
    m_consts[DxsoProgramTypes::VertexShader].buffer = m_dxvkDevice->createBuffer(info, memoryFlags);

    info.stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    info.size   = sizeof(D3D9ShaderConstantsPS);
    m_consts[DxsoProgramTypes::PixelShader].buffer  = m_dxvkDevice->createBuffer(info, memoryFlags);

    info.size = caps::MaxClipPlanes * sizeof(D3D9ClipPlane);
    m_vsClipPlanes = m_dxvkDevice->createBuffer(info, memoryFlags);

    info.size = sizeof(D3D9FixedFunctionVS);
    m_vsFixedFunction = m_dxvkDevice->createBuffer(info, memoryFlags);

    info.size = sizeof(D3D9FixedFunctionPS);
    m_psFixedFunction = m_dxvkDevice->createBuffer(info, memoryFlags);

    info.size = sizeof(D3D9SharedPS);
    m_psShared = m_dxvkDevice->createBuffer(info, memoryFlags);

    auto BindConstantBuffer = [this](
      DxsoProgramType     shaderStage,
      Rc<DxvkBuffer>      buffer,
      DxsoConstantBuffers cbuffer) {
      const uint32_t slotId = computeResourceSlotId(
        shaderStage, DxsoBindingType::ConstantBuffer,
        cbuffer);

      EmitCs([
        cSlotId = slotId,
        cBuffer = buffer
      ] (DxvkContext* ctx) {
        ctx->bindResourceBuffer(cSlotId,
          DxvkBufferSlice(cBuffer, 0, cBuffer->info().size));
      });
    };

    BindConstantBuffer(DxsoProgramTypes::VertexShader, m_consts[DxsoProgramTypes::VertexShader].buffer, DxsoConstantBuffers::VSConstantBuffer);
    BindConstantBuffer(DxsoProgramTypes::VertexShader, m_vsClipPlanes,                                  DxsoConstantBuffers::VSClipPlanes);
    BindConstantBuffer(DxsoProgramTypes::VertexShader, m_vsFixedFunction,                               DxsoConstantBuffers::VSFixedFunction);

    BindConstantBuffer(DxsoProgramTypes::PixelShader,  m_consts[DxsoProgramTypes::PixelShader].buffer,  DxsoConstantBuffers::PSConstantBuffer);
    BindConstantBuffer(DxsoProgramTypes::PixelShader,  m_psFixedFunction,                               DxsoConstantBuffers::PSFixedFunction);
    BindConstantBuffer(DxsoProgramTypes::PixelShader,  m_psShared,                                      DxsoConstantBuffers::PSShared);
    
    m_flags.set(
      D3D9DeviceFlag::DirtyClipPlanes);
  }


  template <DxsoProgramType ShaderStage>
  void D3D9DeviceEx::UploadConstants() {
    auto UploadHelper = [&](auto& src) {
      D3D9ConstantSets& constSet = m_consts[ShaderStage];

      if (!constSet.dirty)
        return;

      constSet.dirty = false;

      DxvkBufferSliceHandle slice = constSet.buffer->allocSlice();

      auto dstData = reinterpret_cast<std::remove_reference_t<decltype(src)> *>(slice.mapPtr);
      auto srcData = &src;

      EmitCs([
        cBuffer = constSet.buffer,
        cSlice  = slice
      ] (DxvkContext* ctx) {
        ctx->invalidateBuffer(cBuffer, cSlice);
      });

      if (constSet.meta->maxConstIndexF)
        std::memcpy(&dstData->fConsts[0], &srcData->fConsts[0], sizeof(Vector4)  * constSet.meta->maxConstIndexF);
      if (constSet.meta->maxConstIndexI)
        std::memcpy(&dstData->iConsts[0], &srcData->iConsts[0], sizeof(Vector4i) * constSet.meta->maxConstIndexI);
      if (constSet.meta->maxConstIndexB)
        dstData->boolBitfield = srcData->boolBitfield;

      if (constSet.meta->needsConstantCopies) {
        Vector4* data = reinterpret_cast<Vector4*>(slice.mapPtr);

        if (ShaderStage == DxsoProgramTypes::VertexShader) {
          auto& shaderConsts = GetCommonShader(m_state.vertexShader)->GetConstants();

          for (const auto& constant : shaderConsts)
            data[constant.uboIdx] = *reinterpret_cast<const Vector4*>(constant.float32);
        }
        else {
          auto& shaderConsts = GetCommonShader(m_state.pixelShader)->GetConstants();

          for (const auto& constant : shaderConsts)
            data[constant.uboIdx] = *reinterpret_cast<const Vector4*>(constant.float32);
        }
      }
    };

    return ShaderStage == DxsoProgramTypes::VertexShader
      ? UploadHelper(m_state.vsConsts)
      : UploadHelper(m_state.psConsts);
  }


  void D3D9DeviceEx::UpdateClipPlanes() {
    m_flags.clr(D3D9DeviceFlag::DirtyClipPlanes);
    
    auto slice = m_vsClipPlanes->allocSlice();
    auto dst = reinterpret_cast<D3D9ClipPlane*>(slice.mapPtr);
    
    for (uint32_t i = 0; i < caps::MaxClipPlanes; i++) {
      dst[i] = (m_state.renderStates[D3DRS_CLIPPLANEENABLE] & (1 << i))
        ? m_state.clipPlanes[i]
        : D3D9ClipPlane();
    }
    
    EmitCs([
      cBuffer = m_vsClipPlanes,
      cSlice  = slice
    ] (DxvkContext* ctx) {
      ctx->invalidateBuffer(cBuffer, cSlice);
    });
  }


  template <uint32_t Offset, uint32_t Length>
  void D3D9DeviceEx::UpdatePushConstant(const void* pData) {
    struct ConstantData { uint8_t Data[Length]; };

    auto* constData = reinterpret_cast<const ConstantData*>(pData);

    EmitCs([
      cData = *constData
    ](DxvkContext* ctx) {
      ctx->pushConstants(Offset, Length, &cData);
    });
  }


  template <D3D9RenderStateItem Item>
  void D3D9DeviceEx::UpdatePushConstant() {
    auto& rs = m_state.renderStates;

    if constexpr (Item == D3D9RenderStateItem::AlphaRef) {
      float alpha = float(rs[D3DRS_ALPHAREF]) / 255.0f;
      UpdatePushConstant<offsetof(D3D9RenderStateInfo, alphaRef), sizeof(float)>(&alpha);
    }
    else
      Logger::warn("D3D9: Invalid push constant set to update.");
  }
  


  void D3D9DeviceEx::Flush() {
    D3D9DeviceLock lock = LockDevice();

    m_initializer->Flush();

    if (m_csIsBusy || !m_csChunk->empty()) {
      // Add commands to flush the threaded
      // context, then flush the command list
      EmitCs([](DxvkContext* ctx) {
        ctx->flushCommandList();
      });

      FlushCsChunk();

      // Reset flush timer used for implicit flushes
      m_lastFlush = std::chrono::high_resolution_clock::now();
      m_csIsBusy = false;
    }
  }


  void D3D9DeviceEx::CheckForHazards() {
    static const std::array<D3DRENDERSTATETYPE, 4> colorWriteIndices = {
      D3DRS_COLORWRITEENABLE,
      D3DRS_COLORWRITEENABLE1,
      D3DRS_COLORWRITEENABLE2,
      D3DRS_COLORWRITEENABLE3
    };

    const auto* shader = GetCommonShader(m_state.pixelShader);

    if (shader == nullptr)
      return;

    for (uint32_t j = 0; j < m_state.renderTargets.size(); j++) {
      auto* rt = GetCommonTexture(m_state.renderTargets[j]);

      // Skip this RT if it doesn't exist
      // or we aren't writing to it anyway.
      if (likely(rt == nullptr || m_state.renderStates[colorWriteIndices[j]] == 0 || !shader->IsRTUsed(j)))
        continue;

      // Check all of the pixel shader textures 
      for (uint32_t i = 0; i < 16; i++) {
        auto* tex = GetCommonTexture(m_state.textures[i]);

        // We only care if there is a hazard in the current draw...
        // Some games don't unbind their textures so we need to check the shaders.
        if (likely(tex == nullptr || !shader->IsSamplerUsed(i)))
          continue;
        
        if (tex == rt) {
          // If we haven't marked this as a hazard before:
          // - Transition the image's layout
          // - Rebind the framebuffer for the new layout
          if (unlikely(!tex->MarkHazardous())) {
            TransitionImage(tex, VK_IMAGE_LAYOUT_GENERAL);
            m_flags.set(D3D9DeviceFlag::DirtyFramebuffer);
          }

          // No need to search for more hazards for this texture.
          break;
        }
      }
    }
  }


  void D3D9DeviceEx::BindFramebuffer() {
    m_flags.clr(D3D9DeviceFlag::DirtyFramebuffer);

    DxvkRenderTargets attachments;

    bool srgb = m_state.renderStates[D3DRS_SRGBWRITEENABLE] != FALSE;

    // D3D9 doesn't have the concept of a framebuffer object,
    // so we'll just create a new one every time the render
    // target bindings are updated. Set up the attachments.
    for (UINT i = 0; i < m_state.renderTargets.size(); i++) {
      if (m_state.renderTargets[i] != nullptr && !m_state.renderTargets[i]->IsNull()) {
        attachments.color[i] = {
          m_state.renderTargets[i]->GetRenderTargetView(srgb),
          m_state.renderTargets[i]->GetRenderTargetLayout() };
      }
    }

    if (m_state.depthStencil != nullptr) {
      attachments.depth = {
        m_state.depthStencil->GetDepthStencilView(),
        m_state.depthStencil->GetDepthLayout() };
    }

    // Create and bind the framebuffer object to the context
    EmitCs([
      cAttachments = std::move(attachments)
    ] (DxvkContext* ctx) {
      ctx->bindRenderTargets(cAttachments, false);
    });
  }


  void D3D9DeviceEx::BindViewportAndScissor() {
    m_flags.clr(D3D9DeviceFlag::DirtyViewportScissor);

    VkViewport viewport;
    VkRect2D scissor;

    // D3D9's coordinate system has its origin in the bottom left,
    // but the viewport coordinates are aligned to the top-left
    // corner so we can get away with flipping the viewport.
    const D3DVIEWPORT9& vp = m_state.viewport;

    // Correctness Factor for 1/2 texel offset
    float cf = 0.5f;

    // HACK: UE3 bug re. tonemapper + shadow sampling being red:-
    // We need to bias this, except when it's
    // NOT powers of two in order to make
    // imprecision biased towards infinity.
    if ((vp.Width  & (vp.Width  - 1)) == 0
     && (vp.Height & (vp.Height - 1)) == 0)
      cf -= 1.0f / 128.0f;

    viewport = VkViewport{
      float(vp.X)     + cf,    float(vp.Height + vp.Y) + cf,
      float(vp.Width),        -float(vp.Height),
      vp.MinZ,                 vp.MaxZ,
    };

    // Scissor rectangles. Vulkan does not provide an easy way
    // to disable the scissor test, so we'll have to set scissor
    // rects that are at least as large as the framebuffer.
    bool enableScissorTest = m_state.renderStates[D3DRS_SCISSORTESTENABLE] != FALSE;

    if (enableScissorTest) {
      RECT sr = m_state.scissorRect;

      VkOffset2D srPosA;
      srPosA.x = std::max<int32_t>(0, sr.left);
      srPosA.y = std::max<int32_t>(0, sr.top);

      VkOffset2D srPosB;
      srPosB.x = std::max<int32_t>(srPosA.x, sr.right);
      srPosB.y = std::max<int32_t>(srPosA.y, sr.bottom);

      VkExtent2D srSize;
      srSize.width  = uint32_t(srPosB.x - srPosA.x);
      srSize.height = uint32_t(srPosB.y - srPosA.y);

      scissor = VkRect2D{ srPosA, srSize };
    }
    else {
      scissor = VkRect2D{
        VkOffset2D { 0, 0 },
        VkExtent2D {
          16383,
          16383 } };
    }

    EmitCs([
      cViewport = viewport,
      cScissor = scissor
    ] (DxvkContext* ctx) {
      ctx->setViewports(
        1,
        &cViewport,
        &cScissor);
    });
  }


  void D3D9DeviceEx::BindMultiSampleState() {
    m_flags.clr(D3D9DeviceFlag::DirtyMultiSampleState);

    DxvkMultisampleState msState;
    msState.sampleMask            = m_flags.test(D3D9DeviceFlag::ValidSampleMask)
      ? m_state.renderStates[D3DRS_MULTISAMPLEMASK]
      : 0xffffffff;
    msState.enableAlphaToCoverage = IsAlphaToCoverageEnabled();

    EmitCs([
      cState = msState
    ] (DxvkContext* ctx) {
      ctx->setMultisampleState(cState);
    });
  }


  void D3D9DeviceEx::BindBlendState() {
    m_flags.clr(D3D9DeviceFlag::DirtyBlendState);

    auto& state = m_state.renderStates;

    bool separateAlpha  = state[D3DRS_SEPARATEALPHABLENDENABLE] != FALSE;

    DxvkBlendMode baseMode;
    baseMode.enableBlending = state[D3DRS_ALPHABLENDENABLE] != FALSE;

    baseMode.colorSrcFactor = DecodeBlendFactor(D3DBLEND  ( state[D3DRS_SRCBLEND]  ), false);
    baseMode.colorDstFactor = DecodeBlendFactor(D3DBLEND  ( state[D3DRS_DESTBLEND] ), false);
    baseMode.colorBlendOp   = DecodeBlendOp    (D3DBLENDOP( state[D3DRS_BLENDOP]   )       );

    baseMode.alphaSrcFactor = DecodeBlendFactor(separateAlpha ? D3DBLEND  ( state[D3DRS_SRCBLENDALPHA]  ) : D3DBLEND  ( state[D3DRS_SRCBLEND]  ), true);
    baseMode.alphaDstFactor = DecodeBlendFactor(separateAlpha ? D3DBLEND  ( state[D3DRS_DESTBLENDALPHA] ) : D3DBLEND  ( state[D3DRS_DESTBLEND] ), true);
    baseMode.alphaBlendOp   = DecodeBlendOp    (separateAlpha ? D3DBLENDOP( state[D3DRS_BLENDOPALPHA]   ) : D3DBLENDOP( state[D3DRS_BLENDOP]   )      );

    std::array<DxvkBlendMode, 4> modes;
    for (uint32_t i = 0; i < modes.size(); i++) {
      auto& mode = modes[i];
      mode = baseMode;

      // These state indices are non-contiguous... Of course.
      static const std::array<D3DRENDERSTATETYPE, 4> colorWriteIndices = {
        D3DRS_COLORWRITEENABLE,
        D3DRS_COLORWRITEENABLE1,
        D3DRS_COLORWRITEENABLE2,
        D3DRS_COLORWRITEENABLE3
      };

      mode.writeMask = state[colorWriteIndices[i]];
    }

    EmitCs([
      cModes = modes
    ](DxvkContext* ctx) {
      for (uint32_t i = 0; i < cModes.size(); i++)
        ctx->setBlendMode(i, cModes[i]);
    });
  }


  void D3D9DeviceEx::BindBlendFactor() {
    DxvkBlendConstants blendConstants;
    DecodeD3DCOLOR(
      D3DCOLOR(m_state.renderStates[D3DRS_BLENDFACTOR]),
      reinterpret_cast<float*>(&blendConstants));

    EmitCs([
      cBlendConstants = blendConstants
    ](DxvkContext* ctx) {
      ctx->setBlendConstants(cBlendConstants);
    });
  }


  void D3D9DeviceEx::BindDepthStencilState() {
    m_flags.clr(D3D9DeviceFlag::DirtyDepthStencilState);

    auto& rs = m_state.renderStates;

    bool stencil            = rs[D3DRS_STENCILENABLE] != FALSE;
    bool twoSidedStencil    = stencil && (rs[D3DRS_TWOSIDEDSTENCILMODE] != FALSE);

    DxvkDepthStencilState state;
    state.enableDepthTest   = rs[D3DRS_ZENABLE]       != FALSE;
    state.enableDepthWrite  = rs[D3DRS_ZWRITEENABLE]  != FALSE;
    state.enableStencilTest = stencil;
    state.depthCompareOp    = DecodeCompareOp(D3DCMPFUNC(rs[D3DRS_ZFUNC]));

    if (stencil) {
      state.stencilOpFront.failOp      = DecodeStencilOp(D3DSTENCILOP(rs[D3DRS_STENCILFAIL]));
      state.stencilOpFront.passOp      = DecodeStencilOp(D3DSTENCILOP(rs[D3DRS_STENCILPASS]));
      state.stencilOpFront.depthFailOp = DecodeStencilOp(D3DSTENCILOP(rs[D3DRS_STENCILZFAIL]));
      state.stencilOpFront.compareOp   = DecodeCompareOp(D3DCMPFUNC  (rs[D3DRS_STENCILFUNC]));
      state.stencilOpFront.compareMask = uint32_t(rs[D3DRS_STENCILMASK]);
      state.stencilOpFront.writeMask   = uint32_t(rs[D3DRS_STENCILWRITEMASK]);
      state.stencilOpFront.reference   = 0;
    }
    else
      state.stencilOpFront = VkStencilOpState();

    if (twoSidedStencil) {
      state.stencilOpBack.failOp      = DecodeStencilOp(D3DSTENCILOP(rs[D3DRS_CCW_STENCILFAIL]));
      state.stencilOpBack.passOp      = DecodeStencilOp(D3DSTENCILOP(rs[D3DRS_CCW_STENCILPASS]));
      state.stencilOpBack.depthFailOp = DecodeStencilOp(D3DSTENCILOP(rs[D3DRS_CCW_STENCILZFAIL]));
      state.stencilOpBack.compareOp   = DecodeCompareOp(D3DCMPFUNC  (rs[D3DRS_CCW_STENCILFUNC]));
      state.stencilOpBack.compareMask = state.stencilOpFront.compareMask;
      state.stencilOpBack.writeMask   = state.stencilOpFront.writeMask;
      state.stencilOpBack.reference   = 0;
    }
    else
      state.stencilOpBack = state.stencilOpFront;

    EmitCs([
      cState = state
    ](DxvkContext* ctx) {
      ctx->setDepthStencilState(cState);
    });
  }


  void D3D9DeviceEx::BindRasterizerState() {
    m_flags.clr(D3D9DeviceFlag::DirtyRasterizerState);

    // TODO: Can we get a specific non-magic number in Vulkan for this based on device/adapter?
    constexpr float DepthBiasFactor = float(1 << 23);

    auto& rs = m_state.renderStates;

    float depthBias            = bit::cast<float>(rs[D3DRS_DEPTHBIAS]) * DepthBiasFactor;
    float slopeScaledDepthBias = bit::cast<float>(rs[D3DRS_SLOPESCALEDEPTHBIAS]);

    DxvkRasterizerState state;
    state.cullMode        = DecodeCullMode(D3DCULL(rs[D3DRS_CULLMODE]));
    state.depthBiasEnable = depthBias != 0.0f || slopeScaledDepthBias != 0.0f;
    state.depthClipEnable = true;
    state.frontFace       = VK_FRONT_FACE_CLOCKWISE;
    state.polygonMode     = DecodeFillMode(D3DFILLMODE(rs[D3DRS_FILLMODE]));
    state.sampleCount     = 0;

    DxvkDepthBias biases;
    biases.depthBiasConstant = depthBias;
    biases.depthBiasSlope    = slopeScaledDepthBias;
    biases.depthBiasClamp    = 0.0f;

    EmitCs([
      cState  = state,
      cBiases = biases
    ](DxvkContext* ctx) {
      ctx->setRasterizerState(cState);
      ctx->setDepthBias(cBiases);
    });
  }


  void D3D9DeviceEx::BindAlphaTestState() {
    m_flags.clr(D3D9DeviceFlag::DirtyAlphaTestState);
    
    auto& rs = m_state.renderStates;
    
    VkCompareOp alphaOp = rs[D3DRS_ALPHATESTENABLE]
      ? DecodeCompareOp(D3DCMPFUNC(rs[D3DRS_ALPHAFUNC]))
      : VK_COMPARE_OP_ALWAYS;
    
    EmitCs([cAlphaOp = alphaOp] (DxvkContext* ctx) {
      ctx->setSpecConstant(D3D9SpecConstantId::AlphaTestEnable, cAlphaOp != VK_COMPARE_OP_ALWAYS);
      ctx->setSpecConstant(D3D9SpecConstantId::AlphaCompareOp,  cAlphaOp);
    });
  }


  void D3D9DeviceEx::BindDepthStencilRefrence() {
    auto& rs = m_state.renderStates;

    uint32_t ref = uint32_t(rs[D3DRS_STENCILREF]);

    EmitCs([
      cRef = ref
    ](DxvkContext* ctx) {
      ctx->setStencilReference(cRef);
    });
  }


  void D3D9DeviceEx::BindSampler(DWORD Sampler) {
    auto& state = m_state.samplerStates[Sampler];

    D3D9SamplerKey key;
    key.AddressU      = D3DTEXTUREADDRESS(state[D3DSAMP_ADDRESSU]);
    key.AddressV      = D3DTEXTUREADDRESS(state[D3DSAMP_ADDRESSV]);
    key.AddressW      = D3DTEXTUREADDRESS(state[D3DSAMP_ADDRESSW]);
    key.MagFilter     = D3DTEXTUREFILTERTYPE(state[D3DSAMP_MAGFILTER]);
    key.MinFilter     = D3DTEXTUREFILTERTYPE(state[D3DSAMP_MINFILTER]);
    key.MipFilter     = D3DTEXTUREFILTERTYPE(state[D3DSAMP_MIPFILTER]);
    key.MaxAnisotropy = state[D3DSAMP_MAXANISOTROPY];
    key.MipmapLodBias = bit::cast<float>(state[D3DSAMP_MIPMAPLODBIAS]);
    key.MaxMipLevel   = state[D3DSAMP_MAXMIPLEVEL];
    DecodeD3DCOLOR(D3DCOLOR(state[D3DSAMP_BORDERCOLOR]), key.BorderColor);

    if (m_d3d9Options.samplerAnisotropy != -1) {
      if (key.MagFilter == D3DTEXF_LINEAR)
        key.MagFilter = D3DTEXF_ANISOTROPIC;

      if (key.MinFilter == D3DTEXF_LINEAR)
        key.MinFilter = D3DTEXF_ANISOTROPIC;

      key.MaxAnisotropy = m_d3d9Options.samplerAnisotropy;
    }

    NormalizeSamplerKey(key);

    auto samplerInfo = RemapStateSamplerShader(Sampler);

    const uint32_t colorSlot = computeResourceSlotId(
      samplerInfo.first, DxsoBindingType::ColorImage,
      samplerInfo.second);

    const uint32_t depthSlot = computeResourceSlotId(
      samplerInfo.first, DxsoBindingType::DepthImage,
      samplerInfo.second);

    EmitCs([
      &cDevice   = m_dxvkDevice,
      &cSamplers = m_samplers,
      cColorSlot = colorSlot,
      cDepthSlot = depthSlot,
      cKey       = key
    ] (DxvkContext* ctx) {
      auto pair = cSamplers.find(cKey);
      if (pair != cSamplers.end()) {
        ctx->bindResourceSampler(cColorSlot, pair->second.color);
        ctx->bindResourceSampler(cDepthSlot, pair->second.depth);
        return;
      }

      auto mipFilter = DecodeMipFilter(cKey.MipFilter);

      DxvkSamplerCreateInfo colorInfo;
      colorInfo.addressModeU   = DecodeAddressMode(cKey.AddressU);
      colorInfo.addressModeV   = DecodeAddressMode(cKey.AddressV);
      colorInfo.addressModeW   = DecodeAddressMode(cKey.AddressW);
      colorInfo.compareToDepth = VK_FALSE;
      colorInfo.compareOp      = VK_COMPARE_OP_NEVER;
      colorInfo.magFilter      = DecodeFilter(cKey.MagFilter);
      colorInfo.minFilter      = DecodeFilter(cKey.MinFilter);
      colorInfo.mipmapMode     = mipFilter.MipFilter;
      colorInfo.maxAnisotropy  = float(cKey.MaxAnisotropy);
      colorInfo.useAnisotropy  = IsAnisotropic(cKey.MinFilter)
                              || IsAnisotropic(cKey.MagFilter);
      colorInfo.mipmapLodBias  = cKey.MipmapLodBias;
      colorInfo.mipmapLodMin   = mipFilter.MipsEnabled ? float(cKey.MaxMipLevel) : 0;
      colorInfo.mipmapLodMax   = mipFilter.MipsEnabled ? FLT_MAX                 : 0;
      colorInfo.usePixelCoord  = VK_FALSE;
      for (uint32_t i = 0; i < 4; i++)
        colorInfo.borderColor.float32[i] = cKey.BorderColor[i];

      DxvkSamplerCreateInfo depthInfo = colorInfo;
      depthInfo.compareToDepth = VK_TRUE;
      depthInfo.compareOp      = VK_COMPARE_OP_LESS_OR_EQUAL;
      depthInfo.magFilter      = VK_FILTER_LINEAR;
      depthInfo.minFilter      = VK_FILTER_LINEAR;

      try {
        D3D9SamplerPair pair;

        pair.color = cDevice->createSampler(colorInfo);
        pair.depth = cDevice->createSampler(depthInfo);

        cSamplers.insert(std::make_pair(cKey, pair));
        ctx->bindResourceSampler(cColorSlot, pair.color);
        ctx->bindResourceSampler(cDepthSlot, pair.depth);
      }
      catch (const DxvkError& e) {
        Logger::err(e.message());
      }
    });
  }


  void D3D9DeviceEx::BindTexture(DWORD StateSampler) {
    auto shaderSampler = RemapStateSamplerShader(StateSampler);

    uint32_t colorSlot = computeResourceSlotId(shaderSampler.first,
      DxsoBindingType::ColorImage, uint32_t(shaderSampler.second));

    uint32_t depthSlot = computeResourceSlotId(shaderSampler.first,
      DxsoBindingType::DepthImage, uint32_t(shaderSampler.second));

    const bool srgb =
      m_state.samplerStates[StateSampler][D3DSAMP_SRGBTEXTURE] != FALSE;

    D3D9CommonTexture* commonTex =
      GetCommonTexture(m_state.textures[StateSampler]);

    // For all our pixel shader textures
    if (likely(StateSampler < 16)) {
      const uint32_t offset = StateSampler * 2;
      const uint32_t textureType = commonTex != nullptr
        ? uint32_t(commonTex->GetType() - D3DRTYPE_TEXTURE)
        : 0;
      const uint32_t textureBitMask = 0b11u << offset;
      const uint32_t textureBits = textureType << offset;

      m_samplerTypeBitfield &= ~textureBitMask;
      m_samplerTypeBitfield |= textureBits;
    }

    if (commonTex == nullptr) {
      EmitCs([
        cColorSlot = colorSlot,
        cDepthSlot = depthSlot
      ](DxvkContext* ctx) {
        ctx->bindResourceView(cColorSlot, nullptr, nullptr);
        ctx->bindResourceView(cDepthSlot, nullptr, nullptr);
      });
      return;
    }

    EmitCs([
      cColorSlot = colorSlot,
      cDepthSlot = depthSlot,
      cDepth     = commonTex->IsShadow(),
      cImageView = commonTex->GetViews().Sample.Pick(srgb)
    ](DxvkContext* ctx) {
      ctx->bindResourceView(cColorSlot, !cDepth ? cImageView : nullptr, nullptr);
      ctx->bindResourceView(cDepthSlot,  cDepth ? cImageView : nullptr, nullptr);
    });
  }


  void D3D9DeviceEx::UndirtySamplers() {
    for (uint32_t dirty = m_dirtySamplerStates; dirty; dirty &= dirty - 1)
      BindSampler(bit::tzcnt(dirty));
    
    m_dirtySamplerStates = 0;
  }


  void D3D9DeviceEx::MarkSamplersDirty() {
    m_dirtySamplerStates = 0x001fffff; // 21 bits.
  }


  D3D9DrawInfo D3D9DeviceEx::GenerateDrawInfo(
          D3DPRIMITIVETYPE PrimitiveType,
          UINT             PrimitiveCount,
          UINT             InstanceCount) {
    D3D9DrawInfo drawInfo;
    drawInfo.vertexCount = GetVertexCount(PrimitiveType, PrimitiveCount);
    drawInfo.instanceCount = m_iaState.streamsInstanced & m_iaState.streamsUsed
      ? InstanceCount
      : 1u;
    return drawInfo;
  }


  uint32_t D3D9DeviceEx::GetInstanceCount() const {
    return std::max(m_state.streamFreq[0] & 0x7FFFFFu, 1u);
  }


  void D3D9DeviceEx::PrepareDraw(bool up) {
    // This is fairly expensive to do!
    // So we only enable it on games & vendors that actually need it (for now)
    // This is not needed at all on NV either, etc...
    if (m_d3d9Options.hasHazards)
      CheckForHazards();

    if (m_flags.test(D3D9DeviceFlag::DirtyFramebuffer))
      BindFramebuffer();

    if (m_flags.test(D3D9DeviceFlag::DirtyViewportScissor))
      BindViewportAndScissor();

    if (m_dirtySamplerStates)
      UndirtySamplers();

    if (m_flags.test(D3D9DeviceFlag::DirtyBlendState))
      BindBlendState();
    
    if (m_flags.test(D3D9DeviceFlag::DirtyDepthStencilState))
      BindDepthStencilState();

    if (m_flags.test(D3D9DeviceFlag::DirtyRasterizerState))
      BindRasterizerState();
    
    if (m_flags.test(D3D9DeviceFlag::DirtyMultiSampleState))
      BindMultiSampleState();

    if (m_flags.test(D3D9DeviceFlag::DirtyAlphaTestState))
      BindAlphaTestState();
    
    if (m_flags.test(D3D9DeviceFlag::DirtyClipPlanes))
      UpdateClipPlanes();

    if (!up && m_flags.test(D3D9DeviceFlag::UpDirtiedVertices)) {
      m_flags.clr(D3D9DeviceFlag::UpDirtiedVertices);
      if (m_state.vertexBuffers[0].vertexBuffer != nullptr)
        BindVertexBuffer(0,
          m_state.vertexBuffers[0].vertexBuffer,
          m_state.vertexBuffers[0].offset,
          m_state.vertexBuffers[0].stride);
    }

    if (!up && m_flags.test(D3D9DeviceFlag::UpDirtiedIndices)) {
      m_flags.clr(D3D9DeviceFlag::UpDirtiedIndices);
      BindIndices();
    }

    if (likely(UseProgrammableVS())) {
      if (unlikely(m_flags.test(D3D9DeviceFlag::DirtyProgVertexShader))) {
        m_flags.set(D3D9DeviceFlag::DirtyInputLayout);
        BindShader(DxsoProgramType::VertexShader, GetCommonShader(m_state.vertexShader));
      }
      UploadConstants<DxsoProgramTypes::VertexShader>();
    }
    else
      UpdateFixedFunctionVS();

    if (m_flags.test(D3D9DeviceFlag::DirtyInputLayout))
      BindInputLayout();

    auto UpdateSamplerTypes = [&](uint32_t value) {
      if (m_lastSamplerTypeBitfield != value)
        UpdateSamplerSpecConsant(value);
    };

    if (likely(UseProgrammablePS())) {
      UploadConstants<DxsoProgramTypes::PixelShader>();

      if (GetCommonShader(m_state.pixelShader)->GetInfo().majorVersion() >= 2)
        UpdateSamplerTypes(0u);
      else
        UpdateSamplerTypes(m_samplerTypeBitfield); // For implicit samplers...
    }
    else {
      UpdateSamplerTypes(0u);

      UpdateFixedFunctionPS();
    }

    if (m_flags.test(D3D9DeviceFlag::DirtySharedPixelShaderData)) {
      m_flags.clr(D3D9DeviceFlag::DirtySharedPixelShaderData);

      DxvkBufferSliceHandle slice = m_psShared->allocSlice();

      EmitCs([
        cBuffer = m_psShared,
        cSlice  = slice
      ] (DxvkContext* ctx) {
        ctx->invalidateBuffer(cBuffer, cSlice);
      });

      D3D9SharedPS* data = reinterpret_cast<D3D9SharedPS*>(slice.mapPtr);

      for (uint32_t i = 0; i < caps::TextureStageCount; i++) {
        // Flip major-ness so we can get away with a nice easy
        // dot in the shader without complex access
        data->Stages[i].BumpEnvMat[0][0] = bit::cast<float>(m_state.textureStages[i][D3DTSS_BUMPENVMAT00]);
        data->Stages[i].BumpEnvMat[1][0] = bit::cast<float>(m_state.textureStages[i][D3DTSS_BUMPENVMAT01]);
        data->Stages[i].BumpEnvMat[0][1] = bit::cast<float>(m_state.textureStages[i][D3DTSS_BUMPENVMAT10]);
        data->Stages[i].BumpEnvMat[1][1] = bit::cast<float>(m_state.textureStages[i][D3DTSS_BUMPENVMAT11]);

        data->Stages[i].BumpEnvLScale    = bit::cast<float>(m_state.textureStages[i][D3DTSS_BUMPENVLSCALE]);
        data->Stages[i].BumpEnvLOffset   = bit::cast<float>(m_state.textureStages[i][D3DTSS_BUMPENVLOFFSET]);
      }
    }
  }


  void D3D9DeviceEx::BindShader(
        DxsoProgramType                   ShaderStage,
  const D3D9CommonShader*                 pShaderModule) {
    EmitCs([
      cStage  = GetShaderStage(ShaderStage),
      cShader = pShaderModule->GetShader()
    ] (DxvkContext* ctx) {
      ctx->bindShader(cStage, cShader);
    });
  }


  void D3D9DeviceEx::BindInputLayout() {
    m_flags.clr(D3D9DeviceFlag::DirtyInputLayout);

    if (m_state.vertexDecl == nullptr) {
      EmitCs([&cIaState = m_iaState] (DxvkContext* ctx) {
        cIaState.streamsUsed = 0;
        ctx->setInputLayout(0, nullptr, 0, nullptr);
      });
    }
    else {
      std::array<uint32_t, caps::MaxStreams> streamFreq;

      for (uint32_t i = 0; i < caps::MaxStreams; i++)
        streamFreq[i] = m_state.streamFreq[i];

      Com<D3D9VertexDecl,   false> vertexDecl = m_state.vertexDecl;
      Com<D3D9VertexShader, false> vertexShader;

      if (UseProgrammableVS())
        vertexShader = m_state.vertexShader;

      EmitCs([
        &cIaState         = m_iaState,
        cVertexDecl       = std::move(vertexDecl),
        cVertexShader     = std::move(vertexShader),
        cStreamsInstanced = m_instancedData,
        cStreamFreq       = streamFreq
      ] (DxvkContext* ctx) {
        cIaState.streamsInstanced = cStreamsInstanced;
        cIaState.streamsUsed      = 0;

        const auto& elements = cVertexDecl->GetElements();

        std::array<DxvkVertexAttribute, 2 * caps::InputRegisterCount> attrList;
        std::array<DxvkVertexBinding,   2 * caps::InputRegisterCount> bindList;

        uint32_t attrMask = 0;
        uint32_t bindMask = 0;

        // TODO we should make fixed-function isgn global
        DxsoIsgn ffIsgn;

        if (cVertexShader == nullptr) {
          ffIsgn.elems[ffIsgn.elemCount++].semantic = DxsoSemantic{ DxsoUsage::Position, 0 };
          ffIsgn.elems[ffIsgn.elemCount++].semantic = DxsoSemantic{ DxsoUsage::Normal, 0 };
          for (uint32_t i = 0; i < 8; i++)
            ffIsgn.elems[ffIsgn.elemCount++].semantic = DxsoSemantic{ DxsoUsage::Texcoord, i };
          ffIsgn.elems[ffIsgn.elemCount++].semantic = DxsoSemantic{ DxsoUsage::Color, 0 };
          ffIsgn.elems[ffIsgn.elemCount++].semantic = DxsoSemantic{ DxsoUsage::Color, 1 };
        }

        const auto& isgn = cVertexShader != nullptr
          ? GetCommonShader(cVertexShader.ptr())->GetIsgn()
          : ffIsgn;

        for (uint32_t i = 0; i < isgn.elemCount; i++) {
          const auto& decl = isgn.elems[i];

          DxvkVertexAttribute attrib;
          attrib.location = i;
          attrib.binding  = NullStreamIdx;
          attrib.format   = VK_FORMAT_R32G32B32A32_SFLOAT;
          attrib.offset   = 0;

          for (const auto& element : elements) {
            DxsoSemantic elementSemantic = { static_cast<DxsoUsage>(element.Usage), element.UsageIndex };
            if (elementSemantic.usage == DxsoUsage::PositionT)
              elementSemantic.usage = DxsoUsage::Position;

            if (elementSemantic == decl.semantic) {
              attrib.binding = uint32_t(element.Stream);
              attrib.format  = DecodeDecltype(D3DDECLTYPE(element.Type));
              attrib.offset  = element.Offset;

              cIaState.streamsUsed |= 1u << attrib.binding;
              break;
            }
          }

          attrList[i] = attrib;

          DxvkVertexBinding binding;
          binding.binding = attrib.binding;

          uint32_t instanceData = cStreamFreq[binding.binding % caps::MaxStreams];
          if (instanceData & D3DSTREAMSOURCE_INSTANCEDATA) {
            binding.fetchRate = instanceData & 0x7FFFFF; // Remove instance packed-in flags in the data.
            binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
          }
          else {
            binding.fetchRate = 0;
            binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
          }

          // Check if the binding was already defined.
          bool bindingDefined = false;

          for (uint32_t j = 0; j < i; j++) {
            uint32_t bindingId = attrList.at(j).binding;

            if (binding.binding == bindingId) {
              bindingDefined = true;
            }
          }

          if (!bindingDefined)
            bindList.at(binding.binding) = binding;

          attrMask |= 1u << i;
          bindMask |= 1u << binding.binding;
        }

        // Compact the attribute and binding lists to filter
        // out attributes and bindings not used by the shader
        uint32_t attrCount = CompactSparseList(attrList.data(), attrMask);
        uint32_t bindCount = CompactSparseList(bindList.data(), bindMask);
      
        ctx->setInputLayout(
          attrCount, attrList.data(),
          bindCount, bindList.data());
      });
    }
  }


  void D3D9DeviceEx::BindVertexBuffer(
        UINT                              Slot,
        D3D9VertexBuffer*                 pBuffer,
        UINT                              Offset,
        UINT                              Stride) {
    EmitCs([
      cSlotId       = Slot,
      cBufferSlice  = pBuffer != nullptr ? 
          pBuffer->GetCommonBuffer()->GetBufferSlice<D3D9_COMMON_BUFFER_TYPE_REAL>(Offset) 
        : DxvkBufferSlice(),
      cStride       = pBuffer != nullptr ? Stride : 0
    ] (DxvkContext* ctx) {
      ctx->bindVertexBuffer(cSlotId, cBufferSlice, cStride);
    });
  }

  void D3D9DeviceEx::BindIndices() {
    D3D9CommonBuffer* buffer = m_state.indices != nullptr
      ? m_state.indices->GetCommonBuffer()
      : nullptr;

    D3D9Format format = buffer != nullptr
                      ? buffer->Desc()->Format
                      : D3D9Format::INDEX32;

    const VkIndexType indexType = DecodeIndexType(format);

    EmitCs([
      cBufferSlice = buffer != nullptr ? buffer->GetBufferSlice<D3D9_COMMON_BUFFER_TYPE_REAL>() : DxvkBufferSlice(),
      cIndexType   = indexType
    ](DxvkContext* ctx) {
      ctx->bindIndexBuffer(cBufferSlice, cIndexType);
    });
  }


  void D3D9DeviceEx::Begin(D3D9Query* pQuery) {
    D3D9DeviceLock lock = LockDevice();

    Com<D3D9Query> queryPtr = pQuery;

    EmitCs([queryPtr](DxvkContext* ctx) {
      queryPtr->Begin(ctx);
    });
  }


  void D3D9DeviceEx::End(D3D9Query* pQuery) {
    D3D9DeviceLock lock = LockDevice();

    Com<D3D9Query> queryPtr = pQuery;

    EmitCs([queryPtr](DxvkContext* ctx) {
      queryPtr->End(ctx);
    });

    if (unlikely(pQuery->IsEvent())) {
      pQuery->NotifyEnd();
      pQuery->IsStalling()
        ? Flush()
        : FlushImplicit(TRUE);
    }
  }


  void D3D9DeviceEx::SetVertexBoolBitfield(uint32_t mask, uint32_t bits) {
    m_state.vsConsts.boolBitfield &= ~mask;
    m_state.vsConsts.boolBitfield |= bits & mask;

    m_consts[DxsoProgramTypes::VertexShader].dirty = true;
  }


  void D3D9DeviceEx::SetPixelBoolBitfield(uint32_t mask, uint32_t bits) {
    m_state.psConsts.boolBitfield &= ~mask;
    m_state.psConsts.boolBitfield |= bits & mask;

    m_consts[DxsoProgramTypes::PixelShader].dirty = true;
  }


  HRESULT D3D9DeviceEx::CreateShaderModule(
        D3D9CommonShader*     pShaderModule,
        VkShaderStageFlagBits ShaderStage,
  const DWORD*                pShaderBytecode,
  const DxsoModuleInfo*       pModuleInfo) {
    try {
      *pShaderModule = m_shaderModules->GetShaderModule(this,
        ShaderStage, pModuleInfo, pShaderBytecode);

      return D3D_OK;
    }
    catch (const DxvkError& e) {
      Logger::err(e.message());
      return D3DERR_INVALIDCALL;
    }
  }


  template <
    DxsoProgramType  ProgramType,
    D3D9ConstantType ConstantType,
    typename         T>
    HRESULT D3D9DeviceEx::SetShaderConstants(
            UINT  StartRegister,
      const T*    pConstantData,
            UINT  Count) {
    constexpr uint32_t regCountHardware = DetermineRegCount(ProgramType, ConstantType, false);
    constexpr uint32_t regCountSoftware = DetermineRegCount(ProgramType, ConstantType, true);

    if (unlikely(StartRegister + Count > regCountSoftware))
      return D3DERR_INVALIDCALL;

    Count = UINT(
      std::max<INT>(
        std::clamp<INT>(Count + StartRegister, 0, regCountHardware) - INT(StartRegister),
        0));

    if (unlikely(Count == 0))
      return D3D_OK;

    if (unlikely(pConstantData == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(ShouldRecord()))
      return m_recorder->SetShaderConstants<ProgramType, ConstantType, T>(
        StartRegister,
        pConstantData,
        Count);

    auto DetermineMaxCount = [&](const auto& shader) {
      if (unlikely(shader == nullptr))
        return 0u;

      const auto& meta = GetCommonShader(shader)->GetMeta();

      if constexpr      (ConstantType == D3D9ConstantType::Float)
        return meta.maxConstIndexF;
      else if constexpr (ConstantType == D3D9ConstantType::Int)
        return meta.maxConstIndexI;
      else
        return meta.maxConstIndexB;
    };

    uint32_t maxCount = ProgramType == DxsoProgramTypes::VertexShader
      ? DetermineMaxCount(m_state.vertexShader)
      : DetermineMaxCount(m_state.pixelShader);

    m_consts[ProgramType].dirty |= StartRegister < maxCount;

    UpdateStateConstants<ProgramType, ConstantType, T>(
      &m_state,
      StartRegister,
      pConstantData,
      Count);

    return D3D_OK;
  }


  void D3D9DeviceEx::UpdateFixedFunctionVS() {
    // Shader...
    bool hasPositionT = m_state.vertexDecl != nullptr ? m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasPositionT) : false;

    if (unlikely(hasPositionT && m_state.vertexShader != nullptr && !m_flags.test(D3D9DeviceFlag::DirtyProgVertexShader))) {
      m_flags.set(D3D9DeviceFlag::DirtyInputLayout);
      m_flags.set(D3D9DeviceFlag::DirtyFFVertexShader);
      m_flags.set(D3D9DeviceFlag::DirtyProgVertexShader);
    }

    if (m_flags.test(D3D9DeviceFlag::DirtyFFVertexShader)) {
      m_flags.clr(D3D9DeviceFlag::DirtyFFVertexShader);

      D3D9FFShaderKeyVS key;
      key.HasColor0    = m_state.vertexDecl != nullptr ? m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasColor0)   : false;
      key.HasColor1    = m_state.vertexDecl != nullptr ? m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasColor1)   : false;
      key.HasPositionT = hasPositionT;

      bool lighting    = m_state.renderStates[D3DRS_LIGHTING] != 0 && !key.HasPositionT;
      bool colorVertex = m_state.renderStates[D3DRS_COLORVERTEX] != 0;
      uint32_t mask    = (lighting && colorVertex)
                       ? (key.HasColor0 ? D3DMCS_COLOR1 : D3DMCS_MATERIAL)
                       | (key.HasColor1 ? D3DMCS_COLOR2 : D3DMCS_MATERIAL)
                       : 0;

      key.UseLighting      = lighting;
      key.NormalizeNormals = m_state.renderStates[D3DRS_NORMALIZENORMALS];

      key.DiffuseSource  = D3DMATERIALCOLORSOURCE(m_state.renderStates[D3DRS_DIFFUSEMATERIALSOURCE]  & mask);
      key.AmbientSource  = D3DMATERIALCOLORSOURCE(m_state.renderStates[D3DRS_AMBIENTMATERIALSOURCE]  & mask);
      key.SpecularSource = D3DMATERIALCOLORSOURCE(m_state.renderStates[D3DRS_SPECULARMATERIALSOURCE] & mask);
      key.EmissiveSource = D3DMATERIALCOLORSOURCE(m_state.renderStates[D3DRS_EMISSIVEMATERIALSOURCE] & mask);

      for (uint32_t i = 0; i < key.TexcoordIndices.size(); i++) {
        key.TransformFlags[i]  = m_state.textureStages[i][D3DTSS_TEXTURETRANSFORMFLAGS] & ~(D3DTTFF_PROJECTED);
        key.TexcoordIndices[i] = m_state.textureStages[i][D3DTSS_TEXCOORDINDEX];
      }

      key.LightCount = 0;

      if (key.UseLighting) {
        for (uint32_t i = 0; i < caps::MaxEnabledLights; i++) {
          if (m_state.enabledLightIndices[i] != UINT32_MAX)
            key.LightCount++;
        }
      }

      EmitCs([
        this,
        cKey     = key,
       &cShaders = m_ffModules
      ](DxvkContext* ctx) {
        auto shader = cShaders.GetShaderModule(this, cKey);
        ctx->bindShader(VK_SHADER_STAGE_VERTEX_BIT, shader.GetShader());
      });
    }

    if (hasPositionT && m_flags.test(D3D9DeviceFlag::DirtyFFViewport)) {
      m_flags.clr(D3D9DeviceFlag::DirtyFFViewport);
      m_flags.set(D3D9DeviceFlag::DirtyFFVertexData);

      const auto& vp = m_state.viewport;
      // For us to account for the Vulkan viewport rules
      // when translating Window Coords -> Real Coords:
      // We need to negate the inverse extent we multiply by,
      // this follows through to the offset when that gets
      // timesed by it.
      // The 1.0f additional offset however does not,
      // so we account for that there manually.

      m_viewportInfo.inverseExtent = Vector4(
         2.0f / float(vp.Width),
        -2.0f / float(vp.Height),
        1.0f,
        1.0f);

      m_viewportInfo.inverseOffset = Vector4(
        -float(vp.X), -float(vp.Y),
         0.0f,         0.0f);

      m_viewportInfo.inverseOffset = m_viewportInfo.inverseOffset * m_viewportInfo.inverseExtent;

      m_viewportInfo.inverseOffset = m_viewportInfo.inverseOffset + Vector4(-1.0f, 1.0f, 0.0f, 0.0f);
    }

    // Constants...
    if (m_flags.test(D3D9DeviceFlag::DirtyFFVertexData)) {
      m_flags.clr(D3D9DeviceFlag::DirtyFFVertexData);

      DxvkBufferSliceHandle slice = m_vsFixedFunction->allocSlice();

      EmitCs([
        cBuffer = m_vsFixedFunction,
        cSlice  = slice
      ] (DxvkContext* ctx) {
        ctx->invalidateBuffer(cBuffer, cSlice);
      });

      auto WorldView    = m_state.transforms[GetTransformIndex(D3DTS_VIEW)] * m_state.transforms[GetTransformIndex(D3DTS_WORLD)];
      auto NormalMatrix = inverse(WorldView);

      D3D9FixedFunctionVS* data = reinterpret_cast<D3D9FixedFunctionVS*>(slice.mapPtr);
      data->WorldView    = WorldView;
      data->NormalMatrix = NormalMatrix;
      data->Projection   = m_state.transforms[GetTransformIndex(D3DTS_PROJECTION)];

      for (uint32_t i = 0; i < data->TexcoordMatrices.size(); i++)
        data->TexcoordMatrices[i] = m_state.transforms[GetTransformIndex(D3DTS_TEXTURE0) + i];

      data->ViewportInfo = m_viewportInfo;
      
      DecodeD3DCOLOR(m_state.renderStates[D3DRS_AMBIENT], data->GlobalAmbient.data);

      uint32_t lightIdx = 0;
      for (uint32_t i = 0; i < caps::MaxEnabledLights; i++) {
        auto idx = m_state.enabledLightIndices[i];
        if (idx == UINT32_MAX)
          continue;

        data->Lights[lightIdx++] = D3D9Light(m_state.lights[idx].value(), m_state.transforms[GetTransformIndex(D3DTS_VIEW)]);
      }

      data->Material = m_state.material;
    }
  }


  void D3D9DeviceEx::UpdateFixedFunctionPS() {
    // Shader...
    if (m_flags.test(D3D9DeviceFlag::DirtyFFPixelShader)) {
      m_flags.clr(D3D9DeviceFlag::DirtyFFPixelShader);

      // Used args for a given operation.
      auto ArgsMask = [](DWORD Op) {
        switch (Op) {
          case D3DTOP_DISABLE:
            return 0b0u; // No Args
          case D3DTOP_SELECTARG1:
          case D3DTOP_PREMODULATE:
            return 0b10u; // Arg 1
          case D3DTOP_SELECTARG2:
            return 0b100u; // Arg 2
          case D3DTOP_MULTIPLYADD:
          case D3DTOP_LERP:
            return 0b111u; // Arg 0, 1, 2 
          default:
            return 0b110u; // Arg 1, 2
        }
      };

      D3D9FFShaderKeyFS key;
      for (uint32_t i = 0; i < caps::TextureStageCount; i++) {
        auto& stage = key.Stages[i].data;
        auto& data  = m_state.textureStages[i];

        stage.ColorOp = D3DTOP_DISABLE;
        stage.AlphaOp = D3DTOP_DISABLE;

        // Subsequent stages do not occur if this is true.
        if (data[D3DTSS_COLOROP] == D3DTOP_DISABLE)
          break;

        // If the stage is invalid (ie. no texture bound),
        // this and all subsequent stages get disabled.
        if (m_state.textures[i] == nullptr) {
          if (((data[D3DTSS_COLORARG0] & D3DTA_SELECTMASK) == D3DTA_TEXTURE && (ArgsMask(data[D3DTSS_COLOROP]) & (1 << 0u)))
           || ((data[D3DTSS_COLORARG1] & D3DTA_SELECTMASK) == D3DTA_TEXTURE && (ArgsMask(data[D3DTSS_COLOROP]) & (1 << 1u)))
           || ((data[D3DTSS_COLORARG2] & D3DTA_SELECTMASK) == D3DTA_TEXTURE && (ArgsMask(data[D3DTSS_COLOROP]) & (1 << 2u))))
            break;
        }

        stage.ColorOp = data[D3DTSS_COLOROP];
        stage.AlphaOp = data[D3DTSS_ALPHAOP];

        stage.ColorArg0 = data[D3DTSS_COLORARG0];
        stage.ColorArg1 = data[D3DTSS_COLORARG1];
        stage.ColorArg2 = data[D3DTSS_COLORARG2];

        stage.AlphaArg0 = data[D3DTSS_ALPHAARG0];
        stage.AlphaArg1 = data[D3DTSS_ALPHAARG1];
        stage.AlphaArg2 = data[D3DTSS_ALPHAARG2];

        const uint32_t samplerOffset = i * 2;
        stage.Type         = (m_samplerTypeBitfield >> samplerOffset) & 0xffu;
        stage.ResultIsTemp = data[D3DTSS_RESULTARG] == D3DTA_TEMP;

        stage.Projected = data[D3DTSS_TEXTURETRANSFORMFLAGS] & D3DTTFF_PROJECTED ? 1 : 0;
      }

      EmitCs([
        this,
        cKey     = key,
       &cShaders = m_ffModules
      ](DxvkContext* ctx) {
        auto shader = cShaders.GetShaderModule(this, cKey);
        ctx->bindShader(VK_SHADER_STAGE_FRAGMENT_BIT, shader.GetShader());
      });
    }

    // Constants

    if (m_flags.test(D3D9DeviceFlag::DirtyFFPixelData)) {
      m_flags.clr(D3D9DeviceFlag::DirtyFFPixelData);

      DxvkBufferSliceHandle slice = m_psFixedFunction->allocSlice();

      EmitCs([
        cBuffer = m_psFixedFunction,
        cSlice  = slice
      ] (DxvkContext* ctx) {
        ctx->invalidateBuffer(cBuffer, cSlice);
      });

      auto& rs = m_state.renderStates;

      D3D9FixedFunctionPS* data = reinterpret_cast<D3D9FixedFunctionPS*>(slice.mapPtr);
      DecodeD3DCOLOR((D3DCOLOR)rs[D3DRS_TEXTUREFACTOR], data->textureFactor.data);
    }
  }


  bool D3D9DeviceEx::UseProgrammableVS() {
    return m_state.vertexShader != nullptr
      && m_state.vertexDecl != nullptr
      && !m_state.vertexDecl->TestFlag(D3D9VertexDeclFlag::HasPositionT);
  }


  bool D3D9DeviceEx::UseProgrammablePS() {
    return m_state.pixelShader != nullptr;
  }


  void D3D9DeviceEx::UpdateSamplerSpecConsant(uint32_t value) {
    EmitCs([cBitfield = value](DxvkContext* ctx) {
      ctx->setSpecConstant(D3D9SpecConstantId::SamplerType, cBitfield);
    });

    m_lastSamplerTypeBitfield = value;
  }


  void D3D9DeviceEx::ApplyPrimitiveType(
    DxvkContext*      pContext,
    D3DPRIMITIVETYPE  PrimType) {
    if (m_iaState.primitiveType != PrimType) {
      m_iaState.primitiveType = PrimType;

      auto iaState = DecodeInputAssemblyState(PrimType);
      pContext->setInputAssemblyState(iaState);
    }
  }


  void D3D9DeviceEx::ResolveZ() {
    D3D9Surface*           src = static_cast<D3D9Surface*>(m_state.depthStencil);
    IDirect3DBaseTexture9* dst = m_state.textures[0];

    if (unlikely(!src || !dst))
      return;

    D3D9CommonTexture* srcTextureInfo = GetCommonTexture(src);
    D3D9CommonTexture* dstTextureInfo = GetCommonTexture(dst);

    const D3D9_COMMON_TEXTURE_DESC* srcDesc = srcTextureInfo->Desc();
    const D3D9_COMMON_TEXTURE_DESC* dstDesc = dstTextureInfo->Desc();

    if (unlikely(dstDesc->MultiSample > D3DMULTISAMPLE_NONMASKABLE))
      return;

    const D3D9_VK_FORMAT_MAPPING srcFormatInfo = LookupFormat(srcDesc->Format);
    const D3D9_VK_FORMAT_MAPPING dstFormatInfo = LookupFormat(dstDesc->Format);
    
    auto srcVulkanFormatInfo = imageFormatInfo(srcFormatInfo.FormatColor);
    auto dstVulkanFormatInfo = imageFormatInfo(dstFormatInfo.FormatColor);
    
    const VkImageSubresource dstSubresource =
      dstTextureInfo->GetSubresourceFromIndex(
        dstVulkanFormatInfo->aspectMask, 0);
    
    const VkImageSubresource srcSubresource =
      srcTextureInfo->GetSubresourceFromIndex(
        srcVulkanFormatInfo->aspectMask, src->GetSubresource());
    
    const VkImageSubresourceLayers dstSubresourceLayers = {
      dstSubresource.aspectMask,
      dstSubresource.mipLevel,
      dstSubresource.arrayLayer, 1 };
    
    const VkImageSubresourceLayers srcSubresourceLayers = {
      srcSubresource.aspectMask,
      srcSubresource.mipLevel,
      srcSubresource.arrayLayer, 1 };

    if (dstDesc->MultiSample <= D3DMULTISAMPLE_NONMASKABLE) {
      EmitCs([
        cDstImage  = dstTextureInfo->GetImage(),
        cSrcImage  = srcTextureInfo->GetImage(),
        cDstLayers = dstSubresourceLayers,
        cSrcLayers = srcSubresourceLayers
      ] (DxvkContext* ctx) {
        ctx->copyImage(
          cDstImage, cDstLayers, VkOffset3D { 0, 0, 0 },
          cSrcImage, cSrcLayers, VkOffset3D { 0, 0, 0 },
          cDstImage->mipLevelExtent(cDstLayers.mipLevel));
      });
    } else {      
      EmitCs([
        cDstImage  = dstTextureInfo->GetImage(),
        cSrcImage  = srcTextureInfo->GetImage(),
        cDstSubres = dstSubresourceLayers,
        cSrcSubres = srcSubresourceLayers
      ] (DxvkContext* ctx) {
        VkImageResolve region;
        region.srcSubresource = cSrcSubres;
        region.srcOffset      = VkOffset3D { 0, 0, 0 };
        region.dstSubresource = cDstSubres;
        region.dstOffset      = VkOffset3D { 0, 0, 0 };
        region.extent         = cDstImage->mipLevelExtent(cDstSubres.mipLevel);

        ctx->resolveImage(cDstImage, cSrcImage, region, cDstImage->info().format);
      });
    }
  }


  void D3D9DeviceEx::TransitionImage(D3D9CommonTexture* pResource, VkImageLayout NewLayout) {
    EmitCs([
      cImage        = pResource->GetImage(),
      cNewLayout    = NewLayout
    ] (DxvkContext* ctx) {
      ctx->changeImageLayout(
        cImage, cNewLayout);
    });
  }

}
