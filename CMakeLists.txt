CMAKE_MINIMUM_REQUIRED(VERSION 3.10)
PROJECT(Radio)

SET(CMAKE_CXX_STANDARD 14)

FIND_PACKAGE(Boost 1.40 REQUIRED COMPONENTS program_options)
INCLUDE_DIRECTORIES(${Boost_INCLUDE_DIR})

ADD_EXECUTABLE(sikradio-sender radio_transmitter.cpp audiogram.h audio_transmitter.h const.h transmitter.h receiver.h)
ADD_EXECUTABLE(sikradio-receiver radio_receiver.cpp audiogram.h audio_transmitter.h transmitter.h receiver.h)

TARGET_LINK_LIBRARIES(sikradio-sender LINK_PUBLIC ${Boost_LIBRARIES})
TARGET_LINK_LIBRARIES(sikradio-receiver LINK_PUBLIC ${Boost_LIBRARIES})
