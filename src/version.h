// version.h - wam's version, set here and nowhere else. wam.exe's file properties (res\version.rc),
// the About line in Settings and the user agent sent to addon sites all read it,
// and tools\release.ps1 refuses to release a version that doesn't match it. Macros only: the
// resource compiler includes it too.

#define WAM_VERSION_MAJOR 1
#define WAM_VERSION_MINOR 5
#define WAM_VERSION_PATCH 0
#define WAM_VERSION       "1.5.0" // the three numbers above (release.ps1 checks they agree)

#define WAM_PUBLIC_URL    "https://github.com/davidsama/Wam" // releases and issues (Settings > About)
