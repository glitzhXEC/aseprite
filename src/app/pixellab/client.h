// PixelLab integration work in progress: in-process service, not yet connected to Aseprite UI.
#pragma once
#include <functional>
#include <memory>
#include <string>
namespace app::pixellab {
class Client {
public:
  using ProgressFn = std::function<void(const std::string&)>;
  Client();
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  // Synchronous. Call from a background job; progress runs on the calling
  // thread and must not touch Aseprite UI or call run() recursively. Request and
  // response are UTF-8 JSON; authentication stays in process memory.
  std::string run(const std::string& requestJson, const ProgressFn& progress = {});
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
