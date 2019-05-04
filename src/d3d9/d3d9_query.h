#pragma once

#include "d3d9_device_child.h"

namespace dxvk {

  enum D3D9_VK_QUERY_STATE : uint32_t {
    D3D9_VK_QUERY_INITIAL,
    D3D9_VK_QUERY_BEGUN,
    D3D9_VK_QUERY_ENDED,
  };

  class D3D9Query : public Direct3DDeviceChild9<IDirect3DQuery9> {

  public:

    D3D9Query(
            Direct3DDevice9Ex* pDevice,
            D3DQUERYTYPE       QueryType);

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject);

    D3DQUERYTYPE STDMETHODCALLTYPE GetType() final;

    DWORD STDMETHODCALLTYPE GetDataSize() final;

    HRESULT STDMETHODCALLTYPE Issue(DWORD dwIssueFlags) final;

    HRESULT STDMETHODCALLTYPE GetData(void* pData, DWORD dwSize, DWORD dwGetDataFlags) final;

    void Begin(DxvkContext* ctx);
    void End(DxvkContext* ctx);

    static bool QueryBeginnable(D3DQUERYTYPE QueryType);
    static bool QueryEndable(D3DQUERYTYPE QueryType);

    static HRESULT QuerySupported(D3DQUERYTYPE QueryType);

  private:

    D3DQUERYTYPE      m_queryType;

    D3D9_VK_QUERY_STATE m_state;

    Rc<DxvkGpuQuery>  m_query;
    Rc<DxvkGpuEvent>  m_event;

    UINT64 GetTimestampQueryFrequency() const;

  };

}