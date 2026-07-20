#ifndef IPC_COMMON_UTIL_RAND_H_
#define IPC_COMMON_UTIL_RAND_H_

#include <concepts>
#include <cstdint>
#include <random>

namespace ipc::common {

// A random value of T, seeded from std::random_device. Not cryptographic.
// T is restricted to what std::uniform_int_distribution supports -- e.g.
// std::int8_t would otherwise fail deep inside <random> with a worse error.
template <typename T>
  requires std::same_as<T, std::int16_t> || std::same_as<T, std::int32_t> ||
           std::same_as<T, std::int64_t> || std::same_as<T, std::uint16_t> ||
           std::same_as<T, std::uint32_t> || std::same_as<T, std::uint64_t>
T RandomNumber() {
  std::random_device randomDevice;
  std::mt19937_64 generator(randomDevice());
  std::uniform_int_distribution<T> distribution;

  return distribution(generator);
}

}  // namespace ipc::common

#endif  // IPC_COMMON_UTIL_RAND_H_
