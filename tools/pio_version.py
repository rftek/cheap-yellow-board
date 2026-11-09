"""PlatformIO pre-script for every app and the launcher (extra_scripts).

ESP-IDF reads the project's version.txt into PROJECT_VER, which becomes
esp_app_desc_t.version in the image, but only while CMake configures the
project. PlatformIO re-runs that step only when CMakeLists.txt or an sdkconfig
file changes, so a bumped version.txt would leave an incremental build
reporting the old version. Removing build.ninja when version.txt is newer
makes PlatformIO configure again; the compiled objects are kept.
"""

import os

Import("env")  # noqa: F821 (provided by PlatformIO)

version_file = os.path.join(env.subst("$PROJECT_DIR"), "version.txt")  # noqa: F821
ninja_file = os.path.join(env.subst("$BUILD_DIR"), "build.ninja")  # noqa: F821

if (os.path.isfile(version_file) and os.path.isfile(ninja_file)
        and os.path.getmtime(version_file) > os.path.getmtime(ninja_file)):
    print("version.txt changed: configuring ESP-IDF again")
    os.remove(ninja_file)
