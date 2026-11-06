# DMAP-1a: bounds GLFW 3.4's DirectInput object offsets to the DIJOYSTATE the
# device is read into, and fixes the signed-char GUID text. Idempotent. Apply
# to a build-private copy; shared FETCHCONTENT_SOURCE_DIR_GLFW stays read-only.
if(NOT DEFINED PELICAN_GLFW_SOURCE_DIR)
  message(FATAL_ERROR "PELICAN_GLFW_SOURCE_DIR is required")
endif()
set(target_file "${PELICAN_GLFW_SOURCE_DIR}/src/win32_joystick.c")
if(NOT EXISTS "${target_file}")
  message(FATAL_ERROR "GLFW win32_joystick.c was not found: ${target_file}")
endif()
file(READ "${target_file}" contents)
string(REPLACE "\r\n" "\n" contents "${contents}")
string(SHA256 source_hash "${contents}")
set(patched_hash "dc07fd5eb01bfed66d53341f3c5cf491f3cf299cdeeb92664d984bf02b93f38f")
if(source_hash STREQUAL patched_hash)
  return()
endif()
set(pristine_hash "b9c95eed4eeed6eb64864fa031349de83e3421ab7a6c3dcd86cf80fd6b1ad8b1")
if(NOT source_hash STREQUAL pristine_hash)
  message(FATAL_ERROR
    "GLFW win32_joystick.c has an unexpected, unknown, or partially patched revision "
    "(SHA256 ${source_hash}); review the patch before updating the dependency")
endif()

set(old_1 [=[// Data produced with DirectInput device object enumeration
//
typedef struct _GLFWobjenumWin32
{
    IDirectInputDevice8W*   device;
    _GLFWjoyobjectWin32*    objects;
    int                     objectCount;]=])
set(new_1 [=[// Altered by Pelican (DMAP-1a): DirectInput object bounds and GUID text.
// This is not the original GLFW 3.4 source.

// Data produced with DirectInput device object enumeration
//
typedef struct _GLFWobjenumWin32
{
    IDirectInputDevice8W*   device;
    _GLFWjoyobjectWin32*    objects;
    int                     objectCount;
    int                     objectCapacity;]=])

set(old_2 [=[    _GLFWobjenumWin32* data = user;
    _GLFWjoyobjectWin32* object = data->objects + data->objectCount;
]=])
set(new_2 [=[    _GLFWobjenumWin32* data = user;
    _GLFWjoyobjectWin32* object;

    // Pelican: check before forming a pointer outside the allocation.
    if (!data || !data->objects || data->objectCount < 0 ||
        data->objectCapacity <= 0 || data->objectCount >= data->objectCapacity)
        return DIENUM_STOP;
    object = data->objects + data->objectCount;
]=])

set(old_3 [=[        if (memcmp(&doi->guidType, &GUID_Slider, sizeof(GUID)) == 0)
            object->offset = DIJOFS_SLIDER(data->sliderCount);
        else if (memcmp(&doi->guidType, &GUID_XAxis, sizeof(GUID)) == 0)]=])
set(new_3 [=[        if (memcmp(&doi->guidType, &GUID_Slider, sizeof(GUID)) == 0)
        {
            // Pelican: DIJOYSTATE holds 2 sliders.
            if (data->sliderCount >= 2)
                return DIENUM_CONTINUE;
            object->offset = DIJOFS_SLIDER(data->sliderCount);
        }
        else if (memcmp(&doi->guidType, &GUID_XAxis, sizeof(GUID)) == 0)]=])

set(old_4 [=[        object->offset = DIJOFS_BUTTON(data->buttonCount);]=])
set(new_4 [=[        // Pelican: DIJOYSTATE holds 32 buttons.
        if (data->buttonCount >= 32)
            return DIENUM_CONTINUE;
        object->offset = DIJOFS_BUTTON(data->buttonCount);]=])

set(old_5 [=[        object->offset = DIJOFS_POV(data->povCount);]=])
set(new_5 [=[        // Pelican: DIJOYSTATE holds 4 POVs.
        if (data->povCount >= 4)
            return DIENUM_CONTINUE;
        object->offset = DIJOFS_POV(data->povCount);]=])

set(old_6 [=[    data->objectCount++;
    return DIENUM_CONTINUE;]=])
set(new_6 [=[    else
    {
        // Pelican: an unrecognized object must not consume an array entry.
        return DIENUM_CONTINUE;
    }

    data->objectCount++;
    return DIENUM_CONTINUE;]=])

set(old_7 [=[    memset(&data, 0, sizeof(data));
    data.device = device;
    data.objects = _glfw_calloc(dc.dwAxes + (size_t) dc.dwButtons + dc.dwPOVs,
                                sizeof(_GLFWjoyobjectWin32));
]=])
set(new_7 [=[    memset(&data, 0, sizeof(data));
    data.device = device;
    // Pelican: cap before addition, conversion to int, or allocation.
    if (dc.dwAxes > 44 || dc.dwButtons > 44 || dc.dwPOVs > 44)
    {
        IDirectInputDevice8_Release(device);
        return DIENUM_CONTINUE;
    }
    {
        const size_t advertisedCount = (size_t) dc.dwAxes +
                                       (size_t) dc.dwButtons +
                                       (size_t) dc.dwPOVs;
        const size_t objectCapacity = advertisedCount < 44 ? advertisedCount : 44;
        data.objectCapacity = (int) objectCapacity;
        data.objects = _glfw_calloc(objectCapacity ? objectCapacity : 1,
                                    sizeof(_GLFWjoyobjectWin32));
    }
]=])

set(old_8 [=[    char guid[33];
    char name[256];]=])
set(new_8 [=[    char guid[33];
    char name[256] = {0};]=])

set(old_9 [=[                name[0], name[1], name[2], name[3],
                name[4], name[5], name[6], name[7],
                name[8], name[9], name[10]);]=])
set(new_9 [=[                (uint8_t) name[0], (uint8_t) name[1], (uint8_t) name[2], (uint8_t) name[3],
                (uint8_t) name[4], (uint8_t) name[5], (uint8_t) name[6], (uint8_t) name[7],
                (uint8_t) name[8], (uint8_t) name[9], (uint8_t) name[10]);]=])

foreach(index RANGE 1 9)
  string(FIND "${contents}" "${old_${index}}" anchor)
  if(anchor EQUAL -1)
    message(FATAL_ERROR
      "GLFW win32_joystick.c anchor ${index} changed; "
      "review the patch before updating the dependency")
  endif()
  string(REPLACE "${old_${index}}" "${new_${index}}" contents "${contents}")
endforeach()
string(SHA256 result_hash "${contents}")
if(NOT result_hash STREQUAL patched_hash)
  message(FATAL_ERROR
    "GLFW patch produced unreviewed bytes (SHA256 ${result_hash}); review the patch")
endif()
file(WRITE "${target_file}" "${contents}")
