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

#include "daemon/client.h"

#include <string>

#include "daemon/commands_factory.h"

namespace fastocloud {
namespace server {

ProtocoledDaemonClient::ProtocoledDaemonClient(common::libev::IoLoop* server, const common::net::socket_info& info)
    : base_class(server, info) {}

common::ErrnoError ProtocoledDaemonClient::StopMe() {
  const common::daemon::commands::StopInfo stop_req;
  fastotv::protocol::request_t req;
  common::Error err_ser = StopServiceRequest(NextRequestID(), stop_req, &req);
  if (err_ser) {
    return common::make_errno_error(err_ser->GetDescription(), EAGAIN);
  }

  return WriteRequest(req);
}

common::ErrnoError ProtocoledDaemonClient::StopFail(fastotv::protocol::sequance_id_t id, common::Error err) {
  const std::string error_str = err->GetDescription();
  fastotv::protocol::response_t resp;
  common::Error err_ser = StopServiceResponseFail(id, error_str, &resp);
  if (err_ser) {
    return common::make_errno_error(err_ser->GetDescription(), EAGAIN);
  }

  return WriteResponse(resp);
}

common::ErrnoError ProtocoledDaemonClient::StopSuccess(fastotv::protocol::sequance_id_t id) {
  fastotv::protocol::response_t resp;
  common::Error err_ser = StopServiceResponseSuccess(id, &resp);
  if (err_ser) {
    return common::make_errno_error(err_ser->GetDescription(), EAGAIN);
  }

  return WriteResponse(resp);
}

common::ErrnoError ProtocoledDaemonClient::Ping() {
  common::daemon::commands::ClientPingInfo server_ping_info;
  return Ping(server_ping_info);
}

common::ErrnoError ProtocoledDaemonClient::Ping(const common::daemon::commands::ClientPingInfo& server_ping_info) {
  fastotv::protocol::request_t ping_request;
  common::Error err_ser = PingRequest(NextRequestID(), server_ping_info, &ping_request);
  if (err_ser) {
    return common::make_errno_error(err_ser->GetDescription(), EAGAIN);
  }

  return WriteRequest(ping_request);
}

common::ErrnoError ProtocoledDaemonClient::Pong(fastotv::protocol::sequance_id_t id) {
  common::daemon::commands::ServerPingInfo server_ping_info;
  return Pong(id, server_ping_info);
}

common::ErrnoError ProtocoledDaemonClient::Pong(fastotv::protocol::sequance_id_t id,
                                                const common::daemon::commands ::ServerPingInfo& pong) {
  fastotv::protocol::response_t resp;
  common::Error err_ser = PingServiceResponce(id, pong, &resp);
  if (err_ser) {
    return common::make_errno_error(err_ser->GetDescription(), EAGAIN);
  }
  return WriteResponse(resp);
}

common::ErrnoError ProtocoledDaemonClient::ActivateFail(fastotv::protocol::sequance_id_t id, common::Error err) {
  const std::string error_str = err->GetDescription();
  fastotv::protocol::response_t resp;
  common::Error err_ser = ActivateResponseFail(id, error_str, &resp);
  if (err_ser) {
    return common::make_errno_error(err_ser->GetDescription(), EAGAIN);
  }

  return WriteResponse(resp);
}

common::ErrnoError ProtocoledDaemonClient::ActivateSuccess(fastotv::protocol::sequance_id_t id,
                                                           const std::string& result) {
  fastotv::protocol::response_t resp;
  common::Error err_ser = ActivateResponse(id, result, &resp);
  if (err_ser) {
    return common::make_errno_error(err_ser->GetDescription(), EAGAIN);
  }

  return WriteResponse(resp);
}

common::ErrnoError ProtocoledDaemonClient::RefreshUrlFail(fastotv::protocol::sequance_id_t id, common::Error err) {
  const std::string error_str = err->GetDescription();
  fastotv::protocol::response_t resp;
  common::Error err_ser = RefreshUrlResponseFail(id, error_str, &resp);
  if (err_ser) {
    return common::make_errno_error(err_ser->GetDescription(), EAGAIN);
  }

  return WriteResponse(resp);
}

common::ErrnoError ProtocoledDaemonClient::RefreshUrlSuccess(fastotv::protocol::sequance_id_t id) {
  fastotv::protocol::response_t resp;
  common::Error err_ser = RefreshUrlResponse(id, &resp);
  if (err_ser) {
    return common::make_errno_error(err_ser->GetDescription(), EAGAIN);
  }

  return WriteResponse(resp);
}

common::ErrnoError ProtocoledDaemonClient::PrepareServiceSuccess(fastotv::protocol::sequance_id_t id,
                                                                 const service::StateInfo& state) {
  fastotv::protocol::response_t resp;
  common::Error err_ser = PrepareServiceResponceSuccess(id, state, &resp);
  if (err_ser) {
    return common::make_errno_error(err_ser->GetDescription(), EAGAIN);
  }

  return WriteResponse(resp);
}

common::ErrnoError ProtocoledDaemonClient::SyncServiceSuccess(fastotv::protocol::sequance_id_t id) {
  fastotv::protocol::response_t resp;
  common::Error err_ser = SyncServiceResponceSuccess(id, &resp);
  if (err_ser) {
    return common::make_errno_error(err_ser->GetDescription(), EAGAIN);
  }

  return WriteResponse(resp);
}

common::ErrnoError ProtocoledDaemonClient::GetLogServiceSuccess(fastotv::protocol::sequance_id_t id) {
  fastotv::protocol::response_t resp;
  common::Error err_ser = GetLogServiceResponseSuccess(id, &resp);
  if (err_ser) {
    return common::make_errno_error(err_ser->GetDescription(), EAGAIN);
  }

  return WriteResponse(resp);
}

}  // namespace server
}  // namespace fastocloud
