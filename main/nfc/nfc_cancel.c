#include "nfc_cancel.h"

#include <stdatomic.h>

static atomic_bool s_cancel = false;

void nfc_cancel_request(void) { atomic_store(&s_cancel, true); }
void nfc_cancel_clear(void) { atomic_store(&s_cancel, false); }
bool nfc_cancel_pending(void) { return atomic_load(&s_cancel); }
