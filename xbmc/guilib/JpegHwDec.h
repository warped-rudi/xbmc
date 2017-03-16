/*
*      Copyright (C) 2005-2016 Team XBMC
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

#ifndef GUILIB_JPEGHWDEC_H
#define GUILIB_JPEGHWDEC_H

class CJpegHwDec
{
protected:
  CJpegHwDec() {};
  virtual ~CJpegHwDec() {}
  
protected:
   /*!
   \brief Initialize this instance
   */
  virtual bool Init() { return true; }

  /*!
   \brief Deinitialize this instance
   */
  virtual void Dispose() {};

public:
  /*!
   \brief Return the minimum supported nominator for downscaling while decoding
   \return The minimal nominator
   */
  virtual unsigned int FirstScale() { return 1; }
  
  /*!
   \brief Return the next or previous nominator for downsacling while decoding
   \param currScale The current nominator 
   \param direction +1 for next, -1 for previous, otheres reserved
   \return The next or previous nominator
   */
  virtual unsigned int NextScale(unsigned int currScale, 
                                 int direction) { return currScale + direction; }

  /*!
   \brief (Re-)allocate input buffer
   \param buffer Pointer to current input buffer, can be zero
   \param size Requested size of the new input buffer 
   \return Pointer a new input buffer of requested size
   */
  virtual unsigned char *ReallocBuffer(unsigned char *buffer, unsigned int size);

  /*!
   \brief Free input buffer
   \param buffer Pointer to current input buffer
   */
  virtual void FreeBuffer(unsigned char *buffer);

  /*!
   \brief Prepare input buffer for use by hardware
   \param numBytes Number of valid bytes in input buffer
   */
  virtual void PrepareBuffer(unsigned int numBytes);

  /*!
   \brief Check, if hardware decoding should be used
   \param width The width of the image to be decoded
   \param height The height of the image to be decoded
   \param featureFlags A combination of ffXXXX enums
   \return true if the hardware can/should handle this image
   */
  virtual bool CanDecode(unsigned int featureFlags, 
                         unsigned int width, unsigned int height) const { return false; }
  enum { ffForceFallback = 0x01, ffProgressive = 0x02, ffArithmeticCoding = 0x04 };
                                   
  /*!
   \brief Decode the image
   \param buffer Pointer to output buffer, size must be >= maxHeight * pitch
   \param pitch Length of a line (in bytes) in the output buffer
   \param format Output image format (currently XB_FMT_RGB8 or XB_FMT_A8R8G8B8)
   \param maxWidth The maximum width of the output image
   \param maxHeight The maximum height of the output image
   \return true if image was decoded successfully
   */
  virtual bool Decode(unsigned char *dst, 
                      unsigned int pitch, unsigned int format,
                      unsigned int maxWidth, unsigned int maxHeight,
                      unsigned int scaleNum, unsigned int scaleDenom) { return false; }

  /*!
   \brief Fabricate an instance. Modify this when adding new hardware.
   \return Pointer to new CJpegHwDec instance
   */
  static CJpegHwDec  *create();

  /*!
   \brief Destruction of an instance
   \param d Pointer to CJpegHwDec instance, can be 0
   */
  static void        destroy(CJpegHwDec *d);
  
  /*!
   \brief Load a file into the hardware buffer
   \param filename Name of the file to load
   \param buffer Receives a buffer that is usable by the hardware decoder
   \return actual size or 0 in case of error
   */
  ssize_t LoadFile(const std::string &filename, unsigned char **buffer);

  /*!
   \brief Copy data into the hardware buffer
   \param src Pointer to source data
   \param size Size of source data
   \param buffer Receives a buffer that is usable by the hardware decoder
   \return actual size or 0 in case of error
   */
  ssize_t LoadBuffer(const void *src, unsigned int size, unsigned char **buffer);
};

#endif
