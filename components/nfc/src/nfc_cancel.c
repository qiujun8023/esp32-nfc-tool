#include "nfc_cancel.h"

static volatile bool s_cancel = false;

void nfc_cancel_request(void) { s_cancel = true; }
void nfc_cancel_clear(void) { s_cancel = false; }
bool nfc_cancel_pending(void) { return s_cancel; }
