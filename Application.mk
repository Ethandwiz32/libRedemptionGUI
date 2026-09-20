# Meta Quest is always ARM64 — no 32-bit needed
APP_ABI          := arm64-v8a
APP_PLATFORM     := android-26
APP_STL          := c++_static
APP_OPTIM        := release
APP_CPPFLAGS     += -std=c++17
