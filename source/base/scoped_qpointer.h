//
// Aspia Project
// Copyright (C) 2016-2026 Dmitry Chapyshev
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//

#ifndef BASE_SCOPED_QPOINTER_H
#define BASE_SCOPED_QPOINTER_H

#include <QObject>
#include <QPointer>

#include <type_traits>
#include <utility>

// Owning smart pointer for QObject-derived types. Behaves like std::unique_ptr, but on reset()
// schedules deletion via deleteLater() instead of calling delete directly. This makes it safe to
// release objects that may still be processing queued events or live in another thread.
//
// The held pointer is stored as QPointer<T>, so if the object is destroyed externally (for
// example by its Qt parent), the pointer auto-nulls and reset() becomes a no-op instead of
// dereferencing a dangling pointer.
//
// NOTE: reset() does not disconnect signals. If the held object is still emitting signals
// between the deleteLater() call and its actual destruction, those signals will reach connected
// receivers as usual. Call disconnect() manually before reset() if this matters (typical reasons:
// the object lives in another thread and is actively emitting, or there are external subscribers
// that must stop receiving signals immediately).
//
// T may be forward-declared at the point of member declaration; the underlying QPointer<T>
// requires T to derive from QObject, but the check is deferred until the type is actually used.
template <typename T>
class ScopedQPointer
{
public:
    ScopedQPointer() noexcept = default;

    explicit ScopedQPointer(T* ptr) noexcept
        : ptr_(ptr)
    {
        // Nothing
    }

    ~ScopedQPointer()
    {
        reset();
    }

    ScopedQPointer(ScopedQPointer&& other) noexcept
        : ptr_(other.ptr_)
    {
        other.ptr_ = nullptr;
    }

    ScopedQPointer& operator=(ScopedQPointer&& other) noexcept
    {
        if (this != &other)
        {
            reset(other.ptr_.data());
            other.ptr_ = nullptr;
        }
        return *this;
    }

    // Move from a ScopedQPointer of a derived type. Enabled only when U* converts to T*.
    template <typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    ScopedQPointer(ScopedQPointer<U>&& other) noexcept
        : ptr_(other.release())
    {
        // Nothing
    }

    template <typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    ScopedQPointer& operator=(ScopedQPointer<U>&& other) noexcept
    {
        reset(other.release());
        return *this;
    }

    ScopedQPointer& operator=(T* ptr) noexcept
    {
        reset(ptr);
        return *this;
    }

    T* get() const noexcept { return ptr_.data(); }
    T* operator->() const noexcept { return ptr_.data(); }
    T& operator*() const noexcept { return *ptr_; }
    operator T*() const noexcept { return ptr_.data(); }

    // Releases ownership without deleting. Caller is responsible for cleanup.
    [[nodiscard]] T* release() noexcept
    {
        T* p = ptr_.data();
        ptr_ = nullptr;
        return p;
    }

    void swap(ScopedQPointer& other) noexcept
    {
        std::swap(ptr_, other.ptr_);
    }

    // Schedules deletion of the held object via deleteLater() and replaces the held pointer with
    // new_ptr. If the held object has already been destroyed externally, the QPointer is null and
    // this becomes a no-op.
    void reset(T* new_ptr = nullptr) noexcept
    {
        if (ptr_ && ptr_.data() != new_ptr)
            ptr_->deleteLater();
        ptr_ = new_ptr;
    }

    // Asynchronous reset that calls |on_destroyed| once the held object has been fully torn down.
    // If no object is held, |on_destroyed| runs immediately. Otherwise the callback is connected
    // to the held object's QObject::destroyed signal (with |context| as the receiver, so Qt
    // auto-disconnects if |context| dies first), deleteLater() is scheduled, and the internal
    // pointer is cleared right away.
    template <typename F, typename = std::enable_if_t<std::is_invocable_v<F&>>>
    void resetThen(const QObject* context, F&& on_destroyed)
    {
        if (!ptr_)
        {
            std::forward<F>(on_destroyed)();
            return;
        }

        T* p = ptr_.data();
        QObject::connect(p, &QObject::destroyed, context,
                         [cb = std::forward<F>(on_destroyed)]() mutable { cb(); });
        p->deleteLater();
        ptr_ = nullptr;
    }

private:
    QPointer<T> ptr_;
    Q_DISABLE_COPY(ScopedQPointer)
};

template <typename T>
inline void swap(ScopedQPointer<T>& a, ScopedQPointer<T>& b) noexcept
{
    a.swap(b);
}

#endif // BASE_SCOPED_QPOINTER_H
