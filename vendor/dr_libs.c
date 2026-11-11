// dr_libs implementations in their own translation unit, built with the vendor flags (see Makefile).
// Our code includes the headers without the IMPLEMENTATION defines.
#define DR_WAV_IMPLEMENTATION
#include "dr_libs/dr_wav.h"
#define DR_MP3_IMPLEMENTATION
#include "dr_libs/dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_libs/dr_flac.h"
