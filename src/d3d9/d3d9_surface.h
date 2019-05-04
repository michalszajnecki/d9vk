#pragma once

#include "d3d9_subresource.h"

#include "d3d9_common_texture.h"

namespace dxvk {

  using D3D9SurfaceBase = D3D9Subresource<IDirect3DSurface9>;
  class D3D9Surface final : public D3D9SurfaceBase {

  public:

    D3D9Surface(
            D3D9DeviceEx*             pDevice,
      const D3D9TextureDesc*          pDesc);

    D3D9Surface(
            D3D9DeviceEx*             pDevice,
            D3D9CommonTexture*        pTexture,
            UINT                      Face,
            UINT                      MipLevel,
            IUnknown*                 pContainer);

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject);

    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() final;

    HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC *pDesc) final;

    HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect, DWORD Flags) final;

    HRESULT STDMETHODCALLTYPE UnlockRect() final;

    HRESULT STDMETHODCALLTYPE GetDC(HDC *phdc) final;

    HRESULT STDMETHODCALLTYPE ReleaseDC(HDC hdc) final;

  };
}