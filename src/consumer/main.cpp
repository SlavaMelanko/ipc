#include <exception>
#include <print>

#include "common/cli.h"
#include "consumer/app.h"

int main(int argc, char** argv) {
  auto config = ipc::common::ParseCommandLineArguments(argc, argv, "consumer");
  if (!config) {
    return config.error();
  }

  try {
    return ipc::consumer::App(*config).Run() ? 0 : 1;
  } catch (const std::exception& e) {
    std::println(stderr, "consumer: {}", e.what());

    return 1;
  } catch (...) {
    return 1;
  }
}
