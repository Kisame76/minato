// What xsdc generates from ART's CacheInfo.xsd, as far as ART reads it (ADR-0033 a): odrefresh's cache-info.xml,
// whose system properties the CMC collector checks. The guest has no odrefresh, no such file and no CMC (ADR-0033
// decision 5), and ART parses the file only when it exists; the parser reports every file as unreadable.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace com::android::art {

class KeyValuePair {
  public:
    const std::string& getK() const { return k_; }
    const std::string& getV() const { return v_; }

  private:
    std::string k_, v_;
};

class KeyValuePairList {
  public:
    const std::vector<KeyValuePair>& getItem() const { return item_; }

  private:
    std::vector<KeyValuePair> item_;
};

class CacheInfo {
  public:
    const KeyValuePairList* getFirstSystemProperties() const { return nullptr; }
};

std::optional<CacheInfo> parse(const char* xml);

}  // namespace com::android::art
