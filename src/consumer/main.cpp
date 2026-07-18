#include <exception>
#include <print>

#include "consumer/app.h"

int main(int argc, char** argv) {
  try {
    return ipc::consumer::App(argc, argv).Run() ? 0 : 1;
  } catch (const std::exception& e) {
    std::println(stderr, "consumer: {}", e.what());

    return 1;
  } catch (...) {
    return 1;
  }
}
