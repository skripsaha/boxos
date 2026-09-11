#ifndef BOXCXX_TLS_STRAND_H
#define BOXCXX_TLS_STRAND_H


#ifdef __cplusplus
extern "C" {
#endif

void __boxcxx_tls_strand_init(void);

void __boxcxx_thread_storage_enter(void);

void __boxcxx_thread_storage_exit(void);

#ifdef __cplusplus
}
#endif

#endif