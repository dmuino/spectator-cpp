#pragma once

#include <string>
#include <unordered_map>

namespace spectator {

struct Config {
  std::string endpoint;
  std::unordered_map<std::string, std::string> common_tags;
};

}  // namespace spectator
