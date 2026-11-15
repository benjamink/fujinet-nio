#include "doctest.h"
#include "../src/platform/pico/detail/socket_state.h"
using namespace fujinet::platform::pico::detail;
TEST_CASE("Pico byte ring wraps and rejects full writes atomically") {
    ByteRing<4> ring;
    const unsigned char bytes[]{1,2,3,4}; unsigned char out[4]{};
    CHECK(ring.push(bytes,4)); CHECK_FALSE(ring.push(bytes,1));
    REQUIRE(ring.pop(out,3)==3); CHECK(out[0]==1); CHECK(out[2]==3);
    CHECK(ring.push(bytes,3)); REQUIRE(ring.pop(out,4)==4);
    CHECK(out[0]==4); CHECK(out[1]==1); CHECK(out[3]==3); CHECK(ring.size()==0);
}
TEST_CASE("Pico handle slots reject stale handles and bound allocation") {
    HandleTable<int,2> table;
    const int first=table.allocate(), second=table.allocate();
    REQUIRE(table.get(first)); *table.get(first)=42;
    CHECK(table.allocate()<0); table.release(first);
    const int third=table.allocate(); CHECK(third!=first); CHECK(table.get(first)==nullptr);
    REQUIRE(table.get(third)); CHECK(*table.get(third)==0); CHECK(table.get(second));
}
TEST_CASE("Pico UDP queue keeps empty packets, rejects oversize, discards truncation") {
    DatagramQueue<4,2> queue; unsigned char bytes[]{1,2,3,4,5}, out[5]{};
    CHECK_FALSE(queue.push(bytes,5)); CHECK(queue.push(bytes,0)); CHECK_FALSE(queue.empty());
    CHECK(queue.push(bytes,4)); CHECK_FALSE(queue.push(bytes,1));
    CHECK(queue.pop(out,5)==0); CHECK(queue.pop(out,2)==2); CHECK(out[1]==2); CHECK(queue.empty());
    CHECK(queue.push(bytes,4)); CHECK(queue.pop(out,5)==4); CHECK(out[3]==4);
}
