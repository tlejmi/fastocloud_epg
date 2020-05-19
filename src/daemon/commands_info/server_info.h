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

#include <common/net/types.h>
#include <common/serializer/json_serializer.h>

#include <fastotv/commands_info/operation_system_info.h>
#include <fastotv/types.h>

#include "daemon/commands_info/details/shots.h"

namespace fastocloud {
namespace server {
namespace service {

class OnlineUsers : public common::serializer::JsonSerializer<OnlineUsers> {
 public:
  typedef JsonSerializer<OnlineUsers> base_class;
  OnlineUsers();
  explicit OnlineUsers(size_t daemon);

 protected:
  common::Error DoDeSerialize(json_object* serialized) override;
  common::Error SerializeFields(json_object* out) const override;

 private:
  size_t daemon_;
};

class ServerInfo : public common::serializer::JsonSerializer<ServerInfo> {
 public:
  typedef JsonSerializer<ServerInfo> base_class;
  typedef double cpu_load_t;
  typedef double gpu_load_t;
  ServerInfo();
  ServerInfo(cpu_load_t cpu_load,
             const std::string& uptime,
             const MemoryShot& mem_shot,
             const HddShot& hdd_shot,
             fastotv::bandwidth_t net_bytes_recv,
             fastotv::bandwidth_t net_bytes_send,
             const SysinfoShot& sys,
             fastotv::timestamp_t timestamp,
             const OnlineUsers& online_users);

  cpu_load_t GetCpuLoad() const;
  std::string GetUptime() const;
  MemoryShot GetMemShot() const;
  HddShot GetHddShot() const;
  fastotv::bandwidth_t GetNetBytesRecv() const;
  fastotv::bandwidth_t GetNetBytesSend() const;
  fastotv::timestamp_t GetTimestamp() const;
  OnlineUsers GetOnlineUsers() const;

 protected:
  common::Error DoDeSerialize(json_object* serialized) override;
  common::Error SerializeFields(json_object* out) const override;

 private:
  cpu_load_t cpu_load_;
  std::string uptime_;
  MemoryShot mem_shot_;
  HddShot hdd_shot_;
  fastotv::bandwidth_t net_bytes_recv_;
  fastotv::bandwidth_t net_bytes_send_;
  fastotv::timestamp_t current_ts_;
  SysinfoShot sys_shot_;
  OnlineUsers online_users_;
};

class FullServiceInfo : public ServerInfo {
 public:
  typedef ServerInfo base_class;
  FullServiceInfo();
  explicit FullServiceInfo(common::time64_t exp_time, const base_class& base);

  std::string GetProjectVersion() const;

 protected:
  common::Error DoDeSerialize(json_object* serialized) override;
  common::Error SerializeFields(json_object* out) const override;

 private:
  common::time64_t exp_time_;
  std::string project_;
  std::string proj_ver_;
  fastotv::commands_info::OperationSystemInfo os_;
};

}  // namespace service
}  // namespace server
}  // namespace fastocloud
