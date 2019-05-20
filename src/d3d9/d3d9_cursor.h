#pragma once

#include "d3d9_include.h"

namespace dxvk {

  class D3D9Cursor {

  public:

    D3D9Cursor();

    void UpdateCursor(int x, int y, bool immediate);

    void FlushCursor();

    BOOL ShowCursor(BOOL bShow);

  private:

    bool m_updatePending;
    int  m_pendingX;
    int  m_pendingY;

    BOOL m_visible;

  };

}