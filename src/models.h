#pragma once

#include <QString>

// The directory holding the face models used for head tracking, or an empty
// string if they are not installed. Looks in $EYENIX_MODEL_DIR, next to a
// build in the source tree, then where the package installs them.
QString modelDirectory();

// eyenix-bridge.exe, the helper started inside a game's Wine prefix, or an
// empty string if it is not installed. Looks in $EYENIX_BRIDGE, next to a
// build in the source tree, then where the package installs it.
QString bridgeExecutable();
