#pragma once

#include <utility>
#include <cstddef>

namespace csy {

template <typename T>
class Rc {
public:
    Rc() noexcept : ctrl_(nullptr) {}
    Rc(std::nullptr_t) noexcept : ctrl_(nullptr) {}

    Rc(const Rc& other) noexcept : ctrl_(other.ctrl_) {
        if (ctrl_) ++ctrl_->count;
    }
    Rc(Rc&& other) noexcept : ctrl_(other.ctrl_) {
        other.ctrl_ = nullptr;
    }

    Rc& operator=(const Rc& other) noexcept {
        if (this != &other) {
            release();
            ctrl_ = other.ctrl_;
            if (ctrl_) ++ctrl_->count;
        }
        return *this;
    }
    Rc& operator=(Rc&& other) noexcept {
        if (this != &other) {
            release();
            ctrl_ = other.ctrl_;
            other.ctrl_ = nullptr;
        }
        return *this;
    }
    Rc& operator=(std::nullptr_t) noexcept {
        release();
        return *this;
    }

    ~Rc() { release(); }

    T* get() const noexcept { return ctrl_ ? &ctrl_->value : nullptr; }
    T* operator->() const noexcept { return get(); }
    T& operator*() const noexcept { return *get(); }
    explicit operator bool() const noexcept { return ctrl_ != nullptr; }
    bool unique() const noexcept { return ctrl_ && ctrl_->count == 1; }

    bool operator==(const Rc& other) const noexcept { return ctrl_ == other.ctrl_; }
    bool operator!=(const Rc& other) const noexcept { return ctrl_ != other.ctrl_; }
    bool operator==(std::nullptr_t) const noexcept { return ctrl_ == nullptr; }
    bool operator!=(std::nullptr_t) const noexcept { return ctrl_ != nullptr; }

    template <typename... Args>
    static Rc<T> make(Args&&... args) {
        return Rc<T>(new Ctrl(std::forward<Args>(args)...));
    }

private:
    struct Ctrl {
        T value;
        int count;
        template <typename... Args>
        explicit Ctrl(Args&&... args) : value(std::forward<Args>(args)...), count(1) {}
    };

    explicit Rc(Ctrl* c) noexcept : ctrl_(c) {}

    void release() noexcept {
        if (ctrl_ && --ctrl_->count == 0) {
            delete ctrl_;
        }
        ctrl_ = nullptr;
    }

    Ctrl* ctrl_;
};

template <typename T, typename... Args>
Rc<T> make_rc(Args&&... args) {
    return Rc<T>::make(std::forward<Args>(args)...);
}

}

