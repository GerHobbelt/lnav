/**
 * Copyright (c) 2020, Timothy Stack
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * * Neither the name of Timothy Stack nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef lnav_future_util_hh
#define lnav_future_util_hh

#include <deque>
#include <future>

namespace lnav {
namespace futures {

/**
 * Create a future that is ready to immediately return a result.
 *
 * @tparam T The result type of the future.
 * @param t The value the future should return.
 * @return The new future.
 */
template<class T>
std::future<std::decay_t<T>> make_ready_future( T&& t ) {
    std::promise<std::decay_t<T>> pr;
    auto r = pr.get_future();
    pr.set_value(std::forward<T>(t));
    return r;
}

/**
 * A queue used to limit the number of futures that are running concurrently.
 *
 * @tparam T The result of the futures.
 * @tparam MAX_QUEUE_SIZE The maximum number of futures that can be in flight.
 */
template<typename T, int MAX_QUEUE_SIZE = 8>
class future_queue {
public:
    /**
     * @param processor The function to execute with the result of a future.
     */
    explicit future_queue(std::function<void(const T&)> processor)
        : fq_processor(processor) {};

    ~future_queue() {
        this->pop_to();
    }

    /**
     * Add a future to the queue.  If the size of the queue is greater than the
     * MAX_QUEUE_SIZE, this call will block waiting for the first queued
     * future to return a result.
     *
     * @param f The future to add to the queue.
     */
    void push_back(std::future<T>&& f) {
        this->fq_deque.emplace_back(std::move(f));
        this->pop_to(MAX_QUEUE_SIZE);
    }

    /**
     * Removes the next future from the queue, waits for the result, and then
     * repeats until the queue reaches the given size.
     *
     * @param size The new desired size of the queue.
     */
    void pop_to(size_t size = 0) {
        while (this->fq_deque.size() > size) {
            this->fq_processor(this->fq_deque.front().get());
            this->fq_deque.pop_front();
        }
    }

    std::function<void(const T&)> fq_processor;
    std::deque<std::future<T>> fq_deque;
};

}
}

#endif
