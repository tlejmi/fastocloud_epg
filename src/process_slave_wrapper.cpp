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
#include <common/daemon/commands/stop_info.h>
#include <common/libev/inotify/inotify_client.h>
#include <common/license/expire_license.h>
#include <common/net/net.h>

#include "daemon/client.h"
#include "daemon/commands.h"
#include "daemon/server.h"

#define PROGRAMME_TAG "programme"
#define CHANNEL_ATTR "channel"
#define TV_TAG "tv"

namespace {

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

}  // namespace

namespace fastocloud {
namespace server {

ProcessSlaveWrapper::ProcessSlaveWrapper(const Config& config)
    : config_(config), epg_watched_dir_(nullptr), loop_(nullptr), check_license_timer_(INVALID_TIMER_ID) {
  loop_ = new DaemonServer(config.host, this);
  loop_->SetName("client_server");

  epg_watched_dir_ = new common::libev::inotify::IoInotifyClient(loop_, this);
  const common::file_system::ascii_directory_string_path epg_watched_dir(config.epg_in_path);
  epg_watched_dir_->WatchDirectory(epg_watched_dir,
                                   common::libev::inotify::EV_IN_CREATE | common::libev::inotify::EV_IN_CLOSE_WRITE);
}

int ProcessSlaveWrapper::SendStopDaemonRequest(const Config& config) {
  if (!config.IsValid()) {
    return EXIT_FAILURE;
  }

  const common::net::HostAndPort host = config.host;
  common::net::socket_info client_info;
  common::ErrnoError err = common::net::connect(host, common::net::ST_SOCK_STREAM, nullptr, &client_info);
  if (err) {
    return EXIT_FAILURE;
  }

  std::unique_ptr<ProtocoledDaemonClient> connection(new ProtocoledDaemonClient(nullptr, client_info));
  err = connection->StopMe();
  if (err) {
    ignore_result(connection->Close());
    return EXIT_FAILURE;
  }

  ignore_result(connection->Close());
  return EXIT_SUCCESS;
}

ProcessSlaveWrapper::~ProcessSlaveWrapper() {
  destroy(&loop_);
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
  loop_->RegisterClient(epg_watched_dir_);
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
  UNUSED(server);
  if (check_license_timer_ == id) {
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

  std::map<std::string, std::ofstream*> all_programms;
  common::file_system::ascii_directory_string_path out_epg_folder(config_.epg_out_path);
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

  if (check_license_timer_ != INVALID_TIMER_ID) {
    server->RemoveTimer(check_license_timer_);
    check_license_timer_ = INVALID_TIMER_ID;
  }
}

common::ErrnoError ProcessSlaveWrapper::HandleRequestClientStopService(ProtocoledDaemonClient* dclient,
                                                                       const fastotv::protocol::request_t* req) {
  CHECK(loop_->IsLoopThread());
  if (!dclient->IsVerified()) {
    const auto info = dclient->GetInfo();
    common::net::HostAndPort host(info.host(), info.port());
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

    common::ErrnoError err_ser = dclient->ActivateSuccess(req->id);
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

}  // namespace server
}  // namespace fastocloud
