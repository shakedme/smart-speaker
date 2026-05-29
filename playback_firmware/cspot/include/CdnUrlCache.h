#pragma once

#include <cstdint>
#include <string>

namespace cspot {

class CdnUrlCache {
 public:
  static bool lookup(const std::string& fileIdHex, std::string& urlOut);
  static void store(const std::string& fileIdHex, const std::string& url);
  static void clear();
};

}  // namespace cspot
