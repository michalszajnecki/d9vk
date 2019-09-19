#include "d3d9_swapchain.h"
#include "d3d9_surface.h"
#include "d3d9_monitor.h"

#include <d3d9_presenter_frag.h>
#include <d3d9_presenter_vert.h>

namespace dxvk {

  static uint16_t MapGammaControlPoint(float x) {
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    return uint16_t(65535.0f * x);
  }


  struct D3D9PresentInfo {
    float scale[2];
    float offset[2];
  };


  D3D9SwapChainEx::D3D9SwapChainEx(
          D3D9DeviceEx*          pDevice,
          D3DPRESENT_PARAMETERS* pPresentParams,
    const D3DDISPLAYMODEEX*      pFullscreenDisplayMode)
    : D3D9SwapChainExBase(pDevice)
    , m_device           (pDevice->GetDXVKDevice())
    , m_context          (m_device->createContext()) {
    UpdateMonitorInfo();

    this->NormalizePresentParameters(pPresentParams);
    m_presentParams = *pPresentParams;
    m_window = m_presentParams.hDeviceWindow;

    UpdatePresentRegion(nullptr, nullptr);
    if (!pDevice->GetOptions()->deferSurfaceCreation)
      CreatePresenter();

    CreateBackBuffer();
    CreateHud();

    InitRenderState();
    InitSamplers();
    InitShaders();
    InitRamp();

    // Apply initial window mode and fullscreen state
    if (!m_presentParams.Windowed && FAILED(EnterFullscreenMode(pPresentParams, pFullscreenDisplayMode)))
      throw DxvkError("D3D9: Failed to set initial fullscreen state");
  }


  D3D9SwapChainEx::~D3D9SwapChainEx() {
    RestoreDisplayMode(m_monitor);

    m_device->waitForIdle();

    if (m_backBuffer)
      m_backBuffer->ReleasePrivate();
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::QueryInterface(REFIID riid, void** ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(IDirect3DSwapChain9)
     || (GetParent()->IsExtended() && riid == __uuidof(IDirect3DSwapChain9Ex))) {
      *ppvObject = ref(this);
      return S_OK;
    }

    Logger::warn("D3D9SwapChainEx::QueryInterface: Unknown interface query");
    Logger::warn(str::format(riid));
    return E_NOINTERFACE;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::Present(
    const RECT*    pSourceRect,
    const RECT*    pDestRect,
          HWND     hDestWindowOverride,
    const RGNDATA* pDirtyRegion,
          DWORD    dwFlags) {
    auto lock = m_parent->LockDevice();

    uint32_t presentInterval = m_presentParams.PresentationInterval;

    // This is not true directly in d3d9 to to timing differences that don't matter for us.
    // For our purposes...
    // D3DPRESENT_INTERVAL_DEFAULT (0) == D3DPRESENT_INTERVAL_ONE (1) which means VSYNC.
    presentInterval = std::max(presentInterval, 1u);

    if (presentInterval == D3DPRESENT_INTERVAL_IMMEDIATE || (dwFlags & D3DPRESENT_FORCEIMMEDIATE))
      presentInterval = 0;

    auto options = m_parent->GetOptions();

    if (options->presentInterval >= 0)
      presentInterval = options->presentInterval;

    bool vsync  = presentInterval != 0;

    HWND window = m_presentParams.hDeviceWindow;
    if (hDestWindowOverride != nullptr)
      window    = hDestWindowOverride;

    bool recreate = false;
    recreate   |= m_presenter == nullptr;
    recreate   |= window != m_window;    

    m_window    = window;

    m_dirty    |= vsync != m_vsync;
    m_dirty    |= UpdatePresentRegion(pSourceRect, pDestRect);
    m_dirty    |= recreate;
    m_vsync     = vsync;

    if (recreate)
      CreatePresenter();

    if (std::exchange(m_dirty, false))
      RecreateSwapChain(vsync);

    FlushDevice();

    try {
      PresentImage(presentInterval);
      return D3D_OK;
    } catch (const DxvkError& e) {
      Logger::err(e.message());
      return D3DERR_INVALIDCALL;
    }
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetFrontBufferData(IDirect3DSurface9* pDestSurface) {
    Logger::warn("D3D9SwapChainEx::GetFrontBufferData: Stub");
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetBackBuffer(
          UINT                iBackBuffer,
          D3DBACKBUFFER_TYPE  Type,
          IDirect3DSurface9** ppBackBuffer) {
    InitReturnPtr(ppBackBuffer);

    if (ppBackBuffer == nullptr)
      return D3DERR_INVALIDCALL;

    if (iBackBuffer > 0) {
      Logger::err("D3D9: GetBackBuffer: iBackBuffer > 0 not supported");
      return D3DERR_INVALIDCALL;
    }

    *ppBackBuffer = ref(m_backBuffer);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetRasterStatus(D3DRASTER_STATUS* pRasterStatus) {
    // We could use D3DKMTGetScanLine but Wine doesn't implement that.
    // So... we lie here and make some stuff up
    // enough that it makes games work.

    // Assume there's 20 lines in a vBlank.
    constexpr uint32_t vBlankLineCount = 20;

    if (pRasterStatus == nullptr)
      return D3DERR_INVALIDCALL;

    D3DDISPLAYMODEEX mode;
    mode.Size = sizeof(mode);
    if (FAILED(this->GetDisplayModeEx(&mode, nullptr)))
      return D3DERR_INVALIDCALL;

    uint32_t scanLineCount = mode.Height + vBlankLineCount;

    auto nowUs = std::chrono::time_point_cast<std::chrono::microseconds>(
      std::chrono::high_resolution_clock::now())
      .time_since_epoch();

    auto frametimeUs = std::chrono::microseconds(1000000u / mode.RefreshRate);
    auto scanLineUs  = frametimeUs / scanLineCount;

    pRasterStatus->ScanLine = (nowUs % frametimeUs) / scanLineUs;
    pRasterStatus->InVBlank = pRasterStatus->ScanLine >= mode.Height;

    if (pRasterStatus->InVBlank)
      pRasterStatus->ScanLine = 0;

    return D3D_OK;
  }

  
  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetDisplayMode(D3DDISPLAYMODE* pMode) {
    if (pMode == nullptr)
      return D3DERR_INVALIDCALL;

    *pMode = D3DDISPLAYMODE();

    D3DDISPLAYMODEEX mode;
    mode.Size = sizeof(mode);
    HRESULT hr = this->GetDisplayModeEx(&mode, nullptr);

    if (FAILED(hr))
      return hr;

    pMode->Width       = mode.Width;
    pMode->Height      = mode.Height;
    pMode->Format      = mode.Format;
    pMode->RefreshRate = mode.RefreshRate;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetPresentParameters(D3DPRESENT_PARAMETERS* pPresentationParameters) {
    if (pPresentationParameters == nullptr)
      return D3DERR_INVALIDCALL;

    *pPresentationParameters = m_presentParams;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetLastPresentCount(UINT* pLastPresentCount) {
    Logger::warn("D3D9SwapChainEx::GetLastPresentCount: Stub");
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetPresentStats(D3DPRESENTSTATS* pPresentationStatistics) {
    Logger::warn("D3D9SwapChainEx::GetPresentStats: Stub");
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetDisplayModeEx(D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRotation) {
    if (pMode == nullptr && pRotation == nullptr)
      return D3DERR_INVALIDCALL;

    if (pRotation != nullptr)
      *pRotation = D3DDISPLAYROTATION_IDENTITY;

    if (pMode != nullptr) {
      DEVMODEW devMode = DEVMODEW();
      devMode.dmSize = sizeof(devMode);

      if (!::EnumDisplaySettingsW(m_monInfo.szDevice, ENUM_CURRENT_SETTINGS, &devMode)) {
        Logger::err("D3D9SwapChainEx::GetDisplayModeEx: Failed to enum display settings");
        return D3DERR_INVALIDCALL;
      }

      pMode->Size             = sizeof(D3DDISPLAYMODEEX);
      pMode->Width            = devMode.dmPelsWidth;
      pMode->Height           = devMode.dmPelsHeight;
      pMode->RefreshRate      = devMode.dmDisplayFrequency;
      pMode->Format           = D3DFMT_X8R8G8B8;
      pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
    }

    return D3D_OK;
  }


  HRESULT D3D9SwapChainEx::Reset(
          D3DPRESENT_PARAMETERS* pPresentParams,
          D3DDISPLAYMODEEX*      pFullscreenDisplayMode) {
    auto lock = m_parent->LockDevice();

    this->NormalizePresentParameters(pPresentParams);

    m_dirty    |= m_presentParams.BackBufferFormat   != pPresentParams->BackBufferFormat
               || m_presentParams.BackBufferWidth    != pPresentParams->BackBufferWidth
               || m_presentParams.BackBufferHeight   != pPresentParams->BackBufferHeight
               || m_presentParams.BackBufferCount    != pPresentParams->BackBufferCount;

    bool changeFullscreen = m_presentParams.Windowed != pPresentParams->Windowed;

    if (pPresentParams->Windowed) {
      if (changeFullscreen)
        this->LeaveFullscreenMode();

      // Adjust window position and size
      RECT newRect = { 0, 0, 0, 0 };
      RECT oldRect = { 0, 0, 0, 0 };
      
      ::GetWindowRect(m_window, &oldRect);
      ::SetRect(&newRect, 0, 0, pPresentParams->BackBufferWidth, pPresentParams->BackBufferHeight);
      ::AdjustWindowRectEx(&newRect,
        ::GetWindowLongW(m_window, GWL_STYLE), FALSE,
        ::GetWindowLongW(m_window, GWL_EXSTYLE));
      ::SetRect(&newRect, 0, 0, newRect.right - newRect.left, newRect.bottom - newRect.top);
      ::OffsetRect(&newRect, oldRect.left, oldRect.top);    
      ::MoveWindow(m_window, newRect.left, newRect.top,
        newRect.right - newRect.left, newRect.bottom - newRect.top, TRUE);
    }
    else {
      if (changeFullscreen)
        this->EnterFullscreenMode(pPresentParams, pFullscreenDisplayMode);
      else
        ChangeDisplayMode(pPresentParams, pFullscreenDisplayMode);

      // Move the window so that it covers the entire output    
      RECT rect;
      GetMonitorRect(GetDefaultMonitor(), &rect);
    
      ::SetWindowPos(m_window, HWND_TOPMOST,
        rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
        SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    }

    m_presentParams = *pPresentParams;

    if (changeFullscreen)
      SetGammaRamp(0, &m_ramp);

    UpdatePresentRegion(nullptr, nullptr);
    CreateBackBuffer();
    return D3D_OK;
  }


  HRESULT D3D9SwapChainEx::WaitForVBlank() {
    Logger::warn("D3D9SwapChainEx::WaitForVBlank: Stub");
    return D3D_OK;
  }


  void    D3D9SwapChainEx::SetGammaRamp(
            DWORD         Flags,
      const D3DGAMMARAMP* pRamp) {
    if (unlikely(pRamp == nullptr))
      return;

    m_ramp = *pRamp;

    bool isIdentity = true;

    std::array<D3D9_VK_GAMMA_CP, NumControlPoints> cp;
      
    for (uint32_t i = 0; i < NumControlPoints; i++) {
      uint16_t identity = MapGammaControlPoint(float(i) / float(NumControlPoints - 1));

      cp[i].R = pRamp->red[i];
      cp[i].G = pRamp->green[i];
      cp[i].B = pRamp->blue[i];
      cp[i].A = 0;

      isIdentity &= cp[i].R == identity
                 && cp[i].G == identity
                 && cp[i].B == identity;
    }

    if (isIdentity || m_presentParams.Windowed)
      DestroyGammaTexture();
    else
      CreateGammaTexture(NumControlPoints, cp.data());
  }


  void    D3D9SwapChainEx::GetGammaRamp(D3DGAMMARAMP* pRamp) {
    if (likely(pRamp != nullptr))
      *pRamp = m_ramp;
  }


  void    D3D9SwapChainEx::Invalidate(HWND hWindow) {
    if (hWindow == nullptr)
      hWindow = m_parent->GetWindow();

    if (m_presentParams.hDeviceWindow == hWindow)
      m_presenter = nullptr;
  }


  void D3D9SwapChainEx::NormalizePresentParameters(D3DPRESENT_PARAMETERS* pPresentParams) {
    if (pPresentParams->hDeviceWindow == nullptr)
      pPresentParams->hDeviceWindow    = m_parent->GetWindow();

    pPresentParams->BackBufferCount    = std::max(pPresentParams->BackBufferCount, 1u);

    if (pPresentParams->Windowed) {
      GetWindowClientSize(pPresentParams->hDeviceWindow,
        pPresentParams->BackBufferWidth  ? nullptr : &pPresentParams->BackBufferWidth,
        pPresentParams->BackBufferHeight ? nullptr : &pPresentParams->BackBufferHeight);
    }
    else {
      GetMonitorClientSize(GetDefaultMonitor(),
        pPresentParams->BackBufferWidth  ? nullptr : &pPresentParams->BackBufferWidth,
        pPresentParams->BackBufferHeight ? nullptr : &pPresentParams->BackBufferHeight);
    }

    if (pPresentParams->BackBufferFormat == D3DFMT_UNKNOWN)
      pPresentParams->BackBufferFormat = D3DFMT_X8R8G8B8;

    if (env::getEnvVar("DXVK_FORCE_WINDOWED") == "1")
      pPresentParams->Windowed         = TRUE;
  }


  void D3D9SwapChainEx::PresentImage(UINT SyncInterval) {
    // Wait for the sync event so that we respect the maximum frame latency
    auto syncEvent = m_parent->GetFrameSyncEvent(m_presentParams.BackBufferCount);
    syncEvent->wait();
    
    if (m_hud != nullptr)
      m_hud->update();

    for (uint32_t i = 0; i < SyncInterval || i < 1; i++) {
      SynchronizePresent();

      m_context->beginRecording(
        m_device->createCommandList());
      
      // Resolve back buffer if it is multisampled. We
      // only have to do it only for the first frame.
      if (m_swapImageResolve != nullptr && i == 0) {
        VkImageSubresourceLayers resolveSubresource;
        resolveSubresource.aspectMask      = VK_IMAGE_ASPECT_COLOR_BIT;
        resolveSubresource.mipLevel        = 0;
        resolveSubresource.baseArrayLayer  = 0;
        resolveSubresource.layerCount      = 1;

        VkImageResolve resolveRegion;
        resolveRegion.srcSubresource = resolveSubresource;
        resolveRegion.srcOffset      = VkOffset3D { 0, 0, 0 };
        resolveRegion.dstSubresource = resolveSubresource;
        resolveRegion.dstOffset      = VkOffset3D { 0, 0, 0 };
        resolveRegion.extent         = m_swapImage->info().extent;
        
        m_context->resolveImage(
          m_swapImageResolve, m_swapImage,
          resolveRegion, VK_FORMAT_UNDEFINED);
      }
      
      // Presentation semaphores and WSI swap chain image
      vk::PresenterInfo info = m_presenter->info();
      vk::PresenterSync sync = m_presenter->getSyncSemaphores();

      uint32_t imageIndex = 0;

      VkResult status = m_presenter->acquireNextImage(
        sync.acquire, VK_NULL_HANDLE, imageIndex);

      while (status != VK_SUCCESS && status != VK_SUBOPTIMAL_KHR) {
        RecreateSwapChain(m_vsync);
        
        info = m_presenter->info();
        sync = m_presenter->getSyncSemaphores();

        status = m_presenter->acquireNextImage(
          sync.acquire, VK_NULL_HANDLE, imageIndex);
      }

      // Use an appropriate texture filter depending on whether
      // the back buffer size matches the swap image size
      m_context->bindShader(VK_SHADER_STAGE_VERTEX_BIT,   m_vertShader);
      m_context->bindShader(VK_SHADER_STAGE_FRAGMENT_BIT, m_fragShader);

      DxvkRenderTargets renderTargets;
      renderTargets.color[0].view   = m_imageViews.at(imageIndex);
      renderTargets.color[0].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      m_context->bindRenderTargets(renderTargets, false);

      VkViewport viewport;
      viewport.x        = float(m_dstRect.left);
      viewport.y        = float(m_dstRect.top);
      viewport.width    = float(info.imageExtent.width  - m_dstRect.left);
      viewport.height   = float(info.imageExtent.height - m_dstRect.top);
      viewport.minDepth = 0.0f;
      viewport.maxDepth = 1.0f;

      D3D9PresentInfo presentInfoConsts;
      presentInfoConsts.scale[0]  = float(m_srcRect.right  - m_srcRect.left) / float(m_swapImage->info().extent.width);
      presentInfoConsts.scale[1]  = float(m_srcRect.bottom - m_srcRect.top)  / float(m_swapImage->info().extent.height);

      presentInfoConsts.offset[0] = float(m_srcRect.left) / float(m_swapImage->info().extent.width);
      presentInfoConsts.offset[1] = float(m_srcRect.top)  / float(m_swapImage->info().extent.height);

      m_context->pushConstants(0, sizeof(D3D9PresentInfo), &presentInfoConsts);
      
      VkRect2D scissor;
      scissor.offset.x      = 0;
      scissor.offset.y      = 0;
      scissor.extent.width  = info.imageExtent.width  - m_dstRect.left;
      scissor.extent.height = info.imageExtent.height - m_dstRect.top;

      m_context->setViewports(1, &viewport, &scissor);

      m_context->setRasterizerState(m_rsState);
      m_context->setMultisampleState(m_msState);
      m_context->setDepthStencilState(m_dsState);
      m_context->setLogicOpState(m_loState);
      m_context->setBlendMode(0, m_blendMode);
      
      m_context->setInputAssemblyState(m_iaState);
      m_context->setInputLayout(0, nullptr, 0, nullptr);

      m_context->bindResourceSampler(BindingIds::Image, m_samplerFitting);
      m_context->bindResourceSampler(BindingIds::Gamma, m_gammaSampler);

      m_context->bindResourceView(BindingIds::Image, m_swapImageView, nullptr);
      m_context->bindResourceView(BindingIds::Gamma, m_gammaTextureView, nullptr);

      m_context->draw(3, 1, 0, 0);

      if (m_hud != nullptr)
        m_hud->render(m_context, info.imageExtent);
      
      if (i + 1 >= SyncInterval)
        m_context->queueSignal(syncEvent);

      m_device->submitCommandList(
        m_context->endRecording(),
        sync.acquire, sync.present);
      
      m_device->presentImage(m_presenter,
        sync.present, &m_presentStatus);

      if (m_presentStatus.result != VK_NOT_READY
       && m_presentStatus.result != VK_SUCCESS)
        RecreateSwapChain(m_vsync);
    }
  }


  void D3D9SwapChainEx::SynchronizePresent() {
    // Recreate swap chain if the previous present call failed
    VkResult status = m_device->waitForSubmission(&m_presentStatus);

    if (status != VK_SUCCESS)
      RecreateSwapChain(m_vsync);
  }


  void D3D9SwapChainEx::RecreateSwapChain(BOOL Vsync) {
    // Ensure that we can safely destroy the swap chain
    m_device->waitForSubmission(&m_presentStatus);
    m_presentStatus.result = VK_SUCCESS;

    vk::PresenterDesc presenterDesc;
    presenterDesc.imageExtent     = GetPresentExtent();
    presenterDesc.imageCount      = PickImageCount(m_presentParams.BackBufferCount + 1);
    presenterDesc.numFormats      = PickFormats(EnumerateFormat(m_presentParams.BackBufferFormat), presenterDesc.formats);
    presenterDesc.numPresentModes = PickPresentModes(Vsync, presenterDesc.presentModes);

    if (m_presenter->recreateSwapChain(presenterDesc) != VK_SUCCESS)
      throw DxvkError("D3D9SwapChainEx: Failed to recreate swap chain");
    
    CreateRenderTargetViews();
  }


  void D3D9SwapChainEx::CreatePresenter() {
    DxvkDeviceQueue graphicsQueue = m_device->queues().graphics;

    vk::PresenterDevice presenterDevice;
    presenterDevice.queueFamily   = graphicsQueue.queueFamily;
    presenterDevice.queue         = graphicsQueue.queueHandle;
    presenterDevice.adapter       = m_device->adapter()->handle();

    vk::PresenterDesc presenterDesc;
    presenterDesc.imageExtent     = GetPresentExtent();
    presenterDesc.imageCount      = PickImageCount(m_presentParams.BackBufferCount + 1);
    presenterDesc.numFormats      = PickFormats(EnumerateFormat(m_presentParams.BackBufferFormat), presenterDesc.formats);
    presenterDesc.numPresentModes = PickPresentModes(false, presenterDesc.presentModes);

    m_presenter = new vk::Presenter(m_window,
      m_device->adapter()->vki(),
      m_device->vkd(),
      presenterDevice,
      presenterDesc);
    
    CreateRenderTargetViews();
  }


  void D3D9SwapChainEx::CreateRenderTargetViews() {
    vk::PresenterInfo info = m_presenter->info();

    m_imageViews.clear();
    m_imageViews.resize(info.imageCount);

    DxvkImageCreateInfo imageInfo;
    imageInfo.type        = VK_IMAGE_TYPE_2D;
    imageInfo.format      = info.format.format;
    imageInfo.flags       = 0;
    imageInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.extent      = { info.imageExtent.width, info.imageExtent.height, 1 };
    imageInfo.numLayers   = 1;
    imageInfo.mipLevels   = 1;
    imageInfo.usage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    imageInfo.stages      = 0;
    imageInfo.access      = 0;
    imageInfo.tiling      = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.layout      = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    DxvkImageViewCreateInfo viewInfo;
    viewInfo.type         = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format       = info.format.format;
    viewInfo.usage        = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    viewInfo.aspect       = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.minLevel     = 0;
    viewInfo.numLevels    = 1;
    viewInfo.minLayer     = 0;
    viewInfo.numLayers    = 1;

    for (uint32_t i = 0; i < info.imageCount; i++) {
      VkImage imageHandle = m_presenter->getImage(i).image;
      
      Rc<DxvkImage> image = new DxvkImage(
        m_device->vkd(), imageInfo, imageHandle);

      m_imageViews[i] = new DxvkImageView(
        m_device->vkd(), image, viewInfo);
    }
  }


  void D3D9SwapChainEx::FlushDevice() {
    // The presentation code is run from the main rendering thread
    // rather than the command stream thread, so we synchronize.
    m_parent->Flush();
    m_parent->SynchronizeCsThread();
  }


  void D3D9SwapChainEx::CreateBackBuffer() {
    // Explicitly destroy current swap image before
    // creating a new one to free up resources
    if (m_backBuffer)
      m_backBuffer->ReleasePrivate();
    
    m_swapImage         = nullptr;
    m_swapImageResolve  = nullptr;
    m_swapImageView     = nullptr;
    m_backBuffer        = nullptr;

    // Create new back buffer
    D3D9_COMMON_TEXTURE_DESC desc;
    desc.Width              = std::max(m_presentParams.BackBufferWidth,  1u);
    desc.Height             = std::max(m_presentParams.BackBufferHeight, 1u);
    desc.Depth              = 1;
    desc.MipLevels          = 1;
    desc.ArraySize          = 1;
    desc.Format             = EnumerateFormat(m_presentParams.BackBufferFormat);
    desc.MultiSample        = m_presentParams.MultiSampleType;
    desc.MultisampleQuality = m_presentParams.MultiSampleQuality;
    desc.Pool               = D3DPOOL_DEFAULT;
    desc.Usage              = D3DUSAGE_RENDERTARGET;
    desc.Discard            = FALSE;

    m_backBuffer = new D3D9Surface(m_parent, &desc);
    m_backBuffer->AddRefPrivate();

    m_swapImage = m_backBuffer->GetCommonTexture()->GetImage();

    // If the image is multisampled, we need to create
    // another image which we'll use as a resolve target
    if (m_swapImage->info().sampleCount != VK_SAMPLE_COUNT_1_BIT) {
      DxvkImageCreateInfo resolveInfo;
      resolveInfo.type          = VK_IMAGE_TYPE_2D;
      resolveInfo.format        = m_swapImage->info().format;
      resolveInfo.flags         = 0;
      resolveInfo.sampleCount   = VK_SAMPLE_COUNT_1_BIT;
      resolveInfo.extent        = m_swapImage->info().extent;
      resolveInfo.numLayers     = 1;
      resolveInfo.mipLevels     = 1;
      resolveInfo.usage         = VK_IMAGE_USAGE_SAMPLED_BIT
                                | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      resolveInfo.stages        = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                | VK_PIPELINE_STAGE_TRANSFER_BIT;
      resolveInfo.access        = VK_ACCESS_SHADER_READ_BIT
                                | VK_ACCESS_TRANSFER_WRITE_BIT
                                | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                                | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      resolveInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
      resolveInfo.layout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      
      m_swapImageResolve = m_device->createImage(
        resolveInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }

    // Create an image view that allows the
    // image to be bound as a shader resource.
    DxvkImageViewCreateInfo viewInfo;
    viewInfo.type       = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format     = m_swapImage->info().format;
    viewInfo.usage      = VK_IMAGE_USAGE_SAMPLED_BIT;
    viewInfo.aspect     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.minLevel   = 0;
    viewInfo.numLevels  = 1;
    viewInfo.minLayer   = 0;
    viewInfo.numLayers  = 1;
    
    m_swapImageView = m_device->createImageView(
      m_swapImageResolve != nullptr
        ? m_swapImageResolve
        : m_swapImage,
      viewInfo);
    
    // Initialize the image so that we can use it. Clearing
    // to black prevents garbled output for the first frame.
    VkImageSubresourceRange subresources;
    subresources.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    subresources.baseMipLevel   = 0;
    subresources.levelCount     = 1;
    subresources.baseArrayLayer = 0;
    subresources.layerCount     = 1;

    VkClearColorValue clearColor;
    clearColor.float32[0] = 0.0f;
    clearColor.float32[1] = 0.0f;
    clearColor.float32[2] = 0.0f;
    clearColor.float32[3] = 0.0f;

    m_context->beginRecording(
      m_device->createCommandList());
    
    m_context->clearColorImage(
      m_swapImage, clearColor, subresources);

    m_device->submitCommandList(
      m_context->endRecording(),
      VK_NULL_HANDLE,
      VK_NULL_HANDLE);
  }


  void D3D9SwapChainEx::CreateGammaTexture(
            UINT                NumControlPoints,
      const D3D9_VK_GAMMA_CP*   pControlPoints) {
    if (m_gammaTexture == nullptr
     || m_gammaTexture->info().extent.width != NumControlPoints) {
      DxvkImageCreateInfo imgInfo;
      imgInfo.type        = VK_IMAGE_TYPE_1D;
      imgInfo.format      = VK_FORMAT_R16G16B16A16_UNORM;
      imgInfo.flags       = 0;
      imgInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
      imgInfo.extent      = { NumControlPoints, 1, 1 };
      imgInfo.numLayers   = 1;
      imgInfo.mipLevels   = 1;
      imgInfo.usage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT
                          | VK_IMAGE_USAGE_SAMPLED_BIT;
      imgInfo.stages      = VK_PIPELINE_STAGE_TRANSFER_BIT
                          | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      imgInfo.access      = VK_ACCESS_TRANSFER_WRITE_BIT
                          | VK_ACCESS_SHADER_READ_BIT;
      imgInfo.tiling      = VK_IMAGE_TILING_OPTIMAL;
      imgInfo.layout      = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      
      m_gammaTexture = m_device->createImage(
        imgInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

      DxvkImageViewCreateInfo viewInfo;
      viewInfo.type       = VK_IMAGE_VIEW_TYPE_1D;
      viewInfo.format     = VK_FORMAT_R16G16B16A16_UNORM;
      viewInfo.usage      = VK_IMAGE_USAGE_SAMPLED_BIT;
      viewInfo.aspect     = VK_IMAGE_ASPECT_COLOR_BIT;
      viewInfo.minLevel   = 0;
      viewInfo.numLevels  = 1;
      viewInfo.minLayer   = 0;
      viewInfo.numLayers  = 1;
      
      m_gammaTextureView = m_device->createImageView(m_gammaTexture, viewInfo);
    }

    m_context->beginRecording(
      m_device->createCommandList());
    
    m_context->updateImage(m_gammaTexture,
      VkImageSubresourceLayers { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
      VkOffset3D { 0, 0, 0 },
      VkExtent3D { NumControlPoints, 1, 1 },
      pControlPoints, 0, 0);
    
    m_device->submitCommandList(
      m_context->endRecording(),
      VK_NULL_HANDLE,
      VK_NULL_HANDLE);
  }


  void D3D9SwapChainEx::DestroyGammaTexture() {
    m_gammaTexture     = nullptr;
    m_gammaTextureView = nullptr;
  }


  void D3D9SwapChainEx::CreateHud() {
    m_hud = hud::Hud::createHud(m_device);
  }


  void D3D9SwapChainEx::InitRenderState() {
    m_iaState.primitiveTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    m_iaState.primitiveRestart  = VK_FALSE;
    m_iaState.patchVertexCount  = 0;
    
    m_rsState.polygonMode        = VK_POLYGON_MODE_FILL;
    m_rsState.cullMode           = VK_CULL_MODE_BACK_BIT;
    m_rsState.frontFace          = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    m_rsState.depthClipEnable    = VK_FALSE;
    m_rsState.depthBiasEnable    = VK_FALSE;
    m_rsState.sampleCount        = VK_SAMPLE_COUNT_1_BIT;
    
    m_msState.sampleMask            = 0xffffffff;
    m_msState.enableAlphaToCoverage = VK_FALSE;
    
    VkStencilOpState stencilOp;
    stencilOp.failOp      = VK_STENCIL_OP_KEEP;
    stencilOp.passOp      = VK_STENCIL_OP_KEEP;
    stencilOp.depthFailOp = VK_STENCIL_OP_KEEP;
    stencilOp.compareOp   = VK_COMPARE_OP_ALWAYS;
    stencilOp.compareMask = 0xFFFFFFFF;
    stencilOp.writeMask   = 0xFFFFFFFF;
    stencilOp.reference   = 0;
    
    m_dsState.enableDepthTest   = VK_FALSE;
    m_dsState.enableDepthWrite  = VK_FALSE;
    m_dsState.enableStencilTest = VK_FALSE;
    m_dsState.depthCompareOp    = VK_COMPARE_OP_ALWAYS;
    m_dsState.stencilOpFront    = stencilOp;
    m_dsState.stencilOpBack     = stencilOp;
    
    m_loState.enableLogicOp = VK_FALSE;
    m_loState.logicOp       = VK_LOGIC_OP_NO_OP;

    m_blendMode.enableBlending  = VK_FALSE;
    m_blendMode.colorSrcFactor  = VK_BLEND_FACTOR_ONE;
    m_blendMode.colorDstFactor  = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    m_blendMode.colorBlendOp    = VK_BLEND_OP_ADD;
    m_blendMode.alphaSrcFactor  = VK_BLEND_FACTOR_ONE;
    m_blendMode.alphaDstFactor  = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    m_blendMode.alphaBlendOp    = VK_BLEND_OP_ADD;
    m_blendMode.writeMask       = VK_COLOR_COMPONENT_R_BIT
                                | VK_COLOR_COMPONENT_G_BIT
                                | VK_COLOR_COMPONENT_B_BIT
                                | VK_COLOR_COMPONENT_A_BIT;
  }


  void D3D9SwapChainEx::InitSamplers() {
    DxvkSamplerCreateInfo samplerInfo;
    samplerInfo.magFilter       = VK_FILTER_NEAREST;
    samplerInfo.minFilter       = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode      = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.mipmapLodBias   = 0.0f;
    samplerInfo.mipmapLodMin    = 0.0f;
    samplerInfo.mipmapLodMax    = 0.0f;
    samplerInfo.useAnisotropy   = VK_FALSE;
    samplerInfo.maxAnisotropy   = 1.0f;
    samplerInfo.addressModeU    = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV    = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW    = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.compareToDepth  = VK_FALSE;
    samplerInfo.compareOp       = VK_COMPARE_OP_ALWAYS;
    samplerInfo.borderColor     = VkClearColorValue();
    samplerInfo.usePixelCoord   = VK_FALSE;
    m_samplerFitting = m_device->createSampler(samplerInfo);

    samplerInfo.magFilter       = VK_FILTER_LINEAR;
    samplerInfo.minFilter       = VK_FILTER_LINEAR;
    m_samplerScaling = m_device->createSampler(samplerInfo);

    samplerInfo.addressModeU    = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV    = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW    = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    m_gammaSampler = m_device->createSampler(samplerInfo);
  }


  void D3D9SwapChainEx::InitShaders() {
    const SpirvCodeBuffer vsCode(d3d9_presenter_vert);
    const SpirvCodeBuffer fsCode(d3d9_presenter_frag);
    
    const std::array<DxvkResourceSlot, 2> fsResourceSlots = {{
      { BindingIds::Image, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_IMAGE_VIEW_TYPE_2D },
      { BindingIds::Gamma, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_IMAGE_VIEW_TYPE_1D },
    }};

    m_vertShader = m_device->createShader(
      VK_SHADER_STAGE_VERTEX_BIT,
      0, nullptr,
      { 0u, 1u,
        0u, sizeof(D3D9PresentInfo) },
      vsCode);
    
    m_fragShader = m_device->createShader(
      VK_SHADER_STAGE_FRAGMENT_BIT,
      fsResourceSlots.size(),
      fsResourceSlots.data(),
      { 1u, 1u }, fsCode);
  }


  void D3D9SwapChainEx::InitRamp() {
    for (uint32_t i = 0; i < NumControlPoints; i++) {
      DWORD identity = DWORD(MapGammaControlPoint(float(i) / float(NumControlPoints - 1)));

      m_ramp.red[i]   = identity;
      m_ramp.green[i] = identity;
      m_ramp.blue[i]  = identity;
    }
  }


  uint32_t D3D9SwapChainEx::PickFormats(
          D3D9Format                Format,
          VkSurfaceFormatKHR*       pDstFormats) {
    uint32_t n = 0;

    switch (Format) {
      default:
        Logger::warn(str::format("D3D9SwapChainEx: Unexpected format: ", Format));
        
      case D3D9Format::A8R8G8B8:
      case D3D9Format::X8R8G8B8:
      case D3D9Format::A8B8G8R8:
      case D3D9Format::X8B8G8R8: {
        pDstFormats[n++] = { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;

      case D3D9Format::A2R10G10B10:
      case D3D9Format::A2B10G10R10: {
        pDstFormats[n++] = { VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;

      case D3D9Format::X1R5G5B5:
      case D3D9Format::A1R5G5B5: {
        pDstFormats[n++] = { VK_FORMAT_B5G5R5A1_UNORM_PACK16, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_R5G5B5A1_UNORM_PACK16, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_A1R5G5B5_UNORM_PACK16, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      }

      case D3D9Format::R5G6B5: {
        pDstFormats[n++] = { VK_FORMAT_B5G6R5_UNORM_PACK16, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_R5G6B5_UNORM_PACK16, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      }
    }

    return n;
  }


  uint32_t D3D9SwapChainEx::PickPresentModes(
          BOOL                      Vsync,
          VkPresentModeKHR*         pDstModes) {
    uint32_t n = 0;

    if (Vsync) {
      pDstModes[n++] = VK_PRESENT_MODE_FIFO_KHR;
    } else {
      pDstModes[n++] = VK_PRESENT_MODE_IMMEDIATE_KHR;
      pDstModes[n++] = VK_PRESENT_MODE_MAILBOX_KHR;
      pDstModes[n++] = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    }

    return n;
  }


  uint32_t D3D9SwapChainEx::PickImageCount(
          UINT                      Preferred) {
    int32_t option = m_parent->GetOptions()->numBackBuffers;
    return option > 0 ? uint32_t(option) : uint32_t(Preferred);
  }

  HRESULT D3D9SwapChainEx::EnterFullscreenMode(
          D3DPRESENT_PARAMETERS* pPresentParams,
    const D3DDISPLAYMODEEX*      pFullscreenDisplayMode) {    
    // Find a display mode that matches what we need
    ::GetWindowRect(m_window, &m_windowState.rect);
      
    if (FAILED(ChangeDisplayMode(pPresentParams, pFullscreenDisplayMode))) {
      Logger::err("D3D9: EnterFullscreenMode: Failed to change display mode");
      return D3DERR_INVALIDCALL;
    }
    
    // Change the window flags to remove the decoration etc.
    LONG style   = ::GetWindowLongW(m_window, GWL_STYLE);
    LONG exstyle = ::GetWindowLongW(m_window, GWL_EXSTYLE);
    
    m_windowState.style = style;
    m_windowState.exstyle = exstyle;
    
    style   &= ~WS_OVERLAPPEDWINDOW;
    exstyle &= ~WS_EX_OVERLAPPEDWINDOW;
    
    ::SetWindowLongW(m_window, GWL_STYLE, style);
    ::SetWindowLongW(m_window, GWL_EXSTYLE, exstyle);
    
    // Move the window so that it covers the entire output    
    RECT rect;
    GetMonitorRect(GetDefaultMonitor(), &rect);
    
    ::SetWindowPos(m_window, HWND_TOPMOST,
      rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
      SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    
    m_monitor = GetDefaultMonitor();

    return D3D_OK;
  }
  
  
  HRESULT D3D9SwapChainEx::LeaveFullscreenMode() {
    if (!IsWindow(m_window))
      return D3DERR_INVALIDCALL;
    
    if (FAILED(RestoreDisplayMode(m_monitor)))
      Logger::warn("D3D9: LeaveFullscreenMode: Failed to restore display mode");
    
    m_monitor = nullptr;
    
    // Only restore the window style if the application hasn't
    // changed them. This is in line with what native D3D9 does.
    LONG curStyle   = ::GetWindowLongW(m_window, GWL_STYLE) & ~WS_VISIBLE;
    LONG curExstyle = ::GetWindowLongW(m_window, GWL_EXSTYLE) & ~WS_EX_TOPMOST;
    
    if (curStyle == (m_windowState.style & ~(WS_VISIBLE | WS_OVERLAPPEDWINDOW))
     && curExstyle == (m_windowState.exstyle & ~(WS_EX_TOPMOST | WS_EX_OVERLAPPEDWINDOW))) {
      ::SetWindowLongW(m_window, GWL_STYLE,   m_windowState.style);
      ::SetWindowLongW(m_window, GWL_EXSTYLE, m_windowState.exstyle);
    }
    
    // Restore window position and apply the style
    const RECT rect = m_windowState.rect;
    
    ::SetWindowPos(m_window, 0,
      rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
      SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
    
    return D3D_OK;
  }
  
  
  HRESULT D3D9SwapChainEx::ChangeDisplayMode(
          D3DPRESENT_PARAMETERS* pPresentParams,
    const D3DDISPLAYMODEEX*      pFullscreenDisplayMode) {
    D3DDISPLAYMODEEX mode;

    if (pFullscreenDisplayMode == nullptr) {
      mode.Width            = pPresentParams->BackBufferWidth;
      mode.Height           = pPresentParams->BackBufferHeight;
      mode.Format           = pPresentParams->BackBufferFormat;
      mode.RefreshRate      = pPresentParams->FullScreen_RefreshRateInHz;
      mode.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
      mode.Size             = sizeof(D3DDISPLAYMODEEX);
    }

    return SetMonitorDisplayMode(GetDefaultMonitor(), pFullscreenDisplayMode == nullptr ? &mode : pFullscreenDisplayMode);
  }
  
  
  HRESULT D3D9SwapChainEx::RestoreDisplayMode(HMONITOR hMonitor) {
    if (hMonitor == nullptr)
      return D3DERR_INVALIDCALL;
    
    DEVMODEW devMode = { };
    devMode.dmSize = sizeof(devMode);

    if (!::EnumDisplaySettingsW(m_monInfo.szDevice, ENUM_REGISTRY_SETTINGS, &devMode))
      return D3DERR_INVALIDCALL;
    
    Logger::info(str::format("D3D9: Setting display mode: ",
      devMode.dmPelsWidth, "x", devMode.dmPelsHeight, "@",
      devMode.dmDisplayFrequency));
    
    D3DDISPLAYMODEEX mode;
    mode.Width            = devMode.dmPelsWidth;
    mode.Height           = devMode.dmPelsHeight;
    mode.RefreshRate      = devMode.dmDisplayFrequency;
    mode.Format           = D3DFMT_X8R8G8B8; // Fix me
    mode.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
    mode.Size             = sizeof(D3DDISPLAYMODEEX);

    return SetMonitorDisplayMode(GetDefaultMonitor(), &mode);
  }

  bool    D3D9SwapChainEx::UpdatePresentRegion(const RECT* pSourceRect, const RECT* pDestRect) {
    if (pSourceRect == nullptr) {
      m_srcRect.top    = 0;
      m_srcRect.left   = 0;
      m_srcRect.right  = m_presentParams.BackBufferWidth;
      m_srcRect.bottom = m_presentParams.BackBufferHeight;
    }
    else
      m_srcRect = *pSourceRect;

    RECT dstRect;
    if (pDestRect == nullptr) {
      // TODO: Should we hook WM_SIZE message for this?
      UINT width, height;
      GetWindowClientSize(m_window, &width, &height);

      dstRect.top    = 0;
      dstRect.left   = 0;
      dstRect.right  = LONG(width);
      dstRect.bottom = LONG(height);
    }
    else
      dstRect = *pDestRect;

    bool recreate = 
       m_dstRect.left   != dstRect.left
    || m_dstRect.top    != dstRect.top
    || m_dstRect.right  != dstRect.right
    || m_dstRect.bottom != dstRect.bottom;

    m_dstRect = dstRect;

    return recreate;
  }

  VkExtent2D D3D9SwapChainEx::GetPresentExtent() {
    return VkExtent2D {
      std::max<uint32_t>(m_dstRect.right  - m_dstRect.left, 1u),
      std::max<uint32_t>(m_dstRect.bottom - m_dstRect.top,  1u) };
  }

  void    D3D9SwapChainEx::UpdateMonitorInfo() {
    m_monInfo.cbSize = sizeof(m_monInfo);

    if (!::GetMonitorInfoW(GetDefaultMonitor(), reinterpret_cast<MONITORINFO*>(&m_monInfo)))
      throw DxvkError("D3D9SwapChainEx::GetDisplayModeEx: Failed to query monitor info");
  }

}