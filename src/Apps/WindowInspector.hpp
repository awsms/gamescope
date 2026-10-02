#pragma once

#include <string_view>

namespace gamescope
{
    // Inspect the Xwayland display used by a nested Gamescope instance.
    int RunWindowInspection( std::string_view command, const char *displayName );
}
