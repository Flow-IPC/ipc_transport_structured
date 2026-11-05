/* Flow-IPC: Structured Transport
 * Copyright (c) 2023 Akamai Technologies, Inc.; and other contributors.
 * Each commit is copyright by its respective author or author's employer.
 *
 * Licensed under the MIT License:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE. */

#include "ipc/transport/struc/test/serializer_stats_test.hpp"

namespace ipc::transport::struc::test
{

// A slice of the test battery (the ShmType-CLASSIC tests); see similarly named .hpp.

TEST(Struc_serializer_stats_test, send_receive_shm_classic)
{
  test_send_receive_serializer_stats_shm<session::schema::ShmType::CLASSIC>();
}

TEST(Struc_serializer_stats_test, send_receive_app_scope_shm_classic)
{
  test_send_receive_app_scope_serializer_stats_shm<session::schema::ShmType::CLASSIC>();
}

TEST(Struc_serializer_stats_test, direct_builder_smoke_shm_classic)
{
  test_direct_builder_smoke<session::schema::ShmType::CLASSIC>();
}

TEST(Struc_serializer_stats_test, app_shm_configs_via_session_server_shm_classic)
{
  test_app_shm_configs_via_session_server<session::schema::ShmType::CLASSIC>();
}

} // namespace ipc::transport::struc::test
