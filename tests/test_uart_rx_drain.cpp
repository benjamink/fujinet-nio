#include "doctest.h"

#include "fujinet/io/uart_rx_drain.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

using fujinet::io::uart_rx_dispatch;
using fujinet::io::uart_rx_drain;
using fujinet::io::uart_rx_service;

namespace {

using Bytes = std::vector<std::uint8_t>;

// Models the ESP-IDF UART driver: received bytes go into a ring buffer and
// a size-announcing event goes onto a bounded queue. When the queue is full
// the event is dropped but its bytes stay in the ring buffer.
struct FakeUartDriver {
    struct Event {
        bool data{true};
        std::size_t size{0};
    };

    explicit FakeUartDriver(std::size_t queue_capacity) : capacity(queue_capacity) {}

    void receive(const Bytes& bytes)
    {
        ring.insert(ring.end(), bytes.begin(), bytes.end());
        post(Event{true, bytes.size()});
    }

    void post(const Event& event)
    {
        if (queue.size() < capacity) {
            queue.push_back(event);
        } else {
            ++dropped;
        }
    }

    bool next_event(Event& event)
    {
        if (queue.empty()) return false;
        event = queue.front();
        queue.pop_front();
        return true;
    }

    bool is_data(const Event& event) const { return event.data; }

    void handle(const Event&) { ++handled; }

    bool buffered_len(std::size_t& len)
    {
        if (fail_buffered_len) return false;
        len = ring.size();
        return true;
    }

    int read(std::uint8_t* dst, std::size_t len)
    {
        if (fail_read) return -1;
        const std::size_t n = std::min({len, ring.size(), max_read});
        std::copy_n(ring.begin(), n, dst);
        ring.erase(ring.begin(), ring.begin() + static_cast<std::ptrdiff_t>(n));
        return static_cast<int>(n);
    }

    std::size_t capacity;
    std::deque<std::uint8_t> ring;
    std::deque<Event> queue;
    std::size_t dropped{0};
    std::size_t handled{0};
    bool fail_buffered_len{false};
    bool fail_read{false};
    std::size_t max_read{static_cast<std::size_t>(-1)};
};

Bytes burst(std::uint8_t first, std::size_t len)
{
    Bytes b(len);
    for (std::size_t i = 0; i < len; ++i) b[i] = static_cast<std::uint8_t>(first + i);
    return b;
}

Bytes concat(std::initializer_list<Bytes> parts)
{
    Bytes out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

} // namespace

TEST_CASE("UART RX: a packet whose events were dropped is whole after one service")
{
    // Three bursts of one packet arrive while the queue holds only two events.
    FakeUartDriver uart(2);
    const Bytes a = burst(0x10, 40), b = burst(0x40, 40), c = burst(0x70, 40);
    uart.receive(a);
    uart.receive(b);
    uart.receive(c);
    REQUIRE(uart.dropped == 1);

    Bytes fifo;
    uart_rx_service(uart, fifo);
    CHECK(fifo == concat({a, b, c}));
    CHECK(uart.ring.empty());
}

TEST_CASE("UART RX: dropped-event bytes don't turn up inside the next packet")
{
    FakeUartDriver uart(1);
    const Bytes packet1_head = burst(0x10, 32), packet1_tail = burst(0x30, 32);
    uart.receive(packet1_head);
    uart.receive(packet1_tail); // event dropped
    REQUIRE(uart.dropped == 1);

    Bytes fifo;
    uart_rx_service(uart, fifo);
    CHECK(fifo == concat({packet1_head, packet1_tail}));

    fifo.clear();
    const Bytes packet2 = burst(0xA0, 20);
    uart.receive(packet2);
    uart_rx_service(uart, fifo);
    CHECK(fifo == packet2);
}

TEST_CASE("UART RX: events for bytes already read add nothing")
{
    FakeUartDriver uart(8);
    const Bytes a = burst(0x01, 16), b = burst(0x20, 16);
    uart.receive(a);
    uart.receive(b);

    Bytes fifo;
    uart_rx_service(uart, fifo); // the first event reads both bursts
    CHECK(fifo == concat({a, b}));
    CHECK(uart.queue.empty());
}

TEST_CASE("UART RX: other events go to the channel's handler")
{
    FakeUartDriver uart(4);
    uart.post(FakeUartDriver::Event{false, 0}); // e.g. a frame error
    const Bytes a = burst(0x01, 8);
    uart.receive(a);

    Bytes fifo;
    uart_rx_service(uart, fifo);
    CHECK(uart.handled == 1);
    CHECK(fifo == a);
}

TEST_CASE("UART RX: an event taken off the queue elsewhere is routed the same way")
{
    // As wait_for_readable does: it blocks on the queue, then dispatches the
    // event that woke it. A data event must drain, never reach the handler
    // (which only knows non-data events).
    FakeUartDriver uart(4);
    const Bytes a = burst(0x10, 12);
    uart.receive(a);
    FakeUartDriver::Event woke{};
    REQUIRE(uart.next_event(woke));

    Bytes fifo;
    uart_rx_dispatch(uart, woke, fifo);
    CHECK(fifo == a);
    CHECK(uart.handled == 0);

    uart_rx_dispatch(uart, FakeUartDriver::Event{false, 0}, fifo);
    CHECK(uart.handled == 1);
    CHECK(fifo == a);
}

TEST_CASE("UART RX: a drain keeps only the bytes actually read")
{
    FakeUartDriver uart(4);
    uart.ring.assign(10, 0x55);

    Bytes fifo{0x01, 0x02};
    uart.max_read = 4;
    CHECK(uart_rx_drain(uart, fifo) == 4);
    CHECK(fifo == Bytes{0x01, 0x02, 0x55, 0x55, 0x55, 0x55});

    uart.fail_read = true;
    CHECK(uart_rx_drain(uart, fifo) == 0);
    CHECK(fifo.size() == 6);

    uart.fail_read = false;
    uart.fail_buffered_len = true;
    CHECK(uart_rx_drain(uart, fifo) == 0);
    CHECK(fifo.size() == 6);
}
