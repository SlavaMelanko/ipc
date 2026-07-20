#include <exception>
#include <print>

#include "common/cli.h"
#include "producer/app.h"

int main(int argc, char** argv) {
  auto config = ipc::common::ParseCommandLineArguments(argc, argv, "producer");
  if (!config) {
    return config.error();
  }

  try {
    return ipc::producer::App(*config).Run() ? 0 : 1;
  } catch (const std::exception& e) {
    std::println(stderr, "producer: {}", e.what());

    return 1;
  } catch (...) {
    return 1;
  }
}
