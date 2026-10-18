// The PS4 app this build is (CMake NX_PS4_APP, passed as NX_PS4_APP_NAME: SMW for Wonder, TOTK for Tears of the
// Kingdom): its folder in /data holds the settings, logs, saves and, unless the package carries it, the game data.
#pragma once

#ifndef NX_PS4_APP_NAME
#define NX_PS4_APP_NAME SMW
#endif
#define NX_PS4_STR2(x) #x
#define NX_PS4_STR(x) NX_PS4_STR2(x)
#define NX_PS4_APP NX_PS4_STR(NX_PS4_APP_NAME)
#define NX_PS4_DATA "/data/" NX_PS4_APP
