cmake_minimum_required(VERSION 3.16)
project(digital-transceiver LANGUAGES C CXX)

# The radio app for the Raspberry Pi. Qt Core and Network only: there is no GUI.
# It also builds on a Linux PC, for testing with --sim.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Network)
find_package(Threads REQUIRED)

# --- mbelib (AMBE / IMBE decoders), vendored -------------------------------
set(MBE_DIR ${CMAKE_SOURCE_DIR}/third_party/mbelib)
add_library(mbe STATIC
  ${MBE_DIR}/ambe3600x2400.c ${MBE_DIR}/ambe3600x2450.c ${MBE_DIR}/ecc.c
  ${MBE_DIR}/imbe7100x4400.c ${MBE_DIR}/imbe7200x4400.c ${MBE_DIR}/mbelib.c)
target_include_directories(mbe PUBLIC ${MBE_DIR})
target_compile_options(mbe PRIVATE -w)

# --- OP25 AMBE / IMBE encoders (GPLv3), for transmitting ---------------------
# OP25 carries its own modified mbelib, so everything except the op25voc_* wrapper is linked
# into one object and made local, keeping it apart from third_party/mbelib.
set(VOC_DIR ${CMAKE_SOURCE_DIR}/third_party/op25_vocoder)
file(GLOB VOC_IMBE ${VOC_DIR}/imbe_vocoder/*.cc)
add_library(op25voc_objs OBJECT
  ${VOC_DIR}/op25voc.cc ${VOC_DIR}/ambe_encoder.cc ${VOC_DIR}/ambe.c ${VOC_DIR}/mbelib.c
  ${VOC_DIR}/p25p2_vf.cc ${VOC_DIR}/rs.cc ${VOC_IMBE})
target_include_directories(op25voc_objs PRIVATE ${VOC_DIR} ${VOC_DIR}/imbe_vocoder)
target_compile_options(op25voc_objs PRIVATE -w)
set(VOC_PRIVATE ${CMAKE_BINARY_DIR}/op25voc_private.o)
file(WRITE ${CMAKE_BINARY_DIR}/op25voc_keep.txt
  "op25voc_ambe_new\nop25voc_ambe_encode\nop25voc_ambe_free\nop25voc_imbe_new\nop25voc_imbe_encode\nop25voc_imbe_free\n")
add_custom_command(OUTPUT ${VOC_PRIVATE}
  COMMAND ${CMAKE_LINKER} -r -o ${CMAKE_BINARY_DIR}/op25voc_all.o $<TARGET_OBJECTS:op25voc_objs>
  COMMAND ${CMAKE_OBJCOPY} --keep-global-symbols=${CMAKE_BINARY_DIR}/op25voc_keep.txt
          ${CMAKE_BINARY_DIR}/op25voc_all.o ${VOC_PRIVATE}
  DEPENDS op25voc_objs $<TARGET_OBJECTS:op25voc_objs>
  COMMAND_EXPAND_LISTS
  COMMENT "Linking OP25 vocoder with private symbols")
set_source_files_properties(${VOC_PRIVATE} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

# --- the radio -------------------------------------------------------------
add_executable(digital-transceiver
  src/main.cpp
  # the radio: networks, decoding, calls, transmit
  src/Radio.cpp src/Radio.h src/RadioNet.cpp
  src/Networks.cpp src/Networks.h src/Settings.cpp src/Settings.h
  src/DmrDecoder.cpp src/DmrDecoder.h src/VoiceEncoder.cpp src/VoiceEncoder.h
  src/AudioIn.cpp src/AudioIn.h src/AudioOut.cpp src/AudioOut.h
  src/AmbeChip.cpp src/AmbeChip.h src/AmbeSelfTest.cpp
  # front panel: display, controls and the hardware that provides them
  src/FrontPanel.cpp src/FrontPanel.h src/LcdPlate.cpp src/LcdPlate.h
  src/Lcd2004.cpp src/Lcd2004.h src/GpioPanel.cpp src/GpioPanel.h src/LedPanel.cpp src/LedPanel.h
  src/Gpio.cpp src/Gpio.h src/gpio_v2.h
  src/HeadsetButtons.cpp src/HeadsetButtons.h src/SimPanel.cpp src/SimPanel.h
  ${VOC_PRIVATE})
target_include_directories(digital-transceiver PRIVATE src)
target_link_libraries(digital-transceiver PRIVATE Qt6::Core Qt6::Network mbe m ${CMAKE_DL_LIBS} Threads::Threads)

# --- Codec2 (M17 voice) ------------------------------------------------------
find_path(CODEC2_INCLUDE_DIR codec2/codec2.h)
find_library(CODEC2_LIBRARY codec2)
if(CODEC2_INCLUDE_DIR AND CODEC2_LIBRARY)
  message(STATUS "Codec2 found: M17 audio enabled")
  target_compile_definitions(digital-transceiver PRIVATE HAVE_CODEC2)
  target_include_directories(digital-transceiver PRIVATE ${CODEC2_INCLUDE_DIR})
  target_link_libraries(digital-transceiver PRIVATE ${CODEC2_LIBRARY})
else()
  message(STATUS "Codec2 not found: M17 calls are logged without audio")
endif()

install(TARGETS digital-transceiver RUNTIME DESTINATION bin)
