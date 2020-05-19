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

#pragma once

#include <string>

#include <common/libev/inotify/inotify_client_observer.h>
#include <common/libev/io_loop_observer.h>

#include <fastotv/protocol/protocol.h>
#include <fastotv/protocol/types.h>

#include "config.h"

namespace fastocloud {
namespace server {

class ProtocoledDaemonClient;

class ProcessSlaveWrapper : public common::libev::IoLoopObserver,
                            public common::libev::inotify::IoInotifyClientObserver {
 public:
  enum { node_stats_send_seconds = 10, ping_timeout_clients_seconds = 60, check_license_timeout_seconds = 300 };

  explicit ProcessSlaveWrapper(const Config& config);
  ~ProcessSlaveWrapper() override;

  static common::ErrnoError SendStopDaemonRequest(const Config& config);
  common::net::HostAndPort GetServerHostAndPort();

  int Exec() WARN_UNUSED_RESULT;

 protected:
  void HandleChanges(common::libev::inotify::IoInotifyClient* client,
                     const common::file_system::ascii_directory_string_path& directory,
                     const std::string& name,
                     bool is_dir,
                     uint32_t mask) override;

  void PreLooped(common::libev::IoLoop* server) override;
  void Accepted(common::libev::IoClient* client) override;
  void Moved(common::libev::IoLoop* server,
             common::libev::IoClient* client) override;  // owner server, now client is orphan
  void Closed(common::libev::IoClient* client) override;
  void TimerEmited(common::libev::IoLoop* server, common::libev::timer_id_t id) override;

  void Accepted(common::libev::IoChild* child) override;
  void Moved(common::libev::IoLoop* server, common::libev::IoChild* child) override;
  void ChildStatusChanged(common::libev::IoChild* child, int status, int signal) override;

  void DataReceived(common::libev::IoClient* client) override;
  void DataReadyToWrite(common::libev::IoClient* client) override;
  void PostLooped(common::libev::IoLoop* server) override;

  virtual common::ErrnoError HandleRequestServiceCommand(ProtocoledDaemonClient* dclient,
                                                         const fastotv::protocol::request_t* req) WARN_UNUSED_RESULT;
  virtual common::ErrnoError HandleResponceServiceCommand(ProtocoledDaemonClient* dclient,
                                                          const fastotv::protocol::response_t* resp) WARN_UNUSED_RESULT;

 private:
  void StopImpl();

  void BroadcastClients(const fastotv::protocol::request_t& req);
  void HandleEpgFile(const common::file_system::ascii_file_string_path& epg_file_path);

  common::ErrnoError DaemonDataReceived(ProtocoledDaemonClient* dclient) WARN_UNUSED_RESULT;

  // service
  common::ErrnoError HandleRequestClientActivate(ProtocoledDaemonClient* dclient,
                                                 const fastotv::protocol::request_t* req) WARN_UNUSED_RESULT;
  common::ErrnoError HandleRequestClientPingService(ProtocoledDaemonClient* dclient,
                                                    const fastotv::protocol::request_t* req) WARN_UNUSED_RESULT;
  common::ErrnoError HandleRequestClientStopService(ProtocoledDaemonClient* dclient,
                                                    const fastotv::protocol::request_t* req) WARN_UNUSED_RESULT;
  common::ErrnoError HandleRequestClientPrepareService(ProtocoledDaemonClient* dclient,
                                                       const fastotv::protocol::request_t* req);
  common::ErrnoError HandleRequestClientSyncService(ProtocoledDaemonClient* dclient,
                                                    const fastotv::protocol::request_t* req) WARN_UNUSED_RESULT;
  common::ErrnoError HandleRequestClientGetLogService(ProtocoledDaemonClient* dclient,
                                                      const fastotv::protocol::request_t* req) WARN_UNUSED_RESULT;
  common::ErrnoError HandleResponcePingService(ProtocoledDaemonClient* dclient,
                                               const fastotv::protocol::response_t* resp) WARN_UNUSED_RESULT;

  void CheckLicenseExpired();
  std::string MakeServiceStats(common::time64_t expiration_time) const;

  const Config config_;

  common::libev::inotify::IoInotifyClient* epg_watched_dir_;
  common::libev::IoLoop* loop_;

  common::libev::timer_id_t ping_client_timer_;
  common::libev::timer_id_t node_stats_timer_;
  common::libev::timer_id_t check_license_timer_;

  struct NodeStats;
  NodeStats* node_stats_;
};

}  // namespace server
}  // namespace fastocloud
