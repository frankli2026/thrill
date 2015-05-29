/*******************************************************************************
 * c7a/data/buffer_chain.hpp
 *
 * Part of Project c7a.
 *
 *
 * This file has no license. Only Chuck Norris can compile it.
 ******************************************************************************/

#pragma once
#ifndef C7A_DATA_BUFFER_CHAIN_HEADER
#define C7A_DATA_BUFFER_CHAIN_HEADER

#include <vector>
#include <condition_variable>
#include <mutex> //mutex, unique_lock

#include <c7a/data/binary_buffer.hpp>
#include <c7a/data/emitter_target.hpp>

namespace c7a {
namespace data {

//! Elements of a singly linked list, holding a immuteable buffer
struct BufferChainElement {
    BufferChainElement(BinaryBuffer b) : next(nullptr), buffer(b) { }

    inline bool              IsEnd() const { return next == nullptr; }

    struct BufferChainElement* next;
    BinaryBuffer             buffer;
};

//! A Buffer chain holds multiple immuteable buffers.
//! Append in O(1), Delete in O(#buffers)
struct BufferChain : public EmitterTarget {
    BufferChain() : head(nullptr), tail(nullptr), closed_(false) { }

    void Append(BinaryBuffer b) {
        if (tail == nullptr) {
            head = new BufferChainElement(b);
            tail = head;
        }
        else {
            tail->next = new BufferChainElement(b);
            tail = tail->next;
        }
        NotifyWaitingThreads();
    }

    //! Waits until beeing notified
    void Wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_variable_.wait(lock);
    }

    //! Waits until beeing notified and closed == true
    void WaitUntilClosed() {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_variable_.wait(lock, [=]() { return this->closed_; });
    }

    //! Call buffers' destructors and deconstructs the chain
    void Delete() {
        BufferChainElement* current = head;
        while (current != nullptr) {
            BufferChainElement* next = current->next;
            current->buffer.Delete();
            current = next;
        }
    }

    void Close() {
        closed_ = true;
        NotifyWaitingThreads();
    }

    bool IsClosed() { return closed_; }

    struct BufferChainElement* head;
    struct BufferChainElement* tail;

private:
    std::mutex               mutex_;
    std::condition_variable  condition_variable_;
    bool                     closed_;

    void NotifyWaitingThreads() {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_variable_.notify_all();
    }
};

} // namespace data
} // namespace c7a

#endif // !C7A_DATA_BUFFER_CHAIN_HEADER

/******************************************************************************/