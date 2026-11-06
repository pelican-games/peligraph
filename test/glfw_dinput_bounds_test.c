// DMAP-1a: GLFW's DirectInput enumeration code, cut from the patched source by
// test/CMakeLists.txt, run against a fake of the Windows types.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int BOOL;
typedef int32_t LONG;
typedef uint32_t DWORD;
typedef uint8_t BYTE;
typedef struct GUID { uint32_t Data1; uint16_t Data2; uint16_t Data3; uint8_t Data4[8]; } GUID;
typedef struct IDirectInputDevice8W IDirectInputDevice8W;
typedef struct DIDEVICEOBJECTINSTANCEW { GUID guidType; DWORD dwType; } DIDEVICEOBJECTINSTANCEW;
typedef struct DIDEVICEINSTANCE { GUID guidProduct; } DIDEVICEINSTANCE;
typedef struct DIPROPHEADER { DWORD dwSize, dwHeaderSize, dwObj, dwHow; } DIPROPHEADER;
typedef struct DIPROPRANGE { DIPROPHEADER diph; LONG lMin, lMax; } DIPROPRANGE;
typedef struct DIJOYSTATE {
    LONG lX, lY, lZ, lRx, lRy, lRz;
    LONG rglSlider[2];
    DWORD rgdwPOV[4];
    BYTE rgbButtons[32];
} DIJOYSTATE;
typedef struct _GLFWjoyobjectWin32 { int offset; int type; } _GLFWjoyobjectWin32;

#define CALLBACK
#define DIENUM_STOP 0
#define DIENUM_CONTINUE 1
#define DIDFT_AXIS 0x03u
#define DIDFT_BUTTON 0x0Cu
#define DIDFT_POV 0x10u
#define DIDFT_GETTYPE(n) ((n) & 0xFFu)
#define DIPH_BYID 2
#define DIPROP_RANGE 0
#define FAILED(hr) ((hr) < 0)
#define ZeroMemory(p, n) memset((p), 0, (n))
#define IDirectInputDevice8_SetProperty(device, property, header) \
    ((void) (device), (void) (property), (void) (header), 0)
#define DIJOFS_X ((int) offsetof(DIJOYSTATE, lX))
#define DIJOFS_Y ((int) offsetof(DIJOYSTATE, lY))
#define DIJOFS_Z ((int) offsetof(DIJOYSTATE, lZ))
#define DIJOFS_RX ((int) offsetof(DIJOYSTATE, lRx))
#define DIJOFS_RY ((int) offsetof(DIJOYSTATE, lRy))
#define DIJOFS_RZ ((int) offsetof(DIJOYSTATE, lRz))
#define DIJOFS_SLIDER(n) ((int) (offsetof(DIJOYSTATE, rglSlider) + (n) * sizeof(LONG)))
#define DIJOFS_POV(n) ((int) (offsetof(DIJOYSTATE, rgdwPOV) + (n) * sizeof(DWORD)))
#define DIJOFS_BUTTON(n) ((int) (offsetof(DIJOYSTATE, rgbButtons) + (n)))
#define _GLFW_TYPE_AXIS 0
#define _GLFW_TYPE_SLIDER 1
#define _GLFW_TYPE_BUTTON 2
#define _GLFW_TYPE_POV 3

static const GUID GUID_XAxis = {1, 0, 0, {0}};
static const GUID GUID_YAxis = {2, 0, 0, {0}};
static const GUID GUID_ZAxis = {3, 0, 0, {0}};
static const GUID GUID_RxAxis = {4, 0, 0, {0}};
static const GUID GUID_RyAxis = {5, 0, 0, {0}};
static const GUID GUID_RzAxis = {6, 0, 0, {0}};
static const GUID GUID_Slider = {7, 0, 0, {0}};
static const GUID GUID_None = {8, 0, 0, {0}};

#include "enum_struct.inc"
#include "object_callback.inc"

static void makeGuid(const DIDEVICEINSTANCE* di, char* name, char* guid)
{
#include "guid_text.inc"
}

static int failures = 0;
#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);   \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

static BOOL feed(_GLFWobjenumWin32* data, const GUID* type, DWORD kind)
{
    DIDEVICEOBJECTINSTANCEW doi;
    memset(&doi, 0, sizeof(doi));
    doi.guidType = *type;
    doi.dwType = kind;
    return deviceObjectCallback(&doi, data);
}

static void feedAxes(_GLFWobjenumWin32* data)
{
    feed(data, &GUID_XAxis, DIDFT_AXIS);
    feed(data, &GUID_YAxis, DIDFT_AXIS);
    feed(data, &GUID_ZAxis, DIDFT_AXIS);
    feed(data, &GUID_RxAxis, DIDFT_AXIS);
    feed(data, &GUID_RyAxis, DIDFT_AXIS);
    feed(data, &GUID_RzAxis, DIDFT_AXIS);
}

static void checkOffsetsInsideState(const _GLFWobjenumWin32* data)
{
    const int pov = (int) offsetof(DIJOYSTATE, rgdwPOV);
    const int buttons = (int) offsetof(DIJOYSTATE, rgbButtons);
    for (int i = 0; i < data->objectCount; i++) {
        const _GLFWjoyobjectWin32* object = data->objects + i;
        switch (object->type) {
        case _GLFW_TYPE_AXIS:
        case _GLFW_TYPE_SLIDER:
            CHECK(object->offset >= 0 && object->offset + (int) sizeof(LONG) <= pov);
            break;
        case _GLFW_TYPE_POV:
            CHECK(object->offset >= pov && object->offset + (int) sizeof(DWORD) <= buttons);
            break;
        case _GLFW_TYPE_BUTTON:
            CHECK(object->offset >= buttons && object->offset + 1 <= (int) sizeof(DIJOYSTATE));
            break;
        default:
            CHECK(!"unknown object type");
        }
    }
}

static void testOverLimits(void)
{
    static _GLFWjoyobjectWin32 objects[256];
    _GLFWobjenumWin32 data;
    memset(&data, 0, sizeof(data));
    data.objects = objects;
    data.objectCapacity = 256;
    feedAxes(&data);
    for (int i = 0; i < 16; i++) feed(&data, &GUID_Slider, DIDFT_AXIS);
    for (int i = 0; i < 16; i++) feed(&data, &GUID_None, DIDFT_POV);
    for (int i = 0; i < 128; i++) feed(&data, &GUID_None, DIDFT_BUTTON);
    CHECK(data.axisCount == 6);
    CHECK(data.sliderCount == 2);
    CHECK(data.povCount == 4);
    CHECK(data.buttonCount == 32);
    CHECK(data.objectCount == 44);
    checkOffsetsInsideState(&data);
}

static void testAtLimitsUnchanged(void)
{
    static _GLFWjoyobjectWin32 objects[64];
    _GLFWobjenumWin32 data;
    memset(&data, 0, sizeof(data));
    data.objects = objects;
    data.objectCapacity = 64;
    feedAxes(&data);
    for (int i = 0; i < 2; i++) feed(&data, &GUID_Slider, DIDFT_AXIS);
    for (int i = 0; i < 4; i++) feed(&data, &GUID_None, DIDFT_POV);
    for (int i = 0; i < 32; i++) feed(&data, &GUID_None, DIDFT_BUTTON);
    CHECK(data.objectCount == 44);
    CHECK(objects[7].offset == DIJOFS_SLIDER(1));
    CHECK(objects[11].offset == DIJOFS_POV(3));
    CHECK(objects[43].offset == DIJOFS_BUTTON(31));
    checkOffsetsInsideState(&data);
}

static void testCapacityAndUnknownTypes(void)
{
    _GLFWjoyobjectWin32 objects[4];
    _GLFWobjenumWin32 data;
    memset(&data, 0, sizeof(data));
    objects[3].offset = -1;
    objects[3].type = -1;
    data.objects = objects;
    data.objectCapacity = 3;
    for (int i = 0; i < 5; i++) feed(&data, &GUID_None, DIDFT_BUTTON);
    CHECK(data.objectCount == 3);
    CHECK(objects[3].offset == -1 && objects[3].type == -1);

    memset(&data, 0, sizeof(data));
    CHECK(feed(&data, &GUID_None, DIDFT_BUTTON) == DIENUM_STOP);
    CHECK(data.objectCount == 0);

    memset(&data, 0, sizeof(data));
    data.objects = objects;
    data.objectCapacity = 4;
    CHECK(feed(&data, &GUID_None, 0) == DIENUM_CONTINUE);
    CHECK(data.objectCount == 0);
    CHECK(feed(&data, &GUID_XAxis, 0x20u) == DIENUM_CONTINUE);
    CHECK(data.objectCount == 0);
}

static void testGuidText(void)
{
    struct { char guid[33]; char sentinel[128]; } out;
    static const char japanese[] = "\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88\xE6\x9D\xBF";
    char name[256];
    DIDEVICEINSTANCE di;
    static const size_t lengths[] = {0, 1, 3, 10, 11};

    for (size_t length_index = 0; length_index < sizeof(lengths) / sizeof(lengths[0]); length_index++) {
        const size_t length = lengths[length_index];
        memset(name, 0, sizeof(name));
        memcpy(name, japanese, length);
        memset(&out, 0x5A, sizeof(out));
        memset(&di, 0, sizeof(di));
        makeGuid(&di, name, out.guid);
        for (size_t i = 0; i < sizeof(out.sentinel); i++) CHECK(out.sentinel[i] == 0x5A);
        CHECK(strlen(out.guid) == 32);
    }

    memset(name, 0, sizeof(name));
    memcpy(name, japanese, sizeof(japanese) - 1);
    memset(&out, 0x5A, sizeof(out));
    memset(&di, 0, sizeof(di));
    makeGuid(&di, name, out.guid);
    for (size_t i = 0; i < sizeof(out.sentinel); i++) CHECK(out.sentinel[i] == 0x5A);
    CHECK(strcmp(out.guid, "05000000e38386e382b9e38388e69d00") == 0);

    memset(&di, 0, sizeof(di));
    di.guidProduct.Data1 = 0x12345678u;
    memcpy(&di.guidProduct.Data4[2], "PIDVID", 6);
    makeGuid(&di, name, out.guid);
    CHECK(strcmp(out.guid, "03000000785600003412000000000000") == 0);
}

int main(void)
{
    testOverLimits();
    testAtLimitsUnchanged();
    testCapacityAndUnknownTypes();
    testGuidText();
    if (failures == 0) printf("glfw_dinput_bounds: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
