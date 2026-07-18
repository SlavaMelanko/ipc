#include <exception>
#include <print>

#include "producer/app.h"

int main(int argc, char** argv) {
  try {
    return ipc::producer::App(argc, argv).Run() ? 0 : 1;
  } catch (const std::exception& e) {
    std::println(stderr, "producer: {}", e.what());

    return 1;
  } catch (...) {
    return 1;
  }
}
