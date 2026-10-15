#include <gtest/gtest.h>

#include "galaxian/timer.h"


TEST(Timer, GenericKonamiDoesNotSwapBits35) {
    // Generic Konami at t=0 is 0x0E; Frogger would swap that to 0x26.
    EXPECT_EQ(galaxian::konami_sound_timer(0), 0x0E);
    EXPECT_NE(galaxian::konami_sound_timer(2560), galaxian::konami_sound_timer(0));
}
