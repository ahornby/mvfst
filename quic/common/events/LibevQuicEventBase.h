/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <quic/common/events/QuicEventBase.h>
#include <quic/common/events/QuicTimer.h>

#include <ev.h>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include <folly/GLog.h>
#include <folly/IntrusiveList.h>

namespace quic {
/*
 * This is a partial implementation of a QuicEventBase that uses libev.
 * (It is copied from the previous interface in
 * quic/common/QuicLibevEventBase.h)
 */
class LibevQuicEventBase : public QuicEventBase, public QuicTimer {
 public:
  explicit LibevQuicEventBase(struct ev_loop* loop);
  ~LibevQuicEventBase() override;

  void runInLoop(
      QuicEventBaseLoopCallback* callback,
      bool thisIteration = false) override;

  void runInLoop(folly::Function<void()> cb, bool thisIteration = false)
      override;

  void runInEventBaseThreadAndWait(
      folly::Function<void()> fn) noexcept override;

  bool isInEventBaseThread() const override;

  // QuicEventBase
  void scheduleTimeout(
      QuicTimerCallback* callback,
      std::chrono::milliseconds timeout) override;

  // QuicEventBase
  bool scheduleTimeoutHighRes(
      QuicTimerCallback* /*callback*/,
      std::chrono::microseconds /*timeout*/) override;

  // QuicTimer
  void scheduleTimeout(
      QuicTimerCallback* callback,
      std::chrono::microseconds timeout) override;

  bool loop() override {
    return ev_run(ev_loop_, 0);
  }

  bool loopIgnoreKeepAlive() override {
    return false;
  }

  void runInEventBaseThread(folly::Function<void()> /*fn*/) noexcept override {
    LOG(FATAL) << __func__ << " not supported in LibevQuicEventBase";
  }

  void runImmediatelyOrRunInEventBaseThreadAndWait(
      folly::Function<void()> /*fn*/) noexcept override {
    LOG(FATAL) << __func__ << " not supported in LibevQuicEventBase";
  }

  void runAfterDelay(folly::Function<void()> /*cb*/, uint32_t /*milliseconds*/)
      override {
    LOG(FATAL) << __func__ << " not supported in LibevQuicEventBase";
  }

  bool loopOnce(int /*flags*/) override {
    LOG(FATAL) << __func__ << " not supported in LibevQuicEventBase";
  }

  void loopForever() override {
    LOG(FATAL) << __func__ << " not supported in LibevQuicEventBase";
  }

  void terminateLoopSoon() override {
    LOG(WARNING) << __func__ << " is not implemented in LibevQuicEventBase";
  }

  [[nodiscard]] std::chrono::milliseconds getTimerTickInterval()
      const override {
    return std::chrono::milliseconds(1);
  }

  [[nodiscard]] std::chrono::microseconds getTickInterval() const override {
    return std::chrono::milliseconds(1);
  }

  struct ev_loop* getLibevLoop() {
    return ev_loop_;
  }

  // This is public so the libev callback can access it
  void checkCallbacks();

  // This is public so the libev callback can access it
  class TimerCallbackWrapper : public QuicTimerCallback::TimerCallbackImpl {
   public:
    explicit TimerCallbackWrapper(
        QuicTimerCallback* callback,
        struct ev_loop* ev_loop)
        : callback_(callback), ev_loop_(ev_loop) {}

    friend class LibevQuicEventBase;

    void timeoutExpired() noexcept {
      callback_->timeoutExpired();
    }

    void callbackCanceled() noexcept {
      callback_->callbackCanceled();
    }

    void cancelImpl() noexcept override {
      ev_timer_stop(ev_loop_, &ev_timer_);
    }

    [[nodiscard]] bool isScheduledImpl() const noexcept override {
      return ev_is_active(&ev_timer_) || ev_is_pending(&ev_timer_);
    }

    [[nodiscard]] std::chrono::milliseconds getTimeRemainingImpl()
        const noexcept override {
      LOG(FATAL) << __func__ << " not implemented in LibevQuicEventBase";
    }

   private:
    QuicTimerCallback* callback_;
    struct ev_loop* ev_loop_;
    ev_timer ev_timer_;
  };

 private:
  class LoopCallbackWrapper
      : public QuicEventBaseLoopCallback::LoopCallbackImpl {
   public:
    explicit LoopCallbackWrapper(QuicEventBaseLoopCallback* callback)
        : callback_(callback) {}

    ~LoopCallbackWrapper() override {
      listHook_.unlink();
    }

    friend class LibevQuicEventBase;

    void runLoopCallback() noexcept {
      listHook_.unlink();
      callback_->runLoopCallback();
    }

    void cancelImpl() noexcept override {
      // Removing the callback from the instrusive list is effectively
      // cancelling it.
      listHook_.unlink();
    }
    [[nodiscard]] bool isScheduledImpl() const noexcept override {
      return listHook_.is_linked();
    }

   private:
    QuicEventBaseLoopCallback* callback_;
    folly::IntrusiveListHook listHook_;
  };

  struct ev_loop* ev_loop_{EV_DEFAULT};

  folly::IntrusiveList<LoopCallbackWrapper, &LoopCallbackWrapper::listHook_>
      loopCallbackWrappers_;
  ev_check checkWatcher_;
  std::atomic<std::thread::id> loopThreadId_;
};
} // namespace quic
