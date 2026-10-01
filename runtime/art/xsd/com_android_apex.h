// What xsdc generates from apexd's apex-info-list.xsd, as far as ART reads it (ADR-0033 a): the APEX info list at
// /apex/apex-info-list.xml, which apexd writes. The guest has no apexd and no such file, and ART parses it only
// when it exists (Runtime's APEX versions); the parser reports every file as unreadable.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace com::android::apex {

class ApexInfo {
  public:
    const std::string& getModuleName() const { return module_name_; }
    int64_t getVersionCode() const { return version_code_; }
    bool getIsFactory() const { return is_factory_; }
    bool getIsActive() const { return is_active_; }
    bool hasLastUpdateMillis() const { return false; }
    int64_t getLastUpdateMillis() const { return 0; }

  private:
    std::string module_name_;
    int64_t version_code_ = 0;
    bool is_factory_ = true;
    bool is_active_ = false;
};

class ApexInfoList {
  public:
    const std::vector<ApexInfo>& getApexInfo() const { return apex_info_; }

  private:
    std::vector<ApexInfo> apex_info_;
};

std::optional<ApexInfoList> readApexInfoList(const char* configFile);

}  // namespace com::android::apex
