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

#include "threads/SingleLock.h"
#include "settings/Settings.h"
#include "guilib/Resolution.h"
#include "utils/log.h"
#include "DVDClock.h"

#include <sys/time.h>
#include <inttypes.h>

#ifdef CLASSNAME
#undef CLASSNAME
#endif

#define CLASSNAME "CDVDVideoCodecVMETA"

#include "utils/BitstreamConverter.h"
#include "DVDCodecs/DVDCodecs.h"


#define ENABLE_MPEG1            // use vMeta for MPEG1 decoding
//#define ENABLE_PTS              // honour presentation time stamps


#define ALIGN_SIZE(x, n)        (((x) + (n) - 1) & (~((n) - 1)))
#define ALIGN_OFFSET(x, n)      ((-(x)) & ((n) - 1))

#define PADDED_SIZE(s)          ALIGN_SIZE((s), 128)    // align to multiple of 128 (vMeta requirement)
#define PADDING_LEN(s)          ALIGN_OFFSET((s), 128)  // align to multiple of 128 (vMeta requirement)
#define PADDING_BYTE            0x88            // the vmeta decoder needs a padding of 0x88 at the end of a frame

#define STREAM_VDECBUF_SIZE     (512*1024U)     // must equal to or greater than 64k and multiple of 128
#define STREAM_VDECBUF_NUM      7               // number of stream buffers
#define STREAM_PICBUF_NUM       24              // number of picture buffers
#define VMETA_QUEUE_HIGHMARK    22              // maximal # of buffered pictures
#define VMETA_QUEUE_LOWMARK     2               // minimal # of buffered pictures


#if STREAM_VDECBUF_NUM >= STREAM_FIFO_SIZE
#error "Please adjust STREAM_FIFO_SIZE !"
#endif

#if STREAM_PICBUF_NUM >= PICTURE_FIFO_SIZE
#error "Please adjust PICTURE_FIFO_SIZE !"
#endif


#if 1
  #define CLEAR_STREAMBUF(p)    do { \
    (p)->nDataLen = 0; \
    } while (0)
#else
  #define CLEAR_STREAMBUF(p)    do { \
    (p)->nDataLen = 0; \
    memset((p)->pBuf, PADDING_BYTE, (p)->nBufSize); \
  } while (0)
#endif


CDVDVideoCodecVMETA::CDVDVideoCodecVMETA()
  :  m_HwLock(g_CritSecVMETA),
     m_DllVMETA(g_DllLibVMETA),
     m_DllMiscGen(g_DllLibMiscGen)
{
  m_is_open           = false;
  m_extradata         = NULL;
  m_extrasize         = 0;
  m_converter         = NULL;
  m_video_convert     = false;
  m_video_codec_name  = "";
  m_frame_no          = 0;
  m_numPicBufSubmitted  = 0;
  m_numStrmBufSubmitted = 0;

  m_pDecState         = NULL;
  m_pCbTable          = NULL;
  m_itime_inc_bits    = -1;
  m_low_delay         = -1;
  m_codec_species     = -1;

  DllLibVMETA::SetHardwareClock(CSettings::GetInstance().GetInt("videoscreen.vmeta_clk") == VMETA_CLK_667);
}


CDVDVideoCodecVMETA::~CDVDVideoCodecVMETA()
{
  Dispose();

  DllLibVMETA::SetHardwareClock(false);
}


bool CDVDVideoCodecVMETA::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options)
{
  IppCodecStatus ret;

  m_picture_width  = m_display_width  = hints.width;
  m_picture_height = m_display_height = hints.height;

  memset(&m_VDecInfo, 0, sizeof(IppVmetaDecInfo));
  memset(&m_VDecParSet, 0, sizeof(IppVmetaDecParSet));

  if(!m_display_width || !m_display_height)
    return false;

  if (!m_DllVMETA.Load() || !m_DllMiscGen.Load())
  {
    CLog::Log(LOGERROR, "%s::%s Error : failed to load vMeta libs !", CLASSNAME, __func__);
    return false;
  }

  m_converter = new CBitstreamConverter();

  if (m_converter->Open(hints.codec,
                        (uint8_t *)hints.extradata,
                        hints.extrasize, true) )
  {
    if (m_converter->GetExtraData() && m_converter->GetExtraSize() > 0)
    {
      m_extrasize = m_converter->GetExtraSize();
      m_extradata = (uint8_t *)malloc(m_extrasize);
      memcpy(m_extradata, m_converter->GetExtraData(), m_extrasize);
    }
  }
  else
  {
    if(hints.extrasize > 0 && hints.extradata != NULL)
    {
      m_extrasize = hints.extrasize;
      m_extradata = (uint8_t *)malloc(m_extrasize);
      memcpy(m_extradata, hints.extradata, m_extrasize);
    }
  }

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

      if (hints.codec_tag == MKTAG('X','V','I','D'))
        m_codec_species = 3;
      else if (hints.codec_tag == MKTAG('D','X','5','0'))
        m_codec_species = 2;
      else if (hints.codec_tag == MKTAG('D','I','V','X'))
        m_codec_species = 1;
      else
        m_codec_species = 0;

      //m_codec_species = 2; /* TODO - Better detect xvid, and divx versions */
      break;

    case CODEC_ID_MPEG2VIDEO:
      m_VDecParSet.strm_fmt = IPP_VIDEO_STRM_FMT_MPG2;
      m_video_codec_name = "vmeta-mpeg2";
      break;

    case CODEC_ID_H263:
      m_VDecParSet.strm_fmt = IPP_VIDEO_STRM_FMT_H263;
      m_video_codec_name = "vmeta-h263";
      break;

    case CODEC_ID_VC1:
      m_VDecParSet.strm_fmt = IPP_VIDEO_STRM_FMT_VC1;
      m_video_codec_name = "vmeta-vc1";
      break;

    case CODEC_ID_WMV3:
      m_VDecParSet.strm_fmt = IPP_VIDEO_STRM_FMT_VC1M;
      m_video_codec_name = "vmeta-wmv3";
      break;

#ifdef ENABLE_MPEG1
    case CODEC_ID_MPEG1VIDEO:
      m_VDecParSet.strm_fmt = IPP_VIDEO_STRM_FMT_MPG1;
      m_video_codec_name = "vmeta-mpeg1";
      break;
#endif

    default:
      CLog::Log(LOGDEBUG, "%s::%s CodecID 0x%08x (%.4s) not supported by VMETA decoder",
                CLASSNAME, __func__, hints.codec, (char *)&hints.codec_tag);
      Dispose();
      return false;
  }


  if(m_DllMiscGen.miscInitGeneralCallbackTable(&m_pCbTable) != 0)
  {
    CLog::Log(LOGERROR, "%s::%s Error : miscInitGeneralCallbackTable", CLASSNAME, __func__);
    Dispose();
    return false;
  }

  m_VDecParSet.opt_fmt = IPP_YCbCr422I;
  ret = m_DllVMETA.DecoderInitAlloc_Vmeta(&m_VDecParSet, m_pCbTable, &m_pDecState);
  if(ret != IPP_STATUS_NOERR)
  {
    CLog::Log(LOGERROR, "%s::%s Error : DecoderInitAlloc_Vmeta", CLASSNAME, __func__);
    Dispose();
    return false;
  }

  for (size_t i = 0; i < STREAM_VDECBUF_NUM; i++)
  {
    IppVmetaBitstream *pStream = (IppVmetaBitstream *)malloc(sizeof(IppVmetaBitstream));
    memset(pStream, 0, sizeof(IppVmetaBitstream));

    pStream->pBuf = (Ipp8u *)m_DllVMETA.vdec_os_api_dma_alloc_writecombine(
                                STREAM_VDECBUF_SIZE, VMETA_STRM_BUF_ALIGN, &pStream->nPhyAddr);
    pStream->nBufSize = STREAM_VDECBUF_SIZE;
    CLEAR_STREAMBUF(pStream);

    m_input_buffers.push_front(pStream);
    m_input_available.putTail(pStream);
  }

  for (size_t i = 0; i < STREAM_PICBUF_NUM; i++)
  {
    IppVmetaPicture *pPicture = (IppVmetaPicture *)malloc(sizeof(IppVmetaPicture));
    memset(pPicture, 0, sizeof(IppVmetaPicture));

    pPicture->pUsrData0 = (void *)i;

    m_output_buffers.push_front(pPicture);
    m_output_available.putTail(pPicture);
  }

  m_video_convert = m_converter->NeedConvert();
  m_numPicBufSubmitted = 0;
  m_numStrmBufSubmitted = 0;
  m_frame_no = 0;
  m_is_open = true;

  SendCodecConfig();

  CLog::Log(LOGDEBUG, "%s::%s - VMETA Decoder opened with codec : %s [%dx%d]",
            CLASSNAME, __func__, m_video_codec_name.c_str(), m_display_width, m_display_height);

  return true;
}


void CDVDVideoCodecVMETA::Dispose()
{
  m_is_open = false;

  m_frame_no = 0;
  m_extrasize = 0;
  m_numPicBufSubmitted = 0;
  m_numStrmBufSubmitted = 0;
  m_video_convert = false;
  m_video_codec_name = "";

  if (m_extradata)
  {
    free(m_extradata);
    m_extradata = 0;
  }

  if (m_converter)
  {
    delete m_converter;
    m_converter = 0;
  }

  if (m_pDecState)
  {
    m_DllVMETA.DecodeSendCmd_Vmeta(IPPVC_STOP_DECODE_STREAM, NULL, NULL, m_pDecState);

    Reset();

    m_DllVMETA.DecoderFree_Vmeta(&m_pDecState);
    m_pDecState = 0;
  }

  if (m_pCbTable)
  {
    m_DllMiscGen.miscFreeGeneralCallbackTable(&m_pCbTable);
    m_pCbTable = 0;
  }


  m_input_ready.flushAll();
  m_input_available.flushAll();
  while (!m_input_buffers.empty())
  {
    IppVmetaBitstream *pStream = m_input_buffers.front();
    m_input_buffers.pop_front();

    if(pStream->pBuf)
      m_DllVMETA.vdec_os_api_dma_free(pStream->pBuf);
    free(pStream);
  }


  m_output_ready.flushAll();
  m_output_available.flushAll();
  while (!m_output_buffers.empty())
  {
    IppVmetaPicture *pPicture = m_output_buffers.front();
    m_output_buffers.pop_front();

    if(pPicture->pBuf)
      m_DllVMETA.vdec_os_api_dma_free(pPicture->pBuf);
    free(pPicture);
  }


  m_DllMiscGen.Unload();
  m_DllVMETA.Unload();
}


void CDVDVideoCodecVMETA::SetDropState(bool bDrop)
{
  m_drop_state = bDrop;
}


IppCodecStatus CDVDVideoCodecVMETA::SendCodecConfig()
{
  unsigned paddingLen;
  IppCodecStatus retCodec;
  IppVmetaBitstream *pStream;

  if (!m_extradata || !m_extrasize || !m_pDecState)
    return IPP_STATUS_ERR;


  if (m_VDecParSet.strm_fmt == IPP_VIDEO_STRM_FMT_VC1M)
  {
    vc1m_seq_header seqInfo;

    seqInfo.num_frames = 0xffffff;
    seqInfo.vert_size  = m_picture_height;
    seqInfo.horiz_size = m_picture_width;
    seqInfo.level      = ((m_extradata[0]>>4) == 4) ? 4 : 2;
    seqInfo.cbr        = 1;
    seqInfo.hrd_buffer = 0x007fff;
    seqInfo.hrd_rate   = 0x00007fff;
    seqInfo.frame_rate = 0xffffffff;
    seqInfo.exthdrsize = std::min<uint32_t>(m_extrasize, sizeof(seqInfo.exthdr));
    memcpy(seqInfo.exthdr, m_extradata, seqInfo.exthdrsize);

    retCodec = m_DllVMETA.DecodeSendCmd_Vmeta(
                  IPPVC_SET_VC1M_SEQ_INFO, &seqInfo, NULL, m_pDecState);

    if (retCodec != IPP_STATUS_NOERR)
      CLog::Log(LOGERROR, "%s::%s Error : Set VC1M sequence info ", CLASSNAME, __func__);

    return retCodec;
  }


  if (!m_input_available.getHead(pStream))
    return IPP_STATUS_ERR;

  memcpy(pStream->pBuf, m_extradata, m_extrasize);
  pStream->nOffset  = 0;
  pStream->nDataLen = m_extrasize;
  pStream->nFlag    = IPP_VMETA_STRM_BUF_END_OF_UNIT;

  paddingLen = PADDING_LEN(m_extrasize);
  if (paddingLen)
    memset(pStream->pBuf + m_extrasize, PADDING_BYTE, paddingLen);

  retCodec = m_DllVMETA.DecoderPushBuffer_Vmeta(
                IPP_VMETA_BUF_TYPE_STRM, pStream, m_pDecState);

  if (retCodec != IPP_STATUS_NOERR)
  {
    CLog::Log(LOGERROR, "%s::%s Error : Push streambuffer", CLASSNAME, __func__);
    m_input_available.putTail(pStream);
  }

  return retCodec;
}


int CDVDVideoCodecVMETA::Decode(uint8_t *pData, int iSize, double dts, double pts)
{
  int iSize2nd = 0;
  uint32_t bufOfs = 0;
  bool bInjectHdr = false;
  const uint32_t VC1FrameStartCode = htobe32(0x0000010d);
#ifdef ENABLE_PTS
  int numStreamBufs = 0;
#endif

  if (pData)
  {
    if (m_VDecParSet.strm_fmt == IPP_VIDEO_STRM_FMT_MPG4)
    {
      // special handling: MPEG4 packed bitstream
      uint8_t *digest_ret = digest_mpeg4_inbuf(pData, iSize);

      if (((uint32_t)digest_ret | 0x1) == 0xffffffff)
      {
        pData = 0;
        iSize = 0;      // Skip null VOP and stuffing byte
      }
      else if (digest_ret)
      {
        int iTemp = digest_ret - pData;
        iSize2nd = iSize - iTemp;
        iSize = iTemp;
      }
    }
    else if (m_VDecParSet.strm_fmt == IPP_VIDEO_STRM_FMT_MPG2)
    {
      // special handling: determine MPEG2 aspect ratio
      digest_mpeg12_inbuf(CODEC_ID_MPEG2VIDEO, pData, iSize);
    }
    else if (m_VDecParSet.strm_fmt == IPP_VIDEO_STRM_FMT_VC1)
    {
      // SMPTE 421M sec. G.8 says the frame start code is optional,
      // but vMeta only can decode the stream with frame start code
      bInjectHdr = digest_vc1_inbuf(pData, iSize);
      if (bInjectHdr)
        bufOfs = sizeof(uint32_t);
    }
    else if (m_VDecParSet.strm_fmt == IPP_VIDEO_STRM_FMT_VC1M)
    {
      // this offset is not strictly necessary, but it may
      // avoid internal memory copy
      bufOfs = VMETA_COM_PKT_HDR_SIZE;
    }
    else if (m_VDecParSet.strm_fmt == IPP_VIDEO_STRM_FMT_MPG1)
    {
      // special handling: determine MPEG1 aspect ratio
      digest_mpeg12_inbuf(CODEC_ID_MPEG1VIDEO, pData, iSize);
    }
  }


  while (iSize > 0)
  {
    IppVmetaBitstream *pStream;

    // retrieve empty stream buffer
    if (m_input_available.getHead(pStream))
    {
      uint32_t dataLen = pStream->nBufSize;

      // convert or copy data
      if (m_video_convert)
      {
        // convert demuxer packet from bitstream to bytestream (AnnexB)
        if (m_converter->Convert(pData, iSize))
        {
          dataLen = m_converter->GetConvertSize();
          if (PADDED_SIZE(dataLen) <= pStream->nBufSize)
            memcpy(pStream->pBuf, m_converter->GetConvertBuffer(), dataLen);
        }
        else
          dataLen = 0;
      }
      else
      {
        memcpy(pStream->pBuf+bufOfs, pData, std::min<uint32_t>(dataLen-bufOfs, iSize));
        dataLen = (uint32_t)iSize+bufOfs;
      }

      // did it fit in ?
      if (PADDED_SIZE(dataLen) > pStream->nBufSize)
      {
        m_DllVMETA.vdec_os_api_dma_free(pStream->pBuf);

        dataLen = ALIGN_SIZE(dataLen, 65536) + 65536;
        pStream->pBuf = (Ipp8u *)m_DllVMETA.vdec_os_api_dma_alloc_writecombine(
                              dataLen, VMETA_STRM_BUF_ALIGN, &pStream->nPhyAddr);
        pStream->nBufSize = dataLen;
        pStream->nOffset  = 0;
        pStream->nDataLen = 0;

        // retry (using a larger buffer)
        if (m_video_convert)
        {
          dataLen = m_converter->GetConvertSize();
          memcpy(pStream->pBuf, m_converter->GetConvertBuffer(), dataLen);
        }
        else
        {
          memcpy(pStream->pBuf+bufOfs, pData, iSize);
          dataLen = (uint32_t)iSize+bufOfs;
        }
      }

      if (dataLen)
      {
        if (bInjectHdr)
        {
          *(uint32_t *)pStream->pBuf = VC1FrameStartCode;
          pStream->nOffset = 0;
        }
        else
        {
          pStream->nOffset = bufOfs;
        }

        pStream->nDataLen = dataLen;
        pStream->nFlag = IPP_VMETA_STRM_BUF_END_OF_UNIT;

        dataLen = PADDING_LEN(dataLen);
        if (dataLen)
          memset(pStream->pBuf + pStream->nDataLen, PADDING_BYTE, dataLen);

        if( !m_input_ready.putTail(pStream) )
        {
          CLog::Log(LOGERROR, "%s::%s m_input_ready fifo overflow", CLASSNAME, __func__);

          CLEAR_STREAMBUF(pStream);
          m_input_available.putTail(pStream);
          break;
        }

#ifdef ENABLE_PTS
        if( numStreamBufs++ == 0)
        {
          m_pts_queue.putTail(pts);
        }
#endif
      }
      else
      {
        CLog::Log(LOGERROR, "%s::%s converter returned nothing !", CLASSNAME, __func__);

        m_input_available.putTail(pStream);
      }
    }
    else
    {
      CLog::Log(LOGERROR, "%s::%s no stream buffer available", CLASSNAME, __func__);
      break;
    }

    // MPEG4 packed bitstream second VOP. Change pointers to point to second VOP
    pData   += iSize;
    iSize    = iSize2nd;
    iSize2nd = 0;
  }


  if (pData)
  {
    double start = CDVDClock::GetAbsoluteClock();

    for (;;)
    {
      IppCodecStatus retCodec = DecodeInternal();

      if (retCodec == IPP_STATUS_FRAME_COMPLETE || retCodec == IPP_STATUS_NEED_INPUT || retCodec == IPP_STATUS_ERR)
        break;

      // decoding timeout.
      // TODO: should we store the decoding data and try it on the next decode again ?
      if ((CDVDClock::GetAbsoluteClock() - start) > (double)DVD_MSEC_TO_TIME(500))
      {
        CLog::Log(LOGERROR, "%s::%s decoder timeout retCodec = %d", CLASSNAME, __func__, retCodec);
        break;
      }
    }
  }

  int pictureCount = m_output_ready.usedCount();

  if( pictureCount >= VMETA_QUEUE_HIGHMARK )
    return VC_PICTURE;

  if( pData && pictureCount < VMETA_QUEUE_LOWMARK )
    return VC_BUFFER;

  return ( pictureCount ) ?  (VC_PICTURE | VC_BUFFER) : VC_BUFFER;
}


IppCodecStatus CDVDVideoCodecVMETA::DecodeInternal()
{
  IppVmetaPicture *pPicture;
  IppVmetaBitstream *pStream;

  IppCodecStatus retCodec = m_DllVMETA.DecodeFrame_Vmeta(&m_VDecInfo, m_pDecState);

  switch(retCodec)
  {
    case IPP_STATUS_NEED_INPUT:
      //CLog::Log(LOGNOTICE, "IPP_STATUS_NEED_INPUT");
      while (m_input_ready.getHead(pStream))
      {
        retCodec = m_DllVMETA.DecoderPushBuffer_Vmeta(IPP_VMETA_BUF_TYPE_STRM, pStream, m_pDecState);
        if (retCodec != IPP_STATUS_NOERR)
        {
          CLog::Log(LOGERROR, "IPP_STATUS_NEED_INPUT: push streambuffer failed");

          CLEAR_STREAMBUF(pStream);
          m_input_available.putTail(pStream);
          return IPP_STATUS_ERR;
        }

        m_numStrmBufSubmitted++;
      }
      break;

    case IPP_STATUS_RETURN_INPUT_BUF:
      //CLog::Log(LOGNOTICE, "IPP_STATUS_RETURN_INPUT_BUF");
      while (m_numStrmBufSubmitted)
      {
        m_DllVMETA.DecoderPopBuffer_Vmeta(IPP_VMETA_BUF_TYPE_STRM, (void **)&pStream, m_pDecState);

        if (!pStream)
          break;

        CLEAR_STREAMBUF(pStream);
        m_input_available.putTail(pStream);
        m_numStrmBufSubmitted--;
      }
      break;

    case IPP_STATUS_FRAME_COMPLETE:
      //CLog::Log(LOGNOTICE, "IPP_STATUS_FRAME_COMPLETE");
      m_DllVMETA.DecoderPopBuffer_Vmeta(IPP_VMETA_BUF_TYPE_PIC, (void **)&pPicture, m_pDecState);

      if (pPicture)
      {
        pPicture->pUsrData1 = (void *)(m_frame_no++);
        m_numPicBufSubmitted--;

        if (!m_output_ready.putTail(pPicture))
        {
          CLog::Log(LOGERROR, "%s::%s m_output_ready fifo overflow", CLASSNAME, __func__);
          m_output_available.putTail(pPicture);
        }
      }

      while (m_numStrmBufSubmitted)
      {
        m_DllVMETA.DecoderPopBuffer_Vmeta(IPP_VMETA_BUF_TYPE_STRM, (void **)&pStream, m_pDecState);

        if (!pStream)
          break;

        CLEAR_STREAMBUF(pStream);
        m_input_available.putTail(pStream);
        m_numStrmBufSubmitted--;
      }

      // The gstreamer plugins says this is needed for DOVE
      if (m_DllVMETA.vdec_os_api_suspend_check())
      {
        IppCodecStatus suspendRet = m_DllVMETA.DecodeSendCmd_Vmeta(IPPVC_PAUSE, NULL, NULL, m_pDecState);
        if (suspendRet == IPP_STATUS_NOERR)
        {
          m_DllVMETA.vdec_os_api_suspend_ready();

          suspendRet = m_DllVMETA.DecodeSendCmd_Vmeta(IPPVC_RESUME, NULL, NULL, m_pDecState);
          if (suspendRet != IPP_STATUS_NOERR)
          {
            CLog::Log(LOGERROR, "%s::%s resume failed", CLASSNAME, __func__);
          }
        }
        else
        {
          CLog::Log(LOGERROR, "%s::%s pause failed", CLASSNAME, __func__);
        }
      }
      break;

    case IPP_STATUS_NEED_OUTPUT_BUF:
      //CLog::Log(LOGNOTICE, "IPP_STATUS_NEED_OUTPUT_BUF");
      if (!m_output_available.getHead(pPicture))
      {
        CLog::Log(LOGDEBUG, "IPP_STATUS_NEED_OUTPUT_BUF: no free pictures buffers");
        return IPP_STATUS_ERR;
      }

      if (m_VDecInfo.seq_info.dis_buf_size > pPicture->nBufSize)
      {
        if (pPicture->pBuf)
          m_DllVMETA.vdec_os_api_dma_free(pPicture->pBuf);

        pPicture->pBuf = (Ipp8u *)m_DllVMETA.vdec_os_api_dma_alloc(
                            m_VDecInfo.seq_info.dis_buf_size, VMETA_DIS_BUF_ALIGN, &(pPicture->nPhyAddr));
        pPicture->nBufSize = m_VDecInfo.seq_info.dis_buf_size;
      }

      if (!pPicture->pBuf)
      {
        CLog::Log(LOGERROR, "IPP_STATUS_NEED_OUTPUT_BUF: allocate picture failed");
        pPicture->nBufSize = 0;
        pPicture->nPhyAddr = 0;
        m_output_available.putTail(pPicture);
        return IPP_STATUS_ERR;
      }

      retCodec = m_DllVMETA.DecoderPushBuffer_Vmeta(IPP_VMETA_BUF_TYPE_PIC, pPicture, m_pDecState);
      if (retCodec != IPP_STATUS_NOERR)
      {
        CLog::Log(LOGERROR, "IPP_STATUS_NEED_OUTPUT_BUF: push picturebuffer failed");
        m_output_available.putTail(pPicture);
        return IPP_STATUS_ERR;
      }

      m_numPicBufSubmitted++;
      break;

    case IPP_STATUS_NEW_VIDEO_SEQ:
      //CLog::Log(LOGNOTICE, "IPP_STATUS_NEW_VIDEO_SEQ");
      if (m_VDecInfo.seq_info.picROI.width != 0 && m_VDecInfo.seq_info.picROI.height != 0)
      {
        m_picture_width   = m_VDecInfo.seq_info.picROI.width;
        m_picture_height  = m_VDecInfo.seq_info.picROI.height;
        CLog::Log(LOGDEBUG, "IPP_STATUS_NEW_VIDEO_SEQ: New sequence picture dimension [%dx%d]",
                  m_picture_width, m_picture_height);
      }

      while (m_numPicBufSubmitted)
      {
        m_DllVMETA.DecoderPopBuffer_Vmeta(IPP_VMETA_BUF_TYPE_PIC, (void **)&pPicture, m_pDecState);

        if (!pPicture)
          break;

        m_output_available.putTail(pPicture);
        m_numPicBufSubmitted--;
      }
      break;

    case IPP_STATUS_END_OF_STREAM:
      //CLog::Log(LOGNOTICE, "IPP_STATUS_END_OF_STREAM");
      break;

    case IPP_STATUS_WAIT_FOR_EVENT:
      //CLog::Log(LOGNOTICE, "IPP_STATUS_WAIT_FOR_EVENT");
      break;

    default:
      CLog::Log(LOGERROR, "%s::%s DecodeFrame_Vmeta returned %d", CLASSNAME, __func__, retCodec);
      return IPP_STATUS_ERR;
  }

  return retCodec;
}


bool CDVDVideoCodecVMETA::GetPicture(DVDVideoPicture *pDvdVideoPicture)
{
  IppVmetaPicture       *pPicture;

  pDvdVideoPicture->dts             = DVD_NOPTS_VALUE;
  pDvdVideoPicture->pts             = DVD_NOPTS_VALUE;
  pDvdVideoPicture->iDisplayWidth   = m_display_width;
  pDvdVideoPicture->iDisplayHeight  = m_display_height;
  pDvdVideoPicture->iWidth          = m_picture_width;
  pDvdVideoPicture->iHeight         = m_picture_height;

  if (m_output_ready.getHead(pPicture))
  {
    // clone the video picture buffer settings.
    pDvdVideoPicture->vmeta         = pPicture;
    pDvdVideoPicture->format        = RENDER_FMT_VMETA;
    pDvdVideoPicture->iFlags        = DVP_FLAG_ALLOCATED;

    if (m_drop_state || (unsigned)pPicture->pUsrData1 < 2)
      pDvdVideoPicture->iFlags |= DVP_FLAG_DROPPED;

#ifdef ENABLE_PTS
    if (!m_pts_queue.isEmpty())
      m_pts_queue.getHead(pDvdVideoPicture->pts);
#endif

    return true;
  }

  pDvdVideoPicture->vmeta           = 0;
  pDvdVideoPicture->iFlags          = 0;
  pDvdVideoPicture->format          = RENDER_FMT_NONE;

  return false;
}


bool CDVDVideoCodecVMETA::ClearPicture(DVDVideoPicture *pDvdVideoPicture)
{
  // release any previous retained image buffer ref that
  // has not been passed up to renderer (ie. dropped frames, etc).
  if (pDvdVideoPicture->format == RENDER_FMT_VMETA)
    m_output_available.putTail(pDvdVideoPicture->vmeta);

  memset(pDvdVideoPicture, 0, sizeof(DVDVideoPicture));
  return true;
}


void CDVDVideoCodecVMETA::Reset(void)
{
  if (!m_is_open)
    return;

#ifdef ENABLE_PTS
  m_pts_queue.flushAll();
#endif

  IppVmetaBitstream *pStream = 0;
  while (m_input_ready.getHead(pStream))
  {
    CLEAR_STREAMBUF(pStream);
    m_input_available.putTail(pStream);
  }

  for(;;)
  {
    m_DllVMETA.DecoderPopBuffer_Vmeta(IPP_VMETA_BUF_TYPE_STRM, (void **)&pStream, m_pDecState);

    if(!pStream)
      break;

    CLEAR_STREAMBUF(pStream);
    m_input_available.putTail(pStream);
  }

  m_numStrmBufSubmitted = 0;

  IppVmetaPicture *pPicture = 0;
  while (m_output_ready.getHead(pPicture))
    m_output_available.putTail(pPicture);

  for(;;)
  {
    m_DllVMETA.DecoderPopBuffer_Vmeta(IPP_VMETA_BUF_TYPE_PIC, (void **)&pPicture, m_pDecState);

    if(!pPicture)
      break;

    m_output_available.putTail(pPicture);
  }
  
  m_numPicBufSubmitted = 0;
}




/* mpeg4v2 packed bitstream unpacking code - based on code from Marvell vmeta gstreamer plugin */
#define PACKSTM_SKIPVOP_MAXLEN  15	//15 is just a rough estimation
#define MPEG2_SCID_SEQ          0xb3
#define MPEG2_SCID_SEQEND       0xb7
#define MPEG2_SCID_PIC          0x00
#define MPEG2_SCID_GOP          0xb8
#define MPEG2_SCID_EXTHDR       0xb5
#define MPEG4_SCID_VOP          0xb6

static int parse_mpeg4_TIB(unsigned char *p, int len, int *plow_delay)
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
      CLog::Log(LOGDEBUG, "found mpeg4 stream low_delay %d", code);
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

    CLog::Log(LOGDEBUG, "parse_mpeg4_TIB parsed vtir %d", vtir>>16);
    vtir -= 0x10000;
    for(time_inc_bits = 16; time_inc_bits>0; time_inc_bits--)
    {
      if(((int)vtir) < 0)
        break;
      vtir <<= 1;
    }
    if(time_inc_bits == 0)
      time_inc_bits = 1;
    CLog::Log(LOGDEBUG, "parse_mpeg4_TIB() parsed time_inc_bits %d", time_inc_bits);
    return time_inc_bits;
  }
}

static inline unsigned char *Seek4bytesCode(unsigned char *pData, int len, unsigned int n4byteCode)
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

static int is_MPEG4_SkipVop(unsigned char *pData, int len, int itime_inc_bits_len)
{
  if(len > 4 && len <= PACKSTM_SKIPVOP_MAXLEN && itime_inc_bits_len > 0)
  {
    //probably, we needn't to check the frame coding type, check the data length is enough
    unsigned char *p = Seek4bytesCode(pData, len, 0x00000100 | MPEG4_SCID_VOP);
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

static inline unsigned char *seek2ndVop(unsigned char *pData, int len)
{
  unsigned char *p = Seek4bytesCode(pData, len, 0x00000100 | MPEG4_SCID_VOP);
  return p ? Seek4bytesCode(p+4, len-(p+4-pData), 0x00000100 | MPEG4_SCID_VOP) : NULL;
}


uint8_t *CDVDVideoCodecVMETA::digest_mpeg4_inbuf(uint8_t *pData, int iSize)
{
  unsigned char *p2ndVop = NULL;

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


uint8_t *CDVDVideoCodecVMETA::digest_mpeg12_inbuf(uint32_t codecId, uint8_t *pData, int iSize)
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

      if (codecId == CODEC_ID_MPEG1VIDEO)
      {
        static int ratioTable[16] =
        {
/*  0*/   10000,
/*  1*/   10000,
/*  2*/    6735,
/*  3*/    7031,
/*  4*/    7615,
/*  5*/    8055,
/*  6*/    8437,
/*  7*/    8935,
/*  8*/    9157,
/*  9*/    9815,
/* 10*/   10255,
/* 11*/   10695,
/* 12*/   10950,
/* 13*/   11575,
/* 14*/   12015,
/* 15*/   10000,
        };

        m_display_width  = (width * ratioTable[aspect_ratio_code]) / 10000;
      }
      else
      {
        switch (aspect_ratio_code)
        {
        case 2:                   // IAR 4:3
          m_display_width = (height * 4) / 3;
          break;
        case 3:                   // IAR 16:9
          m_display_width = (height * 16) / 9;
          break;
        case 4:                   // IAR 2.21:1
          m_display_width = (height * 221) / 100;
          break;
        default:                  // PAR 1:1
          m_display_width = width;
        }
      }

      m_display_height = height;

      return pSeqHead;
    }
  }

  return 0;
}


bool CDVDVideoCodecVMETA::digest_vc1_inbuf(uint8_t *pData, int iSize)
{
  return (iSize < 3 || pData[0] != 0x00 || pData[1] != 0x00 || pData[2] != 0x01);
}


#endif
