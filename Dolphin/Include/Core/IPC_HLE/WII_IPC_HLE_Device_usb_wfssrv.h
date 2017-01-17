// Copyright 2016 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Core/IPC_HLE/WII_IPC_HLE.h"
#include "Core/IPC_HLE/WII_IPC_HLE_Device.h"
#include "DiscIO/Volume.h"

namespace WFS
{
std::string NativePath(const std::string& wfs_path);
}

class CWII_IPC_HLE_Device_usb_wfssrv : public IWII_IPC_HLE_Device
{
public:
  CWII_IPC_HLE_Device_usb_wfssrv(u32 device_id, const std::string& device_name);

  IPCCommandResult IOCtl(u32 command_address) override;
  IPCCommandResult IOCtlV(u32 command_address) override;

private:
  // WFS device name, e.g. msc01/msc02.
  std::string m_device_name;

  enum
  {
    IOCTL_WFS_INIT = 0x02,
    IOCTL_WFS_DEVICE_INFO = 0x04,
    IOCTL_WFS_GET_DEVICE_NAME = 0x05,
    IOCTL_WFS_FLUSH = 0x0a,
    IOCTL_WFS_GLOB_START = 0x0d,
    IOCTL_WFS_GLOB_NEXT = 0x0e,
    IOCTL_WFS_GLOB_END = 0x0f,
    IOCTL_WFS_SET_HOMEDIR = 0x10,
    IOCTL_WFS_CHDIR = 0x11,
    IOCTL_WFS_GET_HOMEDIR = 0x12,
    IOCTL_WFS_GETCWD = 0x13,
    IOCTL_WFS_DELETE = 0x15,
    IOCTL_WFS_GET_ATTRIBUTES = 0x17,
    IOCTL_WFS_OPEN = 0x1A,
    IOCTL_WFS_CLOSE = 0x1E,
    IOCTL_WFS_READ = 0x20,
    IOCTL_WFS_WRITE = 0x22,
    IOCTL_WFS_ATTACH_DETACH = 0x2d,
    IOCTL_WFS_ATTACH_DETACH_2 = 0x2e,
  };

  enum
  {
    WFS_EEMPTY = -10028,  // Directory is empty of iteration completed.
  };

  struct FileDescriptor
  {
    bool in_use;
    std::string path;
    int mode;
    size_t position;
    File::IOFile file;

    bool Open();
  };
  std::vector<FileDescriptor> m_fds;

  FileDescriptor* FindFileDescriptor(u16 fd);
  u16 GetNewFileDescriptor();
  void ReleaseFileDescriptor(u16 fd);
};
