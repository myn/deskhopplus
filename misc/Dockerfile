FROM alpine:3.21.2

# mtools so the config disk image can be rebuilt here too — without it CMake
# falls back to the committed image and a config UI change would not reach
# the firmware, which is not what "reproducible build" should mean.
RUN apk add --no-cache gcc-arm-none-eabi g++-arm-none-eabi build-base cmake python3 py3-jinja2 mtools
WORKDIR /deskhop
CMD ["sh", "-c", "cmake -S . -B build && cmake --build build"]
