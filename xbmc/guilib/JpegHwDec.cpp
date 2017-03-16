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

#if (defined HAVE_CONFIG_H) && (!defined WIN32)
  #include "config.h"
#elif defined(_WIN32)
  #include "system.h"
#endif

#include "settings/Settings.h"
#include "filesystem/File.h"
#include "URL.h"
#include "utils/log.h"
#include "JpegHwDec.h"

#if defined(HAS_MARVELL_DOVE)
#include "JpegHwDecVMETA.h"
#endif


unsigned char *CJpegHwDec::ReallocBuffer(unsigned char *buffer, unsigned int size)
{
  return (unsigned char *)::realloc(buffer, size);
}

void CJpegHwDec::FreeBuffer(unsigned char *buffer)
{
  if (buffer)
    ::free(buffer);
}

void CJpegHwDec::PrepareBuffer(unsigned int numBytes)
{
  (void)numBytes;
}

CJpegHwDec *CJpegHwDec::create()
{
#if defined(HAS_MARVELL_DOVE)
#if 0
  if (CSettings::GetInstance().GetBool("videoscreen.use_hardware_jpeg"))
#endif
  {
    CJpegHwDec *d = new CJpegHwDecVMeta();

    if (d && d->Init())
      return d;

    delete d;
  }
#endif

  return new CJpegHwDec();
}

void CJpegHwDec::destroy(CJpegHwDec *d)
{
  if (d)
  {
    d->Dispose();
    delete d;
  }
}

ssize_t CJpegHwDec::LoadFile(const std::string &filename, unsigned char **buffer)
{
  XFILE::CFile file;
  static const size_t max_file_size = 32768 * 1024U;
  static const size_t min_chunk_size = 64 * 1024U;
  static const size_t max_chunk_size = 2048 * 1024U;
  
  *buffer = 0;

  if (!file.Open(CURL(filename), READ_TRUNCATED))
    return 0;
  
  int64_t filesize = file.GetLength();
  if (filesize > (int64_t)max_file_size)
    return 0; /* file is too large for this function */

  size_t chunksize = std::min(min_chunk_size, (size_t)filesize + 1);
  size_t totalsize = 0;
  
  do
  {
    *buffer = ReallocBuffer(*buffer, totalsize + chunksize);
    if (*buffer == 0)
    {
      CLog::Log(LOGERROR, 
                "%s unable to allocate buffer of size %u", 
                __FUNCTION__, totalsize + chunksize);
      return 0;
    }

    ssize_t read = file.Read(*buffer + totalsize, chunksize);
    if (read < 0)
    {
      FreeBuffer(*buffer);
      *buffer = 0;
      return 0;
    }
    
    totalsize += read;
    read -= chunksize;
    
    if (chunksize < max_chunk_size)
      chunksize *= 2;

  } while (read < 0);

  PrepareBuffer(totalsize);

  return totalsize;
}

ssize_t CJpegHwDec::LoadBuffer(const void *src, unsigned int size, unsigned char **buffer)
{
    *buffer = ReallocBuffer(0, size);
    
    if (*buffer == 0)
    {
      CLog::Log(LOGERROR, "%s unable to allocate buffer of size %u", __FUNCTION__, size);
      return 0;
    }

    memcpy(*buffer, src, size);

    PrepareBuffer(size);

    return size;
}
