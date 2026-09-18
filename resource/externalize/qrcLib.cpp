// The generated qrc header is model-specific. The target's on_config injects
// PL_BUILD_<platform> (see xmake.lua), so this picks the right tree per build.
// Note: YDP03X resource generation is a follow-up task; the YDP03X header is
// expected to be produced by scripts/gen_qt_res.sh YDP03X before this target is built.
#if PL_BUILD_YDP02X
#include "../models/YDP02X/qrc_qml.h"
#elif PL_BUILD_YDP03X
#include "../models/YDP03X/qrc_qml.h"
#endif

extern "C" {
const unsigned char* get_qt_resource_struct(){
    return qt_resource_struct;
};
const unsigned char* get_qt_resource_data(){
    return qt_resource_data;
};
const unsigned char* get_qt_resource_name(){
    return qt_resource_name;
};
}
