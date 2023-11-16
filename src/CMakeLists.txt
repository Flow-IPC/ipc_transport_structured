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

# See FlowLikeLib.cmake per below; it'll explain inside.
# It mandates the following procedure and documents details.

# We just need executables to link ipc_core (and whatever it says it must link); and Cap'n Proto (further
# referred to as capnp).  Other stuff is transitively specified by the former, even though we do use some of that
# stuff directly too.

# Publicize it as a (global) cache setting for two reasons.  Primary: Our dependencies in meta-project mode
# will need it (so a global helps).  Secondary: This way user can override which could be helpful conceivably.
set(CAPNP_VER 1.0.1 CACHE STRING "Cap'n Proto minimum required version.")
find_package(CapnProto ${CAPNP_VER} CONFIG REQUIRED)

set(DEP_IPC_CORE_VERSION 1.0)
block()
  if(TARGET ipc_core)
    set(dep ipc_core) # In meta-project mode it's loaded by this short name.
  else()
    find_package(IpcCore ${DEP_IPC_CORE_VERSION} CONFIG REQUIRED)
    set(dep IpcCore::ipc_core) # In separate-project mode it's been install(EXPORT)ed with the namespace prefix.
  endif()
  set(DEP_LIBS ${dep} CapnProto::capnp PARENT_SCOPE)
endblock()

# Mirror above find_package() calls.  As directed omit "REQUIRED."
set(DEP_LIBS_PKG_ARG_LISTS "IpcCore ${DEP_IPC_CORE_VERSION} CONFIG" "CapnProto ${CAPNP_VER} CONFIG")

# (See above if wondering why we're not auto-searching for these.  There's a good reason.)
# Ideally keep this in `find -s` order.
set(SRCS
    ipc/transport/struc/channel.cpp
    ipc/transport/struc/error.cpp
    ipc/transport/struc/heap_fixed_builder_capnp_msg_builder.cpp
    ipc/transport/struc/heap_serializer.cpp)
set(CAPNP_SCHEMAS
    ipc/transport/struc/schema/common.capnp
    ipc/transport/struc/schema/detail/structured_msg.capnp)
set(HDRS
    ipc/transport/struc/channel.hpp
    ipc/transport/struc/channel_base.hpp
    ipc/transport/struc/detail/msg_impl.hpp
    ipc/transport/struc/detail/msg_mdt_out.hpp
    ipc/transport/struc/error.hpp
    ipc/transport/struc/heap_fixed_builder_capnp_msg_builder.hpp
    ipc/transport/struc/heap_serializer.hpp
    ipc/transport/struc/msg.hpp
    # ipc/transport/struc/serializer.hpp <-- Not an actual source file (concept/docs only).
    ipc/transport/struc/struc_fwd.hpp
    ipc/transport/struc/sync_io/channel.hpp
    ${CAPNP_SCHEMAS}) # Export these like any regular headers...

# ...but also, at the proper time, generate .c++ from them (add to SRCS) and .h also from them (add to HDRS).
# Reminder: this is a supported optional hook of FlowLikeLib.cmake.
function(generate_custom_srcs)
  capnp_generate_cpp(capnp_generated_srcs capnp_generated_hdrs ${CAPNP_SCHEMAS})

  list(APPEND SRCS ${capnp_generated_srcs})
  list(APPEND HDRS ${capnp_generated_hdrs})
  set(SRCS ${SRCS} PARENT_SCOPE)
  set(HDRS ${HDRS} PARENT_SCOPE)

  message("capnp schema files [${CAPNP_SCHEMAS}] shall be exported; plus generated sources [${capnp_generated_srcs}] "
          "and headers [${capnp_generated_hdrs}] shall be compiled/linked and exported respectively.")
endfunction()

include("${FLOW_LIKE_TOOLS}/FlowLikeLib.cmake")
