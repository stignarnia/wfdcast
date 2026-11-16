# SPDX-FileCopyrightText: 2026 stignarnia
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.22)
project(wfdcast VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(ECM 6.0 REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(KDEInstallDirs)
include(KDECMakeSettings)

find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Widgets Qml Quick QuickControls2 DBus Network)
find_package(KF6 6.0 REQUIRED COMPONENTS Kirigami I18n CoreAddons Config)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-1.0)
find_package(Qt6Keychain REQUIRED) # TV passphrase in KWallet (Secret Service elsewhere)

qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

# Where the privileged helper lives; the polkit policy must name the same path.
set(WFDCAST_HELPER_DIR "${CMAKE_INSTALL_PREFIX}/lib/wfdcast")
set(WFDCAST_HELPER_PATH "${WFDCAST_HELPER_DIR}/wfdcast-helper.py")

qt_add_executable(wfdcast
    src/main.cpp
    src/settings.cpp src/settings.h
    src/portal.cpp src/portal.h
    src/audiorouter.cpp src/audiorouter.h
    src/pipeline.cpp src/pipeline.h
    src/wfdserver.cpp src/wfdserver.h
    src/castcontroller.cpp src/castcontroller.h
)
qt_add_qml_module(wfdcast
    URI io.github.stignarnia.wfdcast
    VERSION 1.0
    QML_FILES src/qml/Main.qml
    NO_IMPORT_SCAN
)
target_compile_definitions(wfdcast PRIVATE
    QT_NO_KEYWORDS
    WFDCAST_VERSION="${PROJECT_VERSION}"
    WFDCAST_HELPER_PATH="${WFDCAST_HELPER_PATH}"
)
target_link_libraries(wfdcast PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::DBus Qt6::Network
    KF6::I18n KF6::I18nQml KF6::CoreAddons KF6::ConfigCore
    PkgConfig::GST
    qt6keychain
)

install(TARGETS wfdcast ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
install(PROGRAMS helper/wfdcast-helper.py DESTINATION ${WFDCAST_HELPER_DIR})
install(FILES data/io.github.stignarnia.wfdcast.desktop DESTINATION ${KDE_INSTALL_APPDIR})

# polkit only reads actions from /usr/share, whatever the install prefix.
configure_file(data/io.github.stignarnia.wfdcast.policy.in
               ${CMAKE_CURRENT_BINARY_DIR}/io.github.stignarnia.wfdcast.policy @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/io.github.stignarnia.wfdcast.policy
        DESTINATION /usr/share/polkit-1/actions)
