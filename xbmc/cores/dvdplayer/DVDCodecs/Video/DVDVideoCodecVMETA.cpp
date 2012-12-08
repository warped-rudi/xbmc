/*
 *      Copyright (C) 2012 Team XBMC
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
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#if (defined HAVE_CONFIG_H) && (!defined WIN32)
  #include "config.h"
#elif defined(_WIN32)
#include "system.h"
#endif

#if defined(HAS_MARVELL_DOVE)
#include "DVDStreamInfo.h"
#include "DVDVideoCodecVMETA.h"
#include "DynamicDll.h"

#include "utils/log.h"
#include "linux/XMemUtils.h"
#include "DVDClock.h"

#include <sys/time.h>
#include <inttypes.h>

#ifdef CLASSNAME
#undef CLASSNAME
#endif
#define CLASSNAME "CDVDVideoCodecVMETA"

#include "utils/BitstreamConverter.h"

#define STREAM_VDECBUF_SIZE   (2048*1024)  //must equal to or greater than 64k and multiple of 128, because of vMeta limitted
#define STREAM_VDECBUF_NUM    16
#define STREAM_PICBUF_NUM     41
#define VMETA_QUEUE_THRESHOLD 20

CDVDVideoCodecVMETA::CDVDVideoCodecVMETA()
{
  m_is_open           = false;
  m_extradata         = NULL;
  m_extrasize         = 0;
  m_converter         = NULL;
  m_video_convert     = false;
  m_video_codec_name  = "";
  m_Frames            = 0;

  m_DllMiscGen        = new DllLibMiscGen();
  m_DllVMETA          = new DllLibVMETA();

  m_pDecState         = NULL;
  m_pCbTable          = NULL;
  m_itime_inc_bits    = -1;
  m_low_delay         = -1;
  m_codec_species     = -1;
}

CDVDVideoCodecVMETA::~CDVDVideoCodecVMETA()
{
  if (m_is_open)
    Dispose();
}

bool CDVDVideoCodecVMETA::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options)
{
  if(!m_DllVMETA->Load() || !m_DllMiscGen->Load())
    return false;

  bool bSendCodecConfig = false;

  m_decoded_width   = hints.width;
  m_decoded_height  = hints.height;
  m_picture_width   = m_decoded_width;
  m_picture_height  = m_decoded_height;

  if(!m_decoded_width || !m_decoded_height)
    return false;

  m_converter     = new CBitstreamConverter();
  m_video_convert = m_converter->Open(hints.codec, (uint8_t *)hints.extradata, hints.extrasize, true);

  if(m_video_convert)
  {
    if(m_converter->GetExtraData() != NULL && m_converter->GetExtraSize() > 0)
    {
      m_extrasize = m_converter->GetExtraSize();
      m_extradata = (uint8_t *)malloc(m_extrasize);
      memcpy(m_extradata, m_converter->GetExtraData(), m_converter->GetExtraSize());
    }
  }
  else
  {
    if(hints.extrasize > 0 && hints.extradata != NULL)
    {
      m_extrasize = hints.extrasize;
      m_extradata = (uint8_t *)malloc(m_extrasize);
      memcpy(m_extradata, hints.extradata, hints.extrasize);
    }
  }

  memset(&m_VDecParSet, 0, sizeof(IppVmetaDecParSet));
  memset(&m_VDecInfo, 0, sizeof(IppVmetaDecInfo));

  switch (hints.codec)
  {
    case CODEC_ID_H264:
    {
      switch(hints.profile)
      {
        case FF_PROFILE_H264_BASELINE:
          m_video_codec_name = "vmeta-h264";
          break;
        case FF_PROFILE_H264_MAIN:
          m_video_codec_name = "vmeta-h264";
          break;
        case FF_PROFILE_H264_HIGH:
          m_video_codec_name = "vmeta-h264";
          break;
        case FF_PROFILE_UNKNOWN:
          m_video_codec_name = "vmeta-h264";
          break;
        default:
          m_video_codec_name = "vmeta-h264";
          break;
      }

      m_VDecParSet.strm_fmt = IPP_VIDEO_STRM_FMT_H264;
    }
    break;

    case CODEC_ID_MPEG4:
      m_VDecParSet.strm_fmt = IPP_VIDEO_STRM_FMT_MPG4;
      m_video_codec_name = "vmeta-mpeg4";
      bSendCodecConfig = true;
      m_codec_species = 2; /* TODO - Better detect xvid, and divx versions */
      break;

    case CODEC_ID_MPEG2VIDEO:
      m_VDecParSet.strm_fmt = IPP_VIDEO_STRM_FMT_MPG2;
      m_video_codec_name = "vmeta-mpeg2";
      bSendCodecConfig = true;
      break;

    case CODEC_ID_H263:
      m_VDecParSet.strm_fmt = IPP_VIDEO_STRM_FMT_H263;
      m_video_codec_name = "vmeta-h263";
      bSendCodecConfig = true;
      break;

    case CODEC_ID_VC1:
      m_VDecParSet.strm_fmt = IPP_VIDEO_STRM_FMT_VC1;
      m_video_codec_name = "vmeta-vc1";
      bSendCodecConfig = true;
      break;
    default:
      CLog::Log(LOGDEBUG, "%s::%s CodecID 0x%08x not supported by VMETA decoder\n", CLASSNAME, __func__, hints.codec);
      return false;
    break;
  }

  m_VDecParSet.opt_fmt = IPP_YCbCr422I;

  IppCodecStatus ret;

  if(m_DllMiscGen->miscInitGeneralCallbackTable(&m_pCbTable) != 0)
  {
    CLog::Log(LOGERROR, "%s::%s Error : miscInitGeneralCallbackTable\n", CLASSNAME, __func__);
    Dispose();
    return false;
  }

  ret = m_DllVMETA->DecoderInitAlloc_Vmeta(&m_VDecParSet, m_pCbTable, &m_pDecState);
  if(ret != IPP_STATUS_NOERR)
  {
    CLog::Log(LOGERROR, "%s::%s Error : DecoderInitAlloc_Vmeta\n", CLASSNAME, __func__);
    Dispose();
    return false;
  }

  for (size_t i = 0; i < STREAM_VDECBUF_NUM; i++)
  {
    IppVmetaBitstream *pStream = NULL;
    pStream = (IppVmetaBitstream *)malloc(sizeof(IppVmetaBitstream));
    memset(pStream, 0, sizeof(IppVmetaBitstream));

    /*
    pStream->nBufSize = STREAM_VDECBUF_SIZE;
    pStream->pBuf = (Ipp8u*)m_DllVMETA->vdec_os_api_dma_alloc(pStream->nBufSize, VMETA_STRM_BUF_ALIGN, &pStream->nPhyAddr);
    if(pStream->pBuf == NULL)
    {
      Dispose();
      return false;
    }
    */

    m_input_buffers.push_back(pStream);
    m_input_available.push(pStream);
  }

  for (size_t i = 0; i < STREAM_PICBUF_NUM; i++)
  {
    IppVmetaPicture *pPicture = NULL;
    pPicture = (IppVmetaPicture *)malloc(sizeof(IppVmetaPicture));
    memset(pPicture, 0, sizeof(IppVmetaPicture));
    pPicture->pUsrData0 = (void *)i;

    m_output_buffers.push_back(pPicture);
    m_output_available.push(pPicture);
  }

  m_Frames        = 0;
  m_is_open       = true;

  if(bSendCodecConfig)
    SendCodecConfig();

  CLog::Log(LOGDEBUG, "%s::%s - VMETA Decoder opened with codec : %s [%dx%d]", CLASSNAME, __func__,
            m_video_codec_name.c_str(), m_decoded_width, m_decoded_height);

  return true;
}

void CDVDVideoCodecVMETA::Dispose()
{
  m_is_open       = false;

  if(m_extradata)
    free(m_extradata);
  m_extradata = NULL;
  m_extrasize = 0;

  if(m_converter)
    delete m_converter;
  m_converter         = NULL;
  m_video_convert     = false;
  m_video_codec_name  = "";

  if(m_pDecState)
  {
    m_DllVMETA->DecodeSendCmd_Vmeta(IPPVC_STOP_DECODE_STREAM, NULL, NULL, m_pDecState);

    Reset();

    m_DllVMETA->DecoderFree_Vmeta(&m_pDecState);
  }
  m_pDecState = NULL;

  if(m_pCbTable)
  {
    m_DllMiscGen->miscFreeGeneralCallbackTable(&m_pCbTable);
  }
  m_pCbTable = NULL;

  for (size_t i = 0; i < m_input_buffers.size(); i++)
  {
    IppVmetaBitstream *pStream = m_input_buffers[i];
    if(pStream->pBuf)
      m_DllVMETA->vdec_os_api_dma_free(pStream->pBuf);
    free(m_input_buffers[i]);
  }

  m_input_buffers.clear();

  while(!m_input_available.empty())
    m_input_available.pop();

  for (size_t i = 0; i < m_output_buffers.size(); i++)
  {
    IppVmetaPicture *pPicture = m_output_buffers[i];
    if(pPicture->pBuf)
      m_DllVMETA->vdec_os_api_dma_free(pPicture->pBuf);
    free(m_output_buffers[i]);
  }

  m_output_buffers.clear();

  while(!m_output_available.empty())
    m_output_available.pop();

  m_Frames        = 0;

  m_DllVMETA->Unload();
  m_DllMiscGen->Unload();

  delete m_DllMiscGen;
  delete m_DllVMETA;
}

void CDVDVideoCodecVMETA::SetDropState(bool bDrop)
{
  m_drop_state = bDrop;
}

IppCodecStatus CDVDVideoCodecVMETA::SendCodecConfig()
{
  IppCodecStatus retCodec;

  if(m_extradata == NULL || m_extrasize == 0 || m_pDecState == NULL || m_input_available.empty())
    return IPP_STATUS_ERR;

  IppVmetaBitstream *pStream = m_input_available.front();

  if(pStream->pBuf)
    m_DllVMETA->vdec_os_api_dma_free(pStream->pBuf);

  pStream->nBufSize = ((m_extrasize + 65*1024) + 127) & ~127;
  pStream->nDataLen = m_extrasize;
  pStream->pBuf = (Ipp8u*)m_DllVMETA->vdec_os_api_dma_alloc(pStream->nBufSize, VMETA_STRM_BUF_ALIGN, &pStream->nPhyAddr);
  pStream->nFlag = IPP_VMETA_STRM_BUF_END_OF_UNIT;

  if(!pStream->pBuf)
  {
    printf("%s::%s Error : Allocate streambuffer\n", CLASSNAME, __func__);
    return IPP_STATUS_ERR;
  }

  memcpy(pStream->pBuf, m_extradata, m_extrasize);

  retCodec = m_DllVMETA->DecoderPushBuffer_Vmeta(IPP_VMETA_BUF_TYPE_STRM, (void *)pStream, m_pDecState);

  if(retCodec != IPP_STATUS_NOERR)
  {
    printf("%s::%s Error : Push streambuffer\n", CLASSNAME, __func__);
    return IPP_STATUS_ERR;
  }

  m_input_available.pop();

  return IPP_STATUS_NOERR;
}

IppCodecStatus CDVDVideoCodecVMETA::DecodeInternal(uint8_t *pData, unsigned int *iSize, double dts, double pts)
{
  void *pPopTmp;
  IppVmetaBitstream *pStream;
  IppVmetaPicture *pPicture;
  IppCodecStatus retCodec;

  retCodec = m_DllVMETA->DecodeFrame_Vmeta(&m_VDecInfo, m_pDecState);

  //printf("m_input_available.size() %d m_output_available.size() %d m_output_ready.size() %d\n",
  //       m_input_available.size(), m_output_available.size(), m_output_ready.size());

  switch(retCodec)
  {
    case IPP_STATUS_WAIT_FOR_EVENT:
      //printf("IPP_STATUS_WAIT_FOR_EVENT\n");
      break;
    case IPP_STATUS_NEED_INPUT:
      if(m_input_available.empty())
         CLog::Log(LOGDEBUG, "IPP_STATUS_NEED_INPUT no free input buffers\n");
      if(!m_input_available.empty() && *iSize != 0)
      {
        //printf("IPP_STATUS_NEED_INPUT\n");
        IppVmetaBitstream *pStream = m_input_available.front();

        if(pStream->pBuf)
          m_DllVMETA->vdec_os_api_dma_free(pStream->pBuf);

        // make sure we allocate enough space for padding. not sure how many the decoder needs. 65*1024 seems fair enough.
        pStream->nBufSize = ((*iSize + 65*1024) + 127) & ~127;
        pStream->pBuf = (Ipp8u*)m_DllVMETA->vdec_os_api_dma_alloc_cached(pStream->nBufSize, VMETA_STRM_BUF_ALIGN , &pStream->nPhyAddr);
        if(!pStream->pBuf)
        {
          CLog::Log(LOGERROR, "%s::%s Error : Allocate streambuffer\n", CLASSNAME, __func__);
          return IPP_STATUS_ERR;
        }

        // the vmeta decoder needs a padding of 0x88 at the end of a frame
        pStream->nDataLen = *iSize;
        pStream->nFlag = IPP_VMETA_STRM_BUF_END_OF_UNIT;
        memset(pStream->pBuf, 0x88, pStream->nBufSize);
        memcpy(pStream->pBuf, pData, *iSize);

        retCodec = m_DllVMETA->DecoderPushBuffer_Vmeta(IPP_VMETA_BUF_TYPE_STRM, (void *)pStream, m_pDecState);

        if(retCodec != IPP_STATUS_NOERR)
        {
          CLog::Log(LOGERROR, "%s::%s Error : Push streambuffer\n", CLASSNAME, __func__);
          return IPP_STATUS_ERR;
        }

        m_input_available.pop();
        *iSize = 0;
      }
      break;
    case IPP_STATUS_END_OF_STREAM:
      //printf("IPP_STATUS_END_OF_STREAM\n");
      break;
    case IPP_STATUS_NEED_OUTPUT_BUF:
      if(m_output_available.empty())
        CLog::Log(LOGDEBUG, "IPP_STATUS_FRAME_COMPLETE no free pictures buffers\n");
      //printf("IPP_STATUS_NEED_OUTPUT_BUF\n");
      if(!m_output_available.empty())
      {
        IppVmetaPicture *pPicture = m_output_available.front();
        m_output_available.pop();
        if(!pPicture)
          return IPP_STATUS_ERR;

        if(m_VDecInfo.seq_info.dis_buf_size >  pPicture->nBufSize)
        {
          if(pPicture->pBuf)
            m_DllVMETA->vdec_os_api_dma_free(pPicture->pBuf);

          pPicture->pBuf = NULL;
          pPicture->pBuf = (Ipp8u*)m_DllVMETA->vdec_os_api_dma_alloc_cached(m_VDecInfo.seq_info.dis_buf_size, VMETA_DIS_BUF_ALIGN, &(pPicture->nPhyAddr));
          pPicture->nBufSize = m_VDecInfo.seq_info.dis_buf_size;
          //printf("vdec_os_api_dma_alloc pPicture->pBuf 0x%08x nr %d\n", (unsigned int)pPicture->pBuf, (int)pPicture->pUsrData0);
        }
        if(pPicture->pBuf == NULL)
        {
          CLog::Log(LOGERROR, "%s::%s Error : Allocate picture\n", CLASSNAME, __func__);
          m_output_available.push(pPicture);
          return IPP_STATUS_ERR;
        }
        m_DllVMETA->DecoderPushBuffer_Vmeta(IPP_VMETA_BUF_TYPE_PIC, (void*)pPicture, m_pDecState);
      }
      break;
    case IPP_STATUS_RETURN_INPUT_BUF:
      //printf("IPP_STATUS_RETURN_INPUT_BUF\n");
      for(;;)
      {
        m_DllVMETA->DecoderPopBuffer_Vmeta(IPP_VMETA_BUF_TYPE_STRM, &pPopTmp, m_pDecState);
        pStream = (IppVmetaBitstream*)pPopTmp;
        if(pStream == NULL)
          break;
        m_input_available.push(pStream);
      }
      break;
    case IPP_STATUS_FRAME_COMPLETE:
      for(;;)
      {
        m_DllVMETA->DecoderPopBuffer_Vmeta(IPP_VMETA_BUF_TYPE_STRM, &pPopTmp, m_pDecState);
        pStream = (IppVmetaBitstream*)pPopTmp;
        if(pStream == NULL)
          break;
        m_input_available.push(pStream);
      }
      //printf("IPP_STATUS_FRAME_COMPLETE\n");
      {
        // The gstreamer plugins says this is needed for DOVE
        IppCodecStatus suspendRet;
        if(m_DllVMETA->vdec_os_api_suspend_check())
        {
          suspendRet = m_DllVMETA->DecodeSendCmd_Vmeta(IPPVC_PAUSE, NULL, NULL, m_pDecState);
          if(suspendRet == IPP_STATUS_NOERR)
          {
            m_DllVMETA->vdec_os_api_suspend_ready();
            suspendRet = m_DllVMETA->DecodeSendCmd_Vmeta(IPPVC_RESUME, NULL, NULL, m_pDecState);
            if(suspendRet != IPP_STATUS_NOERR)
            {
              CLog::Log(LOGERROR, "%s::%s resume failed\n", CLASSNAME, __func__);
            }
          }
          else
          {
            CLog::Log(LOGERROR, "%s::%s pause failed\n", CLASSNAME, __func__);
          }
        }

        m_DllVMETA->DecoderPopBuffer_Vmeta(IPP_VMETA_BUF_TYPE_PIC, &pPopTmp, m_pDecState);
        pPicture = (IppVmetaPicture *)pPopTmp;
        if(pPicture)
        {
          pPicture->pUsrData1 = (void*)m_Frames;
          m_output_ready.push(pPicture);

          m_Frames++;
        }
      }
      break;
    case IPP_STATUS_NEW_VIDEO_SEQ:
      if(m_VDecInfo.seq_info.picROI.width != 0 && m_VDecInfo.seq_info.picROI.height != 0)
      {
        m_picture_width   = m_VDecInfo.seq_info.picROI.width;
        m_picture_height  = m_VDecInfo.seq_info.picROI.height;
        CLog::Log(LOGDEBUG, "%s::%s New sequence picture dimension [%dx%d]\n",
              CLASSNAME, __func__, m_picture_width, m_picture_height);
      }
      for(;;)
      {
        m_DllVMETA->DecoderPopBuffer_Vmeta(IPP_VMETA_BUF_TYPE_PIC, &pPopTmp, m_pDecState);
        pPicture = (IppVmetaPicture *)pPopTmp;
        if(pPicture == NULL)
          break;
        m_output_available.push(pPicture);
      }
      break;
    default:
      return IPP_STATUS_ERR;
      break;
  }

  return retCodec;
}

int CDVDVideoCodecVMETA::Decode(uint8_t *pData, int iSize, double dts, double pts)
{
  IppCodecStatus retCodec;
  int rounds = 1, i, iSize2nd=0;

  if (pData)
  {
    if (m_VDecParSet.strm_fmt == IPP_VIDEO_STRM_FMT_MPG4)
    {
      uint8_t *digest_ret = digest_mpeg4_inbuf(pData, iSize);

      if ((uint32_t)digest_ret == 0xffffffff)
      {
        rounds = 0; // Skip null VOP
      }
      else if ((uint32_t)digest_ret)
      {
        iSize2nd = (pData+iSize) - digest_ret;
        iSize = digest_ret - pData;
        rounds = 2;
      }
    }
    else if (m_VDecParSet.strm_fmt == IPP_VIDEO_STRM_FMT_MPG2)
    {
      digest_mpeg2_inbuf(pData, iSize);
    }
  }

  for (i = 0; i < rounds; i++) {
    // MPEG4 packed bitstream second VOP. Change pointers to point to second VOP
    if (i == 1) {
      pData = pData + iSize;
      iSize = iSize2nd;
    }
    if (pData || iSize > 0)
    {
      unsigned int demuxer_bytes = (unsigned int)iSize;
      uint8_t *demuxer_content = pData;

      if(m_video_convert)
      {
        m_converter->Convert(pData, iSize);
        demuxer_bytes = m_converter->GetConvertSize();
        demuxer_content = m_converter->GetConvertBuffer();
        if(!demuxer_bytes && demuxer_bytes < 1)
        {
          return VC_BUFFER;
        }
      }

      m_pts_queue.push(pts);

      double start = CDVDClock::GetAbsoluteClock();
      for(;;)
      {
        retCodec = DecodeInternal(demuxer_content, &demuxer_bytes, dts, pts);
        if(retCodec == IPP_STATUS_FRAME_COMPLETE || retCodec == IPP_STATUS_NEED_INPUT || retCodec == IPP_STATUS_ERR)
          break;

        // decoding timeout.
        // TODO: should we store the decoding data and try it on the next decode again ?
        if((CDVDClock::GetAbsoluteClock() - start) > (double)DVD_MSEC_TO_TIME(500))
        {
          CLog::Log(LOGERROR, "%s::%s decoder timeout\n", CLASSNAME, __func__);
          break;
        }
      }
    }
  }

  int ret = VC_BUFFER;

  if((iSize == 0) && m_output_ready.size()) /* Demux seems empty  return VC_PICTURE */
    ret |= VC_PICTURE;
  if(m_output_ready.size() >= VMETA_QUEUE_THRESHOLD)
    ret |= VC_PICTURE;

  return ret;
}

bool CDVDVideoCodecVMETA::GetPicture(DVDVideoPicture *pDvdVideoPicture)
{
  // clone the video picture buffer settings.
  bool bRet = false;

  if(!m_output_ready.empty())
  {
    IppVmetaPicture *pPicture = m_output_ready.front();
    m_output_ready.pop();

    if(!pPicture)
    {
      CLog::Log(LOGERROR, "%s::%s Error : Picture NULL\n", CLASSNAME, __func__);
      return false;
    }

    pDvdVideoPicture->vmeta = pPicture;

    pDvdVideoPicture->dts             = DVD_NOPTS_VALUE;
    pDvdVideoPicture->pts             = DVD_NOPTS_VALUE;
    pDvdVideoPicture->format          = RENDER_FMT_UYVY422;

    pDvdVideoPicture->iDisplayWidth   = m_decoded_width;
    pDvdVideoPicture->iDisplayHeight  = m_decoded_height;
    pDvdVideoPicture->iWidth          = m_picture_width;
    pDvdVideoPicture->iHeight         = m_picture_height;

    unsigned char *pDisplayStart      = ((Ipp8u*)pPicture->pic.ppPicPlane[0]) + (pPicture->pic.picROI.y)*(pPicture->pic.picPlaneStep[0]) + ((pPicture->pic.picROI.x)<<1);

    /* data[1] and data[2] are not needed in UYVY */
    pDvdVideoPicture->data[0]         = pDisplayStart;
    pDvdVideoPicture->iLineSize[0]    = ALIGN (pPicture->pic.picWidth, 4);
    pDvdVideoPicture->data[1]         = 0;
    pDvdVideoPicture->iLineSize[1]    = 0;
    pDvdVideoPicture->data[2]         = 0;
    pDvdVideoPicture->iLineSize[2]    = 0;
    if (!m_pts_queue.empty())
    {
      //pDvdVideoPicture->pts = m_pts_queue.front();
      m_pts_queue.pop();
    }

    /*
    printf("%d : pic width [%dx%d] [%d:%d:%d] [0x%08x:0x%08x:0x%08x] %f\n",
           (unsigned int)pPicture->pUsrData1 , pDvdVideoPicture->iDisplayWidth, pDvdVideoPicture->iDisplayHeight,
           pDvdVideoPicture->iLineSize[0], pDvdVideoPicture->iLineSize[1], pDvdVideoPicture->iLineSize[2],
           (unsigned int)pDvdVideoPicture->data[0], (unsigned int)pDvdVideoPicture->data[1],
           (unsigned int)pDvdVideoPicture->data[2], (double)pDvdVideoPicture->pts / (double)DVD_TIME_BASE);
    */

#undef ALIGN

    pDvdVideoPicture->iFlags  = DVP_FLAG_ALLOCATED;
    pDvdVideoPicture->iFlags |= m_drop_state ? DVP_FLAG_DROPPED : 0;
    bRet = true;
  }
  /*
  else
  {
    pDvdVideoPicture->iFlags = DVP_FLAG_DROPPED;
    bRet = false;
  }
  */

  return bRet;
}

bool CDVDVideoCodecVMETA::ClearPicture(DVDVideoPicture* pDvdVideoPicture)
{
  // release any previous retained image buffer ref that
  // has not been passed up to renderer (ie. dropped frames, etc).
  if(pDvdVideoPicture->vmeta)
  {
    IppVmetaPicture *pPicture = (IppVmetaPicture *)pDvdVideoPicture->vmeta;
    //printf("CDVDVideoCodecVMETA::ClearPicture 0x%08x 0x%08x 0x%08x\n", pDvdVideoPicture->vmeta, pPicture, pPicture->pBuf);
    m_output_available.push(pPicture);
    pDvdVideoPicture->vmeta = NULL;
  }

  memset(pDvdVideoPicture, 0, sizeof(DVDVideoPicture));
  return true;
}

void CDVDVideoCodecVMETA::Reset(void)
{
  if(!m_is_open)
    return;

  IppVmetaBitstream *pStream = NULL;
  IppVmetaPicture *pPicture = NULL;

  while(!m_output_ready.empty())
  {
    pPicture = m_output_ready.front();
    m_output_ready.pop();
    m_output_available.push(pPicture);
  }

  pPicture = NULL;

  for(;;) {
    m_DllVMETA->DecoderPopBuffer_Vmeta(IPP_VMETA_BUF_TYPE_STRM, (void**)&pStream, m_pDecState);
    if(pStream != NULL)
    {
      m_input_available.push(pStream);
    }
    else
    {
      break;
    }
  }
  for(;;) {
    m_DllVMETA->DecoderPopBuffer_Vmeta(IPP_VMETA_BUF_TYPE_PIC, (void**)&pPicture, m_pDecState);
    if(pPicture != NULL)
    {
      m_output_available.push(pPicture);
    }
    else
    {
      break;
    }
  }

  while (!m_pts_queue.empty())
    m_pts_queue.pop();

}

/* mpeg4v2 packed bitstream unpacking code - based on code from Marvell vmeta gstreamer plugin */
#define PACKSTM_SKIPVOP_MAXLEN  15	//15 is just a rough estimation
#define MPEG2_SCID_SEQ          0xb3
#define MPEG2_SCID_SEQEND       0xb7
#define MPEG2_SCID_PIC          0x00
#define MPEG2_SCID_GOP          0xb8
#define MPEG4_SCID_VOP          0xb6
static int parse_mpeg4_TIB(unsigned char* p, int len, int* plow_delay)
{
#define __GETnBITS_InByte(B, Bitoff, N, Code)	{Code = (B<<(24+Bitoff))>>(32-N); Bitoff += N;}
  unsigned int SCode;
  if(len < 9)
  {
    //at least, 4byte VOL startcode and 34 bits contain vop_time_increment_resolution
    return -1;
  }
  //ISO 14496-2, sec 6.2.3
  //seek video object layer startcode
  SCode = ((unsigned int)p[0]<<16) | ((unsigned int)p[1]<<8) | ((unsigned int)p[2]);
  len -= 3;
  p += 3;
  while(len > 0) {
    SCode = (SCode<<8) | *p++ ;
    len--;
    if((SCode>>4) == (0x00000120>>4))
      break;
  }
  if(len < 5) //at least, should have 34 bits to contain vop_time_increment_resolution
    return -1;

  if((p[0]&127) == 5 && (p[1]&128) == 0) //video_object_type_indication
    return -1;
  else
  {
    unsigned int vtir, code, Byte = *++p;
    int time_inc_bits;;
    int bitoff;
    len--;
    if(Byte & 0x40)
    {	//is_object_layer_identifier
      len--;
      if(len<=0) {return -1;}
      Byte = *++p;
      bitoff = 1;
    } else
	bitoff = 2;

    __GETnBITS_InByte(Byte, bitoff, 4, code);  //aspect_ratio_info
    if(code == 0xf) {
      len -= 2;
      if(len<=0) return -1;
      p += 2;
      Byte = *p;
    }
    __GETnBITS_InByte(Byte, bitoff, 1, code);  //vol_control_parameters
    if(len<3) return -1;  //video_object_layer_shape+marker_bit+vop_time_increment_resolution have 19bits at least
    if(code) {
      len--;
      Byte = *++p;
      bitoff = bitoff + 2 - 8;
      __GETnBITS_InByte(Byte, bitoff, 1, code);//low_delay
      *plow_delay = code;
      CLog::Log(LOGDEBUG, "found mpeg4 stream low_delay %d\n", code);
      __GETnBITS_InByte(Byte, bitoff, 1, code);//vbv_parameters
      if(code)
      {
        len -= 10;
        if(len<=0) return -1;
        p += 10;
        bitoff -= 1;
        Byte = *p;
      }
    }
    //video_object_layer_shape
    code = (((Byte<<8)|p[1])<<(16+bitoff)) >> 30;
    bitoff += 2;
    if(bitoff >= 8)
    {
      bitoff -= 8;
      len--;
      p++;
    }
    if(code != 0)  //only support video_object_layer_shape==rectangular
    return -2;
    //vop_time_increment_resolution
    if(len < 3)
      return -1;
    vtir = (((unsigned int)p[0]<<16)|((unsigned int)p[1]<<8)|(unsigned int)p[2])<<(8+1+bitoff);
    if((vtir>>16) == 0)
      return -3;

    CLog::Log(LOGDEBUG, "parse_mpeg4_TIB parsed vtir %d\n", vtir>>16);
    vtir -= 0x10000;
    for(time_inc_bits = 16; time_inc_bits>0; time_inc_bits--)
    {
      if(((int)vtir) < 0)
        break;
      vtir <<= 1;
    }
    if(time_inc_bits == 0)
      time_inc_bits = 1;
    CLog::Log(LOGDEBUG, "parse_mpeg4_TIB() parsed time_inc_bits %d\n", time_inc_bits);
    return time_inc_bits;
  }
}

static unsigned char* Seek4bytesCode(unsigned char* pData, int len, unsigned int n4byteCode)
{
  if(len >= 4)
  {
    unsigned int code = (pData[0]<<16) | (pData[1]<<8) | (pData[2]);
    len -= 3;
    pData += 3;
    while(len > 0)
    {
      code = (code<<8) | *pData++ ;
      len--;
      if(code == n4byteCode)
      {
        return pData-4;
      }
    }
  }
  return NULL;
}

static int is_MPEG4_SkipVop(unsigned char* pData, int len, int itime_inc_bits_len)
{
  if(len > 4 && len <= PACKSTM_SKIPVOP_MAXLEN && itime_inc_bits_len > 0)
  {
    //probably, we needn't to check the frame coding type, check the data length is enough
    unsigned char* p = Seek4bytesCode(pData, len, 0x00000100 | MPEG4_SCID_VOP);
    if(p != NULL) {
      p += 4;
      len -= (p-pData);
      if(len > 0 && (p[0] >> 6) == 1) {
        //the skip frame for packed stream is always P frame
        //iso 14496-2: sec 6.2.5
        int bitoffset = 2;
        unsigned int modulo_time_base;
        do
        {
          modulo_time_base = (p[0]<<bitoffset)&0x80;
          bitoffset = (bitoffset+1) & 7;
          if(bitoffset == 0)
          {
            len--;
            p++;
          }
        } while(modulo_time_base!=0 && len>0);
        bitoffset = bitoffset + 2 + itime_inc_bits_len;
        if(bitoffset >= (len<<3)) //if((len<<3)<bitoffset+1)
          return 0;
        else
          return ((p[bitoffset>>3]<<(bitoffset&7))&0x80) == 0;
      }
    }
  }
  return 0;
}

static unsigned char* seek2ndVop(unsigned char* pData, int len)
{
  unsigned char* p = Seek4bytesCode(pData, len, 0x00000100 | MPEG4_SCID_VOP);
  unsigned char* p2 = NULL;
  if(p!=NULL)
    p2 = Seek4bytesCode(p+4, len-(p+4-pData), 0x00000100 | MPEG4_SCID_VOP);
  return p2;
}


uint8_t * CDVDVideoCodecVMETA::digest_mpeg4_inbuf(uint8_t *pData, int iSize)
{
  unsigned char* p2ndVop = NULL;
  if(iSize == 1 && *pData == 0x7f)
    //stuffing bits, spec 14496-2: sec 5.2.3, 5.2.4
    return (uint8_t *)0xfffffffe;

  if(m_itime_inc_bits< 0)
    m_itime_inc_bits = parse_mpeg4_TIB(pData, iSize, &m_low_delay);

  if((m_low_delay != 1) && (m_codec_species >=2))
  {
    //in avi file, xvid & divx probably use "packed bitstream"(if no bvop, impossible to use "packed bitstream"), those skip vop should be ignored
    if(is_MPEG4_SkipVop(pData, iSize, m_itime_inc_bits))
      return (uint8_t *)0xffffffff; // skip VOP
    p2ndVop = seek2ndVop(pData, iSize);
  }
  return p2ndVop;
}


uint8_t * CDVDVideoCodecVMETA::digest_mpeg2_inbuf(uint8_t *pData, int iSize)
{
  uint8_t *pSeqHead = Seek4bytesCode(pData, iSize, 0x00000100|MPEG2_SCID_SEQ);

  if (pSeqHead)
  {
    iSize -= pSeqHead - pData;

    if (iSize >= 12)
    {
      int width, height, aspect_ratio_code;

      width  = (pSeqHead[4] << 4) | (pSeqHead[5] >> 4);
      height = ((pSeqHead[5] & 0x0f) << 8) | pSeqHead[6];
      aspect_ratio_code = pSeqHead[7] >> 4;

      switch (aspect_ratio_code)
      {
      case 2:                   // IAR 4:3
          m_decoded_width = (height * 4) / 3;
          break;
      case 3:                   // IAR 16:9
          m_decoded_width = (height * 16) / 9;
          break;
      case 4:                   // IAR 2.21:1
          m_decoded_width = (height * 221) / 100;
          break;
      default:                  // PAR 1:1
          m_decoded_width = width;
      }

      m_decoded_height = height;

      return pSeqHead;
    }
  }

  return 0;
}


#endif
