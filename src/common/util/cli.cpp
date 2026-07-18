#include "common/util/cli.h"

#include <charconv>

namespace ipc::common {

std::expected<std::uint64_t, std::string> ParsePositiveInt(std::string_view flag,
                                                           std::string_view value) {
  std::uint64_t result = 0;
  auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
  if (ec != std::errc{} || ptr != value.data() + value.size()) {
    return std::unexpected(std::string(flag) +
                           " must be a positive integer, got: " + std::string(value));
  }
  if (result == 0) {
    return std::unexpected(std::string(flag) + " must be greater than zero");
  }
  return result;
}

}  // namespace ipc::common
