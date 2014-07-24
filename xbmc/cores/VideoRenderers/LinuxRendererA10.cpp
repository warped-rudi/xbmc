/*
 *      Copyright (C) 2010-2012 Team XBMC
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

#include "system.h"
#if (defined HAVE_CONFIG_H) && (!defined WIN32)
  #include "config.h"
#endif

#ifdef ALLWINNERA10

#include <sys/ioctl.h>

#include "utils/log.h"
#include "utils/GLUtils.h"
#include "LinuxRendererA10.h"
#include "VideoShaders/YUV2RGBShader.h"

using namespace Shaders;


unsigned int CLinuxRendererA10::PreInit()
{
  unsigned int rc = CLinuxRendererGLES::PreInit();

  m_formats.push_back(RENDER_FMT_A10BUF);

  return rc;
}


void CLinuxRendererA10::UnInit()
{
  A10VLHide();

  CLinuxRendererGLES::UnInit();
}


int CLinuxRendererA10::GetImage(YV12Image *image, int source, bool readonly)
{
  if (m_renderMethod & RENDER_A10BUF)
  {
    if (!image || !m_bValidated)
      return -1;

    /* take next available buffer */
    if( source == AUTOSOURCE )
      source = NextYV12Texture();

    return source;
  }

  return CLinuxRendererGLES::GetImage(image, source, readonly);
}


void CLinuxRendererA10::RenderUpdate(bool clear, DWORD flags, DWORD alpha)
{
  if ((m_renderMethod & (RENDER_A10BUF | RENDER_BYPASS)) != RENDER_A10BUF)
  {
    CLinuxRendererGLES::RenderUpdate(clear, flags, alpha);
    return;
  }


  if (!m_bConfigured)
    return;

  // if its first pass, just init textures and return
  if (ValidateRenderTarget())
    return;

  ManageDisplay();

  if (m_RenderUpdateCallBackFn)
    (*m_RenderUpdateCallBackFn)(m_RenderUpdateCallBackCtx, m_sourceRect, m_destRect);

  A10VLWaitVSYNC();

  g_graphicsContext.BeginPaint();

  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glClearColor(1.0/255, 2.0/255, 3.0/255, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  glClearColor(0, 0, 0, 0);

  g_graphicsContext.EndPaint();

  // this needs to be checked after texture validation
  //if (!m_bImageReady) return;

  int index = m_iYV12RenderBuffer;
  YUVBUFFER& buf =  m_buffers[index];

  if (buf.userData)
  {
    A10VLDisplayQueueItem((A10VLQueueItem *)buf.userData, m_sourceRect, m_destRect);
    m_iLastRenderBuffer = index;
  }

  VerifyGLState();
}


void CLinuxRendererA10::LoadShaders(int field)
{
  if (m_format != RENDER_FMT_A10BUF)
  {
    CLinuxRendererGLES::LoadShaders(field);
    return;
  }

  if (m_pYUVShader)
  {
    m_pYUVShader->Free();
    delete m_pYUVShader;
    m_pYUVShader = NULL;
  }

  CLog::Log(LOGNOTICE, "CLinuxRendererA10: Using A10 render method");
  m_renderMethod = RENDER_A10BUF;

  m_textureUpload = &CLinuxRendererA10::UploadBYPASSTexture;
  m_textureCreate = &CLinuxRendererA10::CreateBYPASSTexture;
  m_textureDelete = &CLinuxRendererA10::DeleteBYPASSTexture;

  if (m_oldRenderMethod != m_renderMethod)
  {
    CLog::Log(LOGDEBUG, "CLinuxRendererA10: Reorder drawpoints due to method change from %i to %i", m_oldRenderMethod, m_renderMethod);
    ReorderDrawPoints();
    m_oldRenderMethod = m_renderMethod;
  }
}


unsigned int CLinuxRendererA10::GetProcessorSize()
{
  return (m_format == RENDER_FMT_A10BUF) ?  
    0 : CLinuxRendererGLES::GetProcessorSize();
}


void CLinuxRendererA10::AddProcessor(A10VLQueueItem *buffer)
{
  YUVBUFFER &buf = m_buffers[NextYV12Texture()];

  buf.userData = buffer;
}



/*
 * Video layer functions
 */

static int             g_hfb = -1;
static int             g_hdisp = -1;
static int             g_screenid = 0;
static int             g_syslayer = 0x64;
static int             g_hlayer = 0;
static unsigned        g_width;
static unsigned        g_height;
static CRect           g_srcRect;
static CRect           g_dstRect;
static int             g_lastnr;
static int             g_decnr;
static int             g_wridx;
static int             g_rdidx;
static A10VLQueueItem  g_dispq[DISPQS];
static pthread_mutex_t g_dispq_mutex;

static bool A10VLBlueScreenFix()
{
  int                 hlayer;
  __disp_layer_info_t layera;
  unsigned long       args[4];

  args[0] = g_screenid;
  args[1] = DISP_LAYER_WORK_MODE_SCALER;
  args[2] = 0;
  args[3] = 0;
  hlayer = ioctl(g_hdisp, DISP_CMD_LAYER_REQUEST, args);
  if (hlayer <= 0)
  {
    CLog::Log(LOGERROR, "A10: DISP_CMD_LAYER_REQUEST failed.\n");
    return false;
  }

  args[0] = g_screenid;
  args[1] = hlayer;
  args[2] = (unsigned long) &layera;
  args[3] = 0;
  ioctl(g_hdisp, DISP_CMD_LAYER_GET_PARA, args);

  layera.mode      = DISP_LAYER_WORK_MODE_SCALER;
  layera.fb.mode   = DISP_MOD_MB_UV_COMBINED;
  layera.fb.format = DISP_FORMAT_YUV420;
  layera.fb.seq    = DISP_SEQ_UVUV;
  ioctl(g_hdisp, DISP_CMD_LAYER_SET_PARA, args);

  args[0] = g_screenid;
  args[1] = hlayer;
  args[2] = 0;
  args[3] = 0;
  ioctl(g_hdisp, DISP_CMD_LAYER_RELEASE, args);

  return true;
}

bool A10VLInit(int &width, int &height, double &refreshRate)
{
  unsigned long       args[4];
  __disp_layer_info_t layera;
  unsigned int        i;

  pthread_mutex_init(&g_dispq_mutex, NULL);

  g_hfb = open("/dev/fb0", O_RDWR);

  g_hdisp = open("/dev/disp", O_RDWR);
  if (g_hdisp == -1)
  {
    CLog::Log(LOGERROR, "A10: open /dev/disp failed. (%d)", errno);
    return false;
  }

  // tell /dev/disp the API version we are using
  args[0] = SUNXI_DISP_VERSION;
  args[1] = 0;
  args[2] = 0;
  args[3] = 0;
  i = ioctl(g_hdisp, DISP_CMD_VERSION, args);
  CLog::Log(LOGNOTICE, "A10: display API version is: %d.%d\n",
            SUNXI_DISP_VERSION_MAJOR_GET(i),
            SUNXI_DISP_VERSION_MINOR_GET(i));

  args[0] = g_screenid;
  args[1] = 0;
  args[2] = 0;
  args[3] = 0;
  width  = g_width  = ioctl(g_hdisp, DISP_CMD_SCN_GET_WIDTH , args);
  height = g_height = ioctl(g_hdisp, DISP_CMD_SCN_GET_HEIGHT, args);

  i = ioctl(g_hdisp, DISP_CMD_HDMI_GET_MODE, args);

  switch(i)
  {
  case DISP_TV_MOD_720P_50HZ:
  case DISP_TV_MOD_1080I_50HZ:
  case DISP_TV_MOD_1080P_50HZ:
    refreshRate = 50.0;
    break;
  case DISP_TV_MOD_720P_60HZ:
  case DISP_TV_MOD_1080I_60HZ:
  case DISP_TV_MOD_1080P_60HZ:
    refreshRate = 60.0;
    break;
  case DISP_TV_MOD_1080P_24HZ:
    refreshRate = 24.0;
    break;
  default:
    CLog::Log(LOGERROR, "A10: display mode %d is unknown. Assume refreh rate 60Hz\n", i);
    refreshRate = 60.0;
    break;
  }

  if ((g_height > 720) && (getenv("A10AB") == NULL))
  {
    //set workmode scaler (system layer)
    args[0] = g_screenid;
    args[1] = g_syslayer;
    args[2] = (unsigned long) (&layera);
    args[3] = 0;
    ioctl(g_hdisp, DISP_CMD_LAYER_GET_PARA, args);
    layera.mode = DISP_LAYER_WORK_MODE_SCALER;
    args[0] = g_screenid;
    args[1] = g_syslayer;
    args[2] = (unsigned long) (&layera);
    args[3] = 0;
    ioctl(g_hdisp, DISP_CMD_LAYER_SET_PARA, args);
  }
  else
  {
    //set workmode normal (system layer)
    args[0] = g_screenid;
    args[1] = g_syslayer;
    args[2] = (unsigned long) (&layera);
    args[3] = 0;
    ioctl(g_hdisp, DISP_CMD_LAYER_GET_PARA, args);
    //source window information
    layera.src_win.x      = 0;
    layera.src_win.y      = 0;
    layera.src_win.width  = g_width;
    layera.src_win.height = g_height;
    //screen window information
    layera.scn_win.x      = 0;
    layera.scn_win.y      = 0;
    layera.scn_win.width  = g_width;
    layera.scn_win.height = g_height;
    layera.mode = DISP_LAYER_WORK_MODE_NORMAL;
    args[0] = g_screenid;
    args[1] = g_syslayer;
    args[2] = (unsigned long) (&layera);
    args[3] = 0;
    ioctl(g_hdisp, DISP_CMD_LAYER_SET_PARA, args);

  }

  for (i = 0x65; i <= 0x67; i++)
  {
    //release possibly lost allocated layers
    args[0] = g_screenid;
    args[1] = i;
    args[2] = 0;
    args[3] = 0;
    ioctl(g_hdisp, DISP_CMD_LAYER_RELEASE, args);
  }

  // Hack: avoid blue picture background
  if (!A10VLBlueScreenFix())
    return false;

  args[0] = g_screenid;
  args[1] = DISP_LAYER_WORK_MODE_SCALER;
  args[2] = 0;
  args[3] = 0;
  g_hlayer = ioctl(g_hdisp, DISP_CMD_LAYER_REQUEST, args);
  if (g_hlayer <= 0)
  {
    g_hlayer = 0;
    CLog::Log(LOGERROR, "A10: DISP_CMD_LAYER_REQUEST failed.\n");
    return false;
  }

  memset(&g_srcRect, 0, sizeof(g_srcRect));
  memset(&g_dstRect, 0, sizeof(g_dstRect));

  g_lastnr = -1;
  g_decnr  = 0;
  g_rdidx  = 0;
  g_wridx  = 0;

  for (i = 0; i < DISPQS; i++)
    g_dispq[i].pict.id = -1;

  return true;
}

void A10VLExit()
{
  unsigned long args[4];

  if (g_hlayer)
  {
    //stop video
    args[0] = g_screenid;
    args[1] = g_hlayer;
    args[2] = 0;
    args[3] = 0;
    ioctl(g_hdisp, DISP_CMD_VIDEO_STOP, args);

    //close layer
    args[0] = g_screenid;
    args[1] = g_hlayer;
    args[2] = 0;
    args[3] = 0;
    ioctl(g_hdisp, DISP_CMD_LAYER_CLOSE, args);

    //release layer
    args[0] = g_screenid;
    args[1] = g_hlayer;
    args[2] = 0;
    args[3] = 0;
    ioctl(g_hdisp, DISP_CMD_LAYER_RELEASE, args);
    g_hlayer = 0;
  }
  if (g_hdisp != -1)
  {
    close(g_hdisp);
    g_hdisp = -1;
  }
  if (g_hfb != -1)
  {
    close(g_hfb);
    g_hfb = -1;
  }
}

void A10VLHide()
{
  unsigned long args[4];

  if (g_hlayer)
  {
    //stop video
    args[0] = g_screenid;
    args[1] = g_hlayer;
    args[2] = 0;
    args[3] = 0;
    ioctl(g_hdisp, DISP_CMD_VIDEO_STOP, args);

    //close layer
    args[0] = g_screenid;
    args[1] = g_hlayer;
    args[2] = 0;
    args[3] = 0;
    ioctl(g_hdisp, DISP_CMD_LAYER_CLOSE, args);
  }

  memset(&g_srcRect, 0, sizeof(g_srcRect));
  memset(&g_dstRect, 0, sizeof(g_dstRect));
}

#define FBIO_WAITFORVSYNC _IOW('F', 0x20, u32)

void A10VLWaitVSYNC()
{
  //ioctl(g_hfb, FBIO_WAITFORVSYNC, NULL);
}

A10VLQueueItem *A10VLPutQueue(A10VLCALLBACK     callback,
                              void             *callbackpriv,
                              void             *pictpriv,
                              cedarv_picture_t &pict)
{
  A10VLQueueItem *pRet;

  pthread_mutex_lock(&g_dispq_mutex);

  pRet = &g_dispq[g_wridx];

  pRet->decnr        = g_decnr++;
  pRet->callback     = callback;
  pRet->callbackpriv = callbackpriv;
  pRet->pictpriv     = pictpriv;
  pRet->pict         = pict;

  g_wridx++;
  if (g_wridx >= DISPQS)
    g_wridx = 0;

  pthread_mutex_unlock(&g_dispq_mutex);

  return pRet;
}

static void A10VLFreeQueueItem(A10VLQueueItem *pItem)
{
  if ((int)pItem->pict.id != -1)
  {
    if (pItem->callback)
      pItem->callback(pItem->callbackpriv, pItem->pictpriv, pItem->pict);
    pItem->pict.id = -1;
  }
}

void A10VLFreeQueue()
{
  int i;

  pthread_mutex_lock(&g_dispq_mutex);

  for (i = 0; i < DISPQS; i++)
    A10VLFreeQueueItem(&g_dispq[i]);

  pthread_mutex_unlock(&g_dispq_mutex);
}

void A10VLDisplayQueueItem(A10VLQueueItem *pItem, CRect &srcRect, CRect &dstRect)
{
  int i;
  int curnr;

  pthread_mutex_lock(&g_dispq_mutex);

  if (!pItem || (pItem->pict.id == -1) || (g_lastnr == pItem->decnr))
  {
    pthread_mutex_unlock(&g_dispq_mutex);
    return;
  }

  curnr = A10VLDisplayPicture(pItem->pict, pItem->decnr, srcRect, dstRect);

  if (curnr != g_lastnr)
  {
    //free older frames, displayed or not
    for (i = 0; i < DISPQS; i++)
    {
      if(g_dispq[g_rdidx].decnr < curnr)
      {
        A10VLFreeQueueItem(&g_dispq[g_rdidx]);

        g_rdidx++;
        if (g_rdidx >= DISPQS)
          g_rdidx = 0;

      } else break;
    }

  }

  g_lastnr = curnr;

  pthread_mutex_unlock(&g_dispq_mutex);
}

int A10VLDisplayPicture(cedarv_picture_t &picture,
                        int               refnr,
                        CRect            &srcRect,
                        CRect            &dstRect)
{
  unsigned long       args[4];
  __disp_layer_info_t layera;
  __disp_video_fb_t   frmbuf;
  __disp_colorkey_t   colorkey;

  memset(&frmbuf, 0, sizeof(__disp_video_fb_t));
  frmbuf.id              = refnr;
  frmbuf.interlace       = picture.is_progressive? 0 : 1;
  frmbuf.top_field_first = picture.top_field_first;
  //frmbuf.frame_rate      = picture.frame_rate;
#ifdef CEDARV_FRAME_HAS_PHY_ADDR
  frmbuf.addr[0]         = (u32)picture.y;
  frmbuf.addr[1]         = (u32)picture.u;
#else
  frmbuf.addr[0]         = mem_get_phy_addr((u32)picture.y);
  frmbuf.addr[1]         = mem_get_phy_addr((u32)picture.u);
#endif

  if ((g_srcRect != srcRect) || (g_dstRect != dstRect))
  {
    args[0] = g_screenid;
    args[1] = g_hlayer;
    args[2] = (unsigned long) (&layera);
    args[3] = 0;
    ioctl(g_hdisp, DISP_CMD_LAYER_GET_PARA, args);
    //set video layer attribute
    layera.mode          = DISP_LAYER_WORK_MODE_SCALER;
    layera.b_from_screen = 0; //what is this? if enabled all is black
    layera.pipe          = 1;
    //use alpha blend
    layera.alpha_en      = 0;
    layera.alpha_val     = 0xff;
    layera.ck_enable     = 0;
    layera.b_trd_out     = 0;
    layera.out_trd_mode  = (__disp_3d_out_mode_t)0;
    //frame buffer pst and size information
    if (picture.display_height < 720)
    {
      layera.fb.cs_mode = DISP_BT601;
    }
    else
    {
      layera.fb.cs_mode = DISP_BT709;
    }
    layera.fb.mode        = DISP_MOD_MB_UV_COMBINED;
    layera.fb.format      = picture.pixel_format == CEDARV_PIXEL_FORMAT_AW_YUV422 ? DISP_FORMAT_YUV422 : DISP_FORMAT_YUV420;
    layera.fb.br_swap     = 0;
    layera.fb.seq         = DISP_SEQ_UVUV;
    layera.fb.addr[0]     = frmbuf.addr[0];
    layera.fb.addr[1]     = frmbuf.addr[1];
    layera.fb.b_trd_src   = 0;
    layera.fb.trd_mode    = (__disp_3d_src_mode_t)0;
    layera.fb.size.width  = picture.display_width;
    layera.fb.size.height = picture.display_height;
    //source window information
    layera.src_win.x      = lrint(srcRect.x1);
    layera.src_win.y      = lrint(srcRect.y1);
    layera.src_win.width  = lrint(srcRect.x2-srcRect.x1);
    layera.src_win.height = lrint(srcRect.y2-srcRect.y1);
    //screen window information
    layera.scn_win.x      = lrint(dstRect.x1);
    layera.scn_win.y      = lrint(dstRect.y1);
    layera.scn_win.width  = lrint(dstRect.x2-dstRect.x1);
    layera.scn_win.height = lrint(dstRect.y2-dstRect.y1);

    CLog::Log(LOGDEBUG, "A10: srcRect=(%lf,%lf)-(%lf,%lf)\n", srcRect.x1, srcRect.y1, srcRect.x2, srcRect.y2);
    CLog::Log(LOGDEBUG, "A10: dstRect=(%lf,%lf)-(%lf,%lf)\n", dstRect.x1, dstRect.y1, dstRect.x2, dstRect.y2);

    if (    (layera.scn_win.x < 0)
         || (layera.scn_win.y < 0)
         || (layera.scn_win.width  > g_width)
         || (layera.scn_win.height > g_height)    )
    {
      double xzoom, yzoom;

      //TODO: this calculation is against the display fullscreen dimensions,
      //but should be against the fullscreen area of xbmc

      xzoom = (dstRect.x2 - dstRect.x1) / (srcRect.x2 - srcRect.x1);
      yzoom = (dstRect.y2 - dstRect.y1) / (srcRect.y2 - srcRect.x1);

      if (layera.scn_win.x < 0)
      {
        layera.src_win.x -= layera.scn_win.x / xzoom;
        layera.scn_win.x = 0;
      }
      if (layera.scn_win.width > g_width)
      {
        layera.src_win.width -= (layera.scn_win.width - g_width) / xzoom;
        layera.scn_win.width = g_width;
      }

      if (layera.scn_win.y < 0)
      {
        layera.src_win.y -= layera.scn_win.y / yzoom;
        layera.scn_win.y = 0;
      }
      if (layera.scn_win.height > g_height)
      {
        layera.src_win.height -= (layera.scn_win.height - g_height) / yzoom;
        layera.scn_win.height = g_height;
      }
    }

    args[0] = g_screenid;
    args[1] = g_hlayer;
    args[2] = (unsigned long)&layera;
    args[3] = 0;
    if(ioctl(g_hdisp, DISP_CMD_LAYER_SET_PARA, args))
      CLog::Log(LOGERROR, "A10: DISP_CMD_LAYER_SET_PARA failed.\n");

    //open layer
    args[0] = g_screenid;
    args[1] = g_hlayer;
    args[2] = 0;
    args[3] = 0;
    if (ioctl(g_hdisp, DISP_CMD_LAYER_OPEN, args))
      CLog::Log(LOGERROR, "A10: DISP_CMD_LAYER_OPEN failed.\n");

    //put behind system layer
    args[0] = g_screenid;
    args[1] = g_hlayer;
    args[2] = 0;
    args[3] = 0;
    if (ioctl(g_hdisp, DISP_CMD_LAYER_BOTTOM, args))
      CLog::Log(LOGERROR, "A10: DISP_CMD_LAYER_BOTTOM failed.\n");

    //turn off colorkey (system layer)
    args[0] = g_screenid;
    args[1] = g_syslayer;
    args[2] = 0;
    args[3] = 0;
    if (ioctl(g_hdisp, DISP_CMD_LAYER_CK_OFF, args))
      CLog::Log(LOGERROR, "A10: DISP_CMD_LAYER_CK_OFF failed.\n");

    if ((g_height > 720) && (getenv("A10AB") == NULL))
    {
      //no tearing at the cost off alpha blending...

      //set colorkey
      colorkey.ck_min.alpha = 0;
      colorkey.ck_min.red   = 1;
      colorkey.ck_min.green = 2;
      colorkey.ck_min.blue  = 3;
      colorkey.ck_max = colorkey.ck_min;
      colorkey.ck_max.alpha = 255;
      colorkey.red_match_rule   = 2;
      colorkey.green_match_rule = 2;
      colorkey.blue_match_rule  = 2;

      args[0] = g_screenid;
      args[1] = (unsigned long)&colorkey;
      args[2] = 0;
      args[3] = 0;
      if (ioctl(g_hdisp, DISP_CMD_SET_COLORKEY, args))
        CLog::Log(LOGERROR, "A10: DISP_CMD_SET_COLORKEY failed.\n");

      //turn on colorkey
      args[0] = g_screenid;
      args[1] = g_hlayer;
      args[2] = 0;
      args[3] = 0;
      if (ioctl(g_hdisp, DISP_CMD_LAYER_CK_ON, args))
        CLog::Log(LOGERROR, "A10: DISP_CMD_LAYER_CK_ON failed.\n");

      //turn on global alpha (system layer)
      args[0] = g_screenid;
      args[1] = g_syslayer;
      args[2] = 0;
      args[3] = 0;
      if (ioctl(g_hdisp, DISP_CMD_LAYER_ALPHA_ON, args))
        CLog::Log(LOGERROR, "A10: DISP_CMD_LAYER_ALPHA_ON failed.\n");
    }
    else
    {
      //turn off global alpha (system layer)
      args[0] = g_screenid;
      args[1] = g_syslayer;
      args[2] = 0;
      args[3] = 0;
      if (ioctl(g_hdisp, DISP_CMD_LAYER_ALPHA_OFF, args))
        CLog::Log(LOGERROR, "A10: DISP_CMD_LAYER_ALPHA_OFF failed.\n");
    }

    //enable vpp
    args[0] = g_screenid;
    args[1] = g_hlayer;
    args[2] = 0;
    args[3] = 0;
    if (ioctl(g_hdisp, DISP_CMD_LAYER_VPP_ON, args))
      CLog::Log(LOGERROR, "A10: DISP_CMD_LAYER_VPP_ON failed.\n");

    //enable enhance
    args[0] = g_screenid;
    args[1] = g_hlayer;
    args[2] = 0;
    args[3] = 0;
    if (ioctl(g_hdisp, DISP_CMD_LAYER_ENHANCE_ON, args))
      CLog::Log(LOGERROR, "A10: DISP_CMD_LAYER_ENHANCE_ON failed.\n");

    //start video
    args[0] = g_screenid;
    args[1] = g_hlayer;
    args[2] = 0;
    args[3] = 0;
    if (ioctl(g_hdisp, DISP_CMD_VIDEO_START, args))
      CLog::Log(LOGERROR, "A10: DISP_CMD_VIDEO_START failed.\n");

    g_srcRect = srcRect;
    g_dstRect = dstRect;
  }

  args[0] = g_screenid;
  args[1] = g_hlayer;
  args[2] = (unsigned long)&frmbuf;
  args[3] = 0;
  if (ioctl(g_hdisp, DISP_CMD_VIDEO_SET_FB, args))
    CLog::Log(LOGERROR, "A10: DISP_CMD_VIDEO_SET_FB failed.\n");

  //CLog::Log(LOGDEBUG, "A10: render %d\n", buffer->picture.id);

  args[0] = g_screenid;
  args[1] = g_hlayer;
  args[2] = 0;
  args[3] = 0;
  return ioctl(g_hdisp, DISP_CMD_VIDEO_GET_FRAME_ID, args);
}

#endif
