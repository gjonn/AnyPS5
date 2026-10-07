#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {

int APS5_VABI sceTextToSpeech2GetSystemStatus() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}

// GT7-LOCAL-PLACEHOLDER BEGIN
extern "C" {
int APS5_VABI sceTextToSpeech2Cancel() { NotImplemented_nid_no_patch(__func__); return 0; }
int APS5_VABI sceTextToSpeech2Close() { NotImplemented_nid_no_patch(__func__); return 0; }
int APS5_VABI sceTextToSpeech2GetSpeechStatus() { NotImplemented_nid_no_patch(__func__); return 0; }
int APS5_VABI sceTextToSpeech2Initialize() { NotImplemented_nid_no_patch(__func__); return 0; }
int APS5_VABI sceTextToSpeech2Open() { NotImplemented_nid_no_patch(__func__); return 0; }
int APS5_VABI sceTextToSpeech2Speak() { NotImplemented_nid_no_patch(__func__); return 0; }
int APS5_VABI sceTextToSpeech2Terminate() { NotImplemented_nid_no_patch(__func__); return 0; }
}
// GT7-LOCAL-PLACEHOLDER END
