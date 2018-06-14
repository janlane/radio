CMAKE_MINIMUM_REQUIRED(VERSION 3.5.1)
PROJECT(Radio)

SET(CMAKE_CXX_STANDARD 14)

FIND_PACKAGE(Boost 1.40 REQUIRED COMPONENTS program_options)
FIND_PACKAGE(Threads)

ADD_EXECUTABLE(sikradio-sender radio_transmitter.cpp audiogram.h audio_transmitter.h const.h transmitter.h receiver.h)
ADD_EXECUTABLE(sikradio-receiver radio_receiver.cpp audiogram.h transmitter.h receiver.h)
ADD_EXECUTABLE(next-receiver menu.cpp menu.h const.h err.h err.cpp radio_receiver.cpp audiogram.h transmitter.h receiver.h)

TARGET_LINK_LIBRARIES(sikradio-sender LINK_PUBLIC ${Boost_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})
TARGET_LINK_LIBRARIES(sikradio-receiver LINK_PUBLIC ${Boost_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})
TARGET_LINK_LIBRARIES(next-receiver LINK_PUBLIC ${Boost_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})