#include "d3d9_device.h"

#include "d3d9_swapchain.h"
#include "d3d9_caps.h"
#include "d3d9_util.h"

#include "../util/util_bit.h"

#include <algorithm>

namespace dxvk {

  Direct3DDevice9Ex::Direct3DDevice9Ex(
    bool extended,
    IDirect3D9Ex* parent,
    UINT adapter,
    Rc<DxvkAdapter> dxvkAdapter,
    Rc<DxvkDevice> dxvkDevice,
    D3DDEVTYPE deviceType,
    HWND window,
    DWORD flags,
    D3DPRESENT_PARAMETERS* presentParams,
    D3DDISPLAYMODEEX* displayMode)
    : m_extended{ extended }
    , m_parent{ parent }
    , m_adapter{ adapter }
    , m_dxvkAdapter{ dxvkAdapter }
    , m_device{ dxvkDevice }
    , m_deviceType{ deviceType }
    , m_window{ window }
    , m_d3d9Formats{ m_dxvkAdapter } {
    HRESULT hr = this->Reset(presentParams);

    if (FAILED(hr))
      throw DxvkError("Direct3DDevice9Ex: device initial reset failed.");
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::QueryInterface(REFIID riid, void** ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(IDirect3DDevice9)
     || (m_extended && riid == __uuidof(IDirect3DDevice9Ex))) {
      *ppvObject = ref(this);
      return S_OK;
    }

    Logger::warn("Direct3DDevice9Ex::QueryInterface: Unknown interface query");
    Logger::warn(str::format(riid));
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::TestCooperativeLevel() {
    // Equivelant of D3D11/DXGI present tests. We can always present.
    return D3D_OK;
  }

  UINT    STDMETHODCALLTYPE Direct3DDevice9Ex::GetAvailableTextureMem() {
    auto memoryProp = m_dxvkAdapter->memoryProperties();

    VkDeviceSize availableTextureMemory = 0;

    for (uint32_t i = 0; i < memoryProp.memoryHeapCount; i++)
      availableTextureMemory += memoryProp.memoryHeaps[i].size;

    // The value returned is a 32-bit value, so we need to clamp it.
    VkDeviceSize maxMemory = UINT32_MAX;
    availableTextureMemory = std::min(availableTextureMemory, maxMemory);

    return UINT(availableTextureMemory);
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::EvictManagedResources() {
    Logger::warn("Direct3DDevice9Ex::EvictManagedResources: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetDirect3D(IDirect3D9** ppD3D9) {
    if (ppD3D9 == nullptr)
      return D3DERR_INVALIDCALL;

    *ppD3D9 = m_parent.ref();
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetDeviceCaps(D3DCAPS9* pCaps) {
    return caps::getDeviceCaps(m_adapter, m_deviceType, pCaps);
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode) {
    auto* swapchain = getInternalSwapchain(iSwapChain);

    if (swapchain == nullptr)
      return D3DERR_INVALIDCALL;

    return swapchain->GetDisplayMode(pMode);
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters) {
    if (pParameters == nullptr)
      return D3DERR_INVALIDCALL;

    pParameters->AdapterOrdinal = m_adapter;
    pParameters->BehaviorFlags = m_flags;
    pParameters->DeviceType = m_deviceType;
    pParameters->hFocusWindow = m_window;

    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetCursorProperties(
    UINT XHotSpot,
    UINT YHotSpot,
    IDirect3DSurface9* pCursorBitmap) {
    Logger::warn("Direct3DDevice9Ex::SetCursorProperties: Stub");
    return D3D_OK;
  }

  void    STDMETHODCALLTYPE Direct3DDevice9Ex::SetCursorPosition(int X, int Y, DWORD Flags) {
    m_cursor.updateCursor(X, Y, Flags & D3DCURSOR_IMMEDIATE_UPDATE);
  }

  BOOL    STDMETHODCALLTYPE Direct3DDevice9Ex::ShowCursor(BOOL bShow) {
    // Ok so if they call FALSE here it means they want to use the regular windows cursor.
    // if they call TRUE here it means they want to use some weird bitmap cursor that I currently dont care about.
    // Therefore we always want to show the regular cursor no matter what!
    ::ShowCursor(true);

    return TRUE;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DSwapChain9** ppSwapChain) {
    InitReturnPtr(ppSwapChain);

    if (ppSwapChain == nullptr || pPresentationParameters == nullptr)
      return D3DERR_INVALIDCALL;

    *ppSwapChain = new Direct3DSwapChain9Ex(this, pPresentationParameters);

    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain) {
    InitReturnPtr(pSwapChain);

    auto* swapchain = getInternalSwapchain(iSwapChain);

    if (swapchain == nullptr || pSwapChain == nullptr)
      return D3DERR_INVALIDCALL;

    *pSwapChain = static_cast<IDirect3DSwapChain9*>(ref(swapchain));

    return D3D_OK;
  }

  UINT    STDMETHODCALLTYPE Direct3DDevice9Ex::GetNumberOfSwapChains() {
    return UINT(m_swapchains.size());
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::Reset(D3DPRESENT_PARAMETERS* pPresentationParameters) {
    return ResetEx(pPresentationParameters, nullptr);
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::Present(
    const RECT* pSourceRect,
    const RECT* pDestRect, HWND hDestWindowOverride,
    const RGNDATA* pDirtyRegion) {
    Logger::warn("Direct3DDevice9Ex::Present: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetBackBuffer(
    UINT iSwapChain,
    UINT iBackBuffer,
    D3DBACKBUFFER_TYPE Type,
    IDirect3DSurface9** ppBackBuffer) {
    InitReturnPtr(ppBackBuffer);

    auto* swapchain = getInternalSwapchain(iSwapChain);

    if (swapchain == nullptr)
      return D3DERR_INVALIDCALL;

    return swapchain->GetBackBuffer(iBackBuffer, Type, ppBackBuffer);
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus) {
    auto* swapchain = getInternalSwapchain(iSwapChain);

    if (swapchain == nullptr)
      return D3DERR_INVALIDCALL;

    return swapchain->GetRasterStatus(pRasterStatus);
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetDialogBoxMode(BOOL bEnableDialogs) {
    Logger::warn("Direct3DDevice9Ex::SetDialogBoxMode: Stub");
    return D3D_OK;
  }

  void    STDMETHODCALLTYPE Direct3DDevice9Ex::SetGammaRamp(
    UINT iSwapChain,
    DWORD Flags,
    const D3DGAMMARAMP* pRamp) {
    auto* swapchain = getInternalSwapchain(iSwapChain);

    if (swapchain == nullptr)
      return;

    swapchain->SetGammaRamp(Flags, pRamp);
  }

  void    STDMETHODCALLTYPE Direct3DDevice9Ex::GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP* pRamp) {
    auto* swapchain = getInternalSwapchain(iSwapChain);

    if (swapchain == nullptr)
      return;

    swapchain->GetGammaRamp(pRamp);
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::CreateTexture(UINT Width,
    UINT Height,
    UINT Levels,
    DWORD Usage,
    D3DFORMAT Format,
    D3DPOOL Pool,
    IDirect3DTexture9** ppTexture,
    HANDLE* pSharedHandle) {
    Logger::warn("Direct3DDevice9Ex::CreateTexture: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::CreateVolumeTexture(
    UINT Width,
    UINT Height,
    UINT Depth,
    UINT Levels,
    DWORD Usage,
    D3DFORMAT Format,
    D3DPOOL Pool,
    IDirect3DVolumeTexture9** ppVolumeTexture,
    HANDLE* pSharedHandle) {
    Logger::warn("Direct3DDevice9Ex::CreateVolumeTexture: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::CreateCubeTexture(
    UINT EdgeLength,
    UINT Levels,
    DWORD Usage,
    D3DFORMAT Format,
    D3DPOOL Pool,
    IDirect3DCubeTexture9** ppCubeTexture,
    HANDLE* pSharedHandle) {
    Logger::warn("Direct3DDevice9Ex::CreateCubeTexture: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::CreateVertexBuffer(
    UINT Length,
    DWORD Usage,
    DWORD FVF,
    D3DPOOL Pool,
    IDirect3DVertexBuffer9** ppVertexBuffer,
    HANDLE* pSharedHandle) {
    Logger::warn("Direct3DDevice9Ex::CreateVertexBuffer: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::CreateIndexBuffer(
    UINT Length,
    DWORD Usage,
    D3DFORMAT Format,
    D3DPOOL Pool,
    IDirect3DIndexBuffer9** ppIndexBuffer,
    HANDLE* pSharedHandle) {
    Logger::warn("Direct3DDevice9Ex::CreateIndexBuffer: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::CreateRenderTarget(
    UINT Width,
    UINT Height,
    D3DFORMAT Format,
    D3DMULTISAMPLE_TYPE MultiSample,
    DWORD MultisampleQuality,
    BOOL Lockable,
    IDirect3DSurface9** ppSurface,
    HANDLE* pSharedHandle) {
    Logger::warn("Direct3DDevice9Ex::CreateRenderTarget: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::CreateDepthStencilSurface(
    UINT Width,
    UINT Height,
    D3DFORMAT Format,
    D3DMULTISAMPLE_TYPE MultiSample,
    DWORD MultisampleQuality,
    BOOL Discard,
    IDirect3DSurface9** ppSurface,
    HANDLE* pSharedHandle) {
    Logger::warn("Direct3DDevice9Ex::CreateDepthStencilSurface: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::UpdateSurface(
    IDirect3DSurface9* pSourceSurface,
    const RECT* pSourceRect,
    IDirect3DSurface9* pDestinationSurface,
    const POINT* pDestPoint) {
    Logger::warn("Direct3DDevice9Ex::UpdateSurface: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::UpdateTexture(IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture) {
    Logger::warn("Direct3DDevice9Ex::UpdateTexture: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetRenderTargetData(IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface) {
    Logger::warn("Direct3DDevice9Ex::GetRenderTargetData: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9* pDestSurface) {
    auto* swapchain = getInternalSwapchain(iSwapChain);

    if (swapchain == nullptr)
      return D3DERR_INVALIDCALL;

    return swapchain->GetFrontBufferData(pDestSurface);
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::StretchRect(
    IDirect3DSurface9* pSourceSurface,
    const RECT* pSourceRect,
    IDirect3DSurface9* pDestSurface,
    const RECT* pDestRect,
    D3DTEXTUREFILTERTYPE Filter) {
    Logger::warn("Direct3DDevice9Ex::StretchRect: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::ColorFill(
    IDirect3DSurface9* pSurface,
    const RECT* pRect,
    D3DCOLOR color) {
    Logger::warn("Direct3DDevice9Ex::ColorFill: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::CreateOffscreenPlainSurface(
    UINT Width,
    UINT Height,
    D3DFORMAT Format,
    D3DPOOL Pool,
    IDirect3DSurface9** ppSurface,
    HANDLE* pSharedHandle) {
    Logger::warn("Direct3DDevice9Ex::CreateOffscreenPlainSurface: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) {
    Logger::warn("Direct3DDevice9Ex::SetRenderTarget: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget) {
    Logger::warn("Direct3DDevice9Ex::GetRenderTarget: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil) {
    Logger::warn("Direct3DDevice9Ex::SetDepthStencilSurface: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface) {
    Logger::warn("Direct3DDevice9Ex::GetDepthStencilSurface: Stub");
    return D3D_OK;
  }

  // The Begin/EndScene functions actually do nothing.
  // Some games don't even call them.

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::BeginScene() {
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::EndScene() {
    // We may want to flush cmds here.

    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::Clear(
    DWORD Count,
    const D3DRECT* pRects,
    DWORD Flags,
    D3DCOLOR Color,
    float Z,
    DWORD Stencil) {
    Logger::warn("Direct3DDevice9Ex::Clear: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) {
    Logger::warn("Direct3DDevice9Ex::SetTransform: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) {
    Logger::warn("Direct3DDevice9Ex::GetTransform: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::MultiplyTransform(D3DTRANSFORMSTATETYPE TransformState, const D3DMATRIX* pMatrix) {
    Logger::warn("Direct3DDevice9Ex::MultiplyTransform: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetViewport(const D3DVIEWPORT9* pViewport) {
    Logger::warn("Direct3DDevice9Ex::SetViewport: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetViewport(D3DVIEWPORT9* pViewport) {
    Logger::warn("Direct3DDevice9Ex::GetViewport: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetMaterial(const D3DMATERIAL9* pMaterial) {
    Logger::warn("Direct3DDevice9Ex::SetMaterial: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetMaterial(D3DMATERIAL9* pMaterial) {
    Logger::warn("Direct3DDevice9Ex::GetMaterial: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetLight(DWORD Index, const D3DLIGHT9* pLight) {
    Logger::warn("Direct3DDevice9Ex::SetLight: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetLight(DWORD Index, D3DLIGHT9* pLight) {
    Logger::warn("Direct3DDevice9Ex::GetLight: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::LightEnable(DWORD Index, BOOL Enable) {
    Logger::warn("Direct3DDevice9Ex::LightEnable: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetLightEnable(DWORD Index, BOOL* pEnable) {
    Logger::warn("Direct3DDevice9Ex::GetLightEnable: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetClipPlane(DWORD Index, const float* pPlane) {
    Logger::warn("Direct3DDevice9Ex::SetClipPlane: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetClipPlane(DWORD Index, float* pPlane) {
    Logger::warn("Direct3DDevice9Ex::GetClipPlane: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) {
    Logger::warn("Direct3DDevice9Ex::SetRenderState: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue) {
    Logger::warn("Direct3DDevice9Ex::GetRenderState: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB) {
    Logger::warn("Direct3DDevice9Ex::CreateStateBlock: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::BeginStateBlock() {
    Logger::warn("Direct3DDevice9Ex::BeginStateBlock: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::EndStateBlock(IDirect3DStateBlock9** ppSB) {
    Logger::warn("Direct3DDevice9Ex::EndStateBlock: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetClipStatus(const D3DCLIPSTATUS9* pClipStatus) {
    Logger::warn("Direct3DDevice9Ex::SetClipStatus: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetClipStatus(D3DCLIPSTATUS9* pClipStatus) {
    Logger::warn("Direct3DDevice9Ex::GetClipStatus: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture) {
    Logger::warn("Direct3DDevice9Ex::GetTexture: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture) {
    Logger::warn("Direct3DDevice9Ex::SetTexture: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetTextureStageState(
    DWORD Stage,
    D3DTEXTURESTAGESTATETYPE Type,
    DWORD* pValue) {
    Logger::warn("Direct3DDevice9Ex::GetTextureStageState: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetTextureStageState(
    DWORD Stage,
    D3DTEXTURESTAGESTATETYPE Type,
    DWORD Value) {
    Logger::warn("Direct3DDevice9Ex::SetTextureStageState: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetSamplerState(
    DWORD Sampler,
    D3DSAMPLERSTATETYPE Type,
    DWORD* pValue) {
    Logger::warn("Direct3DDevice9Ex::GetSamplerState: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetSamplerState(
    DWORD Sampler,
    D3DSAMPLERSTATETYPE Type,
    DWORD Value) {
    Logger::warn("Direct3DDevice9Ex::SetSamplerState: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::ValidateDevice(DWORD* pNumPasses) {
    if (pNumPasses != nullptr)
      *pNumPasses = 1;

    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetPaletteEntries(UINT PaletteNumber, const PALETTEENTRY* pEntries) {
    Logger::warn("Direct3DDevice9Ex::SetPaletteEntries: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries) {
    Logger::warn("Direct3DDevice9Ex::GetPaletteEntries: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetCurrentTexturePalette(UINT PaletteNumber) {
    Logger::warn("Direct3DDevice9Ex::SetCurrentTexturePalette: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetCurrentTexturePalette(UINT *PaletteNumber) {
    Logger::warn("Direct3DDevice9Ex::GetCurrentTexturePalette: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetScissorRect(const RECT* pRect) {
    Logger::warn("Direct3DDevice9Ex::SetScissorRect: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetScissorRect(RECT* pRect) {
    Logger::warn("Direct3DDevice9Ex::GetScissorRect: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetSoftwareVertexProcessing(BOOL bSoftware) {
    Logger::warn("Direct3DDevice9Ex::SetSoftwareVertexProcessing: Stub");
    return D3D_OK;
  }

  BOOL    STDMETHODCALLTYPE Direct3DDevice9Ex::GetSoftwareVertexProcessing() {
    Logger::warn("Direct3DDevice9Ex::GetSoftwareVertexProcessing: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetNPatchMode(float nSegments) {
    Logger::warn("Direct3DDevice9Ex::SetNPatchMode: Stub");
    return D3D_OK;
  }

  float   STDMETHODCALLTYPE Direct3DDevice9Ex::GetNPatchMode() {
    Logger::warn("Direct3DDevice9Ex::GetNPatchMode: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::DrawPrimitive(
    D3DPRIMITIVETYPE PrimitiveType,
    UINT StartVertex,
    UINT PrimitiveCount) {
    Logger::warn("Direct3DDevice9Ex::DrawPrimitive: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::DrawIndexedPrimitive(
    D3DPRIMITIVETYPE PrimitiveType,
    INT BaseVertexIndex,
    UINT MinVertexIndex,
    UINT NumVertices,
    UINT startIndex,
    UINT primCount) {
    Logger::warn("Direct3DDevice9Ex::DrawIndexedPrimitive: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::DrawPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType,
    UINT PrimitiveCount,
    const void* pVertexStreamZeroData,
    UINT VertexStreamZeroStride) {
    Logger::warn("Direct3DDevice9Ex::DrawPrimitiveUP: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::DrawIndexedPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType,
    UINT MinVertexIndex,
    UINT NumVertices,
    UINT PrimitiveCount,
    const void* pIndexData,
    D3DFORMAT IndexDataFormat,
    const void* pVertexStreamZeroData,
    UINT VertexStreamZeroStride) {
    Logger::warn("Direct3DDevice9Ex::DrawIndexedPrimitiveUP: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::ProcessVertices(
    UINT SrcStartIndex,
    UINT DestIndex,
    UINT VertexCount,
    IDirect3DVertexBuffer9* pDestBuffer,
    IDirect3DVertexDeclaration9* pVertexDecl,
    DWORD Flags) {
    Logger::warn("Direct3DDevice9Ex::ProcessVertices: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::CreateVertexDeclaration(const D3DVERTEXELEMENT9* pVertexElements, IDirect3DVertexDeclaration9** ppDecl) {
    Logger::warn("Direct3DDevice9Ex::CreateVertexDeclaration: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl) {
    Logger::warn("Direct3DDevice9Ex::SetVertexDeclaration: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl) {
    Logger::warn("Direct3DDevice9Ex::GetVertexDeclaration: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetFVF(DWORD FVF) {
    Logger::warn("Direct3DDevice9Ex::SetFVF: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetFVF(DWORD* pFVF) {
    Logger::warn("Direct3DDevice9Ex::GetFVF: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::CreateVertexShader(const DWORD* pFunction, IDirect3DVertexShader9** ppShader) {
    Logger::warn("Direct3DDevice9Ex::CreateVertexShader: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetVertexShader(IDirect3DVertexShader9* pShader) {
    Logger::warn("Direct3DDevice9Ex::SetVertexShader: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetVertexShader(IDirect3DVertexShader9** ppShader) {
    Logger::warn("Direct3DDevice9Ex::GetVertexShader: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetVertexShaderConstantF(
    UINT StartRegister,
    const float* pConstantData,
    UINT Vector4fCount) {
    Logger::warn("Direct3DDevice9Ex::SetVertexShaderConstantF: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetVertexShaderConstantF(
    UINT StartRegister,
    float* pConstantData,
    UINT Vector4fCount) {
    Logger::warn("Direct3DDevice9Ex::GetVertexShaderConstantF: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetVertexShaderConstantI(
    UINT StartRegister,
    const int* pConstantData,
    UINT Vector4iCount) {
    Logger::warn("Direct3DDevice9Ex::SetVertexShaderConstantI: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetVertexShaderConstantI(
    UINT StartRegister,
    int* pConstantData,
    UINT Vector4iCount) {
    Logger::warn("Direct3DDevice9Ex::GetVertexShaderConstantI: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetVertexShaderConstantB(
    UINT StartRegister,
    const BOOL* pConstantData,
    UINT BoolCount) {
    Logger::warn("Direct3DDevice9Ex::SetVertexShaderConstantB: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetVertexShaderConstantB(
    UINT StartRegister,
    BOOL* pConstantData,
    UINT BoolCount) {
    Logger::warn("Direct3DDevice9Ex::GetVertexShaderConstantB: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetStreamSource(
    UINT StreamNumber,
    IDirect3DVertexBuffer9* pStreamData,
    UINT OffsetInBytes,
    UINT Stride) {
    Logger::warn("Direct3DDevice9Ex::SetStreamSource: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetStreamSource(
    UINT StreamNumber,
    IDirect3DVertexBuffer9** ppStreamData,
    UINT* pOffsetInBytes,
    UINT* pStride) {
    Logger::warn("Direct3DDevice9Ex::GetStreamSource: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetStreamSourceFreq(UINT StreamNumber, UINT Setting) {
    Logger::warn("Direct3DDevice9Ex::SetStreamSourceFreq: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetStreamSourceFreq(UINT StreamNumber, UINT* pSetting) {
    Logger::warn("Direct3DDevice9Ex::GetStreamSourceFreq: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetIndices(IDirect3DIndexBuffer9* pIndexData) {
    Logger::warn("Direct3DDevice9Ex::SetIndices: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetIndices(IDirect3DIndexBuffer9** ppIndexData) {
    Logger::warn("Direct3DDevice9Ex::GetIndices: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::CreatePixelShader(const DWORD* pFunction, IDirect3DPixelShader9** ppShader) {
    Logger::warn("Direct3DDevice9Ex::CreatePixelShader: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetPixelShader(IDirect3DPixelShader9* pShader) {
    Logger::warn("Direct3DDevice9Ex::SetPixelShader: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetPixelShader(IDirect3DPixelShader9** ppShader) {
    Logger::warn("Direct3DDevice9Ex::GetPixelShader: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetPixelShaderConstantF(
    UINT StartRegister,
    const float* pConstantData,
    UINT Vector4fCount) {
    Logger::warn("Direct3DDevice9Ex::SetPixelShaderConstantF: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetPixelShaderConstantF(
    UINT StartRegister,
    float* pConstantData,
    UINT Vector4fCount) {
    Logger::warn("Direct3DDevice9Ex::GetPixelShaderConstantF: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetPixelShaderConstantI(
    UINT StartRegister,
    const int* pConstantData,
    UINT Vector4iCount) {
    Logger::warn("Direct3DDevice9Ex::SetPixelShaderConstantI: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetPixelShaderConstantI(
    UINT StartRegister,
    int* pConstantData,
    UINT Vector4iCount) {
    Logger::warn("Direct3DDevice9Ex::GetPixelShaderConstantI: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetPixelShaderConstantB(
    UINT StartRegister,
    const BOOL* pConstantData,
    UINT BoolCount) {
    Logger::warn("Direct3DDevice9Ex::SetPixelShaderConstantB: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetPixelShaderConstantB(
    UINT StartRegister,
    BOOL* pConstantData,
    UINT BoolCount) {
    Logger::warn("Direct3DDevice9Ex::GetPixelShaderConstantB: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::DrawRectPatch(
    UINT Handle,
    const float* pNumSegs,
    const D3DRECTPATCH_INFO* pRectPatchInfo) {
    Logger::warn("Direct3DDevice9Ex::DrawRectPatch: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::DrawTriPatch(
    UINT Handle,
    const float* pNumSegs,
    const D3DTRIPATCH_INFO* pTriPatchInfo) {
    Logger::warn("Direct3DDevice9Ex::DrawTriPatch: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::DeletePatch(UINT Handle) {
    Logger::warn("Direct3DDevice9Ex::DeletePatch: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery) {
    Logger::warn("Direct3DDevice9Ex::CreateQuery: Stub");
    return D3D_OK;
  }

  // Ex Methods

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetConvolutionMonoKernel(
    UINT width,
    UINT height,
    float* rows,
    float* columns) {
    Logger::warn("Direct3DDevice9Ex::SetConvolutionMonoKernel: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::ComposeRects(
    IDirect3DSurface9* pSrc,
    IDirect3DSurface9* pDst,
    IDirect3DVertexBuffer9* pSrcRectDescs,
    UINT NumRects,
    IDirect3DVertexBuffer9* pDstRectDescs,
    D3DCOMPOSERECTSOP Operation,
    int Xoffset,
    int Yoffset) {
    Logger::warn("Direct3DDevice9Ex::ComposeRects: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetGPUThreadPriority(INT* pPriority) {
    Logger::warn("Direct3DDevice9Ex::GetGPUThreadPriority: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetGPUThreadPriority(INT Priority) {
    Logger::warn("Direct3DDevice9Ex::SetGPUThreadPriority: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::WaitForVBlank(UINT iSwapChain) {
    auto* swapchain = getInternalSwapchain(iSwapChain);

    if (swapchain == nullptr)
      return D3DERR_INVALIDCALL;

    return swapchain->WaitForVBlank();
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::CheckResourceResidency(IDirect3DResource9** pResourceArray, UINT32 NumResources) {
    Logger::warn("Direct3DDevice9Ex::CheckResourceResidency: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::SetMaximumFrameLatency(UINT MaxLatency) {
    Logger::warn("Direct3DDevice9Ex::SetMaximumFrameLatency: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetMaximumFrameLatency(UINT* pMaxLatency) {
    Logger::warn("Direct3DDevice9Ex::GetMaximumFrameLatency: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::CheckDeviceState(HWND hDestinationWindow) {
    Logger::warn("Direct3DDevice9Ex::CheckDeviceState: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::PresentEx(
    const RECT* pSourceRect,
    const RECT* pDestRect,
    HWND hDestWindowOverride,
    const RGNDATA* pDirtyRegion,
    DWORD dwFlags) {
    Logger::warn("Direct3DDevice9Ex::PresentEx: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::CreateRenderTargetEx(
    UINT Width,
    UINT Height,
    D3DFORMAT Format,
    D3DMULTISAMPLE_TYPE MultiSample,
    DWORD MultisampleQuality,
    BOOL Lockable,
    IDirect3DSurface9** ppSurface,
    HANDLE* pSharedHandle,
    DWORD Usage) {
    Logger::warn("Direct3DDevice9Ex::CreateRenderTargetEx: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::CreateOffscreenPlainSurfaceEx(
    UINT Width,
    UINT Height,
    D3DFORMAT Format,
    D3DPOOL Pool,
    IDirect3DSurface9** ppSurface,
    HANDLE* pSharedHandle,
    DWORD Usage) {
    Logger::warn("Direct3DDevice9Ex::CreateOffscreenPlainSurfaceEx: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::CreateDepthStencilSurfaceEx(
    UINT Width,
    UINT Height,
    D3DFORMAT Format,
    D3DMULTISAMPLE_TYPE MultiSample,
    DWORD MultisampleQuality,
    BOOL Discard,
    IDirect3DSurface9** ppSurface,
    HANDLE* pSharedHandle,
    DWORD Usage) {
    Logger::warn("Direct3DDevice9Ex::CreateDepthStencilSurfaceEx: Stub");
    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::ResetEx(D3DPRESENT_PARAMETERS* pPresentationParameters, D3DDISPLAYMODEEX *pFullscreenDisplayMode) {
    if (pPresentationParameters == nullptr)
      return D3DERR_INVALIDCALL;

    SetDepthStencilSurface(nullptr);

    for (uint32_t i = 0; i < caps::MaxSimultaneousRenderTargets; i++)
      SetRenderTarget(0, nullptr);

    SetRenderState(D3DRS_ZENABLE, pPresentationParameters->EnableAutoDepthStencil != FALSE ? D3DZB_TRUE : D3DZB_FALSE);
    SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
    SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    SetRenderState(D3DRS_LASTPIXEL, TRUE);
    SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
    SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    SetRenderState(D3DRS_ALPHAREF, 0);
    SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_ALWAYS);
    SetRenderState(D3DRS_DITHERENABLE, FALSE);
    SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    SetRenderState(D3DRS_FOGENABLE, FALSE);
    SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    //	SetRenderState(D3DRS_ZVISIBLE, 0);
    SetRenderState(D3DRS_FOGCOLOR, 0);
    SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
    SetRenderState(D3DRS_FOGSTART, bit::cast<DWORD>(0.0f));
    SetRenderState(D3DRS_FOGEND, bit::cast<DWORD>(1.0f));
    SetRenderState(D3DRS_FOGDENSITY, bit::cast<DWORD>(1.0f));
    SetRenderState(D3DRS_RANGEFOGENABLE, FALSE);
    SetRenderState(D3DRS_STENCILENABLE, FALSE);
    SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
    SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
    SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
    SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
    SetRenderState(D3DRS_STENCILREF, 0);
    SetRenderState(D3DRS_STENCILMASK, 0xFFFFFFFF);
    SetRenderState(D3DRS_STENCILWRITEMASK, 0xFFFFFFFF);
    SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFFFFFF);
    SetRenderState(D3DRS_WRAP0, 0);
    SetRenderState(D3DRS_WRAP1, 0);
    SetRenderState(D3DRS_WRAP2, 0);
    SetRenderState(D3DRS_WRAP3, 0);
    SetRenderState(D3DRS_WRAP4, 0);
    SetRenderState(D3DRS_WRAP5, 0);
    SetRenderState(D3DRS_WRAP6, 0);
    SetRenderState(D3DRS_WRAP7, 0);
    SetRenderState(D3DRS_CLIPPING, TRUE);
    SetRenderState(D3DRS_LIGHTING, TRUE);
    SetRenderState(D3DRS_AMBIENT, 0);
    SetRenderState(D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
    SetRenderState(D3DRS_COLORVERTEX, TRUE);
    SetRenderState(D3DRS_LOCALVIEWER, TRUE);
    SetRenderState(D3DRS_NORMALIZENORMALS, FALSE);
    SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
    SetRenderState(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_COLOR2);
    SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
    SetRenderState(D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
    SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
    SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    SetRenderState(D3DRS_POINTSIZE, bit::cast<DWORD>(1.0f));
    SetRenderState(D3DRS_POINTSIZE_MIN, bit::cast<DWORD>(1.0f));
    SetRenderState(D3DRS_POINTSPRITEENABLE, FALSE);
    SetRenderState(D3DRS_POINTSCALEENABLE, FALSE);
    SetRenderState(D3DRS_POINTSCALE_A, bit::cast<DWORD>(1.0f));
    SetRenderState(D3DRS_POINTSCALE_B, bit::cast<DWORD>(0.0f));
    SetRenderState(D3DRS_POINTSCALE_C, bit::cast<DWORD>(0.0f));
    SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE);
    SetRenderState(D3DRS_MULTISAMPLEMASK, 0xFFFFFFFF);
    SetRenderState(D3DRS_PATCHEDGESTYLE, D3DPATCHEDGE_DISCRETE);
    SetRenderState(D3DRS_DEBUGMONITORTOKEN, D3DDMT_ENABLE);
    SetRenderState(D3DRS_POINTSIZE_MAX, bit::cast<DWORD>(64.0f));
    SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
    SetRenderState(D3DRS_COLORWRITEENABLE, 0x0000000F);
    SetRenderState(D3DRS_TWEENFACTOR, bit::cast<DWORD>(0.0f));
    SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    SetRenderState(D3DRS_POSITIONDEGREE, D3DDEGREE_CUBIC);
    SetRenderState(D3DRS_NORMALDEGREE, D3DDEGREE_LINEAR);
    SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, bit::cast<DWORD>(0.0f));
    SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, FALSE);
    SetRenderState(D3DRS_MINTESSELLATIONLEVEL, bit::cast<DWORD>(1.0f));
    SetRenderState(D3DRS_MAXTESSELLATIONLEVEL, bit::cast<DWORD>(1.0f));
    SetRenderState(D3DRS_ADAPTIVETESS_X, bit::cast<DWORD>(0.0f));
    SetRenderState(D3DRS_ADAPTIVETESS_Y, bit::cast<DWORD>(0.0f));
    SetRenderState(D3DRS_ADAPTIVETESS_Z, bit::cast<DWORD>(1.0f));
    SetRenderState(D3DRS_ADAPTIVETESS_W, bit::cast<DWORD>(0.0f));
    SetRenderState(D3DRS_ENABLEADAPTIVETESSELLATION, FALSE);
    SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
    SetRenderState(D3DRS_CCW_STENCILFAIL, D3DSTENCILOP_KEEP);
    SetRenderState(D3DRS_CCW_STENCILZFAIL, D3DSTENCILOP_KEEP);
    SetRenderState(D3DRS_CCW_STENCILPASS, D3DSTENCILOP_KEEP);
    SetRenderState(D3DRS_CCW_STENCILFUNC, D3DCMP_ALWAYS);
    SetRenderState(D3DRS_COLORWRITEENABLE1, 0x0000000F);
    SetRenderState(D3DRS_COLORWRITEENABLE2, 0x0000000F);
    SetRenderState(D3DRS_COLORWRITEENABLE3, 0x0000000F);
    SetRenderState(D3DRS_BLENDFACTOR, 0xFFFFFFFF);
    SetRenderState(D3DRS_SRGBWRITEENABLE, 0);
    SetRenderState(D3DRS_DEPTHBIAS, bit::cast<DWORD>(0.0f));
    SetRenderState(D3DRS_WRAP8, 0);
    SetRenderState(D3DRS_WRAP9, 0);
    SetRenderState(D3DRS_WRAP10, 0);
    SetRenderState(D3DRS_WRAP11, 0);
    SetRenderState(D3DRS_WRAP12, 0);
    SetRenderState(D3DRS_WRAP13, 0);
    SetRenderState(D3DRS_WRAP14, 0);
    SetRenderState(D3DRS_WRAP15, 0);
    SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_ZERO);
    SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);

    for (uint32_t i = 0; i < caps::MaxTextureBlendStages; i++)
    {
      SetTextureStageState(i, D3DTSS_COLOROP, i == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE);
      SetTextureStageState(i, D3DTSS_COLORARG1, D3DTA_TEXTURE);
      SetTextureStageState(i, D3DTSS_COLORARG2, D3DTA_CURRENT);
      SetTextureStageState(i, D3DTSS_ALPHAOP, i == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE);
      SetTextureStageState(i, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
      SetTextureStageState(i, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
      SetTextureStageState(i, D3DTSS_BUMPENVMAT00, bit::cast<DWORD>(0.0f));
      SetTextureStageState(i, D3DTSS_BUMPENVMAT01, bit::cast<DWORD>(0.0f));
      SetTextureStageState(i, D3DTSS_BUMPENVMAT10, bit::cast<DWORD>(0.0f));
      SetTextureStageState(i, D3DTSS_BUMPENVMAT11, bit::cast<DWORD>(0.0f));
      SetTextureStageState(i, D3DTSS_TEXCOORDINDEX, i);
      SetTextureStageState(i, D3DTSS_BUMPENVLSCALE, bit::cast<DWORD>(0.0f));
      SetTextureStageState(i, D3DTSS_BUMPENVLOFFSET, bit::cast<DWORD>(0.0f));
      SetTextureStageState(i, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
      SetTextureStageState(i, D3DTSS_COLORARG0, D3DTA_CURRENT);
      SetTextureStageState(i, D3DTSS_ALPHAARG0, D3DTA_CURRENT);
      SetTextureStageState(i, D3DTSS_RESULTARG, D3DTA_CURRENT);
      SetTextureStageState(i, D3DTSS_CONSTANT, 0x00000000);
    }

    forEachSampler([&](uint32_t i)
    {
      SetTexture(i, 0);
      SetSamplerState(i, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
      SetSamplerState(i, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
      SetSamplerState(i, D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP);
      SetSamplerState(i, D3DSAMP_BORDERCOLOR, 0x00000000);
      SetSamplerState(i, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
      SetSamplerState(i, D3DSAMP_MINFILTER, D3DTEXF_POINT);
      SetSamplerState(i, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
      SetSamplerState(i, D3DSAMP_MIPMAPLODBIAS, 0);
      SetSamplerState(i, D3DSAMP_MAXMIPLEVEL, 0);
      SetSamplerState(i, D3DSAMP_MAXANISOTROPY, 1);
      SetSamplerState(i, D3DSAMP_SRGBTEXTURE, 0);
      SetSamplerState(i, D3DSAMP_ELEMENTINDEX, 0);
      SetSamplerState(i, D3DSAMP_DMAPOFFSET, 0);
    });

    for (uint32_t i = 0; i < caps::MaxClipPlanes; i++) {
      float plane[4] = { 0, 0, 0, 0 };
      SetClipPlane(i, plane);
    }

    // TODO: Flush and Sync here.

    HRESULT hr;
    auto* implicitSwapchain = getInternalSwapchain(0);
    if (implicitSwapchain == nullptr) {
      Com<IDirect3DSwapChain9> swapchain;
      hr = CreateAdditionalSwapChain(pPresentationParameters, &swapchain);
      if (FAILED(hr))
        throw DxvkError("Reset: failed to create implicit swapchain");
    }
    else {
      hr = implicitSwapchain->Reset(pPresentationParameters);
      if (FAILED(hr))
        throw DxvkError("Reset: failed to reset swapchain");
    }

    Com<IDirect3DSurface9> backbuffer;
    hr = m_swapchains[0]->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
    if (FAILED(hr))
      throw DxvkError("Reset: failed to get implicit swapchain backbuffers");

    SetRenderTarget(0, backbuffer.ptr());

    if (pPresentationParameters->EnableAutoDepthStencil) {
      Com<IDirect3DSurface9> autoDepthStencil;
      CreateDepthStencilSurface(
        pPresentationParameters->BackBufferWidth,
        pPresentationParameters->BackBufferHeight,
        pPresentationParameters->AutoDepthStencilFormat,
        pPresentationParameters->MultiSampleType,
        pPresentationParameters->MultiSampleQuality,
        FALSE,
        &autoDepthStencil,
        nullptr);

      SetDepthStencilSurface(autoDepthStencil.ptr());
    }

    ShowCursor(FALSE);

    return D3D_OK;
  }

  HRESULT STDMETHODCALLTYPE Direct3DDevice9Ex::GetDisplayModeEx(
    UINT iSwapChain,
    D3DDISPLAYMODEEX* pMode,
    D3DDISPLAYROTATION* pRotation) {
    auto* swapchain = getInternalSwapchain(iSwapChain);

    if (swapchain == nullptr)
      return D3DERR_INVALIDCALL;

    return swapchain->GetDisplayModeEx(pMode, pRotation);
  }

  bool Direct3DDevice9Ex::IsExtended() {
    return m_extended;
  }

  HWND Direct3DDevice9Ex::GetWindow() {
    return m_window;
  }

  DxvkDeviceFeatures Direct3DDevice9Ex::GetDeviceFeatures(const Rc<DxvkAdapter>& adapter) {
    DxvkDeviceFeatures supported = adapter->features();
    DxvkDeviceFeatures enabled = {};

    // Geometry shaders are used for some meta ops
    enabled.core.features.geometryShader = VK_TRUE;
    enabled.core.features.robustBufferAccess = VK_TRUE;

    enabled.extMemoryPriority.memoryPriority = supported.extMemoryPriority.memoryPriority;

    enabled.extVertexAttributeDivisor.vertexAttributeInstanceRateDivisor = supported.extVertexAttributeDivisor.vertexAttributeInstanceRateDivisor;
    enabled.extVertexAttributeDivisor.vertexAttributeInstanceRateZeroDivisor = supported.extVertexAttributeDivisor.vertexAttributeInstanceRateZeroDivisor;

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

    // SM2 level hardware
    enabled.core.features.occlusionQueryPrecise = VK_TRUE;

    // SM3 level hardware
    enabled.core.features.multiViewport = VK_TRUE;
    enabled.core.features.independentBlend = VK_TRUE;

    // D3D10 level hardware supports this in D3D9 native.
    enabled.core.features.fullDrawIndexUint32 = VK_TRUE;

    return enabled;
  }

  Direct3DSwapChain9Ex* Direct3DDevice9Ex::getInternalSwapchain(UINT index) {
    if (index >= m_swapchains.size())
      return nullptr;

    return static_cast<Direct3DSwapChain9Ex*>(m_swapchains[index]);
  }

  D3D9_VK_FORMAT_INFO Direct3DDevice9Ex::LookupFormat(
    D3D9Format            Format,
    bool                  srgb) const {
    return m_d3d9Formats.GetFormatInfo(Format, srgb);
  }

}
