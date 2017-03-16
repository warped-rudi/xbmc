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

#pragma once

#if defined(HAS_MARVELL_DOVE)
#include "DVDVideoCodec.h"

#include "DllVMETA.h"

#include "utils/FastFifo.h"
#include "threads/SingleLock.h"

#define STREAM_FIFO_SIZE        16      // most efficient, when a power of 2
#define PICTURE_FIFO_SIZE       32      // most efficient, when a power of 2

#include <list>

class CBitstreamConverter;
class DllLibMiscGen;
class DllLibVMETA;

class CDVDVideoCodecVMETA : public CDVDVideoCodec
{
public:
  CDVDVideoCodecVMETA();
  ~CDVDVideoCodecVMETA();

  // Required overrides
  bool Open(CDVDStreamInfo &hints, CDVDCodecOptions &options);
  void Dispose(void);
  int  Decode(uint8_t *pData, int iSize, double dts, double pts);
  void Reset(void);
  void SetDropState(bool bDrop);
  bool GetPicture(DVDVideoPicture *pDvdVideoPicture);
  bool ClearPicture(DVDVideoPicture *pDvdVideoPicture);
  const char *GetName() { return m_video_codec_name.c_str(); };

private:
  IppCodecStatus SendCodecConfig();
  IppCodecStatus DecodeInternal();

  uint8_t *digest_mpeg4_inbuf(uint8_t *pData, int iSize);
  uint8_t *digest_mpeg12_inbuf(uint32_t codecId, uint8_t *pData, int iSize);
  inline bool digest_vc1_inbuf(uint8_t *pData, int iSize);

protected:
  // Video format
  bool                            m_drop_state;
  unsigned int                    m_display_width;
  unsigned int                    m_display_height;
  unsigned int                    m_picture_width;
  unsigned int                    m_picture_height;
  bool                            m_is_open;
  uint8_t                         *m_extradata;
  unsigned int                    m_extrasize;
  CBitstreamConverter             *m_converter;
  bool                            m_video_convert;
  std::string                     m_video_codec_name;

  MiscGeneralCallbackTable        *m_pCbTable;
  IppVmetaDecParSet               m_VDecParSet;
  IppVmetaDecInfo                 m_VDecInfo;
  void                            *m_pDecState;

  std::list<IppVmetaBitstream*>   m_input_buffers;
  std::list<IppVmetaPicture*>     m_output_buffers;

  FastFiFo<IppVmetaBitstream*, STREAM_FIFO_SIZE> m_input_available;
  FastFiFo<IppVmetaBitstream*, STREAM_FIFO_SIZE> m_input_ready;

  FastFiFo<IppVmetaPicture*, PICTURE_FIFO_SIZE>  m_output_available;
  FastFiFo<IppVmetaPicture*, PICTURE_FIFO_SIZE>  m_output_ready;

  FastFiFo<double, PICTURE_FIFO_SIZE>            m_pts_queue;

  unsigned int                    m_frame_no;
  unsigned int                    m_numPicBufSubmitted;
  unsigned int                    m_numStrmBufSubmitted;

  int                             m_itime_inc_bits;
  int                             m_low_delay;
  int                             m_codec_species;

  CSingleLock                     m_HwLock;
  DllLibVMETA                     &m_DllVMETA;
  DllLibMiscGen                   &m_DllMiscGen;
};

#endif
