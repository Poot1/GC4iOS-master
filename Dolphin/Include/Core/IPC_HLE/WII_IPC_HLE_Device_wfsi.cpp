// Copyright 2016 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/IPC_HLE/WII_IPC_HLE_Device_wfsi.h"

#include <mbedtls/aes.h>
#include <stack>
#include <string>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/HW/Memmap.h"
#include "Core/IPC_HLE/ESFormats.h"
#include "Core/IPC_HLE/WII_IPC_HLE_Device_usb_wfssrv.h"
#include "DiscIO/NANDContentLoader.h"

void ARCUnpacker::Reset()
{
  m_whole_file.clear();
}

void ARCUnpacker::AddBytes(const std::vector<u8>& bytes)
{
  m_whole_file.insert(m_whole_file.end(), bytes.begin(), bytes.end());
}

void ARCUnpacker::Extract(const WriteCallback& callback)
{
  u32 fourcc = Common::swap32(m_whole_file.data());
  if (fourcc != 0x55AA382D)
  {
    ERROR_LOG(WII_IPC_HLE, "ARCUnpacker: invalid fourcc (%08x)", fourcc);
    return;
  }

  // Read the root node to get the number of nodes.
  u8* nodes_directory = m_whole_file.data() + 0x20;
  u32 nodes_count = Common::swap32(nodes_directory + 8);
  constexpr u32 NODE_SIZE = 0xC;
  char* string_table = reinterpret_cast<char*>(nodes_directory + nodes_count * NODE_SIZE);

  std::stack<std::pair<u32, std::string>> directory_stack;
  directory_stack.emplace(std::make_pair(nodes_count, ""));
  for (u32 i = 1; i < nodes_count; ++i)
  {
    while (i >= directory_stack.top().first)
    {
      directory_stack.pop();
    }
    const std::string& current_directory = directory_stack.top().second;
    u8* node = nodes_directory + i * NODE_SIZE;
    u32 name_offset = (node[1] << 16) | Common::swap16(node + 2);
    u32 data_offset = Common::swap32(node + 4);
    u32 size = Common::swap32(node + 8);
    std::string basename = string_table + name_offset;
    std::string fullname =
        current_directory.empty() ? basename : current_directory + "/" + basename;

    u8 flags = *node;
    if (flags == 1)
    {
      directory_stack.emplace(std::make_pair(size, fullname));
    }
    else
    {
      std::vector<u8> contents(m_whole_file.data() + data_offset,
                               m_whole_file.data() + data_offset + size);
      callback(fullname, contents);
    }
  }
}

CWII_IPC_HLE_Device_wfsi::CWII_IPC_HLE_Device_wfsi(u32 device_id, const std::string& device_name)
    : IWII_IPC_HLE_Device(device_id, device_name)
{
}

IPCCommandResult CWII_IPC_HLE_Device_wfsi::Open(u32 command_address, u32 mode)
{
  INFO_LOG(WII_IPC_HLE, "/dev/wfsi: Open");
  return IWII_IPC_HLE_Device::Open(command_address, mode);
}

IPCCommandResult CWII_IPC_HLE_Device_wfsi::IOCtl(u32 command_address)
{
  u32 command = Memory::Read_U32(command_address + 0xC);
  u32 buffer_in = Memory::Read_U32(command_address + 0x10);
  u32 buffer_in_size = Memory::Read_U32(command_address + 0x14);
  u32 buffer_out = Memory::Read_U32(command_address + 0x18);
  u32 buffer_out_size = Memory::Read_U32(command_address + 0x1C);

  u32 return_error_code = IPC_SUCCESS;

  switch (command)
  {
  case IOCTL_WFSI_PREPARE_DEVICE:
  {
    u32 tmd_addr = Memory::Read_U32(buffer_in);
    u32 tmd_size = Memory::Read_U32(buffer_in + 4);

    INFO_LOG(WII_IPC_HLE, "IOCTL_WFSI_PREPARE_DEVICE");

    constexpr u32 MAX_TMD_SIZE = 0x4000;
    if (tmd_size > MAX_TMD_SIZE)
    {
      ERROR_LOG(WII_IPC_HLE, "IOCTL_WFSI_INIT: TMD size too large (%d)", tmd_size);
      return_error_code = IPC_EINVAL;
      break;
    }
    std::vector<u8> tmd_bytes;
    tmd_bytes.resize(tmd_size);
    Memory::CopyFromEmu(tmd_bytes.data(), tmd_addr, tmd_size);
    m_tmd.SetBytes(std::move(tmd_bytes));

    std::vector<u8> ticket = DiscIO::FindSignedTicket(m_tmd.GetTitleId());
    if (ticket.size() == 0)
    {
      return_error_code = -11028;
      break;
    }

    memcpy(m_aes_key, DiscIO::GetKeyFromTicket(ticket).data(), sizeof(m_aes_key));
    mbedtls_aes_setkey_dec(&m_aes_ctx, m_aes_key, 128);

    break;
  }

  case IOCTL_WFSI_PREPARE_PROFILE:
    m_base_extract_path = StringFromFormat("/vol/%s/tmp/", m_device_name.c_str());
  // Fall through intended.

  case IOCTL_WFSI_PREPARE_CONTENT:
  {
    const char* ioctl_name = command == IOCTL_WFSI_PREPARE_PROFILE ? "IOCTL_WFSI_PREPARE_PROFILE" :
                                                                     "IOCTL_WFSI_PREPARE_CONTENT";

    // Initializes the IV from the index of the content in the TMD contents.
    u32 content_id = Memory::Read_U32(buffer_in + 8);
    TMDReader::Content content_info;
    if (!m_tmd.FindContentById(content_id, &content_info))
    {
      WARN_LOG(WII_IPC_HLE, "%s: Content id %08x not found", ioctl_name, content_id);
      return_error_code = -10003;
      break;
    }

    memset(m_aes_iv, 0, sizeof(m_aes_iv));
    m_aes_iv[0] = content_info.index >> 8;
    m_aes_iv[1] = content_info.index & 0xFF;
    INFO_LOG(WII_IPC_HLE, "%s: Content id %08x found at index %d", ioctl_name, content_id,
             content_info.index);

    m_arc_unpacker.Reset();
    break;
  }

  case IOCTL_WFSI_IMPORT_PROFILE:
  case IOCTL_WFSI_IMPORT_CONTENT:
  {
    const char* ioctl_name = command == IOCTL_WFSI_IMPORT_PROFILE ? "IOCTL_WFSI_IMPORT_PROFILE" :
                                                                    "IOCTL_WFSI_IMPORT_CONTENT";

    u32 content_id = Memory::Read_U32(buffer_in + 0xC);
    u32 input_ptr = Memory::Read_U32(buffer_in + 0x10);
    u32 input_size = Memory::Read_U32(buffer_in + 0x14);
    INFO_LOG(WII_IPC_HLE, "%s: %08x bytes of data at %08x from content id %d", ioctl_name,
             content_id, input_ptr, input_size);

    std::vector<u8> decrypted(input_size);
    mbedtls_aes_crypt_cbc(&m_aes_ctx, MBEDTLS_AES_DECRYPT, input_size, m_aes_iv,
                          Memory::GetPointer(input_ptr), decrypted.data());

    m_arc_unpacker.AddBytes(decrypted);
    break;
  }

  case IOCTL_WFSI_FINALIZE_PROFILE:
  case IOCTL_WFSI_FINALIZE_CONTENT:
  {
    const char* ioctl_name = command == IOCTL_WFSI_FINALIZE_PROFILE ?
                                 "IOCTL_WFSI_FINALIZE_PROFILE" :
                                 "IOCTL_WFSI_FINALIZE_CONTENT";
    INFO_LOG(WII_IPC_HLE, "%s", ioctl_name);

    auto callback = [this](const std::string& filename, const std::vector<u8>& bytes) {
      INFO_LOG(WII_IPC_HLE, "Extract: %s (%zd bytes)", filename.c_str(), bytes.size());

      std::string path = WFS::NativePath(m_base_extract_path + "/" + filename);
      File::CreateFullPath(path);
      File::IOFile f(path, "wb");
      if (!f)
      {
        ERROR_LOG(WII_IPC_HLE, "Could not extract %s to %s", filename.c_str(), path.c_str());
        return;
      }
      f.WriteBytes(bytes.data(), bytes.size());
    };
    m_arc_unpacker.Extract(callback);

    // Technically not needed, but let's not keep large buffers in RAM for no
    // reason if we can avoid it.
    m_arc_unpacker.Reset();
    break;
  }

  case IOCTL_WFSI_DELETE_TITLE:
    // Bytes 0-4: ??
    // Bytes 4-8: game id
    // Bytes 1c-1e: title id?
    WARN_LOG(WII_IPC_HLE, "IOCTL_WFSI_DELETE_TITLE: unimplemented");
    break;

  case IOCTL_WFSI_IMPORT_TITLE:
    WARN_LOG(WII_IPC_HLE, "IOCTL_WFSI_IMPORT_TITLE: unimplemented");
    break;

  case IOCTL_WFSI_INIT:
    // Nothing to do.
    INFO_LOG(WII_IPC_HLE, "IOCTL_WFSI_INIT");
    break;

  case IOCTL_WFSI_SET_DEVICE_NAME:
    INFO_LOG(WII_IPC_HLE, "IOCTL_WFSI_SET_DEVICE_NAME");
    m_device_name = Memory::GetString(buffer_in);
    break;

  case IOCTL_WFSI_APPLY_TITLE_PROFILE:
    INFO_LOG(WII_IPC_HLE, "IOCTL_WFSI_APPLY_TITLE_PROFILE");

    m_base_extract_path = StringFromFormat(
        "/vol/%s/_install/%c%c%c%c/content", m_device_name.c_str(),
        static_cast<char>(m_tmd.GetTitleId() >> 24), static_cast<char>(m_tmd.GetTitleId() >> 16),
        static_cast<char>(m_tmd.GetTitleId() >> 8), static_cast<char>(m_tmd.GetTitleId()));
    File::CreateFullPath(WFS::NativePath(m_base_extract_path));

    break;

  default:
    // TODO(wfs): Should be returning an error. However until we have
    // everything properly stubbed it's easier to simulate the methods
    // succeeding.
    WARN_LOG(WII_IPC_HLE, "%s unimplemented IOCtl(0x%08x, size_in=%08x, size_out=%08x)\n%s\n%s",
             m_name.c_str(), command, buffer_in_size, buffer_out_size,
             HexDump(Memory::GetPointer(buffer_in), buffer_in_size).c_str(),
             HexDump(Memory::GetPointer(buffer_out), buffer_out_size).c_str());
    Memory::Memset(buffer_out, 0, buffer_out_size);
    break;
  }

  Memory::Write_U32(return_error_code, command_address + 4);
  return GetDefaultReply();
}

IPCCommandResult CWII_IPC_HLE_Device_wfsi::IOCtlV(u32 command_address)
{
  ERROR_LOG(WII_IPC_HLE, "IOCtlV on /dev/wfsi -- unsupported");
  Memory::Write_U32(IPC_EINVAL, command_address + 4);
  return GetDefaultReply();
}
