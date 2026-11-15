#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/platform/pico/socket_ops_raw.cpp"

namespace {
using namespace fujinet;
struct Locked {
    Locked(){cyw43_arch_lwip_begin();}
    ~Locked(){cyw43_arch_lwip_end();}
};
struct Fixture {
    net::ITcpSocketOps& tcp=platform::default_tcp_socket_ops();
    net::IUdpSocketOps& udp=platform::default_udp_socket_ops();
    std::vector<int> tcps,udps;
    Fixture() {
        fake::connect_result=fake::write_result=fake::close_result=fake::dns_result=ERR_OK;
        fake::on_sleep={};
    }
    ~Fixture() {
        for(auto fd:tcps)tcp.close(fd);
        for(auto fd:udps)udp.close(fd);
        fake::on_sleep={};
    }
    int stream(bool connect=true) {
        auto fd=tcp.socket(4,1,6);tcps.push_back(fd);
        if(connect){
            net::AddrInfo* ai=nullptr;
            REQUIRE(tcp.getaddrinfo("example","80",nullptr,&ai)==0);
            net::SockLen n=0;auto* a=tcp.addrinfo_addr(ai,&n);
            CHECK(tcp.connect(fd,a,n)==-1);CHECK(tcp.last_errno()==EINPROGRESS);
            tcp.freeaddrinfo(ai);
            auto* p=fake::tcp.back().get();
            Locked lock;REQUIRE(p->connected(p->arg,p,ERR_OK)==ERR_OK);
        }
        return fd;
    }
    int datagram(){auto fd=udp.socket(4,2,17);udps.push_back(fd);return fd;}
};
pbuf* packet(std::size_t n,std::uint8_t value=42) {
    auto* p=pbuf_alloc(PBUF_TRANSPORT,n,PBUF_RAM);
    std::memset(p->payload,value,n);return p;
}
}

TEST_CASE_FIXTURE(Fixture,"TCP credits only consumed bytes and retains refused receive packets") {
    const auto fd=stream();auto* p=fake::tcp.back().get();
    {
        Locked lock;
        REQUIRE(p->recv(p->arg,p,packet(FN_PICO_TCP_WINDOW_BYTES),ERR_OK)==ERR_OK);
        CHECK(p->credited==0);
        auto* retry=packet(1,7);const auto freed=fake::freed_pbufs;
        CHECK(p->recv(p->arg,p,retry,ERR_OK)==ERR_MEM);
        CHECK(fake::freed_pbufs==freed);
        std::array<std::uint8_t,FN_PICO_TCP_WINDOW_BYTES> out{};
        CHECK(tcp.recv(fd,out.data(),out.size())==FN_PICO_TCP_WINDOW_BYTES);
        CHECK(p->credited==FN_PICO_TCP_WINDOW_BYTES);
        CHECK(out.front()==42);CHECK(out.back()==42);
        CHECK(p->recv(p->arg,p,retry,ERR_OK)==ERR_OK);
    }
    std::uint8_t out=0;CHECK(tcp.recv(fd,&out,1)==1);CHECK(out==7);
    CHECK(tcp.recv(fd,&out,1)==-1);CHECK(tcp.last_errno()==EAGAIN);
    {Locked lock;CHECK(p->recv(p->arg,p,nullptr,ERR_OK)==ERR_OK);}
    CHECK(tcp.recv(fd,&out,1)==0);
}
TEST_CASE_FIXTURE(Fixture,"TCP preserves queued bytes before reporting peer reset") {
    auto fd=stream();auto* p=fake::tcp.back().get();
    {Locked lock;p->recv(p->arg,p,packet(2),ERR_OK);p->dead=true;p->error(p->arg,ERR_RST);}
    std::array<std::uint8_t,4> out{};
    CHECK(tcp.recv(fd,out.data(),4)==2);
    CHECK(tcp.recv(fd,out.data(),4)==-1);CHECK(tcp.last_errno()==ECONNRESET);
    CHECK(tcp.send(fd,out.data(),4)==-1);CHECK(tcp.last_errno()==ECONNRESET);
    CHECK(tcp.get_so_error(fd)==ECONNRESET);
}
TEST_CASE_FIXTURE(Fixture,"TCP close detaches callbacks before abort and invalidates old handle") {
    auto fd=stream();auto* old=fake::tcp.back().get();
    fake::close_result=ERR_MEM;const auto before=fake::aborts;
    tcp.close(fd);CHECK(fake::aborts==before+1);CHECK(old->arg==nullptr);
    CHECK(old->recv==nullptr);CHECK(old->error==nullptr);
    fake::close_result=ERR_OK;
    auto next=stream();CHECK(next!=fd);
    std::uint8_t byte=0;
    CHECK(tcp.recv(fd,&byte,1)==-1);CHECK(tcp.last_errno()==EBADF);
    CHECK(tcp.send(next,&byte,1)==1);
}
TEST_CASE_FIXTURE(Fixture,"TCP short writes and send queue pressure do not duplicate data") {
    auto fd=stream();auto* p=fake::tcp.back().get();p->sndbuf=2;
    const std::array<std::uint8_t,4> bytes{1,2,3,4};
    CHECK(tcp.send(fd,bytes.data(),4)==2);CHECK(p->sent==std::vector<std::uint8_t>{1,2});
    fake::write_result=ERR_MEM;
    CHECK(tcp.send(fd,bytes.data()+2,2)==-1);CHECK(tcp.last_errno()==EAGAIN);
    CHECK(p->sent.size()==2);
    fake::write_result=ERR_OK;p->sndbuf=0;
    CHECK(tcp.send(fd,bytes.data()+2,2)==-1);CHECK(tcp.last_errno()==EAGAIN);
}
TEST_CASE_FIXTURE(Fixture,"Listener rejects stream operations and owns unaccepted clients") {
    auto fd=stream(false);CHECK(tcp.listen(fd,4)==0);auto* listener=fake::tcp.back().get();
    std::uint8_t byte=0;
    CHECK(tcp.send(fd,&byte,1)==-1);
    CHECK(tcp.recv(fd,&byte,1)==-1);
    CHECK(tcp.shutdown_write(fd)==-1);
    tcp_pcb* child;
    {Locked lock;child=tcp_new_ip_type(0);CHECK(listener->accept(listener->arg,child,ERR_OK)==ERR_OK);}
    tcp.close(fd);CHECK(child->dead);CHECK(listener->dead);
}
TEST_CASE_FIXTURE(Fixture,"UDP truncates one datagram and keeps following empty datagram") {
    auto fd=datagram();auto* p=fake::udp.back().get();ip_addr_t ip{123};
    {Locked lock;p->recv(p->arg,p,packet(4),&ip,80);p->recv(p->arg,p,packet(0),&ip,80);}
    std::uint8_t byte=0;CHECK(udp.recv(fd,&byte,1)==1);CHECK(byte==42);
    CHECK(udp.poll_readable(fd));CHECK(udp.recv(fd,&byte,1)==0);CHECK_FALSE(udp.poll_readable(fd));
}
TEST_CASE_FIXTURE(Fixture,"UDP drops oversized or excess datagrams without corrupting queue") {
    auto fd=datagram();auto* p=fake::udp.back().get();ip_addr_t ip{123};
    {Locked lock;
        p->recv(p->arg,p,packet(1473),&ip,80);
        for(int i=0;i<5;++i)p->recv(p->arg,p,packet(1,i),&ip,80);
    }
    for(int i=0;i<4;++i){std::uint8_t byte=9;CHECK(udp.recv(fd,&byte,1)==1);CHECK(byte==i);}
    CHECK_FALSE(udp.poll_readable(fd));
}
TEST_CASE_FIXTURE(Fixture,"DNS async success and negative callback release stable slots") {
    fake::dns_result=ERR_INPROGRESS;net::AddrInfo* ai=nullptr;
    auto i=fake::dns.size();fake::on_sleep=[i]{fake::on_sleep={};fake::complete_dns(i);};
    REQUIRE(tcp.getaddrinfo("host","80",nullptr,&ai)==0);REQUIRE(ai!=nullptr);tcp.freeaddrinfo(ai);
    i=fake::dns.size();fake::on_sleep=[i]{fake::on_sleep={};fake::complete_dns(i,false);};
    CHECK(tcp.getaddrinfo("host","80",nullptr,&ai)==EHOSTUNREACH);CHECK(ai==nullptr);
}
TEST_CASE_FIXTURE(Fixture,"DNS timeout protects late callbacks and bounds pending requests") {
    fake::dns_result=ERR_INPROGRESS;const auto first=fake::dns.size();
    for(int i=0;i<4;++i){net::AddrInfo* ai=nullptr;CHECK(tcp.getaddrinfo("host","80",nullptr,&ai)==ETIMEDOUT);CHECK(ai==nullptr);}
    net::AddrInfo* ai=nullptr;CHECK(tcp.getaddrinfo("host","80",nullptr,&ai)==EAGAIN);
    for(std::size_t i=first;i<first+4;++i)fake::complete_dns(i);
    fake::dns_result=ERR_OK;CHECK(tcp.getaddrinfo("host","80",nullptr,&ai)==0);tcp.freeaddrinfo(ai);
    CHECK(fake::lock_depth==0);
}

TEST_CASE_FIXTURE(Fixture,"Listener accepts queued clients FIFO across wrap and overflow") {
    const auto fd=stream(false);REQUIRE(tcp.listen(fd,4)==0);auto* listener=fake::tcp.back().get();
    auto arrive=[&](unsigned id) {
        Locked lock;auto* child=tcp_new_ip_type(0);child->remote_port=id;
        const auto status=listener->accept(listener->arg,child,ERR_OK);
        return std::make_pair(child,status);
    };
    auto [a,sa]=arrive(1);auto [b,sb]=arrive(2);CHECK(sa==ERR_OK);CHECK(sb==ERR_OK);
    auto accept_port=[&] {
        net::AddrInfo address{};net::SockLen size=sizeof(address);
        const int child=tcp.accept(fd,reinterpret_cast<sockaddr*>(&address),&size);tcps.push_back(child);
        REQUIRE(child>=0);return address.port;
    };
    CHECK(accept_port()==1);auto [c,sc]=arrive(3);CHECK(sc==ERR_OK);
    CHECK(accept_port()==2);CHECK(accept_port()==3);
    tcp.close(tcps[1]);tcp.close(tcps[2]);tcp.close(tcps[3]);
    for(unsigned i=4;i<7;++i) CHECK(arrive(i).second==ERR_OK);
    auto [overflow,status]=arrive(7);CHECK(status==ERR_ABRT);CHECK(overflow->dead);
    for(unsigned i=4;i<7;++i) CHECK(accept_port()==i);
    CHECK(tcp.accept(fd,nullptr,nullptr)==-1);CHECK(tcp.last_errno()==EAGAIN);
}
TEST_CASE_FIXTURE(Fixture,"TCP monotonic milliseconds do not wrap after 49 days") {
    fake::now=UINT64_C(0xffffffff)-2;const auto before=tcp.now_ms();
    fake::now+=10;CHECK(tcp.now_ms()-before==10);CHECK(tcp.now_ms()>UINT32_MAX);
}
