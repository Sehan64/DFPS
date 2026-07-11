/*
 * test_stubs.c — test-only stubs for binder.c functions.
 *
 * The real daemon links binder.c, but binder.c pulls in the Android-only
 * __system_property_get (used by resolveTransactionCodes). So the regression
 * tests link THIS file instead of binder.c, keeping `make test` buildable
 * on plain Linux CI runners. These symbols are never actually called by
 * the test binaries (their entry point is the test's own main), they only
 * satisfy the linker because main.c references them.
 */

#include "../src/dfps.h"

void resolveTransactionCodes(void) { }

void queryFocusedTask(void) { }

void onBinderDied(void* cookie) { (void)cookie; }

binder_status_t displayCallbackOnTransact(AIBinder* binder, transaction_code_t code,
                                          const AParcel* in, AParcel* out) {
    (void)binder; (void)code; (void)in; (void)out;
    return STATUS_OK;
}

binder_status_t observerOnTransact(AIBinder* binder, transaction_code_t code,
                                     const AParcel* in, AParcel* out) {
    (void)binder; (void)code; (void)in; (void)out;
    return STATUS_OK;
}

binder_status_t batteryListenerOnTransact(AIBinder* binder, transaction_code_t code,
                                           const AParcel* in, AParcel* out) {
    (void)binder; (void)code; (void)in; (void)out;
    return STATUS_OK;
}
