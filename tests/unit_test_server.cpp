#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <string.h>

#include <common/file_system/file_system.h>
#include <fastotv/commands_info/epg_info.h>

#define TEST_FILE_PATH PROJECT_TEST_SOURCES_DIR "/data/epg_tvprofil.net.xml"

#include <tinyxml2.h>

typedef std::map<std::string, std::ofstream*> container_t;
container_t all_programms;

std::ofstream* FindOrCreateFileStream(const std::string& channel, const std::string& work_dir) {
  std::ofstream* file = nullptr;
  container_t::iterator it = all_programms.lower_bound(channel);
  if (it != all_programms.end() && !(all_programms.key_comp()(channel, it->first))) {
    return it->second;
  }

  const std::string file_path = work_dir + "/" + channel + ".xml";
  file = new std::ofstream;
  file->open(file_path);
  if (!file->is_open()) {
    return nullptr;
  }
  all_programms.insert(it, container_t::value_type(channel, file));
  return file;
}

TEST(Xml, parse) {
  const std::string work_dir = "/tmp/test_out";
  common::ErrnoError err = common::file_system::create_directory(work_dir, false);
  ASSERT_FALSE(err);

  tinyxml2::XMLDocument doc;
  tinyxml2::XMLError xerr = doc.LoadFile(TEST_FILE_PATH);
  ASSERT_EQ(xerr, tinyxml2::XML_SUCCESS);

  const tinyxml2::XMLElement* tag_tv = doc.FirstChildElement("tv");
  ASSERT_TRUE(tag_tv);

  std::map<std::string, std::ofstream*> all_programms;
  const tinyxml2::XMLElement* tag_programme = tag_tv->FirstChildElement("programme");
  int pr_tag = 0;
  while (tag_programme) {
    const char* cid = tag_programme->Attribute("channel");
    if (!cid) {
      continue;
    }

    std::ofstream* file = FindOrCreateFileStream(cid, work_dir);

    if (!file) {
      continue;
    }
    all_programms[cid] = file;

    tinyxml2::XMLPrinter printer;
    tag_programme->Accept(&printer);
    *file << printer.CStr();
    tag_programme = tag_programme->NextSiblingElement("programme");
    pr_tag++;
  }

  ASSERT_EQ(pr_tag, 3113);
  ASSERT_EQ(all_programms.size(), 47);
  for (auto it = all_programms.begin(); it != all_programms.end(); ++it) {
    it->second->close();
    delete it->second;
  }

  err = common::file_system::remove_directory(work_dir, true);
  ASSERT_FALSE(err);
}
