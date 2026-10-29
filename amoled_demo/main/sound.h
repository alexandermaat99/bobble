#pragma once
// Little sounds from the square board's speaker, at the volume picked in Settings (device_volume_get). The round
// board has no speaker and the simulator stays quiet: there these do nothing.

void sound_click(void);   // a short tick (Settings plays it when you let go of the volume slider)
