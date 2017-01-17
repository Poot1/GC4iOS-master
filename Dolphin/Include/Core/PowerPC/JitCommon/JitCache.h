// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <bitset>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "Common/CommonTypes.h"

class JitBase;

// A JitBlock is block of compiled code which corresponds to the PowerPC
// code at a given address.
//
// The notion of the address of a block is a bit complicated because of the
// way address translation works, but basically it's the combination of an
// effective address, the address translation bits in MSR, and the physical
// address.
struct JitBlock
{
  enum
  {
    // Mask for the MSR bits which determine whether a compiled block
    // is valid (MSR.IR and MSR.DR, the address translation bits).
    JIT_CACHE_MSR_MASK = 0x30,
  };

  // A special entry point for block linking; usually used to check the
  // downcount.
  const u8* checkedEntry;
  // The normal entry point for the block, returned by Dispatch().
  const u8* normalEntry;

  // The effective address (PC) for the beginning of the block.
  u32 effectiveAddress;
  // The MSR bits expected for this block to be valid; see JIT_CACHE_MSR_MASK.
  u32 msrBits;
  // The physical address of the code represented by this block.
  // Various maps in the cache are indexed by this (start_block_map,
  // block_map, and valid_block in particular). This is useful because of
  // of the way the instruction cache works on PowerPC.
  u32 physicalAddress;
  // The number of bytes of JIT'ed code contained in this block. Mostly
  // useful for logging.
  u32 codeSize;
  // The number of PPC instructions represented by this block. Mostly
  // useful for logging.
  u32 originalSize;
  int runCount;  // for profiling.

  // Whether this struct refers to a valid block. This is mostly useful as
  // a debugging aid.
  // FIXME: Change current users of invalid bit to assertions?
  bool invalid;

  // Information about exits to a known address from this block.
  // This is used to implement block linking.
  struct LinkData
  {
    u8* exitPtrs;  // to be able to rewrite the exit jump
    u32 exitAddress;
    bool linkStatus;  // is it already linked?
  };
  std::vector<LinkData> linkData;

  // we don't really need to save start and stop
  // TODO (mb2): ticStart and ticStop -> "local var" mean "in block" ... low priority ;)
  u64 ticStart;    // for profiling - time.
  u64 ticStop;     // for profiling - time.
  u64 ticCounter;  // for profiling - time.
};

typedef void (*CompiledCode)();

// This is essentially just an std::bitset, but Visual Studia 2013's
// implementation of std::bitset is slow.
class ValidBlockBitSet final
{
public:
  enum
  {
    // ValidBlockBitSet covers the whole 32-bit address-space in 32-byte
    // chunks.
    // FIXME: Maybe we can get away with less? There isn't any actual
    // RAM in most of this space.
    VALID_BLOCK_MASK_SIZE = (1ULL << 32) / 32,
    // The number of elements in the allocated array. Each u32 contains 32 bits.
    VALID_BLOCK_ALLOC_ELEMENTS = VALID_BLOCK_MASK_SIZE / 32
  };
  // Directly accessed by Jit64.
  std::unique_ptr<u32[]> m_valid_block;

  ValidBlockBitSet()
  {
    m_valid_block.reset(new u32[VALID_BLOCK_ALLOC_ELEMENTS]);
    ClearAll();
  }

  void Set(u32 bit) { m_valid_block[bit / 32] |= 1u << (bit % 32); }
  void Clear(u32 bit) { m_valid_block[bit / 32] &= ~(1u << (bit % 32)); }
  void ClearAll() { memset(m_valid_block.get(), 0, sizeof(u32) * VALID_BLOCK_ALLOC_ELEMENTS); }
  bool Test(u32 bit) { return (m_valid_block[bit / 32] & (1u << (bit % 32))) != 0; }
};

class JitBaseBlockCache
{
public:
  static constexpr int MAX_NUM_BLOCKS = 65536 * 2;
  static constexpr u32 iCache_Num_Elements = 0x10000;
  static constexpr u32 iCache_Mask = iCache_Num_Elements - 1;

  explicit JitBaseBlockCache(JitBase& jit);
  virtual ~JitBaseBlockCache();

  void Init();
  void Shutdown();
  void Clear();
  void Reset();
  void SchedulateClearCacheThreadSafe();

  bool IsFull() const;

  // Code Cache
  JitBlock** GetICache();
  void RunOnBlocks(std::function<void(const JitBlock&)> f);

  JitBlock* AllocateBlock(u32 em_address);
  void FinalizeBlock(JitBlock& block, bool block_link, const u8* code_ptr);

  // Look for the block in the slow but accurate way.
  // This function shall be used if FastLookupEntryForAddress() failed.
  // This might return nullptr if there is no such block.
  JitBlock* GetBlockFromStartAddress(u32 em_address, u32 msr);

  // Get the normal entry for the block associated with the current program
  // counter. This will JIT code if necessary. (This is the reference
  // implementation; high-performance JITs will want to use a custom
  // assembly version.)
  const u8* Dispatch();

  void InvalidateICache(u32 address, const u32 length, bool forced);

  u32* GetBlockBitSet() const;

protected:
  JitBase& m_jit;

private:
  virtual void WriteLinkBlock(const JitBlock::LinkData& source, const JitBlock* dest) = 0;
  virtual void WriteDestroyBlock(const JitBlock& block);

  void LinkBlockExits(JitBlock& block);
  void LinkBlock(JitBlock& block);
  void UnlinkBlock(const JitBlock& block);
  void DestroyBlock(JitBlock& block, bool invalidate);

  void MoveBlockIntoFastCache(u32 em_address, u32 msr);

  // Fast but risky block lookup based on iCache.
  JitBlock*& FastLookupEntryForAddress(u32 address);

  // We store the metadata of all blocks in a linear way within this array.
  // Note: blocks[0] must not be used as it is referenced as invalid block in iCache.
  std::array<JitBlock, MAX_NUM_BLOCKS> blocks;  // number -> JitBlock
  int num_blocks = 1;

  // links_to hold all exit points of all valid blocks in a reverse way.
  // It is used to query all blocks which links to an address.
  std::multimap<u32, JitBlock*> links_to;  // destination_PC -> number

  // Map indexed by the physical memory location.
  // It is used to invalidate blocks based on memory location.
  std::map<std::pair<u32, u32>, JitBlock*> block_map;  // (end_addr, start_addr) -> block

  // Map indexed by the physical address of the entry point.
  // This is used to query the block based on the current PC in a slow way.
  // TODO: This is redundant with block_map, and both should be a multimap.
  std::map<u32, JitBlock*> start_block_map;  // start_addr -> block

  // This bitsets shows which cachelines overlap with any blocks.
  // It is used to provide a fast way to query if no icache invalidation is needed.
  ValidBlockBitSet valid_block;

  // This array is indexed with the masked PC and likely holds the correct block id.
  // This is used as a fast cache of start_block_map used in the assembly dispatcher.
  std::array<JitBlock*, iCache_Num_Elements> iCache;  // start_addr & mask -> number
};
