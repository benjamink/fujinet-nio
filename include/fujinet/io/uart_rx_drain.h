#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fujinet::io {

/// Receive side of an event-driven UART driver (the ESP-IDF one, in
/// UartChannel), kept here so its handling can be tested on any host.
///
/// `Driver` provides:
///   using Event = ...;
///   bool next_event(Event&);              // pop a queued event; false when none
///   bool is_data(const Event&) const;     // a "bytes arrived" event
///   void handle(const Event&);            // any other event (errors, overflow)
///   bool buffered_len(std::size_t&);      // bytes the driver holds now
///   int read(std::uint8_t*, std::size_t); // non-blocking read; < 0 on error
///
/// A data event's size is not a count to read. When the driver's event queue
/// is full it drops the event but keeps the bytes, so reading only what each
/// event announced would leave those bytes behind, to surface later inside
/// another packet. Every read takes all the bytes the driver holds instead.

/// Append everything the driver holds to `fifo`. Returns the bytes appended.
template <class Driver>
std::size_t uart_rx_drain(Driver& driver, std::vector<std::uint8_t>& fifo)
{
    std::size_t buffered = 0;
    if (!driver.buffered_len(buffered) || buffered == 0) {
        return 0;
    }
    const std::size_t old_len = fifo.size();
    fifo.resize(old_len + buffered);
    int result = driver.read(&fifo[old_len], buffered);
    if (result < 0) {
        result = 0;
    }
    fifo.resize(old_len + static_cast<std::size_t>(result));
    return static_cast<std::size_t>(result);
}

/// Handle every queued event, then drain what dropped events left behind.
template <class Driver>
void uart_rx_service(Driver& driver, std::vector<std::uint8_t>& fifo)
{
    typename Driver::Event event{};
    while (driver.next_event(event)) {
        if (driver.is_data(event)) {
            uart_rx_drain(driver, fifo);
        } else {
            driver.handle(event);
        }
    }
    uart_rx_drain(driver, fifo);
}

} // namespace fujinet::io
