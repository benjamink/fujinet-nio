#include "doctest.h"
#include "../src/platform/pico/detail/usb_tx.h"
using fujinet::platform::pico::detail::UsbTx;
TEST_CASE("Pico USB queue isolates connections and quarantines overflow") {
    UsbTx<4> tx; unsigned char bytes[]{1,2,3,4}, out[4]{};
    CHECK_FALSE(tx.write(bytes,1)); tx.connection(true); CHECK(tx.epoch()==1);
    CHECK(tx.write(bytes,4)); CHECK(tx.take(out,2)==2); CHECK(out[1]==2);
    CHECK(tx.write(bytes,2)); CHECK(tx.take(out,4)==4); CHECK(out[0]==3); CHECK(out[3]==2);
    CHECK(tx.write(bytes,4)); CHECK_FALSE(tx.write(bytes,1)); CHECK_FALSE(tx.ready());
    CHECK(tx.take(out,4)==0); CHECK(tx.epoch()==2);
    tx.connection(false); tx.connection(true); CHECK(tx.ready()); CHECK(tx.take(out,4)==0);
    CHECK(tx.write(bytes,1)); CHECK(tx.take(out,4)==1); CHECK(out[0]==1);
}

TEST_CASE("Pico USB callback edges retire queued bytes even within one service call") {
    UsbTx<4> tx; unsigned char old[]{1,2},fresh[]{9},out[4]{};
    tx.connection(true);REQUIRE(tx.write(old,2));const auto epoch=tx.epoch();
    tx.connection(false);tx.connection(true); // Both events before the next pump.
    CHECK(tx.epoch()==epoch+2);CHECK(tx.take(out,4)==0);
    REQUIRE(tx.write(fresh,1));CHECK(tx.take(out,4)==1);CHECK(out[0]==9);
}
