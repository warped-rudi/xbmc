/*
 *      Copyright (C) 2012 Team XBMC
 *      http://xbmc.org
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
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#pragma once

#if defined(HAS_MARVELL_DOVE)

#undef __u8
#undef byte

#include "RenderFlags.h"
#include "BaseRenderer.h"
#include "settings/Settings.h"
#include "settings/MediaSettings.h"
#include "settings/DisplaySettings.h"
#include "../dvdplayer/DVDCodecs/Video/DVDVideoCodec.h"
#include "../dvdplayer/DVDCodecs/Video/DllVMETA.h"

class CRenderCapture;
class CBaseTexture;

#define AUTOSOURCE -1

#define IMAGE_FLAG_WRITING   0x01 /* image is in use after a call to GetImage, caller may be reading or writing */
#define IMAGE_FLAG_READING   0x02 /* image is in use after a call to GetImage, caller is only reading */
#define IMAGE_FLAG_DYNAMIC   0x04 /* image was allocated due to a call to GetImage */
#define IMAGE_FLAG_RESERVED  0x08 /* image is reserved, must be asked for specifically used to preserve images */
#define IMAGE_FLAG_READY     0x16 /* image is ready to be uploaded to texture memory */
#define IMAGE_FLAG_INUSE (IMAGE_FLAG_WRITING | IMAGE_FLAG_READING | IMAGE_FLAG_RESERVED)

extern "C"
{
#include <sys/ioctl.h>
#include <linux/fb.h>
#include "dovefb.h"
}

struct DRAWRECT
{
  float left;
  float top;
  float right;
  float bottom;
};

/*enum EFIELDSYNC
{
  FS_NONE,
  FS_TOP, // FS_ODD,
  FS_BOT  // FS_EVEN
};*/

struct YUVRANGE
{
  int y_min, y_max;
  int u_min, u_max;
  int v_min, v_max;
};

struct YUVCOEF
{
  float r_up, r_vp;
  float g_up, g_vp;
  float b_up, b_vp;
};

/*
enum RenderMethod
{
  RENDER_GLSL=0x01,
  RENDER_SW=0x04,
  RENDER_POT=0x10
};

enum RenderQuality
{
  RQ_LOW=1,
  RQ_SINGLEPASS,
  RQ_MULTIPASS,
  RQ_SOFTWARE
};
*/

#define PLANE_Y 0
#define PLANE_U 1
#define PLANE_V 2

#define FIELD_FULL 0
#define FIELD_ODD 1
#define FIELD_EVEN 2

#define MAX_QUEUE_NUM 60

extern YUVRANGE yuv_range_lim;
extern YUVRANGE yuv_range_full;
extern YUVCOEF yuv_coef_bt601;
extern YUVCOEF yuv_coef_bt709;
extern YUVCOEF yuv_coef_ebu;
extern YUVCOEF yuv_coef_smtp240m;

typedef struct _OutputBuffer
{
  unsigned      nFlag;          // see PBF_xxx
  unsigned char *pBuf;
  unsigned      nBufSize;

  unsigned      phyBuf[3];
  unsigned      lineSize[3];
} OutputBuffer;

#define PBF_UNUSED      0x00    // empty
#define PBF_IMPORTED    0x01    // owned by decoder (vMeta)
#define PBF_ALLOCATED   0x02    // owned by renderer

class CDoveOverlayRenderer : public CBaseRenderer
{
public:
  CDoveOverlayRenderer();
  virtual ~CDoveOverlayRenderer();

  virtual void Update(bool bPauseDrawing = false);
  virtual void SetupScreenshot() {};

  bool RenderCapture(CRenderCapture* capture);

  // Player functions
  virtual bool Configure(unsigned int width, unsigned int height,
                         unsigned int d_width, unsigned int d_height,
                         float fps, unsigned int flags, ERenderFormat format,
                         unsigned extended_format, unsigned int orientation);
  virtual bool IsConfigured() { return m_bConfigured; }
  virtual int          GetImage(YV12Image *image, int source = AUTOSOURCE, bool readonly = false) { return -1; }
  virtual void         ReleaseImage(int source, bool preserve = false) {};
  virtual void         FlipPage(int source);
  virtual unsigned int PreInit();
  virtual void         UnInit();
  virtual void         Reset(); /* resets renderer after seek for example */

  virtual void RenderUpdate(bool clear, DWORD flags = 0, DWORD alpha = 255);

  // Re-implemented CBaseRenderer function(s)
  virtual bool AddVideoPicture(DVDVideoPicture* picture, int index)
  {
    (void)index;
    DrawSlice(picture);
    return true;
  }

  // Feature support
  virtual bool SupportsMultiPassRendering();
  virtual bool Supports(ERENDERFEATURE feature);
  virtual bool Supports(EDEINTERLACEMODE mode);
  virtual bool Supports(EINTERLACEMETHOD method);
  virtual bool Supports(ESCALINGMETHOD method);

  virtual EINTERLACEMETHOD AutoInterlaceMethod();

private:
  void          ManageDisplay(bool first);
  bool          DrawSlice(DVDVideoPicture *pDvdVideoPicture);

  bool                  m_bConfigured;
  ERenderFormat         m_format;

  int                   m_overlayfd;
  int                   m_enabled;
  struct _sOvlySurface  m_overlaySurface;
  int                   m_interpolation;

  DllLibMiscGen         *m_DllMiscGen;
  DllLibVMETA           *m_DllVMETA;

  unsigned int          m_currentBuffer;
  OutputBuffer          m_SoftPicture[NUM_BUFFERS];

  unsigned char         *m_FreeBufAddr[MAX_QUEUE_NUM];
};

inline int NP2( unsigned x )
{
  --x;
  x |= x >> 1;
  x |= x >> 2;
  x |= x >> 4;
  x |= x >> 8;
  x |= x >> 16;
  return ++x;
}
#endif
