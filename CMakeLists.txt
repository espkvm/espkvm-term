cmake_minimum_required(VERSION 3.22)
# Tailscale: microlink, from the firmware checkout next to this one (the same
# copy the firmware builds).
set(EXTRA_COMPONENT_DIRS
    ../espkvm/third_party/microlink/components/microlink
    ../espkvm/third_party/microlink/components/microlink/components)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(espkvm-term)
