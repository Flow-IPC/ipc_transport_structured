# Flow-IPC: Structured Transport
# Copyright (c) 2023 Akamai Technologies, Inc.; and other contributors.
# Each commit is copyright by its respective author or author's employer.
#
# Licensed under the MIT License:
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

set(NAME ipc_transport_structured_link_test.exec)

# This test requires a little silly capnp schema.
block()
  find_package(CapnProto CONFIG REQUIRED)

  # The capnp `import` search path needs to contain the lib$PROJ (libipc_transport_structured) src/;
  # we may be being compiled inside `make` (or equivalent) -- before any `make install`.
  # Just find it relative to us (we are in test/basic/link_test).
  set(CAPNPC_IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/../../../src)

  capnp_generate_cpp(capnp_generated_srcs capnp_generated_hdrs_ignored "schema.capnp")
  add_executable(${NAME} main.cpp ${capnp_generated_srcs})
endblock()

# So that schema.capnp.h is found by main.cpp's #include.
target_include_directories(${NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# Do stuff we've resolved to do on all our targets.
common_set_target_properties(${NAME})

# Link good ol' libipc_core.
target_link_libraries(${NAME} PRIVATE ${PROJ})

# Might as well export if they `make install` or equivalent.
install(TARGETS ${NAME}
        RUNTIME DESTINATION bin)
