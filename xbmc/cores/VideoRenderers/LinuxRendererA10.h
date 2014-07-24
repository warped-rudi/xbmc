#ifndef LinuxRendererA10_RENDERER
#define LinuxRendererA10_RENDERER

/*
 *      Copyright (C) 2010-2012 Team XBMC and others
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#ifdef ALLWINNERA10

#include "LinuxRendererGLES.h"

struct A10VLQueueItem;

class CLinuxRendererA10 : public CLinuxRendererGLES
{
  enum { RENDER_A10BUF = RENDER_USERDATA };

public:
  CLinuxRendererA10() : CLinuxRendererGLES() {};
  virtual ~CLinuxRendererA10() {};

  virtual unsigned int PreInit();
  virtual void         UnInit();

  virtual int  GetImage(YV12Image *image, int source = AUTOSOURCE, bool readonly = false);
  virtual void RenderUpdate(bool clear, DWORD flags = 0, DWORD alpha = 255);
  
  virtual void LoadShaders(int field=FIELD_FULL);

  virtual unsigned int GetProcessorSize();
  virtual void AddProcessor(A10VLQueueItem *pVidBuff);
};


/*
 * Video layer functions
 */

extern "C" {
#include <libcedarv.h>
#include <drv_display_sun4i.h>
#ifndef CEDARV_FRAME_HAS_PHY_ADDR
#include <os_adapter.h>
#endif

#ifndef SUNXI_DISP_VERSION
#define SUNXI_DISP_VERSION_MAJOR 1
#define SUNXI_DISP_VERSION_MINOR 0
#define SUNXI_DISP_VERSION ((SUNXI_DISP_VERSION_MAJOR << 16) | SUNXI_DISP_VERSION_MINOR)
#define SUNXI_DISP_VERSION_MAJOR_GET(x) (((x) >> 16) & 0x7FFF)
#define SUNXI_DISP_VERSION_MINOR_GET(x) ((x) & 0xFFFF)
#define DISP_CMD_VERSION DISP_CMD_RESERVE0
#endif
}


#define DISPQS 24

typedef void (*A10VLCALLBACK)(void *callbackpriv, void *pictpriv, cedarv_picture_t &pict); //cleanup function

struct A10VLQueueItem
{
  int               decnr;
  A10VLCALLBACK     callback;
  void             *callbackpriv;
  void             *pictpriv;
  cedarv_picture_t  pict;
};

bool A10VLInit(int &width, int &height, double &refreshRate);

void A10VLExit();

void A10VLHide();

void A10VLWaitVSYNC();

A10VLQueueItem *A10VLPutQueue(A10VLCALLBACK     callback,
                              void             *callbackpriv,
                              void             *pictpriv,
                              cedarv_picture_t &pict);

void A10VLFreeQueue();

void A10VLDisplayQueueItem(A10VLQueueItem *pItem, CRect &srcRect, CRect &dstRect);

int  A10VLDisplayPicture(cedarv_picture_t &pict, int refnr, CRect &srcRect, CRect &dstRect);

#endif

#endif
