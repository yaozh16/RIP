cmake_minimum_required (VERSION 3.1)

project (RIP C)




find_package(Threads REQUIRED)
if(THREADS_HAVE_PTHREAD_ARG)
    set_property(TARGET my_app PROPERTY COMPILE_OPTIONS "-pthread")
    set_property(TARGET my_app PROPERTY INTERFACE_COMPILE_OPTIONS "-pthread")
endif()




add_executable(Test rip.c rip.h)
if(CMAKE_THREAD_LIBS_INIT)
    target_link_libraries(Test "${CMAKE_THREAD_LIBS_INIT}")
endif()