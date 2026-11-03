#include "publish_queue.h"

namespace esphome::isolarcloud {

void PublishQueue::evict_(size_t incoming, bool droppable_only) {
  size_t i = this->front_busy_() ? 1 : 0;
  while (i < this->queue_.size() && this->bytes_ + incoming > this->max_bytes_) {
    if (droppable_only && !this->queue_[i].droppable) {
      i++;
      continue;
    }
    this->bytes_ -= this->queue_[i].bytes();
    this->queue_.erase(this->queue_.begin() + static_cast<std::ptrdiff_t>(i));
    this->evicted_++;
    this->dropped_++;
    this->last_evicted_++;
  }
}

bool PublishQueue::push(OutMessage m) {
  this->last_evicted_ = 0;
  if (m.chunks.empty() || this->queue_.size() >= this->max_messages_) {
    this->dropped_++;
    return false;
  }
  const size_t nb = m.bytes();
  if (this->bytes_ + nb > this->max_bytes_) {
    this->evict_(nb, true);
    if (this->bytes_ + nb > this->max_bytes_) {
      const size_t waiting = this->queue_.size() - (this->front_busy_() ? 1 : 0);
      if (m.droppable && waiting > 0) {
        this->dropped_++;  // telemetry does not push out queued control traffic
        return false;
      }
      this->evict_(nb, false);
    }
  }
  this->bytes_ += nb;
  this->queue_.push_back(std::move(m));
  return true;
}

void PublishQueue::on_puback(int msg_id, uint32_t /*now_ms*/) {
  if (this->in_flight_ && msg_id > 0 && msg_id == this->msg_id_) {
    this->in_flight_ = false;
    this->advance_ = true;
  }
}

void PublishQueue::finish_front_(bool ok) {
  if (!ok)
    this->dropped_++;
  OutMessage m = std::move(this->queue_.front());
  this->queue_.pop_front();
  this->bytes_ -= m.bytes();
  this->next_chunk_ = 0;
  this->in_flight_ = false;
  this->advance_ = false;
  this->attempts_ = 0;
  this->msg_id_ = -1;
  if (this->done_)
    this->done_(m, ok);
}

void PublishQueue::tick(MqttPublisher &pub, uint32_t now_ms) {
  // Bounded: at most a few sends per call (QoS 0 chunks complete immediately).
  for (int guard = 0; guard < 8 && !this->queue_.empty(); guard++) {
    OutMessage &m = this->queue_.front();
    if (this->advance_) {
      this->advance_ = false;
      this->attempts_ = 0;
      if (++this->next_chunk_ >= m.chunks.size()) {
        this->finish_front_(true);
        continue;
      }
    }
    if (this->in_flight_) {
      if (now_ms - this->sent_ms_ < this->ack_timeout_ms_)
        return;
      if (this->attempts_ > this->max_retries_) {
        this->finish_front_(false);  // give up on the rest of this message
        continue;
      }
      this->retries_total_++;
      // fall through: resend the same chunk
    }
    int id = pub.publish(m.topic, m.chunks[this->next_chunk_], m.qos, m.retain);
    this->attempts_++;
    this->sent_ms_ = now_ms;
    if (id == 0 && m.qos == 0) {
      this->advance_ = true;  // QoS 0: no ack
      continue;
    }
    // id < 0 (client refused): wait for the timeout and retry like a missing ack.
    this->msg_id_ = id;
    this->in_flight_ = true;
    return;
  }
}

void PublishQueue::clear() {
  this->queue_.clear();
  this->bytes_ = 0;
  this->next_chunk_ = 0;
  this->in_flight_ = false;
  this->advance_ = false;
  this->attempts_ = 0;
  this->msg_id_ = -1;
}

}  // namespace esphome::isolarcloud
