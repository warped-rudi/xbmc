/*
*      Copyright (C) 2005-2014 Team XBMC
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

#ifndef GUILIB_JPEGHWDECVMETA_H
#define GUILIB_JPEGHWDECVMETA_H

#include "JpegIO.h"
#include "JpegHwDec.h"
#include "threads/SingleLock.h"

#include "cores/dvdplayer/DVDCodecs/Video/DllVMETA.h"

class CJpegHwDecVMeta : public CJpegHwDec
{
  CSingleTryLock                m_HwLock;
  DllLibVMETA                   &m_DllVMETA;
  DllLibMiscGen                 &m_DllMiscGen;

  IppVmetaBitstream             m_input;
  IppVmetaPicture               m_picture;

  MiscGeneralCallbackTable      *m_pCbTable;
  void                          *m_pDecState;
  IppVmetaDecParSet             m_VDecParSet;

public:
  CJpegHwDecVMeta()
  : CJpegHwDec(), 
    m_HwLock(g_CritSecVMETA),
    m_DllVMETA(g_DllLibVMETA),
    m_DllMiscGen(g_DllLibMiscGen)  {}

protected:
  virtual bool Init();
  virtual void Dispose();

  virtual unsigned int  FirstScale();
  virtual unsigned int  NextScale(unsigned int currScale, int direction);

  virtual unsigned char *ReallocBuffer(unsigned char *buffer, unsigned int size);
  virtual void          FreeBuffer(unsigned char *buffer);
  virtual void          PrepareBuffer(unsigned int numBytes);

  virtual bool          CanDecode(unsigned int featureFlags,
                                  unsigned int width, unsigned int height) const;
  virtual bool          Decode(unsigned char *dst, 
                               unsigned int pitch, unsigned int format,
                               unsigned int maxWidth, unsigned int maxHeight,
                               unsigned int scaleNum, unsigned int scaleDenom);

private:
  enum { StreamBufAlloc = 128 * 1024, StreamBufLimit = 2047 * 1024 };
  enum ReturnMode { ReleaseNothing = 0x00, ReleaseStorage = 0x01, ReleaseBuffer = 0x02, ReleaseAll = 0x03 };

  int  DecodePopBuffers(IppVmetaBufferType type, ReturnMode mode, int maxCount = -1);
  bool DecodePicture(unsigned int maxWidth, unsigned int maxHeight, unsigned int scaleDivider);

  void ToBGRA(unsigned char *dst, unsigned int pitch, unsigned int width, unsigned int height);
  void ToRGB(unsigned char *dst, unsigned int pitch, unsigned int width, unsigned int height);
};


#endif
