// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "DiscIO/VolumeWii.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <map>
#include <mbedtls/aes.h>
#include <mbedtls/sha1.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Swap.h"

#include "DiscIO/Blob.h"
#include "DiscIO/Enums.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"

namespace DiscIO
{
constexpr u64 PARTITION_DATA_OFFSET = 0x20000;

VolumeWii::VolumeWii(std::unique_ptr<BlobReader> reader)
    : m_pReader(std::move(reader)), m_game_partition(PARTITION_NONE),
      m_last_decrypted_block(UINT64_MAX)
{
  _assert_(m_pReader);

  if (m_pReader->ReadSwapped<u32>(0x60) != u32(0))
  {
    // No partitions - just read unencrypted data like with a GC disc
    return;
  }

  // Get tickets, TMDs, and decryption keys for all partitions
  for (u32 partition_group = 0; partition_group < 4; ++partition_group)
  {
    const std::optional<u32> number_of_partitions =
        m_pReader->ReadSwapped<u32>(0x40000 + (partition_group * 8));
    if (!number_of_partitions)
      continue;

    std::optional<u32> read_buffer =
        m_pReader->ReadSwapped<u32>(0x40000 + (partition_group * 8) + 4);
    if (!read_buffer)
      continue;
    const u64 partition_table_offset = static_cast<u64>(*read_buffer) << 2;

    for (u32 i = 0; i < number_of_partitions; i++)
    {
      // Read the partition offset
      read_buffer = m_pReader->ReadSwapped<u32>(partition_table_offset + (i * 8));
      if (!read_buffer)
        continue;
      const u64 partition_offset = static_cast<u64>(*read_buffer) << 2;

      // Check if this is the game partition
      const bool is_game_partition =
          m_game_partition == PARTITION_NONE &&
          m_pReader->ReadSwapped<u32>(partition_table_offset + (i * 8) + 4) == u32(0);

      // Read ticket
      std::vector<u8> ticket_buffer(sizeof(IOS::ES::Ticket));
      if (!m_pReader->Read(partition_offset, ticket_buffer.size(), ticket_buffer.data()))
        continue;
      IOS::ES::TicketReader ticket{std::move(ticket_buffer)};
      if (!ticket.IsValid())
        continue;

      // Read TMD
      const std::optional<u32> tmd_size = m_pReader->ReadSwapped<u32>(partition_offset + 0x2a4);
      std::optional<u32> tmd_address = m_pReader->ReadSwapped<u32>(partition_offset + 0x2a8);
      if (!tmd_size || !tmd_address)
        continue;
      *tmd_address <<= 2;
      if (!IOS::ES::IsValidTMDSize(*tmd_size))
      {
        // This check is normally done by ES in ES_DiVerify, but that would happen too late
        // (after allocating the buffer), so we do the check here.
        PanicAlert("Invalid TMD size");
        continue;
      }
      std::vector<u8> tmd_buffer(*tmd_size);
      if (!m_pReader->Read(partition_offset + *tmd_address, *tmd_size, tmd_buffer.data()))
        continue;
      IOS::ES::TMDReader tmd{std::move(tmd_buffer)};

      // Get the decryption key
      const std::vector<u8> key = ticket.GetTitleKey();
      if (key.size() != 16)
        continue;
      std::unique_ptr<mbedtls_aes_context> aes_context = std::make_unique<mbedtls_aes_context>();
      mbedtls_aes_setkey_dec(aes_context.get(), key.data(), 128);

      // We've read everything. Time to store it! (The reason we don't store anything
      // earlier is because we want to be able to skip adding the partition if an error occurs.)
      const Partition partition(partition_offset);
      m_partition_keys[partition] = std::move(aes_context);
      m_partition_tickets[partition] = std::move(ticket);
      m_partition_tmds[partition] = std::move(tmd);
      if (is_game_partition)
        m_game_partition = partition;
    }
  }
}

VolumeWii::~VolumeWii()
{
}

bool VolumeWii::Read(u64 _ReadOffset, u64 _Length, u8* _pBuffer, const Partition& partition) const
{
  if (partition == PARTITION_NONE)
    return m_pReader->Read(_ReadOffset, _Length, _pBuffer);

  // Get the decryption key for the partition
  auto it = m_partition_keys.find(partition);
  if (it == m_partition_keys.end())
    return false;
  mbedtls_aes_context* aes_context = it->second.get();

  std::vector<u8> read_buffer(BLOCK_TOTAL_SIZE);
  while (_Length > 0)
  {
    // Calculate offsets
    u64 block_offset_on_disc =
        partition.offset + PARTITION_DATA_OFFSET + _ReadOffset / BLOCK_DATA_SIZE * BLOCK_TOTAL_SIZE;
    u64 data_offset_in_block = _ReadOffset % BLOCK_DATA_SIZE;

    if (m_last_decrypted_block != block_offset_on_disc)
    {
      // Read the current block
      if (!m_pReader->Read(block_offset_on_disc, BLOCK_TOTAL_SIZE, read_buffer.data()))
        return false;

      // Decrypt the block's data.
      // 0x3D0 - 0x3DF in read_buffer will be overwritten,
      // but that won't affect anything, because we won't
      // use the content of read_buffer anymore after this
      mbedtls_aes_crypt_cbc(aes_context, MBEDTLS_AES_DECRYPT, BLOCK_DATA_SIZE, &read_buffer[0x3D0],
                            &read_buffer[BLOCK_HEADER_SIZE], m_last_decrypted_block_data);
      m_last_decrypted_block = block_offset_on_disc;

      // The only thing we currently use from the 0x000 - 0x3FF part
      // of the block is the IV (at 0x3D0), but it also contains SHA-1
      // hashes that IOS uses to check that discs aren't tampered with.
      // http://wiibrew.org/wiki/Wii_Disc#Encrypted
    }

    // Copy the decrypted data
    u64 copy_size = std::min(_Length, BLOCK_DATA_SIZE - data_offset_in_block);
    memcpy(_pBuffer, &m_last_decrypted_block_data[data_offset_in_block],
           static_cast<size_t>(copy_size));

    // Update offsets
    _Length -= copy_size;
    _pBuffer += copy_size;
    _ReadOffset += copy_size;
  }

  return true;
}

std::vector<Partition> VolumeWii::GetPartitions() const
{
  std::vector<Partition> partitions;
  for (const auto& pair : m_partition_keys)
    partitions.push_back(pair.first);
  return partitions;
}

Partition VolumeWii::GetGamePartition() const
{
  return m_game_partition;
}

std::optional<u64> VolumeWii::GetTitleID(const Partition& partition) const
{
  const IOS::ES::TicketReader& ticket = GetTicket(partition);
  if (!ticket.IsValid())
    return {};
  return ticket.GetTitleId();
}

const IOS::ES::TicketReader& VolumeWii::GetTicket(const Partition& partition) const
{
  auto it = m_partition_tickets.find(partition);
  return it != m_partition_tickets.end() ? it->second : INVALID_TICKET;
}

const IOS::ES::TMDReader& VolumeWii::GetTMD(const Partition& partition) const
{
  auto it = m_partition_tmds.find(partition);
  return it != m_partition_tmds.end() ? it->second : INVALID_TMD;
}

u64 VolumeWii::PartitionOffsetToRawOffset(u64 offset, const Partition& partition)
{
  if (partition == PARTITION_NONE)
    return offset;

  return partition.offset + PARTITION_DATA_OFFSET + (offset / BLOCK_DATA_SIZE * BLOCK_TOTAL_SIZE) +
         (offset % BLOCK_DATA_SIZE);
}

std::string VolumeWii::GetGameID(const Partition& partition) const
{
  char ID[6];

  if (!Read(0, 6, (u8*)ID, partition))
    return std::string();

  return DecodeString(ID);
}

Region VolumeWii::GetRegion() const
{
  const std::optional<u32> region_code = m_pReader->ReadSwapped<u32>(0x4E000);
  return region_code ? static_cast<Region>(*region_code) : Region::UNKNOWN_REGION;
}

Country VolumeWii::GetCountry(const Partition& partition) const
{
  // The 0 that we use as a default value is mapped to COUNTRY_UNKNOWN and UNKNOWN_REGION
  u8 country_byte = ReadSwapped<u8>(3, partition).value_or(0);
  const Region region = GetRegion();

  if (RegionSwitchWii(country_byte) != region)
    return TypicalCountryForRegion(region);

  return CountrySwitch(country_byte);
}

std::string VolumeWii::GetMakerID(const Partition& partition) const
{
  char makerID[2];

  if (!Read(0x4, 0x2, (u8*)&makerID, partition))
    return std::string();

  return DecodeString(makerID);
}

std::optional<u16> VolumeWii::GetRevision(const Partition& partition) const
{
  std::optional<u8> revision = ReadSwapped<u8>(7, partition);
  return revision ? *revision : std::optional<u16>();
}

std::string VolumeWii::GetInternalName(const Partition& partition) const
{
  char name_buffer[0x60];
  if (Read(0x20, 0x60, (u8*)&name_buffer, partition))
    return DecodeString(name_buffer);

  return "";
}

std::map<Language, std::string> VolumeWii::GetLongNames() const
{
  std::unique_ptr<FileSystem> file_system(CreateFileSystem(this, GetGamePartition()));
  if (!file_system)
    return {};

  std::vector<u8> opening_bnr(NAMES_TOTAL_BYTES);
  size_t size = file_system->ReadFile("opening.bnr", opening_bnr.data(), opening_bnr.size(), 0x5C);
  opening_bnr.resize(size);
  return ReadWiiNames(opening_bnr);
}

std::vector<u32> VolumeWii::GetBanner(int* width, int* height) const
{
  *width = 0;
  *height = 0;

  const std::optional<u64> title_id = GetTitleID(GetGamePartition());
  if (!title_id)
    return std::vector<u32>();

  return GetWiiBanner(width, height, *title_id);
}

std::string VolumeWii::GetApploaderDate(const Partition& partition) const
{
  char date[16];

  if (!Read(0x2440, 0x10, (u8*)&date, partition))
    return std::string();

  return DecodeString(date);
}

Platform VolumeWii::GetVolumeType() const
{
  return Platform::WII_DISC;
}

std::optional<u8> VolumeWii::GetDiscNumber(const Partition& partition) const
{
  return ReadSwapped<u8>(6, partition);
}

BlobType VolumeWii::GetBlobType() const
{
  return m_pReader->GetBlobType();
}

u64 VolumeWii::GetSize() const
{
  return m_pReader->GetDataSize();
}

u64 VolumeWii::GetRawSize() const
{
  return m_pReader->GetRawSize();
}

bool VolumeWii::CheckIntegrity(const Partition& partition) const
{
  // Get the decryption key for the partition
  auto it = m_partition_keys.find(partition);
  if (it == m_partition_keys.end())
    return false;
  mbedtls_aes_context* aes_context = it->second.get();

  // Get partition data size
  u32 partSizeDiv4;
  m_pReader->Read(partition.offset + 0x2BC, 4, (u8*)&partSizeDiv4);
  u64 partDataSize = (u64)Common::swap32(partSizeDiv4) * 4;

  u32 nClusters = (u32)(partDataSize / 0x8000);
  for (u32 clusterID = 0; clusterID < nClusters; ++clusterID)
  {
    u64 clusterOff = partition.offset + PARTITION_DATA_OFFSET + (u64)clusterID * 0x8000;

    // Read and decrypt the cluster metadata
    u8 clusterMDCrypted[0x400];
    u8 clusterMD[0x400];
    u8 IV[16] = {0};
    if (!m_pReader->Read(clusterOff, 0x400, clusterMDCrypted))
    {
      WARN_LOG(DISCIO, "Integrity Check: fail at cluster %d: could not read metadata", clusterID);
      return false;
    }
    mbedtls_aes_crypt_cbc(aes_context, MBEDTLS_AES_DECRYPT, 0x400, IV, clusterMDCrypted, clusterMD);

    // Some clusters have invalid data and metadata because they aren't
    // meant to be read by the game (for example, holes between files). To
    // try to avoid reporting errors because of these clusters, we check
    // the 0x00 paddings in the metadata.
    //
    // This may cause some false negatives though: some bad clusters may be
    // skipped because they are *too* bad and are not even recognized as
    // valid clusters. To be improved.
    bool meaningless = false;
    for (u32 idx = 0x26C; idx < 0x280; ++idx)
      if (clusterMD[idx] != 0)
        meaningless = true;

    if (meaningless)
      continue;

    u8 clusterData[0x7C00];
    if (!Read((u64)clusterID * 0x7C00, 0x7C00, clusterData, partition))
    {
      WARN_LOG(DISCIO, "Integrity Check: fail at cluster %d: could not read data", clusterID);
      return false;
    }

    for (u32 hashID = 0; hashID < 31; ++hashID)
    {
      u8 hash[20];

      mbedtls_sha1(clusterData + hashID * 0x400, 0x400, hash);

      // Note that we do not use strncmp here
      if (memcmp(hash, clusterMD + hashID * 20, 20))
      {
        WARN_LOG(DISCIO, "Integrity Check: fail at cluster %d: hash %d is invalid", clusterID,
                 hashID);
        return false;
      }
    }
  }

  return true;
}

}  // namespace
