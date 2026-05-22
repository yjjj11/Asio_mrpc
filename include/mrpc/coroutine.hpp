#ifndef MRC_COROUTINE_HPP
#define MRC_COROUTINE_HPP

#pragma once

#ifdef _USE_COROUTINE

#ifdef WIN32
#   include <experimental/coroutine>
    namespace stdcoro = std::experimental;
#else
#   include <coroutine>
    namespace stdcoro = std;
#endif
#include <exception>
#include <functional>
#include <string>
#include <cstdint>
#include <utility>

namespace mrpc {

template<typename T> class task_promise;
template<typename T> class req_result;

template<typename T>
struct task_awaitable {
    task_awaitable() {}
    task_awaitable(std::function<void(stdcoro::coroutine_handle<>)> suspend_callback,
                   std::function<req_result<T>()> resume_callback)
        : suspend_callback_(std::move(suspend_callback))
        , resume_callback_(std::move(resume_callback)) {
    }

    // Error constructor: creates an awaitable that immediately returns an error without suspending
    task_awaitable(uint32_t err_code, std::string err_msg)
        : has_error_(true)
        , err_code_(err_code)
        , err_msg_(std::move(err_msg)) {
    }

    bool await_ready() const noexcept {
        return has_error_;
    }

    void await_suspend(stdcoro::coroutine_handle<> coroutine) {
        suspend_callback_(coroutine);
    }

    decltype(auto) await_resume() noexcept {
        if (has_error_) {
            return req_result<T>(err_code_, err_msg_);
        }
        return resume_callback_();
    }

  private:
    std::function<void(stdcoro::coroutine_handle<>)> suspend_callback_;
    std::function<req_result<T>()> resume_callback_;
    bool has_error_ = false;
    uint32_t err_code_ = 0;
    std::string err_msg_;
};

template<typename T = void>
class task {
  public:
    using promise_type = task_promise<T>;
    using value_type = T;

    task(stdcoro::coroutine_handle<promise_type> h) noexcept
        : coroutine_(h) {
    }

    ~task() {
        if (coroutine_) {
            coroutine_.destroy();
        }
    }

    task(task&& other) noexcept
        : coroutine_(std::exchange(other.coroutine_, {})) {
    }

    task(const task&) = delete;
    task& operator=(const task&) = delete;
    task& operator=(task&&) = delete;

    bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(stdcoro::coroutine_handle<> awaiting) noexcept {
        coroutine_.promise().set_awaiting(awaiting);
    }

    decltype(auto) await_resume() noexcept {
        if constexpr (std::is_void_v<T>) {
            return;
        } else {
            return coroutine_.promise().get_value();
        }
    }

  private:
    stdcoro::coroutine_handle<promise_type> coroutine_;
};

class task_promise_base {
  public:
    auto initial_suspend() {
        return stdcoro::suspend_never{};
    }

    struct final_awaiter {
        bool await_ready() const noexcept { return false; }
        template<typename Promise>
        void await_suspend(stdcoro::coroutine_handle<Promise> h) noexcept {
            if (auto awaiting = h.promise().try_get_awaiting(); awaiting) {
                awaiting.resume();
            }
        }
        void await_resume() noexcept {}
    };

    auto final_suspend() noexcept {
        return final_awaiter{};
    }

    void unhandled_exception() {
        m_exception = std::current_exception();
    }

    void set_awaiting(stdcoro::coroutine_handle<> awaiting) noexcept {
        awaiting_ = awaiting;
    }

    stdcoro::coroutine_handle<> try_get_awaiting() const noexcept {
        return awaiting_;
    }

  private:
    std::exception_ptr m_exception;
    stdcoro::coroutine_handle<> awaiting_;
};

struct get_promise_t {};
constexpr get_promise_t get_promise = {};

template<typename T>
class task_promise final : public task_promise_base {
  public:
    task<T> get_return_object() noexcept {
        return task<T>(stdcoro::coroutine_handle<task_promise>::from_promise(*this));
    }

    void return_value(T value) {
        value_ = std::move(value);
    }

    T& get_value() {
        return value_;
    }

  private:
    T value_{};
};


template<>
class task_promise<void> final : public task_promise_base {
  public:
    task<void> get_return_object() noexcept {
        return task<void>(stdcoro::coroutine_handle<task_promise>::from_promise(*this));
    }

    void return_void() {
    }
};

} // namespace mrpc

#endif // _USE_COROUTINE

#endif // MRC_COROUTINE_HPP
