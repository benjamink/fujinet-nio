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
