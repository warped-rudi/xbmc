/*
 *      Copyright (C) 2005-2012 Team XBMC
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

#include "FLACcodec.h"
#include "utils/log.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#include "music/tags/TagLoaderTagLib.h"

#define NUM_FRAMES	3

FLACCodec::FLACCodec()
{
  m_SampleRate = 0;
  m_Channels = 0;
  m_BitsPerSample = 0;
  m_DataFormat = AE_FMT_INVALID;
  m_TotalTime=0;
  m_Bitrate = 0;
  m_CodecName = "flac";

  m_pFlacDecoder=NULL;

  m_pBuffer=NULL;
  m_BufferSize=0;
  m_BufferHead=0;
  m_BufferTail=0;
  m_MaxFrameSize=0;
}

FLACCodec::~FLACCodec()
{
  DeInit();
}

bool FLACCodec::Init(const CStdString &strFile, unsigned int filecache)
{
  if (!m_dll.Load())
    return false;

  if (!m_file.Open(strFile, READ_CACHED))
    return false;

  //  Extract ReplayGain info
  CTagLoaderTagLib tagLoaderTagLib;
  tagLoaderTagLib.Load(strFile, m_tag);

  m_pFlacDecoder=m_dll.FLAC__stream_decoder_new();

  if (!m_pFlacDecoder)
  {
    CLog::Log(LOGERROR, "FLACCodec: Error creating decoder");
    return false;
  }

  if (m_dll.FLAC__stream_decoder_init_stream(m_pFlacDecoder, DecoderReadCallback,
                                                             DecoderSeekCallback,
                                                             DecoderTellCallback,
                                                             DecoderLengthCallback,
                                                             DecoderEofCallback,
                                                             DecoderWriteCallback,
                                                             DecoderMetadataCallback,
                                                             DecoderErrorCallback,
                                                             this) != FLAC__STREAM_DECODER_INIT_STATUS_OK)
  {
    CLog::Log(LOGERROR, "FLACCodec: Error initializing decoder");
    FreeDecoder();
    return false;
  }

  //  Process metadata like number of channels...
  if (!m_dll.FLAC__stream_decoder_process_until_end_of_metadata(m_pFlacDecoder))
  {
    CLog::Log(LOGERROR, "FLACCodec: Error while processing metadata");
    FreeDecoder();
    return false;
  }

  //  These are filled by the metadata callback
  if (m_SampleRate==0 || m_Channels==0 || m_BitsPerSample==0 || m_TotalTime==0 || m_MaxFrameSize==0 || m_DataFormat == AE_FMT_INVALID)
  {
    CLog::Log(LOGERROR, "FLACCodec: Can't get stream info, SampleRate=%i, Channels=%i, BitsPerSample=%i, TotalTime=%"PRIu64", MaxFrameSize=%i", m_SampleRate, m_Channels, m_BitsPerSample, m_TotalTime, m_MaxFrameSize);
    FreeDecoder();
    return false;
  }

  m_Bitrate = (int)(((float)m_file.GetLength() * 8) / ((float)m_TotalTime / 1000));

  if (m_pBuffer)
  {
    delete[] m_pBuffer;
    m_pBuffer=NULL;
  }

  //  allocate the buffer to hold the audio data,
  //  it is (NUM_FRAMES + 1) times bigger then a single decoded frame
  m_BufferSize = m_MaxFrameSize * (NUM_FRAMES + 1);
  m_pBuffer = new BYTE[m_BufferSize];
  m_BufferHead = 0;
  m_BufferTail = 0;

  return true;
}

void FLACCodec::DeInit()
{
  FreeDecoder();
  m_file.Close();

  if (m_pBuffer)
  {
    delete[] m_pBuffer;
    m_pBuffer=NULL;
  }
}

int64_t FLACCodec::Seek(int64_t iSeekTime)
{
  //  Seek to the nearest sample
  // set the buffer size to 0 first, as this invokes a WriteCallback which
  // may be called when the buffer is almost full (resulting in a buffer
  // overrun unless we reset m_BufferSize first).
  m_BufferHead = m_BufferTail;
  if(!m_dll.FLAC__stream_decoder_seek_absolute(m_pFlacDecoder, (int64_t)(iSeekTime*m_SampleRate)/1000))
    CLog::Log(LOGERROR, "FLACCodec::Seek - failed to seek");

  if(m_dll.FLAC__stream_decoder_get_state(m_pFlacDecoder)==FLAC__STREAM_DECODER_SEEK_ERROR)
  {
    CLog::Log(LOGINFO, "FLACCodec::Seek - must reset decoder after seek");
    if(!m_dll.FLAC__stream_decoder_flush(m_pFlacDecoder))
      CLog::Log(LOGERROR, "FLACCodec::Seek - flush failed");
  }

  return iSeekTime;
}

int FLACCodec::ReadPCM(BYTE *pBuffer, int size, int *actualsize)
{
  //  fill our buffer NUM_FRAMES decoded frame (the buffer could hold NUM_FRAMES + 1)
  while(BufferFillCount() < m_MaxFrameSize * NUM_FRAMES &&
          m_dll.FLAC__stream_decoder_get_state(m_pFlacDecoder)!=FLAC__STREAM_DECODER_END_OF_STREAM)
  {
    if (!m_dll.FLAC__stream_decoder_process_single(m_pFlacDecoder))
    {
      CLog::Log(LOGERROR, "FLACCodec: Error decoding single block");
      *actualsize=0;
      return READ_ERROR;
    }
  }

  BYTE *pTemp = pBuffer;
  while (size > 0)
  {
    int portion = BufferGetCount();

    if (portion <= 0)
      break;

    if (portion > size)
      portion = size;

    memcpy(pTemp, m_pBuffer + m_BufferHead, portion);
    pTemp += portion;
    size -= portion;
    m_BufferHead = (m_BufferHead + portion) % m_BufferSize;
  }
  *actualsize = pTemp - pBuffer;

  return (m_BufferHead == m_BufferTail &&
          m_dll.FLAC__stream_decoder_get_state(m_pFlacDecoder)==FLAC__STREAM_DECODER_END_OF_STREAM)
          ?  READ_EOF : READ_SUCCESS;
}

bool FLACCodec::CanInit()
{
  return m_dll.CanLoad();
}

void FLACCodec::FreeDecoder()
{
  if (m_pFlacDecoder)
  {
    m_dll.FLAC__stream_decoder_finish(m_pFlacDecoder);
    m_dll.FLAC__stream_decoder_delete(m_pFlacDecoder);
    m_pFlacDecoder=NULL;
  }
}

FLAC__StreamDecoderReadStatus FLACCodec::DecoderReadCallback(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes, void *client_data)
{
  FLACCodec* pThis=(FLACCodec*)client_data;
  if (!pThis)
    return FLAC__STREAM_DECODER_READ_STATUS_ABORT;

  *bytes=pThis->m_file.Read(buffer, *bytes);

  return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

FLAC__StreamDecoderSeekStatus FLACCodec::DecoderSeekCallback(const FLAC__StreamDecoder *decoder, FLAC__uint64 absolute_byte_offset, void *client_data)
{
  FLACCodec* pThis=(FLACCodec*)client_data;
  if (!pThis)
    return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;

  if (pThis->m_file.Seek(absolute_byte_offset, SEEK_SET)<0)
    return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;


  return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

FLAC__StreamDecoderTellStatus FLACCodec::DecoderTellCallback(const FLAC__StreamDecoder *decoder, FLAC__uint64 *absolute_byte_offset, void *client_data)
{
  FLACCodec* pThis=(FLACCodec*)client_data;
  if (!pThis)
    return FLAC__STREAM_DECODER_TELL_STATUS_ERROR;

  *absolute_byte_offset=pThis->m_file.GetPosition();

  return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

FLAC__StreamDecoderLengthStatus FLACCodec::DecoderLengthCallback(const FLAC__StreamDecoder *decoder, FLAC__uint64 *stream_length, void *client_data)
{
  FLACCodec* pThis=(FLACCodec*)client_data;
  if (!pThis)
    return FLAC__STREAM_DECODER_LENGTH_STATUS_ERROR;

  *stream_length=pThis->m_file.GetLength();

  return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

FLAC__bool FLACCodec::DecoderEofCallback(const FLAC__StreamDecoder *decoder, void *client_data)
{
  FLACCodec* pThis=(FLACCodec*)client_data;
  if (!pThis)
    return true;

  return (pThis->m_file.GetLength()==pThis->m_file.GetPosition());
}

FLAC__StreamDecoderWriteStatus FLACCodec::DecoderWriteCallback(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame, const FLAC__int32 * const buffer[], void *client_data)
{
  FLACCodec* pThis=(FLACCodec*)client_data;
  if (!pThis)
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;

  const int bytes_per_sample = frame->header.bits_per_sample/8;
  BYTE* outptr = pThis->m_pBuffer + pThis->m_BufferTail;
  int outcnt = 0, outlen = pThis->BufferPutCount();

  #ifdef __BIG_ENDIAN__
  int endian_shift = 32 - frame->header.bits_per_sample;
  #endif

  for(unsigned int current_sample = 0; current_sample < frame->header.blocksize; current_sample++)
  {
    for(unsigned int channel = 0; channel < frame->header.channels; channel++)
    {
      FLAC__int32 v = buffer[channel][current_sample];

      // make 1 byte samples unsigned
      if (bytes_per_sample == 1) 
        v ^= 0x80;

      #ifdef __BIG_ENDIAN__
      v <<= endian_shift;
      #endif

      if (outlen - outcnt >= 4)
      {
        *((FLAC__int32 *)outptr) = v;

        outptr += bytes_per_sample;
        outcnt += bytes_per_sample;

        if (outcnt >= outlen)
        {
          pThis->m_BufferTail = (pThis->m_BufferTail + outcnt) % pThis->m_BufferSize;
          outcnt = 0;
          outlen = pThis->BufferPutCount();
          outptr = pThis->m_pBuffer + pThis->m_BufferTail;
        }
        
        continue;
      }

      for(int byte_no = 0; byte_no < bytes_per_sample; byte_no++)
      {
        #ifdef __BIG_ENDIAN__
        *outptr++ = (BYTE)(v >> 24);
        v <<= 8;
        #else
        *outptr++ = (BYTE)v;
        v >>= 8;
        #endif

        if (++outcnt >= outlen)
        {
          pThis->m_BufferTail = (pThis->m_BufferTail + outcnt) % pThis->m_BufferSize;
          outcnt = 0;
          outlen = pThis->BufferPutCount();
          outptr = pThis->m_pBuffer + pThis->m_BufferTail;
        }
      }
    }
  }

  pThis->m_BufferTail = (pThis->m_BufferTail + outcnt) % pThis->m_BufferSize;

  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void FLACCodec::DecoderMetadataCallback(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data)
{
  FLACCodec* pThis=(FLACCodec*)client_data;
  if (!pThis)
    return;

  if (metadata->type==FLAC__METADATA_TYPE_STREAMINFO)
  {
    static enum AEChannel map[6][7] = {
      {AE_CH_FC, AE_CH_NULL},
      {AE_CH_FL, AE_CH_FR, AE_CH_NULL},
      {AE_CH_FL, AE_CH_FR, AE_CH_FC, AE_CH_NULL},
      {AE_CH_FL, AE_CH_FR, AE_CH_BL, AE_CH_BR, AE_CH_NULL},
      {AE_CH_FL, AE_CH_FR, AE_CH_FC, AE_CH_BL, AE_CH_BR, AE_CH_NULL},
      {AE_CH_FL, AE_CH_FR, AE_CH_FC, AE_CH_LFE, AE_CH_BL, AE_CH_BR, AE_CH_NULL}
    };

    /* channel counts greater then 6 are undefined */
    if (metadata->data.stream_info.channels > 6)
      pThis->m_ChannelInfo = CAEUtil::GuessChLayout(metadata->data.stream_info.channels);
    else
      pThis->m_ChannelInfo = CAEChannelInfo(map[metadata->data.stream_info.channels - 1]);

    pThis->m_SampleRate    = metadata->data.stream_info.sample_rate;
    pThis->m_Channels      = metadata->data.stream_info.channels;
    pThis->m_BitsPerSample = metadata->data.stream_info.bits_per_sample;
    switch(pThis->m_BitsPerSample)
    {
      case  8: pThis->m_DataFormat = AE_FMT_U8;     break;
      case 16: pThis->m_DataFormat = AE_FMT_S16NE;  break;
      case 24: pThis->m_DataFormat = AE_FMT_S24NE3; break;
      case 32: pThis->m_DataFormat = AE_FMT_FLOAT;  break;
    }
    pThis->m_TotalTime     = (int64_t)metadata->data.stream_info.total_samples * 1000 / metadata->data.stream_info.sample_rate;
    pThis->m_MaxFrameSize  = metadata->data.stream_info.max_blocksize*(pThis->m_BitsPerSample/8)*pThis->m_Channels;
  }
}

void FLACCodec::DecoderErrorCallback(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data)
{
  CLog::Log(LOGERROR, "FLACCodec: Read error %i", status);
}
