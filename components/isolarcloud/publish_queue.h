#pragma once
// PUBACK-gated outbound queue. Host-buildable.
//
// Chunk 1 of a message is published right away and chunks 2..n follow the PUBACK of the previous
// chunk; messages go out one after another, the next one after the previous one's ack. One chunk
// is in flight at any time (esp-mqtt MQTT_EVENT_PUBLISHED with the chunk's msg_id).
//
// No PUBACK within ack_timeout -> the chunk is published again, up to max_retries times; then the
// rest of that message is dropped and the next one continues.
//
// Memory: at most max_messages messages and max_bytes encoded bytes are held. Over the byte cap the
// oldest queued droppable messages (periodic rundata) go first; a droppable message is refused
// while other traffic is still queued; other traffic (devinfo, beat, command replies) evicts older
// queued messages. The message being sent is never evicted, and one message alone may exceed the
// cap.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace esphome::isolarcloud {

class MqttPublisher {
 public:
  virtual ~MqttPublisher() = default;
  /// Hand one PUBLISH to the client. Returns the packet id (> 0) for QoS 1, 0 for QoS 0 (no ack
  /// will come), < 0 on error.
  virtual int publish(const std::string &topic, const std::vector<uint8_t> &payload, int qos, bool retain) = 0;
};

struct OutMessage {
  std::string topic;
  std::vector<std::vector<uint8_t>> chunks;  ///< wire payloads (already encoded)
  std::string label;
  int qos{1};
  bool retain{false};
  /// Periodic telemetry that the next period replaces: evicted first over the byte cap.
  bool droppable{false};
  size_t bytes() const {
    size_t n = 0;
    for (const auto &c : this->chunks)
      n += c.size();
    return n;
  }
};

class PublishQueue {
 public:
  using DoneCallback = std::function<void(const OutMessage &, bool ok)>;

  static constexpr size_t DEFAULT_MAX_BYTES = 24 * 1024;
  explicit PublishQueue(size_t max_messages = 32, uint32_t ack_timeout_ms = 20000, uint8_t max_retries = 3,
                        size_t max_bytes = DEFAULT_MAX_BYTES)
      : max_messages_(max_messages), ack_timeout_ms_(ack_timeout_ms), max_retries_(max_retries),
        max_bytes_(max_bytes) {}
  void configure(uint32_t ack_timeout_ms, uint8_t max_retries) {
    this->ack_timeout_ms_ = ack_timeout_ms;
    this->max_retries_ = max_retries;
  }
  void set_done_callback(DoneCallback cb) { this->done_ = std::move(cb); }

  /// Queue a message. False (and dropped) if it has no chunks, the queue holds max_messages, or it
  /// is droppable and the byte cap is taken by other traffic. May evict older messages (byte cap);
  /// last_push_evicted() says how many.
  bool push(OutMessage m);
  /// A PUBACK arrived for packet id `msg_id`.
  void on_puback(int msg_id, uint32_t now_ms);
  /// Send what is due / handle ack timeouts. Call from the loop (and after on_puback).
  void tick(MqttPublisher &pub, uint32_t now_ms);
  /// Drop everything (session lost).
  void clear();

  bool idle() const { return this->queue_.empty(); }
  size_t size() const { return this->queue_.size(); }
  bool awaiting_ack() const { return this->in_flight_; }
  uint32_t retries() const { return this->retries_total_; }
  uint32_t dropped() const { return this->dropped_; }
  /// Encoded bytes currently held.
  size_t bytes() const { return this->bytes_; }
  /// Messages evicted by the byte cap since start / by the last push().
  uint32_t evicted() const { return this->evicted_; }
  size_t last_push_evicted() const { return this->last_evicted_; }

 protected:
  void finish_front_(bool ok);
  /// queue_.front() is being sent (a chunk is out or acked): never evicted.
  bool front_busy_() const { return this->in_flight_ || this->advance_ || this->next_chunk_ > 0; }
  /// Evict queued (not busy) messages, oldest first, until `incoming` more bytes fit.
  void evict_(size_t incoming, bool droppable_only);

  size_t max_messages_;
  uint32_t ack_timeout_ms_;
  uint8_t max_retries_;
  size_t max_bytes_;
  size_t bytes_{0};
  uint32_t evicted_{0};
  size_t last_evicted_{0};
  DoneCallback done_;
  std::deque<OutMessage> queue_;
  size_t next_chunk_{0};   ///< of queue_.front()
  bool in_flight_{false};  ///< chunk next_chunk_ sent, waiting for its ack
  int msg_id_{-1};
  uint32_t sent_ms_{0};
  uint8_t attempts_{0};
  bool advance_{false};    ///< ack received, move to the next chunk on the next tick
  uint32_t retries_total_{0};
  uint32_t dropped_{0};
};

}  // namespace esphome::isolarcloud
