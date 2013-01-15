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

#include "XRandR.h"

#ifdef HAS_XRANDR

#include <string.h>
#include <sys/wait.h>
#include "system.h"
#include "PlatformInclude.h"
#ifdef TARGET_MARVELL_DOVE
#include <sys/mman.h>
#include "guilib/Resolution.h"
#include "settings/GUISettings.h"
#endif
#include "utils/XBMCTinyXML.h"
#include "../xbmc/utils/log.h"

#if defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/wait.h>
#endif

using namespace std;

CXRandR::CXRandR(bool query)
{
  m_bInit = false;
  if (query)
    Query();
}

bool CXRandR::Query(bool force)
{
  if (!force)
    if (m_bInit)
      return m_outputs.size() > 0;

  m_bInit = true;

  if (getenv("XBMC_BIN_HOME") == NULL)
    return false;

  m_outputs.clear();
  m_current.clear();

  CStdString cmd;
  cmd  = getenv("XBMC_BIN_HOME");
  cmd += "/xbmc-xrandr";

  FILE* file = popen(cmd.c_str(),"r");
  if (!file)
  {
    CLog::Log(LOGERROR, "CXRandR::Query - unable to execute xrandr tool");
    return false;
  }


  CXBMCTinyXML xmlDoc;
  if (!xmlDoc.LoadFile(file, TIXML_DEFAULT_ENCODING))
  {
    CLog::Log(LOGERROR, "CXRandR::Query - unable to open xrandr xml");
    pclose(file);
    return false;
  }
  pclose(file);

  TiXmlElement *pRootElement = xmlDoc.RootElement();
  if (strcasecmp(pRootElement->Value(), "screen") != 0)
  {
    // TODO ERROR
    return false;
  }

  for (TiXmlElement* output = pRootElement->FirstChildElement("output"); output; output = output->NextSiblingElement("output"))
  {
    XOutput xoutput;
    xoutput.name = output->Attribute("name");
    xoutput.name.TrimLeft(" \n\r\t");
    xoutput.name.TrimRight(" \n\r\t");
    xoutput.isConnected = (strcasecmp(output->Attribute("connected"), "true") == 0);
    xoutput.w = (output->Attribute("w") != NULL ? atoi(output->Attribute("w")) : 0);
    xoutput.h = (output->Attribute("h") != NULL ? atoi(output->Attribute("h")) : 0);
    xoutput.x = (output->Attribute("x") != NULL ? atoi(output->Attribute("x")) : 0);
    xoutput.y = (output->Attribute("y") != NULL ? atoi(output->Attribute("y")) : 0);
    xoutput.wmm = (output->Attribute("wmm") != NULL ? atoi(output->Attribute("wmm")) : 0);
    xoutput.hmm = (output->Attribute("hmm") != NULL ? atoi(output->Attribute("hmm")) : 0);

    if (!xoutput.isConnected)
       continue;

    bool hascurrent = false;
    for (TiXmlElement* mode = output->FirstChildElement("mode"); mode; mode = mode->NextSiblingElement("mode"))
    {
      XMode xmode;
      xmode.id = mode->Attribute("id");
      xmode.name = mode->Attribute("name");
      xmode.hz = atof(mode->Attribute("hz"));
      xmode.w = atoi(mode->Attribute("w"));
      xmode.h = atoi(mode->Attribute("h"));
      xmode.isPreferred = (strcasecmp(mode->Attribute("preferred"), "true") == 0);
      xmode.isCurrent = (strcasecmp(mode->Attribute("current"), "true") == 0);
      xoutput.modes.push_back(xmode);
      if (xmode.isCurrent)
      {
        m_current.push_back(xoutput);
        hascurrent = true;
      }
    }
    if (hascurrent)
      m_outputs.push_back(xoutput);
    else
      CLog::Log(LOGWARNING, "CXRandR::Query - output %s has no current mode, assuming disconnected", xoutput.name.c_str());
  }
  return m_outputs.size() > 0;
}

std::vector<XOutput> CXRandR::GetModes(void)
{
  Query();
  return m_outputs;
}

void CXRandR::SaveState()
{
  Query(true);
}

void CXRandR::RestoreState()
{
  vector<XOutput>::iterator outiter;
  for (outiter=m_current.begin() ; outiter!=m_current.end() ; outiter++)
  {
    vector<XMode> modes = (*outiter).modes;
    vector<XMode>::iterator modeiter;
    for (modeiter=modes.begin() ; modeiter!=modes.end() ; modeiter++)
    {
      XMode mode = *modeiter;
      if (mode.isCurrent)
      {
        SetMode(*outiter, mode);
        return;
      }
    }
  }
}

#ifdef TARGET_MARVELL_DOVE
#define MAP_SIZE 4096UL
#define MAP_MASK (MAP_SIZE - 1)
void CXRandR::ChangeGraphicsScaler(void)
{
  /* OK. Now this is a seriously ugly hack. Code needs to be re-written to support ioctl to driver !!! */
  /* Code below originally from devmem2.c */
  int fd;
  off_t target_page = 0xf1820000; /* LCD Controller base address */
  void *map_base;
  unsigned int zoomed;
  unsigned int gr_size;
  int zx,zy;
  GRAPHICS_SCALING scale = (GRAPHICS_SCALING) g_guiSettings.GetInt("videoscreen.graphics_scaling");
  if (scale == -1) scale=GR_SCALE_100;
  if((fd = open("/dev/mem", O_RDWR | O_SYNC)) == -1) {
        CLog::Log(LOGERROR, "XRANDR: Unable to open /dev/mem");
	return;
  }
  fflush(stdout);
  map_base = mmap(0, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, target_page);
  zoomed = * (unsigned int *) (map_base + 0x108);
  zx = zoomed & 0xffff;
  zy = (zoomed & 0xffff0000) >> 16;
  CLog::Log(LOGINFO, "XRANDR: Zoomed area is %dx%d, new area should be %dx%d\n",zx,zy,zx*100/scale,zy*100/scale);
  gr_size = (zy*100/scale) & 0xffff;
  gr_size = (gr_size << 16) | ((zx*100/scale) & 0xffff);
  * (unsigned int *) (map_base + 0x104) = gr_size;
  close(fd);
}
#endif

bool CXRandR::SetMode(XOutput output, XMode mode)
{
#ifdef TARGET_MARVELL_DOVE
  /* Required both on entrance and exit from SetMode. First entrance is required to get the scaler modified */
  ChangeGraphicsScaler ();
#endif
  if ((output.name == m_currentOutput && mode.id == m_currentMode) || (output.name == "" && mode.id == ""))
    return true;

  Query();

  // Make sure the output exists, if not -- complain and exit
  bool isOutputFound = false;
  XOutput outputFound;
  for (size_t i = 0; i < m_outputs.size(); i++)
  {
    if (m_outputs[i].name == output.name)
    {
      isOutputFound = true;
      outputFound = m_outputs[i];
    }
  }

  if (!isOutputFound)
  {
    CLog::Log(LOGERROR, "CXRandR::SetMode: asked to change resolution for non existing output: %s mode: %s", output.name.c_str(), mode.id.c_str());
    return false;
  }

  // try to find the same exact mode (same id, resolution, hz)
  bool isModeFound = false;
  XMode modeFound;
  for (size_t i = 0; i < outputFound.modes.size(); i++)
  {
    if (outputFound.modes[i].id == mode.id)
    {
      if (outputFound.modes[i].w == mode.w &&
          outputFound.modes[i].h == mode.h &&
          outputFound.modes[i].hz == mode.hz)
      {
        isModeFound = true;
        modeFound = outputFound.modes[i];
      }
      else
      {
        CLog::Log(LOGERROR, "CXRandR::SetMode: asked to change resolution for mode that exists but with different w/h/hz: %s mode: %s. Searching for similar modes...", output.name.c_str(), mode.id.c_str());
        break;
      }
    }
  }

  if (!isModeFound)
  {
    for (size_t i = 0; i < outputFound.modes.size(); i++)
    {
      if (outputFound.modes[i].w == mode.w &&
          outputFound.modes[i].h == mode.h &&
          outputFound.modes[i].hz == mode.hz)
      {
        isModeFound = true;
        modeFound = outputFound.modes[i];
        CLog::Log(LOGWARNING, "CXRandR::SetMode: found alternative mode (same hz): %s mode: %s.", output.name.c_str(), outputFound.modes[i].id.c_str());
      }
    }
  }

  if (!isModeFound)
  {
    for (size_t i = 0; i < outputFound.modes.size(); i++)
    {
      if (outputFound.modes[i].w == mode.w &&
          outputFound.modes[i].h == mode.h)
      {
        isModeFound = true;
        modeFound = outputFound.modes[i];
        CLog::Log(LOGWARNING, "CXRandR::SetMode: found alternative mode (different hz): %s mode: %s.", output.name.c_str(), outputFound.modes[i].id.c_str());
      }
    }
  }

  // Let's try finding a mode that is the same
  if (!isModeFound)
  {
    CLog::Log(LOGERROR, "CXRandR::SetMode: asked to change resolution for non existing mode: %s mode: %s", output.name.c_str(), mode.id.c_str());
    return false;
  }

  m_currentOutput = outputFound.name;
  m_currentMode = modeFound.id;
  char cmd[255];
  if (getenv("XBMC_BIN_HOME"))
    snprintf(cmd, sizeof(cmd), "%s/xbmc-xrandr --output %s --mode %s", getenv("XBMC_BIN_HOME"), outputFound.name.c_str(), modeFound.id.c_str());
  else
    return false;
  CLog::Log(LOGINFO, "XRANDR: %s", cmd);
  int status = system(cmd);
  if (status == -1)
    return false;

  if (WEXITSTATUS(status) != 0)
    return false;

#ifdef TARGET_MARVELL_DOVE
  ChangeGraphicsScaler ();
#endif
  return true;
}

XOutput CXRandR::GetCurrentOutput()
{
  Query();
  for (unsigned int j = 0; j < m_outputs.size(); j++)
  {
    if(m_outputs[j].isConnected)
      return m_outputs[j];
  }
  XOutput empty;
  return empty;
}
XMode CXRandR::GetCurrentMode(CStdString outputName)
{
  Query();
  XMode result;

  for (unsigned int j = 0; j < m_outputs.size(); j++)
  {
    if (m_outputs[j].name == outputName || outputName == "")
    {
      for (unsigned int i = 0; i < m_outputs[j].modes.size(); i++)
      {
        if (m_outputs[j].modes[i].isCurrent)
        {
          result = m_outputs[j].modes[i];
          break;
        }
      }
    }
  }

  return result;
}

void CXRandR::LoadCustomModeLinesToAllOutputs(void)
{
  Query();
  CXBMCTinyXML xmlDoc;

  if (!xmlDoc.LoadFile("special://xbmc/userdata/ModeLines.xml"))
  {
    return;
  }

  TiXmlElement *pRootElement = xmlDoc.RootElement();
  if (strcasecmp(pRootElement->Value(), "modelines") != 0)
  {
    // TODO ERROR
    return;
  }

  char cmd[255];
  CStdString name;
  CStdString strModeLine;

  for (TiXmlElement* modeline = pRootElement->FirstChildElement("modeline"); modeline; modeline = modeline->NextSiblingElement("modeline"))
  {
    name = modeline->Attribute("label");
    name.TrimLeft(" \n\t\r");
    name.TrimRight(" \n\t\r");
    strModeLine = modeline->FirstChild()->Value();
    strModeLine.TrimLeft(" \n\t\r");
    strModeLine.TrimRight(" \n\t\r");
    if (getenv("XBMC_BIN_HOME"))
    {
      snprintf(cmd, sizeof(cmd), "%s/xbmc-xrandr --newmode \"%s\" %s > /dev/null 2>&1", getenv("XBMC_BIN_HOME"),
               name.c_str(), strModeLine.c_str());
      if (system(cmd) != 0)
        CLog::Log(LOGERROR, "Unable to create modeline \"%s\"", name.c_str());
    }

    for (unsigned int i = 0; i < m_outputs.size(); i++)
    {
      if (getenv("XBMC_BIN_HOME"))
      {
        snprintf(cmd, sizeof(cmd), "%s/xbmc-xrandr --addmode %s \"%s\"  > /dev/null 2>&1", getenv("XBMC_BIN_HOME"),
                 m_outputs[i].name.c_str(), name.c_str());
        if (system(cmd) != 0)
          CLog::Log(LOGERROR, "Unable to add modeline \"%s\"", name.c_str());
      }
    }
  }
}

CXRandR g_xrandr;

#endif // HAS_XRANDR

/*
  int main()
  {
  CXRandR r;
  r.LoadCustomModeLinesToAllOutputs();
  }
*/
