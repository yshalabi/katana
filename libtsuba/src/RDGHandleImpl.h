#ifndef KATANA_LIBTSUBA_RDGHANDLEIMPL_H_
#define KATANA_LIBTSUBA_RDGHANDLEIMPL_H_

#include <cstdint>

#include "RDGManifest.h"
#include "katana/Uri.h"
#include "tsuba/tsuba.h"

namespace tsuba {

class RDGHandleImpl {
public:
  RDGHandleImpl(uint32_t flags, RDGManifest&& rdg_meta)
      : flags_(flags), rdg_meta_(std::move(rdg_meta)) {}

  /// Perform some checks on assumed invariants
  katana::Result<void> Validate() const;
  constexpr bool AllowsRead() const { return true; }
  constexpr bool AllowsWrite() const { return flags_ & kReadWrite; }

  //
  // Accessors and Mutators
  //
  const RDGManifest& rdg_meta() const { return rdg_meta_; }
  void set_rdg_meta(RDGManifest&& rdg_meta) { rdg_meta_ = std::move(rdg_meta); }

private:
  uint32_t flags_;
  RDGManifest rdg_meta_;
};

}  // namespace tsuba

#endif
