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

@0xad3b629d1380a48e;

using Cxx = import "/capnp/c++.capnp";
# This is not really needed for our ridiculously simple schema; we could've just used built-in Int32
# or something below; but this is a nice test of being able to use utility schemas exported by
# libipc_transport_structured.
using Common = import "/ipc/transport/struc/schema/common.capnp";

$Cxx.namespace("link_test");

struct FunBody
{
  union
  {
    coolReq @0 :CoolReq;
    coolRsp @1 :CoolRsp;
  }
}

struct CoolReq
{
  coolVal @0 :Common.Size;
}

struct CoolRsp
{
  coolVal @0 :Common.Size;
}
