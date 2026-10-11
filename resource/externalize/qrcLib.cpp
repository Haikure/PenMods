// The generated qrc header is model-specific. The target's on_config injects
// PL_BUILD_<platform> (see xmake.lua), so this picks the right tree per build.
// Generate with scripts/gen_qt_res.sh <qrc-source-dir> resource/models/<platform>.
#if PL_BUILD_YDP02X
#include "../models/YDP02X/qrc_qml.h"
#elif PL_BUILD_YDP03X
#include "../models/YDP03X/qrc_qml.h"
#endif

extern "C" {
const char* get_penmods_build_platform() {
#if PL_BUILD_YDP03X
    return "PenMods.Platform=YDP03X";
#elif PL_BUILD_YDP02X
    return "PenMods.Platform=YDP02X";
#else
    return "PenMods.Platform=YDPG3";
#endif
}
const unsigned char* get_qt_resource_struct() { return qt_resource_struct; };
const unsigned char* get_qt_resource_data() { return qt_resource_data; };
const unsigned char* get_qt_resource_name() { return qt_resource_name; };
}
