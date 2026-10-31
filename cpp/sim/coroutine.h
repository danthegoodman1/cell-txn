// A stackful coroutine on a Boost.Context fiber, with the semantics of Go's
// iter.Pull that the simulator relies on: Next runs the body until it
// yields or returns, and Stop resumes a suspended body so that its pending
// Yield throws Stopped and its stack unwinds.
#pragma once

#include <boost/context/fiber.hpp>
#include <boost/context/protected_fixedsize_stack.hpp>
#include <exception>
#include <functional>

#include "db.h"

namespace celltnx::sim {

// Stopped unwinds a coroutine that Stop resumed. Code that catches every
// exception must rethrow it.
struct Stopped {};

class Coroutine {
 public:
  explicit Coroutine(std::function<Status()> body) : body_(std::move(body)) {
    namespace ctx = boost::context;
    self_ = ctx::fiber(std::allocator_arg, ctx::protected_fixedsize_stack(kStack), [this](ctx::fiber &&caller) {
      caller_ = std::move(caller);
      if (!stopping_) {
        try {
          err_ = body_();
        } catch (const Stopped &) {
        } catch (const ctx::detail::forced_unwind &) {
          throw;
        } catch (...) {
          exc_ = std::current_exception();
        }
      }
      done_ = true;
      return std::move(caller_);
    });
  }
  Coroutine(const Coroutine &) = delete;
  Coroutine &operator=(const Coroutine &) = delete;
  ~Coroutine() { Stop(); }

  // Next resumes the body until it yields or returns, reporting whether it
  // can run again. An exception the body threw rethrows here.
  bool Next() {
    started_ = true;
    self_ = std::move(self_).resume();
    if (exc_) {
      std::exception_ptr e = exc_;
      exc_ = nullptr;
      std::rethrow_exception(e);
    }
    return !done_;
  }

  // Yield suspends the body back to Next's caller. Called only from inside
  // the body; throws Stopped when Stop resumed it.
  void Yield() {
    caller_ = std::move(caller_).resume();
    if (stopping_) throw Stopped{};
  }

  // Stop unwinds the body if it has started and not finished.
  void Stop() {
    if (done_) return;
    stopping_ = true;
    self_ = std::move(self_).resume();
    exc_ = nullptr;
  }

  bool done() const { return done_; }
  const Status &err() const { return err_; }

 private:
  static constexpr size_t kStack = 512 * 1024;

  std::function<Status()> body_;
  boost::context::fiber self_;    // resumes the body
  boost::context::fiber caller_;  // resumes Next's caller while the body runs
  bool started_ = false, done_ = false, stopping_ = false;
  Status err_;
  std::exception_ptr exc_;
};

}  // namespace celltnx::sim
