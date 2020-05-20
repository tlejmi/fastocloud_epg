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

#include "daemon/commands_info/refresh_url_info.h"

#define URL_FIELD "url"
#define EXTENSION_FIELD "extension"

namespace fastocloud {
namespace server {
namespace service {

RefreshUrlInfo::RefreshUrlInfo() : base_class(), url_(), extension_() {}

RefreshUrlInfo::RefreshUrlInfo(const url_t& url, const std::string& extension)
    : base_class(), url_(url), extension_(extension) {}

bool RefreshUrlInfo::IsValid() const {
  return url_.is_valid() && !extension_.empty();
}

RefreshUrlInfo::url_t RefreshUrlInfo::GetUrl() const {
  return url_;
}

std::string RefreshUrlInfo::GetExtension() const {
  return extension_;
}

common::Error RefreshUrlInfo::SerializeFields(json_object* out) const {
  if (!IsValid()) {
    return common::make_error_inval();
  }

  const std::string path_str = url_.spec();
  json_object_object_add(out, URL_FIELD, json_object_new_string(path_str.c_str()));
  json_object_object_add(out, EXTENSION_FIELD, json_object_new_string(extension_.c_str()));
  return common::Error();
}

common::Error RefreshUrlInfo::DoDeSerialize(json_object* serialized) {
  RefreshUrlInfo inf;
  json_object* jurl = nullptr;
  json_bool jurl_exists = json_object_object_get_ex(serialized, URL_FIELD, &jurl);
  if (!jurl_exists) {
    return common::make_error_inval();
  }
  inf.url_ = url_t(json_object_get_string(jurl));

  json_object* jextension = nullptr;
  json_bool jextension_exists = json_object_object_get_ex(serialized, EXTENSION_FIELD, &jextension);
  if (!jextension_exists) {
    return common::make_error_inval();
  }
  inf.extension_ = json_object_get_string(jextension);

  *this = inf;
  return common::Error();
}

}  // namespace service
}  // namespace server
}  // namespace fastocloud
