#include "d3d9_vertex_declaration.h"
#include "d3d9_util.h"

#include <algorithm>
#include <cstring>

namespace dxvk {

  D3D9VertexDecl::D3D9VertexDecl(
          D3D9DeviceEx*      pDevice,
          DWORD              FVF)
    : D3D9VertexDeclBase(pDevice) {
    m_elements.reserve(16);
    this->SetFVF(FVF);
  }

  D3D9VertexDecl::D3D9VertexDecl(
          D3D9DeviceEx*      pDevice,
    const D3DVERTEXELEMENT9* pVertexElements,
          uint32_t           DeclCount)
    : D3D9VertexDeclBase( pDevice )
    , m_elements        ( DeclCount )
    , m_fvf             ( 0 ) {
    std::memcpy(m_elements.data(), pVertexElements, sizeof(D3DVERTEXELEMENT9) * DeclCount);
  }

  HRESULT STDMETHODCALLTYPE D3D9VertexDecl::QueryInterface(
          REFIID  riid,
          void** ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(IDirect3DVertexDeclaration9)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    Logger::warn("D3D9VertexDecl::QueryInterface: Unknown interface query");
    Logger::warn(str::format(riid));
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE D3D9VertexDecl::GetDeclaration(
          D3DVERTEXELEMENT9* pElement,
          UINT*              pNumElements) {
    if (pNumElements == nullptr)
      return D3DERR_INVALIDCALL;

    if (pElement == nullptr) {
      *pNumElements = m_elements.size();
      return D3D_OK;
    }

    const UINT count = std::min(*pNumElements, UINT(m_elements.size()));

    std::memcpy(
      pElement,
      m_elements.data(),
      sizeof(D3DVERTEXELEMENT9) * count);

    return D3D_OK;
  }

  void D3D9VertexDecl::SetFVF(DWORD FVF) {
    m_fvf = FVF;

    std::array<D3DVERTEXELEMENT9, 16> elements;
    uint32_t elemCount = 0;
    uint32_t texCount = 0;

    uint32_t betas = 0;
    uint8_t betaIdx = 0xFF;

    switch (FVF & D3DFVF_POSITION_MASK) {
      case D3DFVF_XYZ:
      case D3DFVF_XYZB1:
      case D3DFVF_XYZB2:
      case D3DFVF_XYZB3:
      case D3DFVF_XYZB4:
      case D3DFVF_XYZB5:
        elements[elemCount].Type = D3DDECLTYPE_FLOAT3;
        elements[elemCount].Usage = D3DDECLUSAGE_POSITION;
        elements[elemCount].UsageIndex = 0;
        elemCount++;

        if ((FVF & D3DFVF_POSITION_MASK) == D3DFVF_XYZ)
          break;

        betas = (((FVF & D3DFVF_XYZB5) - D3DFVF_XYZB1) >> 1) + 1;
        if (FVF & D3DFVF_LASTBETA_D3DCOLOR)
          betaIdx = D3DDECLTYPE_D3DCOLOR;
        else if (FVF & D3DFVF_LASTBETA_UBYTE4)
          betaIdx = D3DDECLTYPE_UBYTE4;
        else if ((FVF & D3DFVF_XYZB5) == D3DFVF_XYZB5)
          betaIdx = D3DDECLTYPE_FLOAT1;

        if (betaIdx != 0xFF)
          betas--;

        if (betas > 0) {
          switch (betas) {
            case 1: elements[elemCount].Type = D3DDECLTYPE_FLOAT1; break;
            case 2: elements[elemCount].Type = D3DDECLTYPE_FLOAT2; break;
            case 3: elements[elemCount].Type = D3DDECLTYPE_FLOAT3; break;
            case 4: elements[elemCount].Type = D3DDECLTYPE_FLOAT4; break;
            default: break;
          }
          elements[elemCount].Usage = D3DDECLUSAGE_BLENDWEIGHT;
          elements[elemCount].UsageIndex = 0;
          elemCount++;
        }

        if (betaIdx != 0xFF) {
          elements[elemCount].Type = betaIdx;
          elements[elemCount].Usage = D3DDECLUSAGE_BLENDINDICES;
          elements[elemCount].UsageIndex = 0;
          elemCount++;
        }
        break;

      case D3DFVF_XYZW:
      case D3DFVF_XYZRHW:
        elements[elemCount].Type = D3DDECLTYPE_FLOAT4;
        elements[elemCount].Usage =
          ((FVF & D3DFVF_POSITION_MASK) == D3DFVF_XYZW)
          ? D3DDECLUSAGE_POSITION
          : D3DDECLUSAGE_POSITIONT;
        elements[elemCount].UsageIndex = 0;
        elemCount++;
        break;

      default:
        break;
    }

    if (FVF & D3DFVF_NORMAL) {
      elements[elemCount].Type = D3DDECLTYPE_FLOAT3;
      elements[elemCount].Usage = D3DDECLUSAGE_NORMAL;
      elements[elemCount].UsageIndex = 0;
      elemCount++;
    }
    if (FVF & D3DFVF_PSIZE) {
      elements[elemCount].Type = D3DDECLTYPE_FLOAT1;
      elements[elemCount].Usage = D3DDECLUSAGE_PSIZE;
      elements[elemCount].UsageIndex = 0;
      elemCount++;
    }
    if (FVF & D3DFVF_DIFFUSE) {
      elements[elemCount].Type = D3DDECLTYPE_D3DCOLOR;
      elements[elemCount].Usage = D3DDECLUSAGE_COLOR;
      elements[elemCount].UsageIndex = 0;
      elemCount++;
    }
    if (FVF & D3DFVF_SPECULAR) {
      elements[elemCount].Type = D3DDECLTYPE_D3DCOLOR;
      elements[elemCount].Usage = D3DDECLUSAGE_COLOR;
      elements[elemCount].UsageIndex = 1;
      elemCount++;
    }

    texCount = (FVF & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    texCount = std::min(texCount, 8u);

    for (uint32_t i = 0; i < texCount; i++) {
      switch ((FVF >> (16 + i * 2)) & 0x3) {
        case D3DFVF_TEXTUREFORMAT1:
          elements[elemCount].Type = D3DDECLTYPE_FLOAT1;
          break;

        case D3DFVF_TEXTUREFORMAT2:
          elements[elemCount].Type = D3DDECLTYPE_FLOAT2;
          break;

        case D3DFVF_TEXTUREFORMAT3:
          elements[elemCount].Type = D3DDECLTYPE_FLOAT3;
          break;

        case D3DFVF_TEXTUREFORMAT4:
          elements[elemCount].Type = D3DDECLTYPE_FLOAT4;
          break;

        default:
          break;
      }
      elements[elemCount].Usage = D3DDECLUSAGE_TEXCOORD;
      elements[elemCount].UsageIndex = i;
      elemCount++;
    }

    for (uint32_t i = 0; i < elemCount; i++) {
      elements[i].Stream = 0;
      elements[i].Offset = (i == 0) 
        ? 0
        : (elements[i - 1].Offset + DecltypeSize(D3DDECLTYPE(elements[i - 1].Type)));

      elements[i].Method = D3DDECLMETHOD_DEFAULT;
    }

    m_elements.resize(elemCount);
    std::memcpy(m_elements.data(), elements.data(), sizeof(D3DVERTEXELEMENT9) * elemCount);
  }

}