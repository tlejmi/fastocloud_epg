/*  Copyright (C) 2014-2020 FastoGT. All right reserved.
    This file is part of fastocloud.
    fastocloud is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    fastocloud is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    You should have received a copy of the GNU General Public License
    along with fastocloud.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "process_slave_wrapper.h"

#include <tinyxml2.h>

#include <fstream>
#include <map>
#include <thread>
#include <vector>

#include <common/daemon/commands/activate_info.h>
#include <common/daemon/commands/get_log_info.h>
#include <common/daemon/commands/stop_info.h>
#include <common/libev/inotify/inotify_client.h>
#include <common/license/expire_license.h>
#include <common/net/http_client.h>
#include <common/net/net.h>
#include <common/text_decoders/compress_zlib_edcoder.h>

#include <fastotv/types.h>

#include "daemon/client.h"
#include "daemon/commands.h"
#include "daemon/commands_info/details/shots.h"
#include "daemon/commands_info/prepare_info.h"
#include "daemon/commands_info/refresh_url_info.h"
#include "daemon/commands_info/server_info.h"
#include "daemon/commands_info/state_info.h"
#include "daemon/commands_info/sync_info.h"
#include "daemon/server.h"
#include "https_client.h"

#define PROGRAMME_TAG "programme"
#define CHANNEL_ATTR "channel"
#define TV_TAG "tv"

namespace {

common::Error GetResponse(const common::uri::GURL& url, common::http::HttpResponse* out) {
  if (!url.is_valid() || !out) {
    return common::make_error_inval();
  }

  common::net::HostAndPort real(url.host(), url.EffectiveIntPort());
  std::unique_ptr<common::net::IHttpClient> cl;
  if (url.SchemeIs("http")) {
    cl.reset(new common::net::HttpClient(real));
  } else {
    cl.reset(new fastocloud::server::HttpsClient(real));
  }
  common::ErrnoError cerr = cl->Connect();
  if (cerr) {
    return common::make_error_from_errno(cerr);
  }

  common::Error err = cl->Get(url.PathForRequest());
  if (err) {
    ignore_result(cl->Disconnect());
    return err;
  }

  common::http::HttpResponse resp;
  err = cl->ReadResponse(&resp);
  ignore_result(cl->Disconnect());
  if (err) {
    return err;
  }

  *out = resp;
  return common::Error();
}

bool FindOrCreateFileStream(const std::map<std::string, std::ofstream*>& origin,
                            const std::string& channel,
                            const common::file_system::ascii_directory_string_path& directory,
                            std::ofstream** out_file) {
  if (!out_file) {
    return false;
  }

  const auto it = origin.find(channel);
  if (it != origin.end()) {
    *out_file = it->second;
    return true;
  }

  const auto file_path = directory.MakeFileStringPath(channel + ".xml");
  if (!file_path) {
    return false;
  }

  std::ofstream* file = new std::ofstream;
  file->open(file_path->GetPath());
  if (!file->is_open()) {
    delete file;
    return false;
  }

  *file << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  *file << "<!DOCTYPE tv SYSTEM \"xmltv.dtd\">\n";
  *file << "<tv generator-info-name=\"dvb-epg-gen\">\n";
  *out_file = file;
  return false;
}

void ParseTagTV(const tinyxml2::XMLElement* tag_tv,
                const common::file_system::ascii_directory_string_path& out_epg_folder) {
  std::map<std::string, std::ofstream*> all_programms;
  const tinyxml2::XMLElement* tag_programme = tag_tv->FirstChildElement(PROGRAMME_TAG);
  while (tag_programme) {
    const char* cid = tag_programme->Attribute(CHANNEL_ATTR);
    if (!cid) {
      tag_programme = tag_programme->NextSiblingElement(PROGRAMME_TAG);
      continue;
    }

    std::ofstream* file = nullptr;
    bool find_file = FindOrCreateFileStream(all_programms, cid, out_epg_folder, &file);
    if (!find_file) {
      if (!file) {
        WARNING_LOG() << "Can't open file create file for: " << cid;
        tag_programme = tag_programme->NextSiblingElement(PROGRAMME_TAG);
        continue;
      }
      all_programms[cid] = file;
    }

    tinyxml2::XMLPrinter printer;
    tag_programme->Accept(&printer);
    *file << printer.CStr();
    tag_programme = tag_programme->NextSiblingElement(PROGRAMME_TAG);
  }

  for (auto it = all_programms.begin(); it != all_programms.end(); ++it) {
    *(it->second) << "</tv>\n";
    it->second->close();
    delete it->second;
  }
  INFO_LOG() << "Epg file processing finished, programms count: " << all_programms.size();
}

}  // namespace

namespace fastocloud {
namespace server {

struct ProcessSlaveWrapper::NodeStats {
  NodeStats() : prev(), prev_nshot(), timestamp(common::time::current_utc_mstime()) {}

  service::CpuShot prev;
  service::NetShot prev_nshot;
  fastotv::timestamp_t timestamp;
};

ProcessSlaveWrapper::ProcessSlaveWrapper(const Config& config)
    : config_(config),
      epg_watched_dir_(nullptr),
      loop_(nullptr),
      ping_client_timer_(INVALID_TIMER_ID),
      node_stats_timer_(INVALID_TIMER_ID),
      check_license_timer_(INVALID_TIMER_ID),
      node_stats_(new NodeStats) {
  loop_ = new DaemonServer(config.host, this);
  loop_->SetName("client_server");

  epg_watched_dir_ = new common::libev::inotify::IoInotifyClient(loop_, this);
  const common::file_system::ascii_directory_string_path epg_watched_dir(config.epg_in_path);
  ignore_result(epg_watched_dir_->WatchDirectory(
      epg_watched_dir, common::libev::inotify::EV_IN_CREATE | common::libev::inotify::EV_IN_CLOSE_WRITE));
}

common::ErrnoError ProcessSlaveWrapper::SendStopDaemonRequest(const Config& config) {
  if (!config.IsValid()) {
    return common::make_errno_error_inval();
  }

  common::net::HostAndPort host = config.host;
  if (host.GetHost() == PROJECT_NAME_LOWERCASE) {  // docker image
    host = common::net::HostAndPort::CreateLocalHostIPV4(host.GetPort());
  }

  common::net::socket_info client_info;
  common::ErrnoError err = common::net::connect(host, common::net::ST_SOCK_STREAM, nullptr, &client_info);
  if (err) {
    return err;
  }

  std::unique_ptr<ProtocoledDaemonClient> connection(new ProtocoledDaemonClient(nullptr, client_info));
  err = connection->StopMe();
  if (err) {
    ignore_result(connection->Close());
    return err;
  }

  ignore_result(connection->Close());
  return common::ErrnoError();
}

ProcessSlaveWrapper::~ProcessSlaveWrapper() {
  destroy(&loop_);
  destroy(&node_stats_);
}

int ProcessSlaveWrapper::Exec() {
  int res = EXIT_FAILURE;
  DaemonServer* server = static_cast<DaemonServer*>(loop_);
  common::ErrnoError err = server->Bind(true);
  if (err) {
    DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
    goto finished;
  }

  err = server->Listen(5);
  if (err) {
    DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
    goto finished;
  }

  res = server->Exec();

finished:
  return res;
}

void ProcessSlaveWrapper::HandleChanges(common::libev::inotify::IoInotifyClient* client,
                                        const common::file_system::ascii_directory_string_path& directory,
                                        const std::string& name,
                                        bool is_dir,
                                        uint32_t mask) {
  UNUSED(client);
  UNUSED(mask);
  if (is_dir) {
    return;
  }

  const auto new_epg_file = directory.MakeFileStringPath(name);
  if (!new_epg_file) {
    return;
  }

  HandleEpgFile(*new_epg_file);
}

void ProcessSlaveWrapper::PreLooped(common::libev::IoLoop* server) {
  UNUSED(server);
  ignore_result(loop_->RegisterClient(epg_watched_dir_));
  ping_client_timer_ = server->CreateTimer(ping_timeout_clients_seconds, true);
  node_stats_timer_ = server->CreateTimer(node_stats_send_seconds, true);
  check_license_timer_ = server->CreateTimer(check_license_timeout_seconds, true);
}

void ProcessSlaveWrapper::Accepted(common::libev::IoClient* client) {
  UNUSED(client);
}

void ProcessSlaveWrapper::Moved(common::libev::IoLoop* server, common::libev::IoClient* client) {
  UNUSED(server);
  UNUSED(client);
}

void ProcessSlaveWrapper::Closed(common::libev::IoClient* client) {
  UNUSED(client);
}

void ProcessSlaveWrapper::TimerEmited(common::libev::IoLoop* server, common::libev::timer_id_t id) {
  if (ping_client_timer_ == id) {
    std::vector<common::libev::IoClient*> online_clients = server->GetClients();
    for (size_t i = 0; i < online_clients.size(); ++i) {
      common::libev::IoClient* client = online_clients[i];
      ProtocoledDaemonClient* dclient = dynamic_cast<ProtocoledDaemonClient*>(client);
      if (dclient && dclient->IsVerified()) {
        common::ErrnoError err = dclient->Ping();
        if (err) {
          DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
          ignore_result(dclient->Close());
          delete dclient;
        } else {
          INFO_LOG() << "Sent ping to client[" << client->GetFormatedName() << "], from server["
                     << server->GetFormatedName() << "], " << online_clients.size() << " client(s) connected.";
        }
      }
    }
  } else if (node_stats_timer_ == id) {
    const std::string node_stats = MakeServiceStats(0);
    fastotv::protocol::request_t req;
    common::Error err_ser = StatisitcServiceBroadcast(node_stats, &req);
    if (err_ser) {
      return;
    }

    BroadcastClients(req);
  } else if (check_license_timer_ == id) {
    CheckLicenseExpired();
  }
}

void ProcessSlaveWrapper::Accepted(common::libev::IoChild* child) {
  UNUSED(child);
}

void ProcessSlaveWrapper::Moved(common::libev::IoLoop* server, common::libev::IoChild* child) {
  UNUSED(server);
  UNUSED(child);
}

void ProcessSlaveWrapper::ChildStatusChanged(common::libev::IoChild* child, int status, int signal) {
  UNUSED(child);
  UNUSED(status);
  UNUSED(signal);
}

void ProcessSlaveWrapper::HandleEpgFile(const common::file_system::ascii_file_string_path& epg_file_path) {
  const std::string path_str = epg_file_path.GetPath();
  INFO_LOG() << "New epg file notification: " << path_str;

  tinyxml2::XMLDocument doc;
  tinyxml2::XMLError xerr = doc.LoadFile(path_str.c_str());
  if (xerr != tinyxml2::XML_SUCCESS) {
    WARNING_LOG() << "Invalid epg file: " << path_str << ", error code: " << xerr;
    return;
  }

  const tinyxml2::XMLElement* tag_tv = doc.FirstChildElement(TV_TAG);
  if (!tag_tv) {
    WARNING_LOG() << "Can't find tv tag, file: " << path_str;
    return;
  }
  common::file_system::ascii_directory_string_path out_epg_folder(config_.epg_out_path);
  ParseTagTV(tag_tv, out_epg_folder);
}

void ProcessSlaveWrapper::StopImpl() {
  loop_->Stop();
}

void ProcessSlaveWrapper::BroadcastClients(const fastotv::protocol::request_t& req) {
  std::vector<common::libev::IoClient*> clients = loop_->GetClients();
  for (size_t i = 0; i < clients.size(); ++i) {
    ProtocoledDaemonClient* dclient = dynamic_cast<ProtocoledDaemonClient*>(clients[i]);
    if (dclient && dclient->IsVerified()) {
      common::ErrnoError err = dclient->WriteRequest(req);
      if (err) {
        WARNING_LOG() << "BroadcastClients error: " << err->GetDescription();
      }
    }
  }
}

common::ErrnoError ProcessSlaveWrapper::DaemonDataReceived(ProtocoledDaemonClient* dclient) {
  CHECK(loop_->IsLoopThread());
  std::string input_command;
  common::ErrnoError err = dclient->ReadCommand(&input_command);
  if (err) {
    return err;  // i don't want handle spam, comand must be foramated according protocol
  }

  fastotv::protocol::request_t* req = nullptr;
  fastotv::protocol::response_t* resp = nullptr;
  common::Error err_parse = common::protocols::json_rpc::ParseJsonRPC(input_command, &req, &resp);
  if (err_parse) {
    const std::string err_str = err_parse->GetDescription();
    return common::make_errno_error(err_str, EAGAIN);
  }

  if (req) {
    DEBUG_LOG() << "Received daemon request: " << input_command;
    err = HandleRequestServiceCommand(dclient, req);
    if (err) {
      DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
    }
    delete req;
  } else if (resp) {
    DEBUG_LOG() << "Received daemon responce: " << input_command;
    err = HandleResponceServiceCommand(dclient, resp);
    if (err) {
      DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
    }
    delete resp;
  } else {
    DNOTREACHED();
    return common::make_errno_error("Invalid command type.", EINVAL);
  }

  return common::ErrnoError();
}

void ProcessSlaveWrapper::DataReceived(common::libev::IoClient* client) {
  if (client == epg_watched_dir_) {
    epg_watched_dir_->ProcessRead();
    return;
  }

  if (ProtocoledDaemonClient* dclient = dynamic_cast<ProtocoledDaemonClient*>(client)) {
    common::ErrnoError err = DaemonDataReceived(dclient);
    if (err) {
      DEBUG_MSG_ERROR(err, common::logging::LOG_LEVEL_ERR);
      ignore_result(dclient->Close());
      delete dclient;
    }
  } else {
    NOTREACHED();
  }
}

void ProcessSlaveWrapper::DataReadyToWrite(common::libev::IoClient* client) {
  UNUSED(client);
}

void ProcessSlaveWrapper::PostLooped(common::libev::IoLoop* server) {
  UNUSED(server);

  if (ping_client_timer_ != INVALID_TIMER_ID) {
    server->RemoveTimer(ping_client_timer_);
    ping_client_timer_ = INVALID_TIMER_ID;
  }
  if (node_stats_timer_ != INVALID_TIMER_ID) {
    server->RemoveTimer(node_stats_timer_);
    node_stats_timer_ = INVALID_TIMER_ID;
  }
  if (check_license_timer_ != INVALID_TIMER_ID) {
    server->RemoveTimer(check_license_timer_);
    check_license_timer_ = INVALID_TIMER_ID;
  }

  loop_->UnRegisterClient(epg_watched_dir_);
}

common::ErrnoError ProcessSlaveWrapper::HandleRequestClientStopService(ProtocoledDaemonClient* dclient,
                                                                       const fastotv::protocol::request_t* req) {
  CHECK(loop_->IsLoopThread());
  if (!dclient->IsVerified()) {
    const auto info = dclient->GetInfo();
    common::net::HostAndPort host(info.host(), info.port());
    INFO_LOG() << "Stop request from host: " << common::ConvertToString(host);
    if (!host.IsLocalHost()) {
      return common::make_errno_error_inval();
    }
  }

  if (req->params) {
    const char* params_ptr = req->params->c_str();
    json_object* jstop = json_tokener_parse(params_ptr);
    if (!jstop) {
      return common::make_errno_error_inval();
    }

    common::daemon::commands::StopInfo stop_info;
    common::Error err_des = stop_info.DeSerialize(jstop);
    json_object_put(jstop);
    if (err_des) {
      const std::string err_str = err_des->GetDescription();
      return common::make_errno_error(err_str, EAGAIN);
    }

    StopImpl();
    return dclient->StopSuccess(req->id);
  }

  return common::make_errno_error_inval();
}

common::ErrnoError ProcessSlaveWrapper::HandleRequestClientPrepareService(ProtocoledDaemonClient* dclient,
                                                                          const fastotv::protocol::request_t* req) {
  CHECK(loop_->IsLoopThread());
  if (!dclient->IsVerified()) {
    return common::make_errno_error_inval();
  }

  if (req->params) {
    const char* params_ptr = req->params->c_str();
    json_object* jservice_state = json_tokener_parse(params_ptr);
    if (!jservice_state) {
      return common::make_errno_error_inval();
    }

    service::PrepareInfo state_info;
    common::Error err_des = state_info.DeSerialize(jservice_state);
    json_object_put(jservice_state);
    if (err_des) {
      const std::string err_str = err_des->GetDescription();
      return common::make_errno_error(err_str, EAGAIN);
    }

    service::StateInfo state;
    return dclient->PrepareServiceSuccess(req->id, state);
  }

  return common::make_errno_error_inval();
}

common::ErrnoError ProcessSlaveWrapper::HandleRequestClientSyncService(ProtocoledDaemonClient* dclient,
                                                                       const fastotv::protocol::request_t* req) {
  CHECK(loop_->IsLoopThread());
  if (!dclient->IsVerified()) {
    return common::make_errno_error_inval();
  }

  if (req->params) {
    const char* params_ptr = req->params->c_str();
    json_object* jservice_state = json_tokener_parse(params_ptr);
    if (!jservice_state) {
      return common::make_errno_error_inval();
    }

    service::SyncInfo sync_info;
    common::Error err_des = sync_info.DeSerialize(jservice_state);
    json_object_put(jservice_state);
    if (err_des) {
      const std::string err_str = err_des->GetDescription();
      return common::make_errno_error(err_str, EAGAIN);
    }

    return dclient->SyncServiceSuccess(req->id);
  }

  return common::make_errno_error_inval();
}

common::ErrnoError ProcessSlaveWrapper::HandleRequestClientGetLogService(ProtocoledDaemonClient* dclient,
                                                                         const fastotv::protocol::request_t* req) {
  CHECK(loop_->IsLoopThread());
  if (!dclient->IsVerified()) {
    return common::make_errno_error_inval();
  }

  if (req->params) {
    const char* params_ptr = req->params->c_str();
    json_object* jlog = json_tokener_parse(params_ptr);
    if (!jlog) {
      return common::make_errno_error_inval();
    }

    common::daemon::commands::GetLogInfo get_log_info;
    common::Error err_des = get_log_info.DeSerialize(jlog);
    json_object_put(jlog);
    if (err_des) {
      ignore_result(dclient->GetLogServiceFail(req->id, err_des));
      const std::string err_str = err_des->GetDescription();
      return common::make_errno_error(err_str, EAGAIN);
    }

    const auto remote_log_path = get_log_info.GetLogPath();
    if (!remote_log_path.SchemeIsHTTPOrHTTPS()) {
      common::ErrnoError errn = common::make_errno_error("Not supported protocol", EAGAIN);
      ignore_result(dclient->GetLogServiceFail(req->id, common::make_error_from_errno(errn)));
      return errn;
    }
    common::Error err =
        common::net::PostHttpFile(common::file_system::ascii_file_string_path(config_.log_path), remote_log_path);
    if (err) {
      ignore_result(dclient->GetLogServiceFail(req->id, err));
      const std::string err_str = err->GetDescription();
      return common::make_errno_error(err_str, EAGAIN);
    }

    return dclient->GetLogServiceSuccess(req->id);
  }

  return common::make_errno_error_inval();
}

common::ErrnoError ProcessSlaveWrapper::HandleRequestRefreshUrl(ProtocoledDaemonClient* dclient,
                                                                const fastotv::protocol::request_t* req) {
  CHECK(loop_->IsLoopThread());
  if (req->params) {
    const char* params_ptr = req->params->c_str();
    json_object* jref = json_tokener_parse(params_ptr);
    if (!jref) {
      return common::make_errno_error_inval();
    }

    service::RefreshUrlInfo ref_info;
    common::Error err_des = ref_info.DeSerialize(jref);
    json_object_put(jref);
    if (err_des) {
      const std::string err_str = err_des->GetDescription();
      ignore_result(dclient->RefreshUrlFail(req->id, err_des));
      return common::make_errno_error(err_str, EAGAIN);
    }

    const common::uri::GURL url = ref_info.GetUrl();
    const fastotv::protocol::request_t copy_req = *req;
#if 1
    std::thread th([this, url, copy_req, dclient]() {
      common::Error err = ExecDownloadUrl(url);
      auto func = [this, copy_req, dclient, err]() {
        auto clients = loop_->GetClients();
        for (auto client : clients) {
          if (client == dclient) {
            if (err) {
              const std::string err_str = err->GetDescription();
              ignore_result(dclient->RefreshUrlFail(copy_req.id, err));
              return common::make_errno_error(err_str, EAGAIN);
            }
            return dclient->RefreshUrlSuccess(copy_req.id);
          }
        }
        return common::make_errno_error_inval();
      };
      loop_->ExecInLoopThread(func);
    });
    th.detach();
    return common::ErrnoError();
#else
    common::Error err = ExecDownloadUrl(url);
    if (err) {
      const std::string err_str = err->GetDescription();
      ignore_result(dclient->RefreshUrlFail(copy_req.id, err));
      return common::make_errno_error(err_str, EAGAIN);
    }
    return dclient->RefreshUrlSuccess(copy_req.id);
#endif
  }

  return common::make_errno_error_inval();
}

common::ErrnoError ProcessSlaveWrapper::HandleRequestClientActivate(ProtocoledDaemonClient* dclient,
                                                                    const fastotv::protocol::request_t* req) {
  CHECK(loop_->IsLoopThread());
  if (req->params) {
    const char* params_ptr = req->params->c_str();
    json_object* jactivate = json_tokener_parse(params_ptr);
    if (!jactivate) {
      return common::make_errno_error_inval();
    }

    common::daemon::commands::ActivateInfo activate_info;
    common::Error err_des = activate_info.DeSerialize(jactivate);
    json_object_put(jactivate);
    if (err_des) {
      ignore_result(dclient->ActivateFail(req->id, err_des));
      return common::make_errno_error(err_des->GetDescription(), EAGAIN);
    }

    const auto expire_key = activate_info.GetLicense();
    common::time64_t tm;
    bool is_valid = common::license::GetExpireTimeFromKey(PROJECT_NAME_LOWERCASE, *expire_key, &tm);
    if (!is_valid) {
      common::Error err = common::make_error("Invalid expire key");
      ignore_result(dclient->ActivateFail(req->id, err));
      return common::make_errno_error(err->GetDescription(), EINVAL);
    }

    const std::string node_stats = MakeServiceStats(tm);
    common::ErrnoError err_ser = dclient->ActivateSuccess(req->id, node_stats);
    if (err_ser) {
      return err_ser;
    }

    dclient->SetVerified(true, tm);
    return common::ErrnoError();
  }

  return common::make_errno_error_inval();
}

common::ErrnoError ProcessSlaveWrapper::HandleResponcePingService(ProtocoledDaemonClient* dclient,
                                                                  const fastotv::protocol::response_t* resp) {
  UNUSED(dclient);
  CHECK(loop_->IsLoopThread());
  if (resp->IsMessage()) {
    const char* params_ptr = resp->message->result.c_str();
    json_object* jclient_ping = json_tokener_parse(params_ptr);
    if (!jclient_ping) {
      return common::make_errno_error_inval();
    }

    common::daemon::commands::ClientPingInfo client_ping_info;
    common::Error err_des = client_ping_info.DeSerialize(jclient_ping);
    json_object_put(jclient_ping);
    if (err_des) {
      const std::string err_str = err_des->GetDescription();
      return common::make_errno_error(err_str, EAGAIN);
    }
    return common::ErrnoError();
  }
  return common::ErrnoError();
}

common::ErrnoError ProcessSlaveWrapper::HandleRequestClientPingService(ProtocoledDaemonClient* dclient,
                                                                       const fastotv::protocol::request_t* req) {
  CHECK(loop_->IsLoopThread());
  if (!dclient->IsVerified()) {
    return common::make_errno_error_inval();
  }

  if (req->params) {
    const char* params_ptr = req->params->c_str();
    json_object* jstop = json_tokener_parse(params_ptr);
    if (!jstop) {
      return common::make_errno_error_inval();
    }

    common::daemon::commands::ClientPingInfo client_ping_info;
    common::Error err_des = client_ping_info.DeSerialize(jstop);
    json_object_put(jstop);
    if (err_des) {
      const std::string err_str = err_des->GetDescription();
      return common::make_errno_error(err_str, EAGAIN);
    }

    return dclient->Pong(req->id);
  }

  return common::make_errno_error_inval();
}

common::ErrnoError ProcessSlaveWrapper::HandleRequestServiceCommand(ProtocoledDaemonClient* dclient,
                                                                    const fastotv::protocol::request_t* req) {
  if (req->method == DAEMON_STOP_SERVICE) {
    return HandleRequestClientStopService(dclient, req);
  } else if (req->method == DAEMON_PING_SERVICE) {
    return HandleRequestClientPingService(dclient, req);
  } else if (req->method == DAEMON_ACTIVATE) {
    return HandleRequestClientActivate(dclient, req);
  } else if (req->method == DAEMON_PREPARE_SERVICE) {
    return HandleRequestClientPrepareService(dclient, req);
  } else if (req->method == DAEMON_SYNC_SERVICE) {
    return HandleRequestClientSyncService(dclient, req);
  } else if (req->method == DAEMON_GET_LOG_SERVICE) {
    return HandleRequestClientGetLogService(dclient, req);
  } else if (req->method == DAEMON_REFRESH_URL) {
    return HandleRequestRefreshUrl(dclient, req);
  }

  WARNING_LOG() << "Received unknown method: " << req->method;
  return common::ErrnoError();
}

common::ErrnoError ProcessSlaveWrapper::HandleResponceServiceCommand(ProtocoledDaemonClient* dclient,
                                                                     const fastotv::protocol::response_t* resp) {
  CHECK(loop_->IsLoopThread());
  if (!dclient->IsVerified()) {
    return common::make_errno_error_inval();
  }

  fastotv::protocol::request_t req;
  if (dclient->PopRequestByID(resp->id, &req)) {
    if (req.method == DAEMON_SERVER_PING) {
      ignore_result(HandleResponcePingService(dclient, resp));
    } else {
      WARNING_LOG() << "HandleResponceServiceCommand not handled command: " << req.method;
    }
  }

  return common::ErrnoError();
}

void ProcessSlaveWrapper::CheckLicenseExpired() {
  const auto license = config_.license_key;
  if (!license) {
    WARNING_LOG() << "You have an invalid license, service stopped";
    StopImpl();
    return;
  }

  common::time64_t tm;
  bool is_valid = common::license::GetExpireTimeFromKey(PROJECT_NAME_LOWERCASE, *license, &tm);
  if (!is_valid) {
    WARNING_LOG() << "You have an invalid license, service stopped";
    StopImpl();
    return;
  }

  if (tm < common::time::current_utc_mstime()) {
    WARNING_LOG() << "Your license have expired, service stopped";
    StopImpl();
    return;
  }
}

std::string ProcessSlaveWrapper::MakeServiceStats(common::time64_t expiration_time) const {
  service::CpuShot next = service::GetMachineCpuShot();
  double cpu_load = service::GetCpuMachineLoad(node_stats_->prev, next);
  node_stats_->prev = next;

  service::NetShot next_nshot = service::GetMachineNetShot();
  uint64_t bytes_recv = (next_nshot.bytes_recv - node_stats_->prev_nshot.bytes_recv);
  uint64_t bytes_send = (next_nshot.bytes_send - node_stats_->prev_nshot.bytes_send);
  node_stats_->prev_nshot = next_nshot;

  service::MemoryShot mem_shot = service::GetMachineMemoryShot();
  service::HddShot hdd_shot = service::GetMachineHddShot();
  service::SysinfoShot sshot = service::GetMachineSysinfoShot();
  std::string uptime_str = common::MemSPrintf("%lu %lu %lu", sshot.loads[0], sshot.loads[1], sshot.loads[2]);
  fastotv::timestamp_t current_time = common::time::current_utc_mstime();
  fastotv::timestamp_t ts_diff = (current_time - node_stats_->timestamp) / 1000;
  if (ts_diff == 0) {
    ts_diff = 1;  // divide by zero
  }
  node_stats_->timestamp = current_time;

  size_t daemons_client_count = 0;
  std::vector<common::libev::IoClient*> clients = loop_->GetClients();
  for (size_t i = 0; i < clients.size(); ++i) {
    ProtocoledDaemonClient* dclient = dynamic_cast<ProtocoledDaemonClient*>(clients[i]);
    if (dclient && dclient->IsVerified()) {
      daemons_client_count++;
    }
  }
  service::OnlineUsers online(daemons_client_count);
  service::ServerInfo stat(cpu_load, 0, uptime_str, mem_shot.ram_bytes_total, mem_shot.ram_bytes_free,
                           hdd_shot.hdd_bytes_total, hdd_shot.hdd_bytes_free, bytes_recv / ts_diff,
                           bytes_send / ts_diff, sshot.uptime, current_time, next_nshot.bytes_recv,
                           next_nshot.bytes_send, online);

  std::string node_stats;
  if (expiration_time != 0) {
    service::FullServiceInfo fstat(expiration_time, stat);
    common::Error err_ser = fstat.SerializeToString(&node_stats);
    if (err_ser) {
      const std::string err_str = err_ser->GetDescription();
      WARNING_LOG() << "Failed to generate node full statistic: " << err_str;
    }
  } else {
    common::Error err_ser = stat.SerializeToString(&node_stats);
    if (err_ser) {
      const std::string err_str = err_ser->GetDescription();
      WARNING_LOG() << "Failed to generate node statistic: " << err_str;
    }
  }
  return node_stats;
}

common::Error ProcessSlaveWrapper::ExecDownloadUrl(const common::uri::GURL& url) const {
  INFO_LOG() << "Epg url refresh request: " << url.spec();

  common::uri::GURL copy = url;
  size_t repeat_count = 5;
repeat:
  if (repeat_count == 0) {
    return common::make_error("A lot of redirects");
  }
  common::http::HttpResponse resp;
  common::Error err = GetResponse(copy, &resp);
  if (err) {
    return err;
  }

  const auto status = resp.GetStatus();
  if (status != common::http::HS_OK) {
    common::http::header_t redirect;
    if (status == common::http::HS_FOUND && resp.FindHeaderByKey("Location", false, &redirect)) {
      copy = common::uri::GURL(redirect.value);
      repeat_count--;
      goto repeat;
    } else {
      return common::make_error(common::MemSPrintf("Wrong http response code: %d", status));
    }
  }

  common::http::header_t cont;
  if (!resp.FindHeaderByKey("Content-type", false, &cont)) {
    return common::make_error("Unknown link content");
  }

  size_t delem = cont.value.find_first_of(';');
  if (delem != std::string::npos) {
    cont.value = cont.value.substr(0, delem);
  }

  std::string file_ext = common::file_system::get_file_extension(url.ExtractFileName());
  const char* ext = common::http::MimeTypes::GetExtension(cont.value.c_str());
  if (ext) {
    file_ext = ext;
  }

  if (file_ext.empty()) {
    return common::make_error(common::MemSPrintf("Not handled content type: %s", cont.value));
  }

  const auto parseBody = [&](const common::char_byte_array_t& body) {
    tinyxml2::XMLDocument doc;
    tinyxml2::XMLError xerr = doc.Parse(body.data(), body.size());
    if (xerr != tinyxml2::XML_SUCCESS) {
      return common::make_error(common::MemSPrintf("Xml parse error: %s", tinyxml2::XMLDocument::ErrorIDToName(xerr)));
    }

    const tinyxml2::XMLElement* tag_tv = doc.FirstChildElement(TV_TAG);
    if (!tag_tv) {
      return common::make_error(common::MemSPrintf("Can't find tv tag"));
    }

    common::file_system::ascii_directory_string_path out_epg_folder(config_.epg_out_path);
    ParseTagTV(tag_tv, out_epg_folder);
    return common::Error();
  };

  if (common::EqualsASCII(file_ext, "xml", false) || common::EqualsASCII(file_ext, "*xml", false)) {
    return parseBody(resp.GetBody());
  } else if (common::EqualsASCII(file_ext, "gz", false) || common::EqualsASCII(file_ext, "bin", false)) {
    const auto body = resp.GetBody();
    common::CompressZlibEDcoder gzip(false, common::CompressZlibEDcoder::GZIP_DEFLATE);
    common::char_byte_array_t decoded;
    err = gzip.Decode(body, &decoded);
    if (err) {
      return err;
    }
    return parseBody(decoded);
  }

  return common::make_error(common::MemSPrintf("Not supported content type: %s", cont.value));
}

}  // namespace server
}  // namespace fastocloud
