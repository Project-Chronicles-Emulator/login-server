#pragma once

#include <spdlog/logger.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <cstddef>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace chronicles::testing_support {

// Redirects the default spdlog logger into memory for the lifetime of the object, so a test
// can assert what a component logged. The previous default logger is restored on destruction.
// Install it on the test thread before the code under test logs; spdlog does not synchronize
// changes to the default logger.
class LogCapture {
public:
  static constexpr const char *kLoggerName = "log-capture";

  LogCapture()
      : previous_logger_(spdlog::default_logger()),
        sink_(std::make_shared<spdlog::sinks::ostream_sink_mt>(stream_)) {
    auto capturing_logger = std::make_shared<spdlog::logger>(kLoggerName, sink_);
    capturing_logger->set_level(spdlog::level::trace);
    capturing_logger->flush_on(spdlog::level::trace);
    spdlog::set_default_logger(std::move(capturing_logger));
  }

  ~LogCapture() {
    spdlog::set_default_logger(std::move(previous_logger_));
    // set_default_logger also registered the capturing logger by name. Drop it so the registry
    // holds no logger whose sink references the stream destroyed with this object.
    spdlog::drop(kLoggerName);
  }

  LogCapture(const LogCapture &) = delete;
  LogCapture &operator=(const LogCapture &) = delete;

  std::string text() const {
    return stream_.str();
  }

  // Number of non-overlapping occurrences of the fragment in everything captured so far.
  std::size_t count(std::string_view fragment) const {
    const auto captured = text();
    std::size_t occurrences = 0;
    std::size_t position = captured.find(fragment);
    while (position != std::string::npos) {
      ++occurrences;
      position = captured.find(fragment, position + fragment.size());
    }
    return occurrences;
  }

private:
  std::shared_ptr<spdlog::logger> previous_logger_;
  std::ostringstream stream_;
  std::shared_ptr<spdlog::sinks::ostream_sink_mt> sink_;
};

} // namespace chronicles::testing_support
