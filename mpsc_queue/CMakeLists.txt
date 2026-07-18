cmake_minimum_required(VERSION 3.16)
project(hermes_shm CXX)

# Enable MPI and C++17
find_package(MPI REQUIRED)
find_package(HermesShm CONFIG REQUIRED)

# message("IN DEBUG MODE")
#         set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O0")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(mpsc_queue_timed
test_init.cc
mpsc_queue_timed.cc)


add_executable(boolean_flip_timed
test_init.cc
boolean_flip_timed.cc)


add_executable(mpsc_queue_bool_timed
test_init.cc
mpsc_queue_bool_timed.cc)

add_executable(mpsc_queue_final
test_init.cc
mpsc_queue_final.cc)

add_executable(mpsc_queue_t 
test_init.cc 
mpsc_queue_tagged.cc)

add_executable(mpsc_queue_future
test_init.cc
mpsc_queue_future.cc)

add_executable(mpsc_queue_f
test_init.cc
mpsc_queue_f.cc)

add_executable(mpsc_queue_ptr_chase
test_init.cc
mpsc_queue_ptr_chase.cc)

target_link_libraries(mpsc_queue_timed
  hshm::cxx
  MPI::MPI_CXX
)

target_link_libraries(boolean_flip_timed
  hshm::cxx
  MPI::MPI_CXX
)

target_link_libraries(mpsc_queue_bool_timed
  hshm::cxx
  MPI::MPI_CXX
)

target_link_libraries(mpsc_queue_final
  hshm::cxx
  MPI::MPI_CXX
)

target_link_libraries(mpsc_queue_t
  hshm::cxx
  MPI::MPI_CXX
)

target_link_libraries(mpsc_queue_future
  hshm::cxx
  MPI::MPI_CXX
)

target_link_libraries(mpsc_queue_f
  hshm::cxx
  MPI::MPI_CXX
)

target_link_libraries(mpsc_queue_ptr_chase
  hshm::cxx
  MPI::MPI_CXX
)