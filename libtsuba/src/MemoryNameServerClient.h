#ifndef KATANA_LIBTSUBA_MEMORYNAMESERVERCLIENT_H_
#define KATANA_LIBTSUBA_MEMORYNAMESERVERCLIENT_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "RDGManifest.h"
#include "katana/Result.h"
#include "tsuba/NameServerClient.h"
#include "tsuba/RDG.h"

namespace tsuba {

class KATANA_EXPORT MemoryNameServerClient : public NameServerClient {
  std::mutex mutex_;
  std::unordered_map<std::string, RDGManifest> server_state_;

  katana::Result<RDGManifest> lookup(const std::string& key);

public:
  katana::Result<RDGManifest> Get(const katana::Uri& rdg_name) override;

  katana::Result<void> CreateIfAbsent(
      const katana::Uri& rdg_name, const RDGManifest& meta) override;

  katana::Result<void> Delete(const katana::Uri& rdg_name) override;

  katana::Result<void> Update(
      const katana::Uri& rdg_name, uint64_t old_version,
      const RDGManifest& meta) override;

  katana::Result<void> CheckHealth() override;
};

}  // namespace tsuba

#endif
