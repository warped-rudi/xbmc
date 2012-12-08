/*
 *      Copyright (C) 2012 Team XBMC
 *      http://www.solid-run.com
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
 * Original Dove Overlay Rendere written by Rabeeh Khoury from Solid-Run <support@solid-run.com>
 *
 */

#include "system.h"
#if (defined HAVE_CONFIG_H) && (!defined WIN32)
#include "config.h"
#endif

#if defined(HAS_MARVELL_DOVE)

#include <GLES/gl.h>

#undef COLOR_KEY_BLACK
#define COLOR_KEY_ALPHA
#include "DoveOverlayRenderer.h"
#include "utils/log.h"
#include <stdlib.h>
#include <malloc.h>
#include "guilib/GraphicContext.h"

#if defined(CLASSNAME)
#undef CLASSNAME
#endif

#define CLASSNAME "CDoveOverlayRenderer"


CDoveOverlayRenderer::CDoveOverlayRenderer()
{
  memset(m_SoftPicture, 0, sizeof(OutputBuffer) * NUM_BUFFERS);

  for(int i = 0; i < NUM_BUFFERS; i++)
  {
    m_SoftPicture[i].pPicture = (IppVmetaPicture *)malloc(sizeof(IppVmetaPicture));

    if(m_SoftPicture[i].pPicture)
      memset(m_SoftPicture[i].pPicture, 0, sizeof(IppVmetaPicture));
  }

  m_DllMiscGen        = new DllLibMiscGen();
  m_DllVMETA          = new DllLibVMETA();

  UnInit();
}


CDoveOverlayRenderer::~CDoveOverlayRenderer()
{
  UnInit();

  for(int i = 0; i < NUM_BUFFERS; i++)
  {
    if(m_SoftPicture[i].pPicture)
      free(m_SoftPicture[i].pPicture);
    m_SoftPicture[i].pPicture = NULL;
  }

  m_DllVMETA->Unload();
  m_DllMiscGen->Unload();

  delete m_DllMiscGen;
  delete m_DllVMETA;
}


void CDoveOverlayRenderer::ManageDisplay(bool first)
{
  CRect view;
  struct _sOvlySurface tmp_overlaySurface;

  memcpy (&tmp_overlaySurface, &m_overlaySurface, sizeof(struct _sOvlySurface));
  view.x1 = (float)g_settings.m_ResInfo[m_resolution].Overscan.left;
  view.y1 = (float)g_settings.m_ResInfo[m_resolution].Overscan.top;
  view.x2 = (float)g_settings.m_ResInfo[m_resolution].Overscan.right;
  view.y2 = (float)g_settings.m_ResInfo[m_resolution].Overscan.bottom;

  m_sourceRect.x1 = (float)g_settings.m_currentVideoSettings.m_CropLeft;
  m_sourceRect.y1 = (float)g_settings.m_currentVideoSettings.m_CropTop;
  m_sourceRect.x2 = (float)m_sourceWidth - g_settings.m_currentVideoSettings.m_CropRight;
  m_sourceRect.y2 = (float)m_sourceHeight - g_settings.m_currentVideoSettings.m_CropBottom;

  CalcNormalDisplayRect(view.x1, view.y1,
                        view.Width(), view.Height(),
                        GetAspectRatio() * g_settings.m_fPixelRatio,
                        g_settings.m_fZoomAmount, g_settings.m_fVerticalShift);

  OutputBuffer &currPict = m_SoftPicture[m_currentBuffer];

  if (m_format == RENDER_FMT_UYVY422)
  {
    m_overlaySurface.videoMode = DOVEFB_VMODE_YUV422PACKED_SWAPYUorV;
    if (currPict.iLineSize[0])
      m_overlaySurface.viewPortInfo.ycPitch = currPict.iLineSize[0] * 2;
    else
      m_overlaySurface.viewPortInfo.ycPitch = (m_sourceRect.x2 - m_sourceRect.x1) * 2;
    m_overlaySurface.viewPortInfo.uvPitch = 0;
  }
  else if (m_format == RENDER_FMT_YUV420P)
  {
    m_overlaySurface.videoMode = DOVEFB_VMODE_YUV420PLANAR;
    if (currPict.iLineSize[0])
    {
      m_overlaySurface.viewPortInfo.ycPitch = currPict.iLineSize[0];
      m_overlaySurface.viewPortInfo.uvPitch = currPict.iLineSize[1];
    }
    else
    {
      m_overlaySurface.viewPortInfo.ycPitch = m_sourceRect.x2 - m_sourceRect.x1;
      m_overlaySurface.viewPortInfo.uvPitch = (m_sourceRect.x2 - m_sourceRect.x1) / 2;
    }
  }

  m_overlaySurface.viewPortInfo.srcWidth  = m_sourceRect.x2 - m_sourceRect.x1;
  m_overlaySurface.viewPortInfo.srcHeight = m_sourceRect.y2 - m_sourceRect.y1;
  m_overlaySurface.viewPortInfo.zoomXSize = m_destRect.x2 - m_destRect.x1;
  m_overlaySurface.viewPortInfo.zoomYSize = m_destRect.y2 - m_destRect.y1;

  m_overlaySurface.viewPortOffset.xOffset = m_destRect.x1;
  m_overlaySurface.viewPortOffset.yOffset = m_destRect.y1;

  if (first || (tmp_overlaySurface.videoMode != m_overlaySurface.videoMode))
  {
    if (ioctl(m_overlayfd, DOVEFB_IOCTL_SET_VIDEO_MODE, &m_overlaySurface.videoMode) == -1)
    {
      CLog::Log(LOGERROR, "%s::%s - Failed to setup video mode", CLASSNAME, __func__);
    }
  }

  if (first || memcmp (&tmp_overlaySurface.viewPortInfo, &m_overlaySurface.viewPortInfo, sizeof (struct _sViewPortInfo)))
  {
    if (ioctl(m_overlayfd, DOVEFB_IOCTL_SET_VIEWPORT_INFO, &m_overlaySurface.viewPortInfo) != 0)
    {
      CLog::Log(LOGERROR, "%s::%s - Failed to setup video port", CLASSNAME, __func__);
    }
  }

  if (first || memcmp (&tmp_overlaySurface.viewPortOffset, &m_overlaySurface.viewPortOffset, sizeof (struct _sVideoBufferAddr)))
  {
    if (ioctl(m_overlayfd, DOVEFB_IOCTL_SET_VID_OFFSET, &m_overlaySurface.viewPortOffset) != 0)
    {
      CLog::Log(LOGERROR, "%s::%s - Failed to setup video port offset", CLASSNAME, __func__);
    }
  }
}


bool CDoveOverlayRenderer::Configure(
  unsigned int width, unsigned int height, unsigned int d_width, unsigned int d_height,
  float fps, unsigned int flags, ERenderFormat format, unsigned extended_format, unsigned int orientation)
{
  m_bConfigured = false;

  memset (&m_overlaySurface, 0, sizeof(m_overlaySurface));
  m_overlaySurface.videoBufferAddr.startAddr = 0;
  m_overlaySurface.videoBufferAddr.length = 0;//frameSize;
  m_overlaySurface.videoBufferAddr.inputData = 0;
  m_overlaySurface.videoBufferAddr.frameID = 0;

  if (format != RENDER_FMT_UYVY422 && format != RENDER_FMT_YUV420P)
  {
    CLog::Log(LOGERROR, "%s::%s - Unknown format 0x%x", CLASSNAME, __func__, format);

    m_format = RENDER_FMT_NONE;
    return false;
  }

  m_sourceWidth   = width;
  m_sourceHeight  = height;
  m_iFlags        = flags;
  m_format        = format;

  // Calculate the input frame aspect ratio.
  CalculateFrameAspectRatio(d_width, d_height);
  ChooseBestResolution(fps);
  SetViewMode(g_settings.m_currentVideoSettings.m_ViewMode);

  /* Open the video overlay */
  m_overlayfd = open("/dev/fb1", O_RDWR);
  if (m_overlayfd == -1)
  {
    CLog::Log(LOGERROR, "%s::%s - Failed to open framebuffer", CLASSNAME, __func__);
    return false;
  }

  ManageDisplay(true);

  CLog::Log(LOGDEBUG, "%s::%s - Setting ycPitch to %d, uvPitch to %d", CLASSNAME, __func__,
      m_overlaySurface.viewPortInfo.ycPitch ,m_overlaySurface.viewPortInfo.uvPitch);

  CLog::Log(LOGDEBUG, "m_sourceRect.x1 %f m_sourceRect.x2 %f m_sourceRect.y1 %f m_sourceRect.y2 %f m_sourceFrameRatio %f",
      m_sourceRect.x1, m_sourceRect.x2, m_sourceRect.y1, m_sourceRect.y2, m_sourceFrameRatio);
  CLog::Log(LOGDEBUG, "m_destRect.x1 %f m_destRect.x2 %f m_destRect.y1 %f m_destRect.y2 %f",
      m_destRect.x1, m_destRect.x2, m_destRect.y1, m_destRect.y2);

  m_enabled = 0;

  int srcMode = SHM_NORMAL;

  if (ioctl(m_overlayfd, DOVEFB_IOCTL_SET_SRC_MODE, &srcMode) == -1)
  {
    CLog::Log(LOGERROR, "%s::%s - Failed to enable video overlay", CLASSNAME, __func__);
    return false;
  }

  int interpolation = 3; // bi-linear interpolation

  if (ioctl(m_overlayfd, DOVEFB_IOCTL_SET_INTERPOLATION_MODE, &interpolation) != 0)
  {
    CLog::Log(LOGERROR, "%s::%s - Failed to setup video interpolation mode", CLASSNAME, __func__);
    return false;
  }

  struct _sColorKeyNAlpha alpha;

  memset (&alpha, 0, sizeof(alpha));

  alpha.mode = DOVEFB_ENABLE_RGB_COLORKEY_MODE;
  alpha.alphapath = DOVEFB_GRA_PATH_ALPHA;
  alpha.config = 0xff;//c0;
#ifdef COLOR_KEY_ALPHA
  alpha.Y_ColorAlpha = 0x02020200;
  alpha.U_ColorAlpha = 0x05050500;
  alpha.V_ColorAlpha = 0x07070700;
#endif
#ifdef COLOR_KEY_BLACK
  alpha.Y_ColorAlpha = 0x0;
  alpha.U_ColorAlpha = 0x0;
  alpha.V_ColorAlpha = 0x0;
#endif

  if (ioctl(m_overlayfd, DOVEFB_IOCTL_SET_COLORKEYnALPHA, &alpha) == -1)
  {
    CLog::Log(LOGERROR, "%s::%s - Failed to configure alpha", CLASSNAME, __func__);
    return false;
  }

  m_currentBuffer = 0;
  m_bConfigured   = true;

  CLog::Log(LOGDEBUG, "%s::%s - Proper format, continuing", CLASSNAME, __func__);

  return m_bConfigured;
}


unsigned int CDoveOverlayRenderer::PreInit()
{
  if(!m_DllVMETA->Load() || !m_DllMiscGen->Load())
    return false;

  UnInit();

  m_currentBuffer = 0;

  m_resolution = g_guiSettings.m_LookAndFeelResolution;
  if ( m_resolution == RES_WINDOW )
    m_resolution = RES_DESKTOP;

  return true;
}


void CDoveOverlayRenderer::FlipPage(int source)
{
  if (!m_bConfigured)
    return;

  ManageDisplay(false);

  unsigned phy_addr[3];
  bool next_frame_present = false;
  OutputBuffer &currPict = m_SoftPicture[m_currentBuffer];
  IppVmetaPicture *pPicture = currPict.pPicture;

  struct shm_private_info info;
  info.method = SHM_VMETA;
  ioctl(m_overlayfd, DOVEFB_IOCTL_SET_SRC_MODE, &info.method);

  m_overlaySurface.videoBufferAddr.frameID = 0;

  if(pPicture && pPicture->nPhyAddr)
  {
    m_overlaySurface.videoBufferAddr.startAddr = (unsigned char *)pPicture->nPhyAddr;
    m_overlaySurface.videoBufferAddr.length    = pPicture->nBufSize;
  }
  else
  {
    m_overlaySurface.videoBufferAddr.startAddr = NULL;
    m_overlaySurface.videoBufferAddr.length    = 0;
  }

  //ioctl by Solid-Run not in marvel kernel
  if (m_format == RENDER_FMT_UYVY422) /* Typically frames from vMeta */
  {
    phy_addr[0] = (unsigned int) currPict.buf[0];
    phy_addr[1] = (unsigned int) currPict.buf[0];
    phy_addr[2] = (unsigned int) currPict.buf[0];
    next_frame_present = true;
    if(ioctl(m_overlayfd, DOVEFB_IOCTL_NEXT_FRAME_PRESENT, &phy_addr) != 0)
      CLog::Log(LOGERROR, "%s::%s - Error flipping", CLASSNAME, __func__);
  }
  else if (m_format == RENDER_FMT_YUV420P)
  {
    phy_addr[0] = (unsigned int) currPict.buf[0];
    phy_addr[1] = (unsigned int) currPict.buf[1];
    phy_addr[2] = (unsigned int) currPict.buf[2];
    next_frame_present = true;
    if(ioctl(m_overlayfd, DOVEFB_IOCTL_NEXT_FRAME_PRESENT, &phy_addr) != 0)
      CLog::Log(LOGERROR, "%s::%s - Error flipping", CLASSNAME, __func__);

  }
  else if(ioctl(m_overlayfd, DOVEFB_IOCTL_FLIP_VID_BUFFER, &m_overlaySurface) != 0)
  {
      CLog::Log(LOGERROR, "%s::%s - Error flipping", CLASSNAME, __func__);
  }

  if (m_enabled == 0)
  {
    m_enabled = 1;

    if (ioctl(m_overlayfd, DOVEFB_IOCTL_SWITCH_VID_OVLY, &m_enabled) == -1)
      CLog::Log(LOGERROR, "%s::%s - Failed to enable video overlay", CLASSNAME, __func__);
  }

  /*
   * Is only needed for DOVEFB_IOCTL_NEXT_FRAME_PRESENT
   */
  if (next_frame_present)
  {
    if (ioctl(m_overlayfd, DOVEFB_IOCTL_WAIT_VSYNC, 0) != 0)
      CLog::Log(LOGERROR, "%s::%s - Error waiting for vsync", CLASSNAME, __func__);
  }

  if( source >= 0 && source < NUM_BUFFERS )
    m_currentBuffer = source;
  else
    m_currentBuffer = (m_currentBuffer + 1) % NUM_BUFFERS;
}


void CDoveOverlayRenderer::Reset()
{
  CLog::Log(LOGNOTICE, "%s::%s - Not implemented", CLASSNAME, __func__);
}


void CDoveOverlayRenderer::Update(bool bPauseDrawing)
{
  if (!m_bConfigured)
    return;

  ManageDisplay(false);
}


void CDoveOverlayRenderer::RenderUpdate(bool clear, DWORD flags, DWORD alpha)
{
  if (!m_bConfigured)
    return;

#ifdef COLOR_KEY_ALPHA
  glEnable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
#endif
}


bool CDoveOverlayRenderer::RenderCapture(CRenderCapture* capture)
{
  CLog::Log(LOGNOTICE, "%s::%s - Not implemented", CLASSNAME, __func__);
  return true;
}


bool CDoveOverlayRenderer::DrawSlice(DVDVideoPicture *pDvdVideoPicture)
{
  OutputBuffer    &currPict = m_SoftPicture[m_currentBuffer];
  IppVmetaPicture *pPicture = currPict.pPicture;

  if(!pPicture)
    return false;

  // Save the original data buffers and pitch
  currPict.data[0] = pDvdVideoPicture->data[0];
  currPict.data[1] = pDvdVideoPicture->data[1];
  currPict.data[2] = pDvdVideoPicture->data[2];
  currPict.data[3] = pDvdVideoPicture->data[3];
  currPict.iLineSize[0] = pDvdVideoPicture->iLineSize[0];
  currPict.iLineSize[1] = pDvdVideoPicture->iLineSize[1];
  currPict.iLineSize[2] = pDvdVideoPicture->iLineSize[2];
  currPict.iLineSize[3] = pDvdVideoPicture->iLineSize[3];

  // from vMeta ?
  if(pDvdVideoPicture->vmeta)
  {
    if(m_format != RENDER_FMT_UYVY422)
      return false;

    // switched from other format...
    if(pPicture->nFlag == PICBUF_ALLOCATED)
      m_DllVMETA->vdec_os_api_dma_free(pPicture->pBuf);

    // Decoder allocated buffer
    pPicture->nPhyAddr = pDvdVideoPicture->vmeta->nPhyAddr;
    pPicture->nBufSize = pDvdVideoPicture->vmeta->nBufSize;
    pPicture->pBuf     = pDvdVideoPicture->vmeta->pBuf;
    pPicture->nFlag    = PICBUF_IMPORTED;

    currPict.buf[0] = (unsigned char *)pPicture->nPhyAddr;
    currPict.buf[1] = (unsigned char *)pPicture->nPhyAddr +
      (pDvdVideoPicture->iLineSize[0] * pDvdVideoPicture->iHeight);
    currPict.buf[2] = (unsigned char *)pPicture->nPhyAddr +
      (pDvdVideoPicture->iLineSize[0] * pDvdVideoPicture->iHeight) +
      (pDvdVideoPicture->iLineSize[1] * pDvdVideoPicture->iHeight / 2);
  }
  else
  {
    if(m_format != RENDER_FMT_YUV420P)
      return false;

    // Software decoding. Allocate buffer for ouput
    unsigned int memSize = (pDvdVideoPicture->iLineSize[0] * pDvdVideoPicture->iHeight) +
                           (pDvdVideoPicture->iLineSize[1] * pDvdVideoPicture->iHeight / 2) +
                           (pDvdVideoPicture->iLineSize[2] * pDvdVideoPicture->iHeight / 2);

    // Check for size change ...
    if(pPicture->nFlag == PICBUF_ALLOCATED && pPicture->nBufSize < memSize)
    {
      m_DllVMETA->vdec_os_api_dma_free(pPicture->pBuf);
      pPicture->nFlag = PICBUF_IMPORTED;
    }

    // Allocate, if necessary
    if(pPicture->nFlag != PICBUF_ALLOCATED)
    {
      pPicture->pBuf = (Ipp8u*)m_DllVMETA->vdec_os_api_dma_alloc_cached(
                                    memSize, VMETA_DIS_BUF_ALIGN, &(pPicture->nPhyAddr));
      pPicture->nBufSize = memSize;
      pPicture->nFlag    = PICBUF_ALLOCATED;
    }

    if(!pPicture->pBuf)
    {
      pPicture->nBufSize = 0;
      pPicture->nPhyAddr = 0;
      pPicture->nFlag    = PICBUF_IMPORTED;
      CLog::Log(LOGERROR, "%s::%s - Failed to alloc memory", CLASSNAME, __func__);
      return false;
    }

    currPict.buf[0] = (unsigned char *)pPicture->nPhyAddr;
    currPict.buf[1] = (unsigned char *)pPicture->nPhyAddr +
      (pDvdVideoPicture->iLineSize[0] * pDvdVideoPicture->iHeight);
    currPict.buf[2] = (unsigned char *)pPicture->nPhyAddr +
      (pDvdVideoPicture->iLineSize[0] * pDvdVideoPicture->iHeight) +
      (pDvdVideoPicture->iLineSize[1] * pDvdVideoPicture->iHeight / 2);

    unsigned char *dst = pPicture->pBuf;
    memcpy( dst, pDvdVideoPicture->data[0], pDvdVideoPicture->iLineSize[0] * pDvdVideoPicture->iHeight );
    dst += pDvdVideoPicture->iLineSize[0] * pDvdVideoPicture->iHeight;
    memcpy( dst, pDvdVideoPicture->data[1], pDvdVideoPicture->iLineSize[1] * pDvdVideoPicture->iHeight / 2 );
    dst += pDvdVideoPicture->iLineSize[1] * pDvdVideoPicture->iHeight / 2;
    memcpy( dst, pDvdVideoPicture->data[2], pDvdVideoPicture->iLineSize[2] * pDvdVideoPicture->iHeight / 2 );
  }

  return true;
}


void CDoveOverlayRenderer::UnInit()
{
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);

  memset(m_FreeBufAddr, 0, MAX_QUEUE_NUM * sizeof(unsigned char*));
  if(m_overlayfd != -1)
    ioctl(m_overlayfd, DOVEFB_IOCTL_GET_FREELIST, &m_FreeBufAddr);

  if(m_enabled)
  {
    m_enabled = 0;

    if (ioctl(m_overlayfd, DOVEFB_IOCTL_WAIT_VSYNC, 0) != 0)
      CLog::Log(LOGERROR, "%s::%s - Error waiting for vsync", CLASSNAME, __func__);

    if(ioctl(m_overlayfd, DOVEFB_IOCTL_SWITCH_VID_OVLY, &m_enabled) == -1)
      CLog::Log(LOGERROR, "%s::%s Failed to disable video overlay", CLASSNAME, __func__);
  }

  if (m_overlayfd != -1)
    close(m_overlayfd);

  for(int i = 0; i < NUM_BUFFERS; i++)
  {
    IppVmetaPicture *pPicture = m_SoftPicture[i].pPicture;

    if(pPicture)
    {
      if(pPicture->nFlag == PICBUF_ALLOCATED)
        m_DllVMETA->vdec_os_api_dma_free(pPicture->pBuf);

      pPicture->pBuf     = NULL;
      pPicture->nPhyAddr = 0;
      pPicture->nBufSize = 0;
      pPicture->nFlag    = PICBUF_IMPORTED;
    }
  }

  m_currentBuffer           = 0;
  m_iFlags                  = 0;
  m_bConfigured             = false;
  m_overlayfd               = -1;
  m_sourceWidth             = 0;
  m_sourceHeight            = 0;

  memset(&m_overlaySurface, 0, sizeof(struct _sOvlySurface));
  memset(&m_overlayPlaneInfo, 0, sizeof(struct _sViewPortInfo));
}


bool CDoveOverlayRenderer::Supports(EDEINTERLACEMODE mode)
{
  return false;
}


bool CDoveOverlayRenderer::Supports(ERENDERFEATURE feature)
{
  return false;
}


bool CDoveOverlayRenderer::SupportsMultiPassRendering()
{
  return false;
}


bool CDoveOverlayRenderer::Supports(EINTERLACEMETHOD method)
{
  return false;
}


bool CDoveOverlayRenderer::Supports(ESCALINGMETHOD method)
{
  if(method == VS_SCALINGMETHOD_NEAREST || method == VS_SCALINGMETHOD_LINEAR)
    return true;

  return false;
}


EINTERLACEMETHOD CDoveOverlayRenderer::AutoInterlaceMethod()
{
  return VS_INTERLACEMETHOD_NONE;
}

#endif
