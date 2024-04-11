/* Copyright 2022 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef XLA_PJRT_PJRT_FUTURE_H_
#define XLA_PJRT_PJRT_FUTURE_H_

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/types/span.h"
#include "tsl/concurrency/async_value.h"
#include "tsl/concurrency/async_value_ref.h"
#include "tsl/concurrency/ref_count.h"
#include "tsl/platform/logging.h"

namespace xla {

template <class T = void>
class PjRtFuture;

namespace internal {
template <class T>
class PjRtFutureBase;
}

// Returns a `PjRtFuture` that will be successful if all `futures` complete
// successfully, or return a first encountered error.
PjRtFuture<> JoinFutures(absl::Span<const PjRtFuture<>> futures);

// An RAII event that a caller can use to tell the PjRtClient about asynchronous
// actions outside PjRt.
//
// A ScopedAsyncTrackingEvent can be generated by the caller by calling a method
// on PjRtDevice, and the creation of a ScopedAsyncTrackingEvent tells the
// PjRtClient that the client is creating some outstanding asynchronous work
// that depends on activities happening on the PjRtDevice.
//
// The caller can indicate that a ScopedAsyncTrackingEvent event cannot complete
// until after some PjRtFuture becomes ready, by calling
// future.AssertHappensBefore(event).
//
// The caller indicates that the work tracked by the ScopedAsyncTrackingEvent
// has completed by letting the event go out of scope.
//
// ScopedAsyncTrackingEvents are used by some PjRtClient implementations to
// monitor system-wide dependencies.
class ScopedAsyncTrackingEvent {
 public:
  virtual ~ScopedAsyncTrackingEvent() = default;

 private:
  template <class T>
  friend class internal::PjRtFutureBase;

  // Indicates that the ScopedAsyncTrackingEvent won't complete until dependency
  // becomes available. Called only by PjRtFuture.
  virtual void AddDependency(tsl::RCReference<tsl::AsyncValue> dependency) = 0;
};

// Helpers for using PjRtFutures.
struct PjRtFutureHelpers {
 public:
  // Keys that are returned by an implementation-specific handler when a client
  // starts to block on a promise.
  //
  // For now, contains a single UID that can be used to identify a TraceMe, but
  // made extensible to allow support for other profilers such as endoscope.
  struct ProfilingKeys {
    uint64_t traceme_context_id = -1;
  };

  // Signature of handler called by the PjRtFuture class before it starts to
  // block a thread.
  using OnBlockStartFn = std::function<ProfilingKeys()>;

  // Signature of handler called by the PjRtFuture class after it finishes
  // blocking a thread.
  using OnBlockEndFn = std::function<void(ProfilingKeys)>;
};

namespace internal {
// A base class for a stateful future PjRtFuture<T> and a stateless future
// PjRtFuture<void>.
template <typename T>
class PjRtFutureBase {
 public:
  bool IsValid() const { return promise_ != nullptr; }

  // Two functions exist to know whether the future is ready, to accommodate
  // the fact some backends (e.g. distributed ones) could take a non-trivial
  // time to check the state of a future.
  //
  // `IsReady()` is guaranteed to return true if the future became ready
  // before `IsReady()` was called. `IsReady()` will return immediately if a
  // call to `Await()` has already returned, or any callback passed to
  // `OnReady` has already been triggered. Otherwise IsReady() may block for
  // the duration of a network message on some backends.
  bool IsReady() {
    CHECK(IsValid());
    return promise_.IsAvailable();
  }
  // `IsKnownReady()` is guaranteed to return immediately. `IsKnownReady()` will
  // always return true if a call to `Await()` has already returned, or any
  // callback passed to `OnReady` has already been triggered. Otherwise,
  // `IsKnownReady()` may return false in some cases in which the future was
  // ready before `IsKnownReady()` was called.
  bool IsKnownReady() {
    CHECK(IsValid());
    return promise_.IsAvailable();
  }

  // Indicates that event will not complete until after this becomes ready.
  //
  // May safely be called with event==nullptr in which case AssertHappensBefore
  // has no effect.
  void AssertHappensBefore(ScopedAsyncTrackingEvent* event) {
    CHECK(IsValid());
    if (event) event->AddDependency(promise_.CopyRCRef());
  }

 protected:
  // Wrapper for AsyncValueRef<T> that can be used by clients that don't
  // natively use TSL concurrency library. Stateless and stateful PjRtFuture<T>
  // specializations define their own Promise type inheriting from this one.
  class Promise {
   public:
    Promise() = default;

    Promise(Promise&& other) = default;
    Promise& operator=(Promise&& other) = default;

    Promise(const Promise& other) : ref_(other.ref_.CopyRef()) {}
    Promise& operator=(const Promise& other) {
      ref_ = other.ref_.CopyRef();
      return *this;
    }

    operator bool() const { return static_cast<bool>(ref_); }  // NOLINT

   protected:
    friend class PjRtFuture<T>;
    friend class PjRtFuture<void>;

    explicit Promise(tsl::AsyncValueRef<T> ref) : ref_(std::move(ref)) {}

    void SetStateConcrete() {
      DCHECK(ref_) << "Promise must wrap an async value";
      ref_.SetStateConcrete();
    }

    void SetError(absl::Status error) {
      DCHECK(ref_) << "Promise must wrap an async value";
      ref_.SetError(std::move(error));
    }

    template <typename... Args>
    void emplace(Args&&... args) const {
      DCHECK(ref_) << "Promise must wrap an async value";
      ref_.template emplace<T>(std::forward<Args>(args)...);
    }

    tsl::AsyncValueRef<T> ExtractRef() && { return std::move(ref_); }

   private:
    tsl::AsyncValueRef<T> ref_;
  };

  PjRtFutureBase() = default;

  PjRtFutureBase(tsl::AsyncValueRef<T> promise,
                 PjRtFutureHelpers::OnBlockStartFn on_block_start,
                 PjRtFutureHelpers::OnBlockEndFn on_block_end)
      : promise_(std::move(promise)),
        on_block_start_(std::move(on_block_start)),
        on_block_end_(std::move(on_block_end)) {}

  tsl::AsyncValuePtr<T> promise() const { return promise_.AsPtr(); }

  PjRtFutureHelpers::ProfilingKeys OnBlockStart() {
    return on_block_start_ ? on_block_start_()
                           : PjRtFutureHelpers::ProfilingKeys();
  }

  void OnBlockEnd(PjRtFutureHelpers::ProfilingKeys keys) {
    if (on_block_end_) on_block_end_(std::move(keys));
  }

 private:
  tsl::AsyncValueRef<T> promise_;

  // Function that is called before a thread starts blocking on the promise.
  PjRtFutureHelpers::OnBlockStartFn on_block_start_;
  // Function that is called after a thread finishes blocking on the promise.
  PjRtFutureHelpers::OnBlockEndFn on_block_end_;
};

}  // namespace internal

// PjRtFuture<T> is a simple future that is returned by PjRt APIs that
// enqueue asynchronous work, reporting a value of type T (frequently T=Status)
// when the work is complete.
//
// PjRtFuture can be used by the client to wait for work to complete, either via
// a blocking call or a callback.
//
// The implementation wraps a tsl::AsyncValueRef<T>, but we prefer to
// encapsulate the AVR rather than returning it directly for two reasons.
//
// First, we want to retain portability in case a future implementation moves
// away from AsyncValueRef ---- we don't want clients to call arbitrary
// AsyncValueRef APIs.
//
// Second, we want to export different semantics, for example we support
// integration between blocking and profiling (e.g., TraceMe).
//
// There are two ways to construct a PjRtFuture, one used by clients that
// natively use TSL concurrency library, which already have import APIs for
// constructing AsyncValueRefs; and another that avoids exposing TSL APIs and
// can be used by non-TSL clients.
template <class T>
class PjRtFuture : public internal::PjRtFutureBase<T> {
  using Base = internal::PjRtFutureBase<T>;

 public:
  // Wrapper for AsyncValueRef<T> that can be used by clients that don't
  // natively use TSL concurrency library.
  class Promise : public Base::Promise {
   public:
    using Base::Promise::Promise;

    // Sets the value of the promise. Must be called at most once.
    //
    // After Set is called, value will be delivered to waiters on the parent
    // PjRtFuture, via blocking or callbacks.
    void Set(T value) { Base::Promise::emplace(std::move(value)); }
  };

  // Returns a Promise that can be used to construct a PjRtFuture, and then Set
  // later.
  //
  // Used by clients that do not use TSL concurrency library natively.
  static Promise CreatePromise() {
    return Promise(tsl::MakeUnconstructedAsyncValueRef<T>());
  }

  PjRtFuture() = default;

  // Constructor for an already-available PjRtFuture.
  //
  // Typically used to eagerly return error values when async work will not
  // be enqueued, e.g., due to invalid arguments.
  explicit PjRtFuture(T t)
      : Base(tsl::MakeAvailableAsyncValueRef<T>(std::move(t)),
             /*on_block_start=*/nullptr,
             /*on_block_end=*/nullptr) {}

  // Constructor used by clients that natively use TSL concurrency library.
  //
  // on_block_start is called before Await starts to block.
  // on_block_end is called after Await finishes blocking.
  explicit PjRtFuture(
      tsl::AsyncValueRef<T> async_value,
      PjRtFutureHelpers::OnBlockStartFn on_block_start = nullptr,
      PjRtFutureHelpers::OnBlockEndFn on_block_end = nullptr)
      : Base(std::move(async_value), std::move(on_block_start),
             std::move(on_block_end)) {}

  // Constructor used by clients that don't natively use TSL concurrency library
  // and want to use the wrapped PjRtFuture<T>::Promise class.
  //
  // on_block_start is called before Await starts to block.
  // on_block_end is called after Await finishes blocking.
  explicit PjRtFuture(
      Promise promise,
      PjRtFutureHelpers::OnBlockStartFn on_block_start = nullptr,
      PjRtFutureHelpers::OnBlockEndFn on_block_end = nullptr)
      : Base(std::move(promise).ExtractRef(), std::move(on_block_start),
             std::move(on_block_end)) {}

  // Blocks the calling thread until the future is ready, then returns the
  // final value.
  T Await() {
    CHECK(Base::IsValid());
    if (!Base::promise().IsAvailable()) {
      PjRtFutureHelpers::ProfilingKeys keys = Base::OnBlockStart();
      BlockUntilReady(Base::promise());
      Base::OnBlockEnd(std::move(keys));
    }
    DCHECK(Base::promise().IsConcrete());
    return *Base::promise();
  }

  // Registers callback to be called once the promise is ready, with the final
  // value.
  //
  // callback may be called on an internal system thread or the calling thread.
  // The client should avoid any potentially re-entrant API calls within the
  // callback, for example by using the callback to enqueue work on a
  // client-owned threadpool.
  void OnReady(absl::AnyInvocable<void(T) &&> callback) {
    CHECK(Base::IsValid());
    Base::promise().AndThen(
        [promise = Base::promise(), callback = std::move(callback)]() mutable {
          DCHECK(promise.IsConcrete());
          if constexpr (std::is_copy_constructible_v<T>) {
            std::move(callback)(*promise);
            return;
          }
          // For non-copyable types, we have no ways to check the number of
          // waiters but we have to move the data into the consumer callback.
          // Registering two callbacks will lead to double-move of the data. It
          // is users' responsibility to make sure only one waiter is
          // registered.
          // TODO(yunlongl): Implement `PjRtUniqueFuture`.
          std::move(callback)(std::move(*promise));
        });
  }
};

// PjRtFuture<void> specialization for communicating stateless events.
//
// See PjRtFuture<T> documentation above for more details.
template <>
class PjRtFuture<void> : public internal::PjRtFutureBase<std::nullopt_t> {
  using Base = internal::PjRtFutureBase<std::nullopt_t>;

 public:
  // Wrapper for AsyncValueRef<T> that can be used by clients that don't
  // natively use TSL concurrency library.
  class Promise : public Base::Promise {
   public:
    using Base::Promise::Promise;

    // Sets the promise completed. Must be called at most once.
    //
    // After Set is called, completion event will be delivered to waiters on the
    // parent PjRtFuture, via blocking or callbacks.
    void Set() { Base::Promise::SetStateConcrete(); }

    // Sets the promise completed with an error. Must be called at most once.
    //
    // After SetError is called, completion event will be delivered to waiters
    // on the parent PjRtFuture, via blocking or callbacks.
    void SetError(absl::Status err) { Base::Promise::SetError(std::move(err)); }
  };

  // Returns a Promise that can be used to construct a PjRtFuture, and then Set
  // later.
  //
  // Used by clients that do not use TSL concurrency library.
  static Promise CreatePromise() {
    return Promise(
        tsl::MakeConstructedAsyncValueRef<std::nullopt_t>(std::nullopt));
  }

  // Constructor for an already-available PjRtFuture. OkStatus means that future
  // is already successfully completed. Error means that future is already
  // completed with an error.
  explicit PjRtFuture(absl::Status status)
      : Base(status.ok()
                 ? tsl::MakeAvailableAsyncValueRef<std::nullopt_t>(std::nullopt)
                 : tsl::MakeErrorAsyncValueRef(std::move(status)),
             /*on_block_start=*/nullptr, /*on_block_end=*/nullptr) {}

  // Constructor used by clients that don't natively use TSL concurrency library
  // and want to use the wrapped PjRtFuture<T>::Promise class.
  //
  // on_block_start is called before Await starts to block.
  // on_block_end is called after Await finishes blocking.
  explicit PjRtFuture(
      Promise promise,
      PjRtFutureHelpers::OnBlockStartFn on_block_start = nullptr,
      PjRtFutureHelpers::OnBlockEndFn on_block_end = nullptr)
      : Base(std::move(promise).ExtractRef(), std::move(on_block_start),
             std::move(on_block_end)) {}

  // Blocks the calling thread until the future is ready.
  absl::Status Await() {
    CHECK(Base::IsValid());
    if (!Base::promise().IsAvailable()) {
      PjRtFutureHelpers::ProfilingKeys keys = Base::OnBlockStart();
      BlockUntilReady(Base::promise());
      Base::OnBlockEnd(std::move(keys));
    }
    return Base::promise().IsError() ? Base::promise().GetError()
                                     : absl::OkStatus();
  }

  // Registers callback to be called once the future is ready.
  //
  // callback may be called on an internal system thread or the calling thread.
  // The client should avoid any potentially re-entrant API calls within the
  // callback, for example by using the callback to enqueue work on a
  // client-owned threadpool.
  void OnReady(absl::AnyInvocable<void(absl::Status)> callback) const {
    CHECK(Base::IsValid());
    Base::promise().AndThen(std::move(callback));
  }
};

}  // namespace xla

#endif  // XLA_PJRT_PJRT_FUTURE_H_
