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

#if (defined HAVE_CONFIG_H) && (!defined WIN32)
  #include "config.h"
#elif defined(_WIN32)
  #include "system.h"
#endif

#include "settings/GUISettings.h"
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
  if (g_guiSettings.GetBool("videoscreen.use_hardware_jpeg"))
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

