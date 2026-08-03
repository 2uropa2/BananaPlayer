#pragma once

// Single source of truth for the user-visible version string. Referenced by
// the splash footer, the About panel, and the CMake project() line (kept in
// sync manually - if you bump one, bump the other or the App / installer /
// About dialog will drift).
#define BP_VERSION "1.4.0"
