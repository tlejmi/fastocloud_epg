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

#include "daemon/commands_info/state_info.h"

namespace fastocloud {
namespace server {
namespace service {

StateInfo::StateInfo() : base_class() {}

common::Error StateInfo::SerializeFields(json_object* deserialized) const {
  UNUSED(deserialized);
  return common::Error();
}

common::Error StateInfo::DoDeSerialize(json_object* serialized) {
  UNUSED(serialized);

  *this = StateInfo();
  return common::Error();
}

}  // namespace service
}  // namespace server
}  // namespace fastocloud
