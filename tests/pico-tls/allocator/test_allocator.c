// Actual SDK ALTCP allocator, lwIP heap, and firmware mbedTLS configuration.
#include <stdio.h>
#include "lwip/mem.h"
#include "lwip/stats.h"
#include "altcp_tls_mbedtls_mem.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/platform_time.h"
int mbedtls_hardware_poll(void *p,unsigned char *o,size_t n,size_t *olen) {
    (void)p; (void)o; (void)n; *olen=0; return -1;
}
mbedtls_ms_time_t mbedtls_ms_time(void) { return 0; }
static int rng(void *p,unsigned char *o,size_t n) { (void)p; while(n--) *o++=0x42; return 0; }
int main(void) {
    mem_init(); altcp_mbedtls_mem_init();
    char pem[8192]; FILE *file=fopen(NIO_ROOT "/tests/pico-tls/fixtures/isrg-root-x1.pem","rb");
    if (!file) return 1;
    size_t length=fread(pem,1,sizeof(pem)-1,file); fclose(file); pem[length]=0;
    mbedtls_x509_crt roots[2]; altcp_mbedtls_state_t *states[2];
    mbedtls_ssl_context ssl[2]; mbedtls_ssl_config cfg[2];
    int success=0;
    for (int i=0;i<2;i++) {
        mbedtls_x509_crt_init(&roots[i]); mbedtls_ssl_init(&ssl[i]); mbedtls_ssl_config_init(&cfg[i]);
        int cert=mbedtls_x509_crt_parse(&roots[i],(unsigned char*)pem,length+1);
        states[i]=altcp_mbedtls_alloc(NULL);
        int config=mbedtls_ssl_config_defaults(&cfg[i],MBEDTLS_SSL_IS_CLIENT,
                                             MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT);
        mbedtls_ssl_conf_rng(&cfg[i],rng,NULL);
        mbedtls_ssl_conf_ca_chain(&cfg[i],&roots[i],NULL);
        int setup=mbedtls_ssl_setup(&ssl[i],&cfg[i]);
        if (!cert && states[i] && !config && !setup) ++success;
    }
    printf("heap=%d successful contexts=%d peak=%u (host setup only)\n",MEM_SIZE,success,(unsigned)lwip_stats.mem.max);
    for (int i=0;i<2;i++) {
        mbedtls_ssl_free(&ssl[i]); mbedtls_ssl_config_free(&cfg[i]); mbedtls_x509_crt_free(&roots[i]);
        if(states[i]) altcp_mbedtls_free(NULL,states[i]);
    }
#ifdef TEST_OLD_HEAP
    return success != 0 || lwip_stats.mem.used != 0;
#else
    return success != 2 || lwip_stats.mem.used != 0;
#endif
}
