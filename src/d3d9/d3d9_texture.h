#pragma once

#include "d3d9_surface.h"

#include <vector>

namespace dxvk {

  template <typename SubresourceType, typename... Type>
  class Direct3DBaseTexture9 : public Direct3DResource9<Type...> {

  public:

    Direct3DBaseTexture9(
            Direct3DDevice9Ex*      pDevice,
      const D3D9TextureDesc*        pDesc)
      : m_texture{ new Direct3DCommonTexture9{ pDevice, pDesc } }
      , m_lod{ 0 }
      , m_autogenFilter{ D3DTEXF_LINEAR } {
      auto& desc = *m_texture->Desc();

      const uint32_t arraySlices = desc.Type == D3DRTYPE_CUBETEXTURE ? 6 : 1;
      const uint32_t mipLevels = desc.MipLevels;

      m_subresources.resize(arraySlices * mipLevels);

      for (uint32_t i = 0; i < mipLevels; i++) {
        for (uint32_t j = 0; j < arraySlices; j++) {
          uint32_t subresource = CalcSubresource(i, j);

          SubresourceType* subObj = new SubresourceType{
            pDevice,
            m_texture,
            subresource,
            this};
          subObj->AddPrivateRef();

          m_subresources[subresource] = subObj;
        }
      }
    }

    DWORD STDMETHODCALLTYPE SetLOD(DWORD LODNew) final {
      DWORD oldLod = m_lod;
      m_lod = LODNew;
      return oldLod;
    }

    DWORD STDMETHODCALLTYPE GetLOD() final {
      return m_lod;
    }

    DWORD STDMETHODCALLTYPE GetLevelCount() final {
      return m_texture->Desc()->MipLevels;
    }

    HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType) final {
      m_autogenFilter = FilterType;
      return D3D_OK;
    }

    D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() final {
      return m_autogenFilter;
    }

    void STDMETHODCALLTYPE GenerateMipSubLevels() final {
      Logger::warn("Direct3DBaseTexture9::GenerateMipSubLevels: Stub");
    }

    Rc<Direct3DCommonTexture9> GetCommonTexture() {
      return m_texture;
    }

    static UINT CalcSubresource(UINT Level, UINT ArraySlice) {
      return (ArraySlice * m_texture->Desc()->MipSlices) + Level;
    }

    SubresourceType* GetSubresource(UINT Subresource) {
      if (Subresource >= m_subresources.size())
        return nullptr;

      return m_subresources[Subresource];
    }

  protected:

    Rc<Direct3DCommonTexture9> m_texture;

    DWORD m_lod;
    D3DTEXTUREFILTERTYPE m_autogenFilter;

    std::vector<SubresourceType*> m_subresources;

  };

  class Direct3DTexture9 final : public Direct3DBaseTexture9<Direct3DSurface9, IDirect3DTexture9> {

  public:

    D3DRESOURCETYPE STDMETHODCALLTYPE GetType();

    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc);

    HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT Level, IDirect3DSurface9** ppSurfaceLevel);

    HRESULT STDMETHODCALLTYPE LockRect(UINT Level, D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect, DWORD Flags);

    HRESULT STDMETHODCALLTYPE UnlockRect(UINT Level);

    HRESULT STDMETHODCALLTYPE AddDirtyRect(CONST RECT* pDirtyRect);

  };

}