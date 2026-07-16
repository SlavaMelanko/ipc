#include <print>

int main() {
  try {
    std::println("Hello, IPC!");
  } catch (...) {
    return 1;
  }

  return 0;
}
