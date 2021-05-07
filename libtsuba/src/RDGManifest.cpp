#include "RDGManifest.h"

#include <sstream>

#include "Constants.h"
#include "GlobalState.h"
#include "RDGHandleImpl.h"
#include "RDGPartHeader.h"
#include "katana/JSON.h"
#include "katana/Result.h"
#include "tsuba/Errors.h"
#include "tsuba/FileView.h"
#include "tsuba/tsuba.h"

template <typename T>
using Result = katana::Result<T>;
using json = nlohmann::json;

namespace {

Result<uint64_t>
Parse(const std::string& str) {
  uint64_t val = strtoul(str.c_str(), nullptr, 10);
  if (errno == ERANGE) {
    return KATANA_ERROR(
        katana::ResultErrno(), "meta file found with out of range version");
  }
  return val;
}

katana::Result<std::vector<std::string>>
FileList(const std::string& dir) {
  std::vector<std::string> files;
  auto list_fut = tsuba::FileListAsync(dir, &files);
  KATANA_LOG_ASSERT(list_fut.valid());

  if (auto res = list_fut.get(); !res) {
    return res.error();
  }
  return files;
}

katana::Result<katana::Uri>
FindLatestMetaFile(const katana::Uri& name) {
  KATANA_LOG_DEBUG_ASSERT(!tsuba::RDGManifest::IsMetaUri(name));
  auto list_res = FileList(name.string());
  if (!list_res) {
    return list_res.error();
  }

  uint64_t version = 0;
  std::string found_meta;
  for (const std::string& file : list_res.value()) {
    if (auto res = tsuba::RDGManifest::ParseVersionFromName(file); res) {
      uint64_t new_version = res.value();
      if (new_version >= version) {
        version = new_version;
        found_meta = file;
      }
    }
  }
  if (found_meta.empty()) {
    return KATANA_ERROR(
        tsuba::ErrorCode::NotFound, "failed: could not find meta   file in {}",
        name);
  }
  return name.Join(found_meta);
}

}  // namespace

namespace tsuba {

// katana::RandomAlphanumericString does not include _, making this pattern robust
// TODO (witchel) meta with no _[0-9]+ is deprecated and should be
// eliminated eventually
const std::regex RDGManifest::kManifestVersion(
    "katana_(?:((?:[0-9A-Za-z-])+))_(?:([0-9]+)|)(?:\\.manifest)?$");
//const std::regex RDGManifest::kMetaVersion("meta_(?:((?:[0-9A-Za-z-])+))_(?:([0-9]+)|)_(.*)$");
const std::regex RDGManifest::kMetaVersion(
    "meta_(?:([0-9]+))_(?:([0-9A-Za-z-]+))_([0-9]+)$");
//const std::regex RDGManifest::kMetaVersion("meta_(?:((?:[0-9A-Za-z-])+))_(?:([0-9]+)|)_(?:([0-9]+)|)$");

//Result<tsuba::RDGManifest>
//FindLatestAndMakeFromStorage(const katana::Uri& uri) {
//}

Result<tsuba::RDGManifest>
RDGManifest::MakeFromStorage(const katana::Uri& uri) {
  tsuba::FileView fv;

  if (auto res = fv.Bind(uri.string(), true); !res) {
    return res.error();
    auto latest_meta_uri = FindLatestMetaFile(uri);
    if (!latest_meta_uri) {
      return res.error();
    }
    if (auto res_latest = fv.Bind(latest_meta_uri.value().string(), true);
        !res_latest) {
      return res.error();
    }
  }
  tsuba::RDGManifest meta(uri.DirName());
  auto meta_res = katana::JsonParse<tsuba::RDGManifest>(fv, &meta);

  if (!meta_res) {
    return meta_res.error().WithContext("cannot parse {}", uri.string());
  }

  auto manifest_name = uri.BaseName();
  auto view_name = ParseViewNameFromName(manifest_name);
  auto view_args = ParseViewArgsFromName(manifest_name);

  if (view_name) {
    meta.set_viewtype(view_name.value());
  }

  if (view_args) {
    meta.set_viewargs(view_args.value());
  } else {
    meta.set_viewargs(std::vector<std::string>());
  }
  return meta;
}

Result<RDGManifest>
RDGManifest::Make(const katana::Uri& uri, uint64_t version) {
  KATANA_LOG_ASSERT(0);
  return MakeFromStorage(FileName(uri, "Random", version));
}

Result<RDGManifest>
RDGManifest::Make(RDGHandle handle) {
  return handle.impl_->rdg_meta();
}

Result<RDGManifest>
RDGManifest::Make(const katana::Uri& uri) {
  auto rdg_manifest = MakeFromStorage(uri);
  return rdg_manifest;
}

/*
Result<RDGManifest>
RDGManifest::Make(const katana::Uri& uri) {
  KATANA_LOG_DEBUG("RDGManifest::Make(uri={})",uri.string());
  if (!IsMetaUri(uri)) {
    auto ns_res = NS()->Get(uri);
    if (!ns_res) {
      return ns_res.error();
    }
    if (ns_res) {
      ns_res.value().dir_ = uri;
    }
    return ns_res;
  }
  return MakeFromStorage(uri);
}*/

std::string
RDGManifest::PartitionFileName(uint32_t node_id, uint64_t version) {
  KATANA_LOG_DEBUG_ASSERT(0);
  return fmt::format("meta_{}_{}_{}", "rdg-oec-part2", node_id, version);
}

std::string
RDGManifest::PartitionFileName(
    const std::string& view_type, uint32_t node_id, uint64_t version) {
  return fmt::format("meta_{}_{}_{}", node_id, view_type, version);
}

katana::Uri
RDGManifest::PartitionFileName(
    const katana::Uri& uri, uint32_t node_id, uint64_t version) {
  //KATANA_LOG_DEBUG_ASSERT(view_type_ != "");
  return uri.Join(PartitionFileName("rdg", node_id, version));
}

katana::Uri
RDGManifest::PartitionFileName(
    const std::string& view_type, const katana::Uri& uri, uint32_t node_id,
    uint64_t version) {
  KATANA_LOG_DEBUG_ASSERT(!IsMetaUri(uri));
  return uri.Join(PartitionFileName(view_type, node_id, version));
}

katana::Uri
RDGManifest::PartitionFileName(uint32_t host_id) const {
  return RDGManifest::PartitionFileName(view_type_, dir_, host_id, version());
}

std::string
RDGManifest::ToJsonString() const {
  // POSIX specifies that text files end in a newline
  std::string s = json(*this).dump() + '\n';
  return s;
}

// e.g., rdg_dir == s3://witchel-tests-east2/fault/simple/
katana::Uri
RDGManifest::FileName(
    const katana::Uri& uri, const std::string& view_name, uint64_t version) {
  KATANA_LOG_DEBUG_ASSERT(uri.empty() || !IsMetaUri(uri));

  return uri.Join(fmt::format("katana_{}_{}.manifest", view_name, version));
}

// if it doesn't name a meta file, assume it's meant to be a managed URI
bool
RDGManifest::IsMetaUri(const katana::Uri& uri) {
  bool res = std::regex_match(uri.BaseName(), kManifestVersion);
  return res;
}

/*
// if it doesn't name a meta file, assume it's meant to be a managed URI
bool
RDGManifest::IsManifestUri(const katana::Uri& uri) {
	KATANA_LOG_DEBUG("IsManifestUri({})", uri.string());
  return std::regex_match(uri.BaseName(), kManifestVersion);
}*/

Result<uint64_t>
RDGManifest::ParseVersionFromName(const std::string& file) {
  std::smatch sub_match;
  if (!std::regex_match(file, sub_match, kManifestVersion)) {
    return tsuba::ErrorCode::InvalidArgument;
  }
  //Manifest file
  KATANA_LOG_DEBUG(
      "Found manifest Version Version: {} (sub group[0]={}, sub_group[1]={}, "
      "sub_group[2]={}",
      Parse(sub_match[2]).value(), sub_match[0], sub_match[1], sub_match[2]);
  return Parse(sub_match[2]);
}

Result<std::string>
RDGManifest::ParseViewNameFromName(const std::string& file) {
  std::smatch sub_match;
  if (!std::regex_match(file, sub_match, kManifestVersion)) {
    return tsuba::ErrorCode::InvalidArgument;
  }
  std::string view_specifier = sub_match[1];
  std::istringstream iss(view_specifier);
  std::vector<std::string> view_args;
  std::string s = "";
  std::string view_type = "";
  while (std::getline(iss, s, '-')) {
    view_args.push_back(s);
  }
  if (view_args.size() == 0) {  //arg-less view
    view_type = view_specifier;
  } else {  //with args present first token is view name and r  est are args
    view_type = view_args[0];
    view_args.erase(view_args.begin());
  }
  return view_type;
}

Result<std::vector<std::string>>
RDGManifest::ParseViewArgsFromName(const std::string& file) {
  std::smatch sub_match;
  if (!std::regex_match(file, sub_match, kManifestVersion)) {
    return tsuba::ErrorCode::InvalidArgument;
  }
  std::string view_specifier = sub_match[1];
  std::istringstream iss(view_specifier);
  std::vector<std::string> view_args;
  std::string s = "";
  std::string view_type = "";
  while (std::getline(iss, s, '-')) {
    view_args.push_back(s);
  }
  if (view_args.size() == 0) {  //arg-less view
    view_type = view_specifier;
  } else {  //with args present first token is view name and r  est are args
    view_type = view_args[0];
    view_args.erase(view_args.begin());
  }
  return view_args;
}

// Return the set of file names that hold this RDG's data by reading partition files
// Useful to garbage collect unused files
Result<std::set<std::string>>
RDGManifest::FileNames() {
  std::set<std::string> fnames{};
  fnames.emplace(FileName().BaseName());
  for (auto i = 0U; i < num_hosts(); ++i) {
    // All other file names are directory-local, so we pass an empty
    // directory instead of handle.impl_->rdg_meta.path for the partition files
    fnames.emplace(PartitionFileName(i, version()));

    auto header_res =
        RDGPartHeader::Make(PartitionFileName(dir(), i, version()));

    if (!header_res) {
      KATANA_LOG_DEBUG(
          "problem uri: {} host: {} ver: {} : {}", dir(), i, version(),
          header_res.error());
    } else {
      auto header = std::move(header_res.value());
      for (const auto& node_prop : header.node_prop_info_list()) {
        fnames.emplace(node_prop.path);
      }
      for (const auto& edge_prop : header.edge_prop_info_list()) {
        fnames.emplace(edge_prop.path);
      }
      for (const auto& part_prop : header.part_prop_info_list()) {
        fnames.emplace(part_prop.path);
      }
      // Duplicates eliminated by set
      fnames.emplace(header.topology_path());
    }
  }
  return fnames;
}

}  // namespace tsuba

void
tsuba::to_json(json& j, const tsuba::RDGManifest& meta) {
  j = json{
      {"magic", kRDGMagicNo},
      {"version", meta.version_},
      {"previous_version", meta.previous_version_},
      {"num_hosts", meta.num_hosts_},
      {"policy_id", meta.policy_id_},
      {"transpose", meta.transpose_},
      {"lineage", meta.lineage_},
  };
}

void
tsuba::from_json(const json& j, tsuba::RDGManifest& meta) {
  uint32_t magic;
  j.at("magic").get_to(magic);
  j.at("version").get_to(meta.version_);
  j.at("num_hosts").get_to(meta.num_hosts_);

  // these values are temporarily optional
  if (auto it = j.find("previous_version"); it != j.end()) {
    it->get_to(meta.previous_version_);
  }
  if (auto it = j.find("policy_id"); it != j.end()) {
    it->get_to(meta.policy_id_);
  }
  if (auto it = j.find("transpose"); it != j.end()) {
    it->get_to(meta.transpose_);
  }
  if (auto it = j.find("lineage"); it != j.end()) {
    it->get_to(meta.lineage_);
  }

  if (magic != kRDGMagicNo) {
    // nlohmann::json reports errors using exceptions
    throw std::runtime_error("RDG Magic number mismatch");
  }
}
