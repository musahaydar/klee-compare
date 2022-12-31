add_executable(klee-compare
  main.cpp
)

set(KLEE_LIBS
  kleeCore
)

target_link_libraries(klee-compare ${KLEE_LIBS})

install(TARGETS klee-compare RUNTIME DESTINATION bin)

# add_dependencies(klee-compare BuildKLEERuntimes)
