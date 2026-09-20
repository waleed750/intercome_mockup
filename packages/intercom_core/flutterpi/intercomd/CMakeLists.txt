cmake_minimum_required(VERSION 3.16)
project(syncn-intercom C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes)

# Sanitizers for the test build. The frame parser walks a byte buffer by hand,
# so ASan and UBSan earn their keep here.
option(SYNCN_SANITIZE "Build tests with ASan and UBSan" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CJSON REQUIRED IMPORTED_TARGET libcjson)

# ------------------------------------------------------------ protocol lib ---
# Deliberately free of any hardware, socket or threading dependency, so it
# builds and tests on any machine — including a laptop with no panel attached.
add_library(syncn_protocol STATIC
    src/protocol/frame.c
    src/protocol/alaw.c
    src/protocol/command.c
    src/protocol/discovery.c
    src/audio/jitter.c
    src/video/h264.c
    src/audio/dsp.c
    src/video/dmabuf.c
    src/video/framepool.c
    src/net/ipc.c
    src/util/util.c
    src/audio/ringer_synth.c
)
target_include_directories(syncn_protocol PUBLIC include)
target_link_libraries(syncn_protocol PUBLIC PkgConfig::CJSON m)

# ------------------------------------------------------------- daemon --------
# Needs real devices, so it is optional: the protocol library and simulator
# still build on a machine with no ALSA headers.
pkg_check_modules(ALSA IMPORTED_TARGET alsa)
pkg_check_modules(SPEEXDSP IMPORTED_TARGET speexdsp)

if(ALSA_FOUND)
    add_executable(syncn-intercomd
        src/main.c
        src/core/call.c
        src/core/config.c
        src/net/net.c
        src/audio/audio.c
        src/audio/aec.c
        src/audio/ringer.c
    )
    target_link_libraries(syncn-intercomd PRIVATE syncn_protocol PkgConfig::ALSA m pthread)
    target_compile_definitions(syncn-intercomd PRIVATE _GNU_SOURCE SYNCN_HAVE_ALSA)

    # Rockchip MPP: the only hardware decoder on this SoC. There is no V4L2
    # stateful decoder on the panel — every /dev/video* belongs to the camera.
    pkg_check_modules(ROCKCHIP_MPP IMPORTED_TARGET rockchip_mpp)
    target_sources(syncn-intercomd PRIVATE src/video/decoder_mpp.c src/video/video.c)
    if(ROCKCHIP_MPP_FOUND)
        target_link_libraries(syncn-intercomd PRIVATE PkgConfig::ROCKCHIP_MPP)
        target_compile_definitions(syncn-intercomd PRIVATE SYNCN_HAVE_MPP)
        message(STATUS "hardware video decode: rockchip mpp ${ROCKCHIP_MPP_VERSION}")
    else()
        message(WARNING "librockchip_mpp not found — the daemon will run and "
                        "receive video, but cannot display it. Run "
                        "scripts/provision.sh --only mpp on the panel.")
    endif()

    # RGA: the 2D block that turns the decoder's NV12 into the RGB the only
    # usable display plane accepts. Separate silicon from the GPU, so the video
    # path still never touches the Mali blob.
    pkg_check_modules(LIBRGA IMPORTED_TARGET librga)
    target_sources(syncn-intercomd PRIVATE src/video/rga.c)
    if(LIBRGA_FOUND)
        target_link_libraries(syncn-intercomd PRIVATE PkgConfig::LIBRGA)
        target_compile_definitions(syncn-intercomd PRIVATE SYNCN_HAVE_RGA)
        message(STATUS "colour conversion: rockchip rga ${LIBRGA_VERSION}")
    else()
        message(WARNING "librga not found — decoded video cannot be converted "
                        "for the display plane. It ships with the MPP build; "
                        "run scripts/provision.sh --only mpp on the panel.")
    endif()

    target_sources(syncn-intercomd PRIVATE src/audio/denoise.c)
    if(SPEEXDSP_FOUND)
        target_link_libraries(syncn-intercomd PRIVATE PkgConfig::SPEEXDSP syncn_rnnoise)
        target_compile_definitions(syncn-intercomd PRIVATE SYNCN_HAVE_SPEEXDSP SYNCN_HAVE_RNNOISE)
        message(STATUS "echo cancellation: speexdsp; denoiser: rnnoise")
    else()
        message(WARNING "speexdsp not found — building WITHOUT echo cancellation. "
                        "The far end will hear themselves unless duplex_mode=half.")
    endif()
else()
    message(WARNING "ALSA not found — skipping syncn-intercomd. "
                    "The protocol library and door-sim still build.")
endif()

# ------------------------------------------------------------- rnnoise ------
# Vendored, unmodified, built quietly: it is not our code to warn about. Its
# resampling wrapper needs speexdsp, so the library only matters where that
# is found.
if(SPEEXDSP_FOUND)
    add_library(syncn_rnnoise STATIC
        third_party/rnnoise/src/celt_lpc.c
        third_party/rnnoise/src/denoise.c
        third_party/rnnoise/src/kiss_fft.c
        third_party/rnnoise/src/pitch.c
        third_party/rnnoise/src/rnn.c
        third_party/rnnoise/src/rnn_data.c
        third_party/rnnoise/src/rnn_reader.c
    )
    target_include_directories(syncn_rnnoise PUBLIC third_party/rnnoise/include
                                             PRIVATE third_party/rnnoise/src)
    # -O3 and fast math: the network is dense float arithmetic, and at -O2
    # two of them plus resamplers missed the 20 ms deadline on the panel.
    target_compile_options(syncn_rnnoise PRIVATE -w -O3 -ffast-math -DFLOAT_APPROX)
    target_link_libraries(syncn_rnnoise PUBLIC m)
endif()

# ------------------------------------------------------------- door-sim ------
add_executable(door-sim tools/door_sim.c)
target_link_libraries(door-sim PRIVATE syncn_protocol m)
target_compile_definitions(door-sim PRIVATE _GNU_SOURCE)

add_executable(h264-info tools/h264_info.c)
target_link_libraries(h264-info PRIVATE syncn_protocol)

# ---------------------------------------------------------------- tests ------
enable_testing()

add_executable(syncn_tests
    tests/main.c
    tests/test_frame.c
    tests/test_alaw.c
    tests/test_command.c
    tests/test_discovery.c
    tests/test_jitter.c
    tests/test_h264.c
    tests/test_dsp.c
    tests/test_fdpass.c
    tests/test_identity.c
    tests/test_framepool.c
    tests/test_limiter.c
    tests/test_gate.c
    tests/test_video_proto.c
    tests/test_ringer.c
)
target_include_directories(syncn_tests PRIVATE tests)
target_compile_definitions(syncn_tests PRIVATE _GNU_SOURCE)
target_link_libraries(syncn_tests PRIVATE syncn_protocol)

if(SYNCN_SANITIZE)
    target_compile_options(syncn_tests PRIVATE -fsanitize=address,undefined -g)
    target_link_options(syncn_tests PRIVATE -fsanitize=address,undefined)
endif()

add_test(NAME unit COMMAND syncn_tests)

# The video pipeline, tested on any machine.
#
# A second binary rather than another suite in the first, because it links fake
# Rockchip hardware and a fake DMA heap in place of the real ones. The
# substitution is at link time: src/video/video.c and src/video/framepool.c are
# the shipping sources with no conditional compilation and no test-only branch.
# That is the whole point — the rules these tests enforce (never block on the
# consumer, always give the decoder its frame back) are the rules that decide
# whether the picture freezes on the panel.
add_executable(syncn_video_tests
    tests/video_main.c
    tests/test_video.c
    tests/fake_video_hw.c
    tests/fake_dmabuf.c
    src/video/video.c
    src/video/framepool.c
    src/video/h264.c
    src/util/util.c
)
target_include_directories(syncn_video_tests PRIVATE tests include)
target_compile_definitions(syncn_video_tests PRIVATE _GNU_SOURCE)

if(SYNCN_SANITIZE)
    target_compile_options(syncn_video_tests PRIVATE -fsanitize=address,undefined -g)
    target_link_options(syncn_video_tests PRIVATE -fsanitize=address,undefined)
endif()

add_test(NAME video COMMAND syncn_video_tests)

# The echo canceller, offline. Links speexdsp, so it only exists where the
# daemon's cancellation does; the protocol library must build without it.
if(SPEEXDSP_FOUND)
    add_executable(syncn_aec_tests
        tests/aec_main.c
        tests/test_return.c
        tests/test_denoise.c
        src/audio/aec.c
        src/audio/denoise.c
        src/util/util.c
    )
    target_include_directories(syncn_aec_tests PRIVATE tests include)
    target_compile_definitions(syncn_aec_tests PRIVATE _GNU_SOURCE SYNCN_HAVE_SPEEXDSP SYNCN_HAVE_RNNOISE)
    target_link_libraries(syncn_aec_tests PRIVATE PkgConfig::SPEEXDSP syncn_rnnoise m)
    add_test(NAME aec COMMAND syncn_aec_tests)
endif()

find_program(PYTHON3 python3)
if(PYTHON3)
    add_test(NAME integration
             COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/integration/call_flow.py
                     $<TARGET_FILE:door-sim>)
endif()
