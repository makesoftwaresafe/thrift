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

const ttypes = require("./gen-nodejs/JsDeepConstructorTest_types");
const thrift = require("thrift");
const test = require("tape");
const bufferEquals = require("buffer-equals");

function serializeBinary(data) {
  let buff;
  const transport = new thrift.TBufferedTransport(null, function (msg) {
    buff = msg;
  });
  const prot = new thrift.TBinaryProtocol(transport);
  data[Symbol.for("write")](prot);
  prot.flush();
  return buff;
}

function deserializeBinary(serialized, type) {
  const t = new thrift.TFramedTransport(serialized);
  const p = new thrift.TBinaryProtocol(t);
  const data = new type();
  data[Symbol.for("read")](p);
  return data;
}

function serializeJSON(data) {
  let buff;
  const transport = new thrift.TBufferedTransport(null, function (msg) {
    buff = msg;
  });
  const protocol = new thrift.TJSONProtocol(transport);
  protocol.writeMessageBegin("", 0, 0);
  data[Symbol.for("write")](protocol);
  protocol.writeMessageEnd();
  protocol.flush();
  return buff;
}

function deserializeJSON(serialized, type) {
  const transport = new thrift.TFramedTransport(serialized);
  const protocol = new thrift.TJSONProtocol(transport);
  protocol.readMessageBegin();
  const data = new type();
  data[Symbol.for("read")](protocol);
  protocol.readMessageEnd();
  return data;
}

function createThriftObj() {
  return new ttypes.Complex({
    struct_field: new ttypes.Simple({ value: "a" }),

    struct_list_field: [
      new ttypes.Simple({ value: "b" }),
      new ttypes.Simple({ value: "c" }),
    ],

    struct_set_field: [
      new ttypes.Simple({ value: "d" }),
      new ttypes.Simple({ value: "e" }),
    ],

    struct_map_field: {
      A: new ttypes.Simple({ value: "f" }),
      B: new ttypes.Simple({ value: "g" }),
    },

    struct_nested_containers_field: [
      [
        {
          C: [
            new ttypes.Simple({ value: "h" }),
            new ttypes.Simple({ value: "i" }),
          ],
        },
      ],
    ],

    struct_nested_containers_field2: {
      D: [
        {
          DA: new ttypes.Simple({ value: "j" }),
        },
        {
          DB: new ttypes.Simple({ value: "k" }),
        },
      ],
    },

    list_of_list_field: [
      ["l00", "l01", "l02"],
      ["l10", "l11", "l12"],
      ["l20", "l21", "l22"],
    ],

    list_of_list_of_list_field: [
      [
        ["m000", "m001", "m002"],
        ["m010", "m011", "m012"],
        ["m020", "m021", "m022"],
      ],
      [
        ["m100", "m101", "m102"],
        ["m110", "m111", "m112"],
        ["m120", "m121", "m122"],
      ],
      [
        ["m200", "m201", "m202"],
        ["m210", "m211", "m212"],
        ["m220", "m221", "m222"],
      ],
    ],
  });
}

function createJsObj() {
  return {
    struct_field: { value: "a" },

    struct_list_field: [{ value: "b" }, { value: "c" }],

    struct_set_field: [{ value: "d" }, { value: "e" }],

    struct_map_field: {
      A: { value: "f" },
      B: { value: "g" },
    },

    struct_nested_containers_field: [
      [
        {
          C: [{ value: "h" }, { value: "i" }],
        },
      ],
    ],

    struct_nested_containers_field2: {
      D: [
        {
          DA: { value: "j" },
        },
        {
          DB: { value: "k" },
        },
      ],
    },

    list_of_list_field: [
      ["l00", "l01", "l02"],
      ["l10", "l11", "l12"],
      ["l20", "l21", "l22"],
    ],

    list_of_list_of_list_field: [
      [
        ["m000", "m001", "m002"],
        ["m010", "m011", "m012"],
        ["m020", "m021", "m022"],
      ],
      [
        ["m100", "m101", "m102"],
        ["m110", "m111", "m112"],
        ["m120", "m121", "m122"],
      ],
      [
        ["m200", "m201", "m202"],
        ["m210", "m211", "m212"],
        ["m220", "m221", "m222"],
      ],
    ],
  };
}

function assertValues(obj, assert) {
  assert.equals(obj.struct_field.value, "a");
  assert.equals(obj.struct_list_field[0].value, "b");
  assert.equals(obj.struct_list_field[1].value, "c");
  assert.equals(obj.struct_set_field[0].value, "d");
  assert.equals(obj.struct_set_field[1].value, "e");
  assert.equals(obj.struct_map_field.A.value, "f");
  assert.equals(obj.struct_map_field.B.value, "g");
  assert.equals(obj.struct_nested_containers_field[0][0].C[0].value, "h");
  assert.equals(obj.struct_nested_containers_field[0][0].C[1].value, "i");
  assert.equals(obj.struct_nested_containers_field2.D[0].DA.value, "j");
  assert.equals(obj.struct_nested_containers_field2.D[1].DB.value, "k");
  assert.equals(obj.list_of_list_field[0][0], "l00");
  assert.equals(obj.list_of_list_field[0][1], "l01");
  assert.equals(obj.list_of_list_field[0][2], "l02");
  assert.equals(obj.list_of_list_field[1][0], "l10");
  assert.equals(obj.list_of_list_field[1][1], "l11");
  assert.equals(obj.list_of_list_field[1][2], "l12");
  assert.equals(obj.list_of_list_field[2][0], "l20");
  assert.equals(obj.list_of_list_field[2][1], "l21");
  assert.equals(obj.list_of_list_field[2][2], "l22");

  assert.equals(obj.list_of_list_of_list_field[0][0][0], "m000");
  assert.equals(obj.list_of_list_of_list_field[0][0][1], "m001");
  assert.equals(obj.list_of_list_of_list_field[0][0][2], "m002");
  assert.equals(obj.list_of_list_of_list_field[0][1][0], "m010");
  assert.equals(obj.list_of_list_of_list_field[0][1][1], "m011");
  assert.equals(obj.list_of_list_of_list_field[0][1][2], "m012");
  assert.equals(obj.list_of_list_of_list_field[0][2][0], "m020");
  assert.equals(obj.list_of_list_of_list_field[0][2][1], "m021");
  assert.equals(obj.list_of_list_of_list_field[0][2][2], "m022");

  assert.equals(obj.list_of_list_of_list_field[1][0][0], "m100");
  assert.equals(obj.list_of_list_of_list_field[1][0][1], "m101");
  assert.equals(obj.list_of_list_of_list_field[1][0][2], "m102");
  assert.equals(obj.list_of_list_of_list_field[1][1][0], "m110");
  assert.equals(obj.list_of_list_of_list_field[1][1][1], "m111");
  assert.equals(obj.list_of_list_of_list_field[1][1][2], "m112");
  assert.equals(obj.list_of_list_of_list_field[1][2][0], "m120");
  assert.equals(obj.list_of_list_of_list_field[1][2][1], "m121");
  assert.equals(obj.list_of_list_of_list_field[1][2][2], "m122");

  assert.equals(obj.list_of_list_of_list_field[2][0][0], "m200");
  assert.equals(obj.list_of_list_of_list_field[2][0][1], "m201");
  assert.equals(obj.list_of_list_of_list_field[2][0][2], "m202");
  assert.equals(obj.list_of_list_of_list_field[2][1][0], "m210");
  assert.equals(obj.list_of_list_of_list_field[2][1][1], "m211");
  assert.equals(obj.list_of_list_of_list_field[2][1][2], "m212");
  assert.equals(obj.list_of_list_of_list_field[2][2][0], "m220");
  assert.equals(obj.list_of_list_of_list_field[2][2][1], "m221");
  assert.equals(obj.list_of_list_of_list_field[2][2][2], "m222");
}

function createTestCases(serialize, deserialize) {
  const cases = {
    "Serialize/deserialize should return equal object": function (assert) {
      const tObj = createThriftObj();
      const received = deserialize(serialize(tObj), ttypes.Complex);
      assert.ok(tObj !== received, "not the same object");
      assert.deepEqual(tObj, received);
      assert.end();
    },

    "Nested structs and containers initialized from plain js objects should serialize same as if initialized from thrift objects":
      function (assert) {
        const tObj1 = createThriftObj();
        const tObj2 = new ttypes.Complex(createJsObj());
        assertValues(tObj2, assert);
        const s1 = serialize(tObj1);
        const s2 = serialize(tObj2);
        assert.ok(bufferEquals(s1, s2));
        assert.end();
      },

    "Modifications to args object should not affect constructed Thrift object":
      function (assert) {
        const args = createJsObj();
        assertValues(args, assert);

        const tObj = new ttypes.Complex(args);
        assertValues(tObj, assert);

        args.struct_field.value = "ZZZ";
        args.struct_list_field[0].value = "ZZZ";
        args.struct_list_field[1].value = "ZZZ";
        args.struct_set_field[0].value = "ZZZ";
        args.struct_set_field[1].value = "ZZZ";
        args.struct_map_field.A.value = "ZZZ";
        args.struct_map_field.B.value = "ZZZ";
        args.struct_nested_containers_field[0][0].C[0] = "ZZZ";
        args.struct_nested_containers_field[0][0].C[1] = "ZZZ";
        args.struct_nested_containers_field2.D[0].DA = "ZZZ";
        args.struct_nested_containers_field2.D[0].DB = "ZZZ";

        assertValues(tObj, assert);
        assert.end();
      },

    "nulls are ok": function (assert) {
      const tObj = new ttypes.Complex({
        struct_field: null,
        struct_list_field: null,
        struct_set_field: null,
        struct_map_field: null,
        struct_nested_containers_field: null,
        struct_nested_containers_field2: null,
      });
      const received = deserialize(serialize(tObj), ttypes.Complex);
      assert.strictEqual(tObj.struct_field, null);
      assert.ok(tObj !== received);
      assert.deepEqual(tObj, received);
      assert.end();
    },

    "Can make list with objects": function (assert) {
      const tObj = new ttypes.ComplexList({
        struct_list_field: [new ttypes.Complex({})],
      });
      const innerObj = tObj.struct_list_field[0];
      assert.ok(innerObj instanceof ttypes.Complex);
      assert.strictEqual(innerObj.struct_field, null);
      assert.strictEqual(innerObj.struct_list_field, null);
      assert.strictEqual(innerObj.struct_set_field, null);
      assert.strictEqual(innerObj.struct_map_field, null);
      assert.strictEqual(innerObj.struct_nested_containers_field, null);
      assert.strictEqual(innerObj.struct_nested_containers_field2, null);
      assert.end();
    },
  };
  return cases;
}

function run(name, cases) {
  Object.keys(cases).forEach(function (caseName) {
    test(name + ": " + caseName, cases[caseName]);
  });
}

run("binary", createTestCases(serializeBinary, deserializeBinary));
run("json", createTestCases(serializeJSON, deserializeJSON));
