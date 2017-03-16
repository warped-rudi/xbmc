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
 * Original Dove Overlay Renderer written by Rabeeh Khoury from Solid-Run <support@solid-run.com>
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


// missing in dovefb.h
#define DOVEFB_INTERPOLATION_NONE       0
#define DOVEFB_INTERPOLATION_BILINEAR   3



CDoveOverlayRenderer::CDoveOverlayRenderer()
{
  memset(m_SoftPicture, 0, sizeof(OutputBuffer) * NUM_BUFFERS);

  m_DllMiscGen        = new DllLibMiscGen();
  m_DllVMETA          = new DllLibVMETA();
  m_overlayfd         = -1;
  m_enabled           = 0;

  UnInit();
}


CDoveOverlayRenderer::~CDoveOverlayRenderer()
{
  UnInit();

  m_DllVMETA->Unload();
  m_DllMiscGen->Unload();

  delete m_DllMiscGen;
  delete m_DllVMETA;
}


void CDoveOverlayRenderer::ManageDisplay(bool first)
{
  CRect view;
  int interpolation = m_interpolation;
  struct _sOvlySurface overlaySurface = m_overlaySurface;

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

  if (m_format == RENDER_FMT_YUV420P)
  {
    m_overlaySurface.videoMode = DOVEFB_VMODE_YUV420PLANAR;
    if (currPict.lineSize[0] && currPict.lineSize[1])
    {
      m_overlaySurface.viewPortInfo.ycPitch = currPict.lineSize[0];
      m_overlaySurface.viewPortInfo.uvPitch = currPict.lineSize[1];
    }
    else
    {
      m_overlaySurface.viewPortInfo.ycPitch = m_sourceRect.x2 - m_sourceRect.x1;
      m_overlaySurface.viewPortInfo.uvPitch = (m_sourceRect.x2 - m_sourceRect.x1) / 2;
    }
  }
  else /*if (m_format == RENDER_FMT_VMETA || m_format == RENDER_FMT_UYVY422)*/
  {
    m_overlaySurface.videoMode = DOVEFB_VMODE_YUV422PACKED_SWAPYUorV;
    if (currPict.lineSize[0])
      m_overlaySurface.viewPortInfo.ycPitch = currPict.lineSize[0];
    else
      m_overlaySurface.viewPortInfo.ycPitch = (m_sourceRect.x2 - m_sourceRect.x1) * 2;
    m_overlaySurface.viewPortInfo.uvPitch = 0;
  }

  m_overlaySurface.viewPortInfo.srcWidth  = m_sourceRect.x2 - m_sourceRect.x1;
  m_overlaySurface.viewPortInfo.srcHeight = m_sourceRect.y2 - m_sourceRect.y1;
  m_overlaySurface.viewPortInfo.zoomXSize = m_destRect.x2 - m_destRect.x1;
  // Hack to avoid flickering line on bottom of screen with some MPEG1 videos
  m_overlaySurface.viewPortInfo.zoomYSize = m_destRect.y2 - m_destRect.y1 - 1;

  m_overlaySurface.viewPortOffset.xOffset = m_destRect.x1;
  m_overlaySurface.viewPortOffset.yOffset = m_destRect.y1;

  if (first || (overlaySurface.videoMode != m_overlaySurface.videoMode))
  {
    if (ioctl(m_overlayfd, DOVEFB_IOCTL_SET_VIDEO_MODE, &m_overlaySurface.videoMode) == -1)
      CLog::Log(LOGERROR, "%s::%s - Failed to setup video mode", CLASSNAME, __func__);
  }

  if (first || memcmp (&overlaySurface.viewPortInfo, &m_overlaySurface.viewPortInfo, sizeof (struct _sViewPortInfo)))
  {
    CLog::Log(LOGDEBUG, "m_sourceRect.x1 %f m_sourceRect.x2 %f m_sourceRect.y1 %f m_sourceRect.y2 %f m_sourceFrameRatio %f",
        m_sourceRect.x1, m_sourceRect.x2, m_sourceRect.y1, m_sourceRect.y2, m_sourceFrameRatio);
    CLog::Log(LOGDEBUG, "m_destRect.x1 %f m_destRect.x2 %f m_destRect.y1 %f m_destRect.y2 %f",
        m_destRect.x1, m_destRect.x2, m_destRect.y1, m_destRect.y2);
    CLog::Log(LOGDEBUG, "%s::%s - Setting ycPitch to %d, uvPitch to %d", CLASSNAME, __func__,
        m_overlaySurface.viewPortInfo.ycPitch ,m_overlaySurface.viewPortInfo.uvPitch);

    if (ioctl(m_overlayfd, DOVEFB_IOCTL_SET_VIEWPORT_INFO, &m_overlaySurface.viewPortInfo) != 0)
      CLog::Log(LOGERROR, "%s::%s - Failed to setup video port", CLASSNAME, __func__);
  }

  if (first || memcmp (&overlaySurface.viewPortOffset, &m_overlaySurface.viewPortOffset, sizeof (struct _sVideoBufferAddr)))
  {
    if (ioctl(m_overlayfd, DOVEFB_IOCTL_SET_VID_OFFSET, &m_overlaySurface.viewPortOffset) != 0)
      CLog::Log(LOGERROR, "%s::%s - Failed to setup video port offset", CLASSNAME, __func__);
  }

  /* Rabeeh said: Scaler is set differently when using graphics scaler */
  m_interpolation = (g_graphicsContext.getGraphicsScale() == GR_SCALE_100 &&
                      g_settings.m_currentVideoSettings.m_ScalingMethod != VS_SCALINGMETHOD_NEAREST) ?
                        DOVEFB_INTERPOLATION_BILINEAR : DOVEFB_INTERPOLATION_NONE;
  if (first || interpolation != m_interpolation)
  {
    if (ioctl(m_overlayfd, DOVEFB_IOCTL_SET_INTERPOLATION_MODE, &m_interpolation) != 0)
      CLog::Log(LOGERROR, "%s::%s - Failed to setup video interpolation mode", CLASSNAME, __func__);
  }

  if (first) 
  {
    int srcMode = SHM_VMETA;
    if (ioctl(m_overlayfd, DOVEFB_IOCTL_SET_SRC_MODE, &srcMode) != 0)
      CLog::Log(LOGERROR, "%s::%s - Failed to set source mode", CLASSNAME, __func__);
  }
}


bool CDoveOverlayRenderer::Configure(
  unsigned int width, unsigned int height, unsigned int d_width, unsigned int d_height,
  float fps, unsigned int flags, ERenderFormat format, unsigned extended_format, unsigned int orientation)
{
  m_bConfigured   = false;
  m_enabled       = 0;
  m_currentBuffer = 0;

  memset (&m_overlaySurface, 0, sizeof(m_overlaySurface));

  if (format != RENDER_FMT_VMETA && format != RENDER_FMT_UYVY422 && format != RENDER_FMT_YUV420P)
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

  int srcMode = SHM_NORMAL;
  if (ioctl(m_overlayfd, DOVEFB_IOCTL_SET_SRC_MODE, &srcMode) == -1)
  {
    CLog::Log(LOGERROR, "%s::%s - Failed to set source mode", CLASSNAME, __func__);
    return false;
  }

  int interpolation = DOVEFB_INTERPOLATION_NONE;
  if (ioctl(m_overlayfd, DOVEFB_IOCTL_SET_INTERPOLATION_MODE, &interpolation) == -1)
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

  CLog::Log(LOGDEBUG, "%s::%s - Proper format, continuing", CLASSNAME, __func__);

  m_bConfigured = true;
  return m_bConfigured;
}


unsigned int CDoveOverlayRenderer::PreInit()
{
  if(!m_DllVMETA->Load() || !m_DllMiscGen->Load())
    return false;

  UnInit();

  m_resolution = g_guiSettings.m_LookAndFeelResolution;
  if ( m_resolution == RES_WINDOW )
    m_resolution = RES_DESKTOP;

  return true;
}


void CDoveOverlayRenderer::FlipPage(int source)
{
  bool next_frame_present = false;
  OutputBuffer &currPict = m_SoftPicture[m_currentBuffer];

  if (!m_bConfigured)
    return;

  ManageDisplay(!m_enabled);

  m_overlaySurface.videoBufferAddr.frameID = 0;

  if (currPict.phyBuf[0])
  {
    m_overlaySurface.videoBufferAddr.startAddr = (unsigned char *)currPict.phyBuf[0];
    m_overlaySurface.videoBufferAddr.length    = currPict.nBufSize;
  }
  else
  {
    m_overlaySurface.videoBufferAddr.startAddr = NULL;
    m_overlaySurface.videoBufferAddr.length    = 0;
  }

  if (m_format == RENDER_FMT_VMETA || m_format == RENDER_FMT_UYVY422 || m_format == RENDER_FMT_YUV420P)
  {
    //ioctl by Solid-Run not in marvel kernel
    if(ioctl(m_overlayfd, DOVEFB_IOCTL_NEXT_FRAME_PRESENT, currPict.phyBuf) != 0)
      CLog::Log(LOGERROR, "%s::%s - Error flipping", CLASSNAME, __func__);
    next_frame_present = true;
  }
  else if (ioctl(m_overlayfd, DOVEFB_IOCTL_FLIP_VID_BUFFER, &m_overlaySurface) != 0)
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
  OutputBuffer &currPict = m_SoftPicture[m_currentBuffer];

  if (pDvdVideoPicture->format != m_format)
      return false;

  // from vMeta ?
  if (m_format == RENDER_FMT_VMETA)
  {
    // switched from other format...
    if(currPict.nFlag & PBF_ALLOCATED)
      m_DllVMETA->vdec_os_api_dma_free(currPict.pBuf);

    // Decoder allocated buffer
    currPict.nFlag       = PBF_IMPORTED;
    currPict.pBuf        = pDvdVideoPicture->vmeta->pBuf;
    currPict.nBufSize    = pDvdVideoPicture->vmeta->nBufSize;
    currPict.phyBuf[0]   = pDvdVideoPicture->vmeta->nPhyAddr;
    currPict.lineSize[0] = pDvdVideoPicture->vmeta->pic.picPlaneStep[0];

    currPict.phyBuf[1]   = currPict.phyBuf[2] = 0;
    currPict.lineSize[1] = currPict.lineSize[2] = 0;
  }
  else
  {
    unsigned int nBufSize = pDvdVideoPicture->iLineSize[0] * pDvdVideoPicture->iHeight;

    // Software decoding. Allocate buffer for ouput
    if (m_format == RENDER_FMT_YUV420P)
    {
      nBufSize += (pDvdVideoPicture->iLineSize[1] * pDvdVideoPicture->iHeight / 2) +
                  (pDvdVideoPicture->iLineSize[2] * pDvdVideoPicture->iHeight / 2);
    }
    else if (m_format != RENDER_FMT_UYVY422)
    {
      return false;
    }

    // Check for size change ...
    if((currPict.nFlag & PBF_ALLOCATED) && currPict.nBufSize < nBufSize)
    {
      m_DllVMETA->vdec_os_api_dma_free(currPict.pBuf);
      currPict.nFlag = PBF_UNUSED;
    }

    // Allocate, if necessary
    if(!(currPict.nFlag & PBF_ALLOCATED))
    {
      currPict.pBuf = (Ipp8u*)m_DllVMETA->vdec_os_api_dma_alloc_cached(
                                nBufSize, VMETA_DIS_BUF_ALIGN, &currPict.phyBuf[0]);
      currPict.nBufSize = nBufSize;
      currPict.nFlag    = PBF_ALLOCATED;
    }

    if(!currPict.pBuf)
    {
      currPict.nFlag    = PBF_UNUSED;
      CLog::Log(LOGERROR, "%s::%s - Failed to alloc memory", CLASSNAME, __func__);
      return false;
    }

    unsigned char *dst = currPict.pBuf;
    memcpy( dst, pDvdVideoPicture->data[0], pDvdVideoPicture->iLineSize[0] * pDvdVideoPicture->iHeight );
    currPict.lineSize[0] = pDvdVideoPicture->iLineSize[0];

    if (m_format == RENDER_FMT_YUV420P)
    {
      currPict.phyBuf[1] = currPict.phyBuf[0] + (pDvdVideoPicture->iLineSize[0] * pDvdVideoPicture->iHeight);
      currPict.phyBuf[2] = currPict.phyBuf[1] + (pDvdVideoPicture->iLineSize[1] * pDvdVideoPicture->iHeight / 2);

      dst += pDvdVideoPicture->iLineSize[0] * pDvdVideoPicture->iHeight;
      memcpy( dst, pDvdVideoPicture->data[1], pDvdVideoPicture->iLineSize[1] * pDvdVideoPicture->iHeight / 2 );
      currPict.lineSize[1] = pDvdVideoPicture->iLineSize[1];

      dst += pDvdVideoPicture->iLineSize[1] * pDvdVideoPicture->iHeight / 2;
      memcpy( dst, pDvdVideoPicture->data[2], pDvdVideoPicture->iLineSize[2] * pDvdVideoPicture->iHeight / 2 );
      currPict.lineSize[2] = pDvdVideoPicture->iLineSize[2];
    }
    else
    {
      currPict.phyBuf[1] = currPict.phyBuf[2] = 0;
      currPict.lineSize[1] = currPict.lineSize[2] = 0;
    }
  }

  return true;
}


void CDoveOverlayRenderer::UnInit()
{
  CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);

  memset(m_FreeBufAddr, 0, MAX_QUEUE_NUM * sizeof(m_FreeBufAddr[0]));

  if(m_overlayfd != -1)
  {
    ioctl(m_overlayfd, DOVEFB_IOCTL_GET_FREELIST, &m_FreeBufAddr);

    if(m_enabled)
    {
      m_enabled = 0;

      if (ioctl(m_overlayfd, DOVEFB_IOCTL_WAIT_VSYNC, 0) != 0)
        CLog::Log(LOGERROR, "%s::%s - Error waiting for vsync", CLASSNAME, __func__);

      if(ioctl(m_overlayfd, DOVEFB_IOCTL_SWITCH_VID_OVLY, &m_enabled) == -1)
        CLog::Log(LOGERROR, "%s::%s Failed to disable video overlay", CLASSNAME, __func__);
    }

    close(m_overlayfd);
    m_overlayfd = -1;
  }

  for(int i = 0; i < NUM_BUFFERS; i++)
  {
    OutputBuffer &currPict = m_SoftPicture[i];

    if(currPict.nFlag & PBF_ALLOCATED)
      m_DllVMETA->vdec_os_api_dma_free(currPict.pBuf);
  }
  memset(m_SoftPicture, 0, sizeof(OutputBuffer) * NUM_BUFFERS);

  m_bConfigured             = false;
  m_currentBuffer           = 0;
  m_iFlags                  = 0;
  m_sourceWidth             = 0;
  m_sourceHeight            = 0;
  m_interpolation           = DOVEFB_INTERPOLATION_NONE;

  memset(&m_overlaySurface, 0, sizeof(struct _sOvlySurface));
}


bool CDoveOverlayRenderer::Supports(EDEINTERLACEMODE mode)
{
  return false;
}


bool CDoveOverlayRenderer::Supports(ERENDERFEATURE feature)
{
  if( feature == RENDERFEATURE_STRETCH 
      || feature == RENDERFEATURE_ZOOM
      || feature == RENDERFEATURE_PIXEL_RATIO
//      || feature == RENDERFEATURE_VERTICAL_SHIFT
//      || feature == RENDERFEATURE_NONLINSTRETCH
//      || feature == RENDERFEATURE_CROP
    )
    return true;

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
