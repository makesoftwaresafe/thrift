/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements. See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership. The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

namespace java thrift.test.annotations

typedef string my_typedef (a = "a", c = "d")

struct OneOfEachBeansWithAnnotations {
  1: bool boolean_field,
  2: byte a_bite (compression = "false"),
  3: i16 integer16 (must_be_postive = "true"),
  4: i32 integer32,
  5: i64 integer64,
  6: double double_precision (nan_inf_allowed = "false"),
  7: string some_characters,
  8: binary base64,
  9: list<byte> byte_list (non_empty = "true"),
  10: list<i16> i16_list,
  11: list<i64> i64_list,
  // a is overridden to b
  12: my_typedef typedef_meta (a = "b"),
}
