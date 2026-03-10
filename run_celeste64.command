#!/bin/bash
# Celeste64 launcher for macOS 10.9 compatibility
# Uses stub libraries for missing frameworks and system symbols

DIR="$(cd "$(dirname "$0")" && pwd)"

# Redirect missing framework/library loads to local stubs
export DYLD_FRAMEWORK_PATH="$DIR/compat:${DYLD_FRAMEWORK_PATH}"
export DYLD_LIBRARY_PATH="$DIR/compat:$DIR/compat/swift:${DYLD_LIBRARY_PATH}"

# Force flat namespace so shims can provide missing system symbols
export DYLD_FORCE_FLAT_NAMESPACE=1

# Insert MacPorts Legacy Support (clock_gettime, fstatat, etc.)
# and our custom compat shim (Security stubs, syslog, CCRandom)
export DYLD_INSERT_LIBRARIES="/usr/local/lib/libMacportsLegacySupport.dylib:$DIR/compat/libcompat.dylib"

# Disable W^X double mapping (not supported on older macOS)
export DOTNET_EnableWriteXorExecute=0

# Use invariant globalization (system ICU is too old on 10.9)
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

# Activate game window after a short delay (prevents "invalid drawable"
# when Terminal steals focus on macOS 10.9)
(unset DYLD_FORCE_FLAT_NAMESPACE DYLD_INSERT_LIBRARIES DYLD_FRAMEWORK_PATH DYLD_LIBRARY_PATH
 sleep 2 && osascript -e 'tell application "System Events" to set frontmost of process "Celeste64" to true' 2>/dev/null) &

exec "$DIR/Celeste64" "$@"
