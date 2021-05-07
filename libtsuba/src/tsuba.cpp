#include "tsuba/tsuba.h"

#include <sstream>

#include "GlobalState.h"
#include "RDGHandleImpl.h"
#include "katana/Backtrace.h"
#include "katana/CommBackend.h"
#include "katana/Env.h"
#include "tsuba/Errors.h"
#include "tsuba/NameServerClient.h"
#include "tsuba/Preload.h"
#include "tsuba/file.h"

namespace {

katana::NullCommBackend default_comm_backend;
std::unique_ptr<tsuba::NameServerClient> default_ns_client;

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

/*
bool
ContainsValidMetaFile(const std::string& dir) {
  auto list_res = FileList(dir);
  if (!list_res) {
    KATANA_LOG_DEBUG(
        "ContainsValidMetaFile dir: {}: {}", dir, list_res.error());
    return false;
  }
  for (const std::string& file : list_res.value()) {
    auto dir_file_uri = katana::Uri::Make(dir).value().Join(file);

    if(tsuba::RDGManifest::IsMetaUri(dir_file_uri)) {
      KATANA_LOG_DEBUG("ContainsValidMetaFile dir: {}: {}", dir,file);
      return true;
    }
  }
  return false;
}*/

katana::Result<katana::Uri>
FindBaseMetaFile(const katana::Uri& name) {
  KATANA_LOG_DEBUG_ASSERT(!tsuba::RDGManifest::IsMetaUri(name));
  auto list_res = FileList(name.string());
  if (!list_res) {
    return list_res.error();
  }

  uint64_t last_version = 0;
  std::string found_meta;
  for (const std::string& file : list_res.value()) {
    if (auto res = tsuba::RDGManifest::ParseViewNameFromName(file); res) {
      if (res.value() != "rdg") {
        continue;
      }

      if (auto res = tsuba::RDGManifest::ParseVersionFromName(file); res) {
        if (res.value() > last_version) {
          found_meta = file;
          last_version = res.value();
        }
      }
    }
  }
  if (found_meta.empty()) {
    return KATANA_ERROR(
        tsuba::ErrorCode::NotFound, "failed: could not find meta file in {}",
        name);
  }

  auto res = name.Join(found_meta);

  return name.Join(found_meta);
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
        tsuba::ErrorCode::NotFound, "failed: could not find meta file in {}",
        name);
  }
  return name.Join(found_meta);
}

}  // namespace

katana::Result<tsuba::RDGHandle>
tsuba::Open(const std::string& rdg_name, uint32_t flags) {
  if (!OpenFlagsValid(flags)) {
    return KATANA_ERROR(
        ErrorCode::InvalidArgument, "invalid value for flags ({:#x})", flags);
  }

  auto uri_res = katana::Uri::Make(rdg_name);
  if (!uri_res) {
    return uri_res.error();
  }
  katana::Uri uri = std::move(uri_res.value());

  if (RDGManifest::IsMetaUri(uri)) {
    auto meta_res = tsuba::RDGManifest::Make(uri);
    if (!meta_res) {
      return meta_res.error();
    }

    return RDGHandle{
        .impl_ = new RDGHandleImpl(flags, std::move(meta_res.value()))};
  }

  auto latest_uri = FindLatestMetaFile(uri);
  if (!latest_uri) {
    return KATANA_ERROR(
        ErrorCode::InvalidArgument, "failed to find latest RDGManifest at {}",
        uri.string());
  }

  auto meta_res = tsuba::RDGManifest::Make(latest_uri.value());
  if (!meta_res) {
    return meta_res.error();
  }

  return RDGHandle{
      .impl_ = new RDGHandleImpl(flags, std::move(meta_res.value()))};
}

katana::Result<void>
tsuba::Close(RDGHandle handle) {
  delete handle.impl_;
  return katana::ResultSuccess();
}

katana::Result<void>
tsuba::Create(const std::string& name) {
  auto uri_res = katana::Uri::Make(name);
  if (!uri_res) {
    return uri_res.error();
  }
  katana::Uri uri = std::move(uri_res.value());

  KATANA_LOG_DEBUG_ASSERT(!RDGManifest::IsMetaUri(uri));
  // the default construction is the empty RDG
  tsuba::RDGManifest meta{};

  katana::CommBackend* comm = Comm();
  if (comm->ID == 0) {
    //if (ContainsValidMetaFile(name)) {
    //  return KATANA_ERROR(
    ///      ErrorCode::InvalidArgument,
    //      "unable to create {}: path already contains a valid meta file", name);
    // }
    std::string s = meta.ToJsonString();
    // TODO(yasser): this is not correct view type, place holder while rest is updated
    if (auto res = tsuba::FileStore(
            tsuba::RDGManifest::FileName(uri, "rdg", meta.version()).string(),
            reinterpret_cast<const uint8_t*>(s.data()), s.size());
        !res) {
      comm->NotifyFailure();
      return res.error().WithContext(
          "failed to store RDG file: {}", uri.string());
    }
  }

  // NS handles MPI coordination
  if (auto res = tsuba::NS()->CreateIfAbsent(uri, meta); !res) {
    return res.error().WithContext(
        "failed to create RDG name: {}", uri.string());
  }

  return katana::ResultSuccess();
}

katana::Result<
    std::vector<std::tuple<std::string, std::vector<std::string>, std::string>>>
tsuba::StatViews(const std::string& rdg_dir) {
  std::vector<std::tuple<std::string, std::vector<std::string>, std::string>>
      views_found;
  auto list_res = FileList(rdg_dir);
  if (!list_res) {
    return list_res.error();
  }

  for (const std::string& file : list_res.value()) {
    auto view_type_res = tsuba::RDGManifest::ParseViewNameFromName(file);
    auto view_args_res = tsuba::RDGManifest::ParseViewArgsFromName(file);
    if (!view_type_res || !view_args_res)
      continue;
    views_found.push_back({view_type_res.value(), view_args_res.value(), file});
  }

  return views_found;
}

katana::Result<katana::Uri>
tsuba::StatBaseUri(const std::string& rdg_name) {
  auto uri_res = katana::Uri::Make(rdg_name);
  if (!uri_res) {
    return uri_res.error();
  }
  katana::Uri uri = std::move(uri_res.value());

  auto base_uri = FindBaseMetaFile(uri);
  if (!base_uri) {
    KATANA_LOG_DEBUG("failed to find base rdg file: {}", base_uri.error());
    return base_uri.error();
  }
  return base_uri;
}

katana::Result<tsuba::RDGStat>
tsuba::StatBase(const std::string& rdg_name) {
  KATANA_LOG_DEBUG("tsuba::StatBase(rdg_name={})", rdg_name);
  auto uri_res = katana::Uri::Make(rdg_name);
  if (!uri_res) {
    return uri_res.error();
  }
  katana::Uri uri = std::move(uri_res.value());

  auto base_uri = FindBaseMetaFile(uri);
  if (!base_uri) {
    return base_uri.error();
  }

  auto rdg_res = RDGManifest::Make(base_uri.value());
  if (!rdg_res) {
    return rdg_res.error();
  }

  RDGManifest meta = rdg_res.value();
  return RDGStat{
      .num_partitions = meta.num_hosts(),
      .policy_id = meta.policy_id(),
      .transpose = meta.transpose(),
  };
}

katana::Result<tsuba::RDGStat>
tsuba::Stat(const std::string& rdg_name) {
  auto uri_res = katana::Uri::Make(rdg_name);
  if (!uri_res) {
    return uri_res.error();
  }
  katana::Uri uri = std::move(uri_res.value());

  // IF this is a directory, try to find the latest version within and return
  if (!RDGManifest::IsMetaUri(uri)) {
    auto latest_uri = FindLatestMetaFile(uri);
    if (!latest_uri) {
      return latest_uri.error();
    }
    return tsuba::Stat(latest_uri.value().string());
  }

  auto rdg_res = RDGManifest::Make(uri);
  if (!rdg_res) {
    if (rdg_res.error() == katana::ErrorCode::JsonParseFailed) {
      return RDGStat{
          .num_partitions = 1,
          .policy_id = 0,
          .transpose = false,
      };
    }
    return rdg_res.error();
  }

  RDGManifest meta = rdg_res.value();
  return RDGStat{
      .num_partitions = meta.num_hosts(),
      .policy_id = meta.policy_id(),
      .transpose = meta.transpose(),
  };
}

katana::Uri
tsuba::MakeTopologyFileName(tsuba::RDGHandle handle) {
  return GetRDGDir(handle).RandFile("topology");
}

katana::Uri
tsuba::GetRDGDir(tsuba::RDGHandle handle) {
  return handle.impl_->rdg_meta().dir();
}

katana::Result<void>
tsuba::Init(katana::CommBackend* comm) {
  tsuba::Preload();
  auto client_res = GlobalState::MakeNameServerClient();
  if (!client_res) {
    return client_res.error();
  }
  default_ns_client = std::move(client_res.value());
  katana::InitBacktrace();
  return GlobalState::Init(comm, default_ns_client.get());
}

katana::Result<void>
tsuba::Init() {
  return Init(&default_comm_backend);
}

katana::Result<void>
tsuba::Fini() {
  auto r = GlobalState::Fini();
  tsuba::PreloadFini();
  return r;
}
