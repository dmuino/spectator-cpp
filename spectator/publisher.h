#pragma once

#include <atomic>
#include <mutex>
#include <thread>
#include "logger.h"
#include "http_client.h"
#include "measurement.h"

namespace spectator {

static constexpr int kSendEveryMillis = 5000;

template <typename R>
class Publisher {
 public:
  explicit Publisher(R* registry)
      : registry_(registry), started_{false}, should_stop_{false} {}
  void Start() {
    static auto http_initialized = false;
    if (!http_initialized) {
      http_initialized = true;
      HttpClient::GlobalInit();
    }
    const auto& cfg = registry_->GetConfig();
    if (cfg.uri.empty()) {
      Logger()->warn("Registry config has no uri. Ignoring start request");
      return;
    }
    if (started_.exchange(true)) {
      Logger()->warn("Registry already started. Ignoring start request");
      return;
    }

    sender_thread_ = std::thread(&Publisher::sender, this);
  }

  void Stop() {
    if (!started_) {
      return;
    }

    should_stop_ = true;
    cv_.notify_all();
    sender_thread_.join();
    HttpClient::GlobalShutdown();
  }

 private:
  R* registry_;
  std::atomic<bool> started_;
  std::atomic<bool> should_stop_;
  std::mutex cv_mutex_;
  std::condition_variable cv_;
  std::thread sender_thread_;

  void sender() noexcept {
    using std::chrono::duration_cast;
    using std::chrono::milliseconds;
    while (!should_stop_) {
      auto start = R::clock::now();
      try {
        send_metrics();
      } catch (std::exception& e) {
        Logger()->error("Ignoring exception while sending metrics: {}",
                        e.what());
      }
      auto elapsed = R::clock::now() - start;
      auto millis = duration_cast<milliseconds>(elapsed).count();
      Logger()->debug("Sent metrics in {} ms", millis);

      if (millis < kSendEveryMillis) {
        std::unique_lock<std::mutex> lock{cv_mutex_};
        auto sleep = milliseconds(kSendEveryMillis) - elapsed;
        Logger()->debug("Sleeping {}ms until the next interval",
                        kSendEveryMillis - millis);
        cv_.wait_for(lock, sleep);
      }
    }
    Logger()->info("Stopping Publisher");
  }

  // for testing
 protected:
  using StrTable = ska::flat_hash_map<std::string, int>;
  StrTable build_str_table(rapidjson::Document* payload,
                           std::vector<Measurement>::const_iterator first,
                           std::vector<Measurement>::const_iterator last) {
    StrTable strings;
    const auto& common_tags = registry_->GetConfig().common_tags;
    for (const auto& tag : common_tags) {
      strings[tag.first] = 0;
      strings[tag.second] = 0;
    }
    strings["name"] = 0;
    for (auto it = first; it != last; ++it) {
      const auto& m = *it;
      strings[m.id->Name()] = 0;
      for (const auto& tag : m.id->GetTags()) {
        strings[tag.first] = 0;
        strings[tag.second] = 0;
      }
    }
    std::vector<std::string> sorted_strings;
    sorted_strings.reserve(strings.size());
    for (const auto& s : strings) {
      sorted_strings.emplace_back(s.first);
    }
    std::sort(sorted_strings.begin(), sorted_strings.end());
    for (auto i = 0u; i < sorted_strings.size(); ++i) {
      strings[sorted_strings[i]] = i;
    }
    auto& alloc = payload->GetAllocator();
    payload->PushBack(static_cast<int64_t>(strings.size()), alloc);
    for (const auto& s : sorted_strings) {
      rapidjson::Value str(s.c_str(),
                           static_cast<rapidjson::SizeType>(s.length()), alloc);
      payload->PushBack(str, alloc);
    }
    return strings;
  }

  enum class Op { Unknown = -1, Add = 0, Max = 10 };

  Op op_from_tags(const Tags& tags) {
    auto stat = tags.at("statistic");
    if (stat == "count" || stat == "totalAmount" || stat == "totalTime" ||
        stat == "totalOfSquares" || stat == "percentile") {
      return Op::Add;
    } else if (stat == "max" || stat == "gauge" || stat == "activeTasks" ||
               stat == "duration") {
      return Op::Max;
    }
    return Op::Unknown;
  }

  template <typename T>
  void add_tags(rapidjson::Document* payload, const StrTable& strings,
                const T& tags) {
    using rapidjson::Value;
    auto& alloc = payload->GetAllocator();
    for (const auto& tag : tags) {
      auto k_pair = strings.find(tag.first);
      auto v_pair = strings.find(tag.second);
      assert(k_pair != strings.end());
      assert(v_pair != strings.end());
      payload->PushBack(k_pair->second, alloc);
      payload->PushBack(v_pair->second, alloc);
    }
  }

  void append_measurement(rapidjson::Document* payload, const StrTable& strings,
                          const Measurement& m) {
    auto& alloc = payload->GetAllocator();
    auto op = op_from_tags(m.id->GetTags());
    if (op != Op::Unknown) {
      const auto& common_tags = registry_->GetConfig().common_tags;
      int64_t total_tags = m.id->GetTags().size() + 1 + common_tags.size();
      payload->PushBack(total_tags, alloc);
      add_tags(payload, strings, common_tags);
      add_tags(payload, strings, m.id->GetTags());
      auto name_idx = strings.find("name")->second;
      auto name_value_idx = strings.find(m.id->Name())->second;
      payload->PushBack(name_idx, alloc);
      payload->PushBack(name_value_idx, alloc);
      auto op_num = static_cast<int>(op);
      payload->PushBack(op_num, alloc);
      payload->PushBack(m.value, alloc);
    }
  }

  rapidjson::Document measurements_to_json(
      std::vector<Measurement>::const_iterator first,
      std::vector<Measurement>::const_iterator last, int64_t* added) {
    using rapidjson::Document;
    using rapidjson::kArrayType;
    Document payload{kArrayType};

    auto strings = build_str_table(&payload, first, last);
    for (auto it = first; it != last; ++it) {
      append_measurement(&payload, strings, *it);
    }
    auto num_added = std::distance(first, last);
    *added = static_cast<int64_t>(num_added);
    return payload;
  }

  void update_sent(int64_t num_sent) {
    registry_->GetCounter("spectator.measurementsSent")->Add(num_sent);
  }

  void update_http_err(int status_code, int64_t num_not_sent) {
    Tags tags{{"error", "httpError"}};
    tags.add("statusCode", std::to_string(status_code));
    registry_
        ->GetCounter(registry_->CreateId("spectator.measurementsErr", tags))
        ->Add(num_not_sent);
  }

  void send_metrics() {
    const auto& cfg = registry_->GetConfig();
    auto read_timeout = cfg.read_timeout || 2;
    auto connect_timeout = cfg.connect_timeout || 1;
    HttpClient client{registry_, connect_timeout, read_timeout};
    auto batch_size =
        static_cast<std::vector<Measurement>::difference_type>(cfg.batch_size);
    auto measurements = registry_->Measurements();
    std::vector<rapidjson::Document> batches;
    std::vector<int64_t> batch_sizes;

    auto from = measurements.begin();
    auto end = measurements.end();
    while (from != end) {
      auto to_end = std::distance(from, end);
      auto to_advance = std::min(batch_size, to_end);
      auto to = from;
      std::advance(to, to_advance);

      int64_t added;
      batches.emplace_back(measurements_to_json(from, to, &added));
      batch_sizes.emplace_back(added);
      from = to;
    }

    auto http_codes = client.PostBatches(cfg.uri, batches);
    for (auto i = 0u; i < http_codes.size(); ++i) {
      auto http_code = http_codes[i];
      auto num_measurements = batch_sizes[i];
      if (http_code != 200) {
        Logger()->error(
            "Unable to send batch of {} measurements to publish: {}",
            num_measurements, http_code);

        update_http_err(http_code, num_measurements);
      } else {
        update_sent(num_measurements);
      }
    }
  }
};

}  // namespace spectator
