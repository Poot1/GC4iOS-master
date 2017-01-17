// Copyright 2009 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <cstring>
#include <map>
#include <string>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/SymbolDB.h"

void SymbolDB::List()
{
  for (const auto& func : functions)
  {
    DEBUG_LOG(OSHLE, "%s @ %08x: %i bytes (hash %08x) : %i calls", func.second.name.c_str(),
              func.second.address, func.second.size, func.second.hash, func.second.numCalls);
  }
  INFO_LOG(OSHLE, "%zu functions known in this program above.", functions.size());
}

void SymbolDB::Clear(const char* prefix)
{
  // TODO: honor prefix
  functions.clear();
  checksumToFunction.clear();
}

void SymbolDB::Index()
{
  int i = 0;
  for (auto& func : functions)
  {
    func.second.index = i++;
  }
}

Symbol* SymbolDB::GetSymbolFromName(const std::string& name)
{
  for (auto& func : functions)
  {
    if (func.second.function_name == name)
      return &func.second;
  }

  return nullptr;
}

std::vector<Symbol*> SymbolDB::GetSymbolsFromName(const std::string& name)
{
  std::vector<Symbol*> symbols;

  for (auto& func : functions)
  {
    if (func.second.function_name == name)
      symbols.push_back(&func.second);
  }

  return symbols;
}

Symbol* SymbolDB::GetSymbolFromHash(u32 hash)
{
  XFuncPtrMap::iterator iter = checksumToFunction.find(hash);
  if (iter != checksumToFunction.end())
    return *iter->second.begin();
  else
    return nullptr;
}

std::vector<Symbol*> SymbolDB::GetSymbolsFromHash(u32 hash)
{
  std::vector<Symbol*> symbols;

  for (const auto& iter : checksumToFunction)
    if (iter.first == hash)
      for (const auto& symbol : iter.second)
        symbols.push_back(symbol);

  return symbols;
}

void SymbolDB::AddCompleteSymbol(const Symbol& symbol)
{
  functions.emplace(symbol.address, symbol);
}