﻿/*
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

#include <limits>
#include <locale>
#include <ios>
#include <iostream>
#include <sstream>
#include <thrift/protocol/TBinaryProtocol.h>
#include <thrift/protocol/TCompactProtocol.h>
#include <thrift/protocol/THeaderProtocol.h>
#include <thrift/protocol/TJSONProtocol.h>
#include <thrift/protocol/TMultiplexedProtocol.h>
#include <thrift/transport/THttpClient.h>
#include <thrift/transport/TTransportUtils.h>
#include <thrift/transport/TSocket.h>
#include <thrift/transport/TSSLSocket.h>
#include <thrift/transport/TZlibTransport.h>
#include <thrift/async/TEvhttpClientChannel.h>
#include <thrift/server/TNonblockingServer.h> // <event.h>

#ifdef HAVE_STDINT_H
#include <stdint.h>
#endif
#ifdef HAVE_INTTYPES_H
#include <inttypes.h>
#endif

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <boost/random/random_device.hpp>
#if _WIN32
#include <thrift/windows/TWinsockSingleton.h>
#endif

#include "SecondService.h"
#include "ThriftTest.h"

using namespace std;
using namespace apache::thrift;
using namespace apache::thrift::async;
using namespace apache::thrift::protocol;
using namespace apache::thrift::transport;
using namespace thrift::test;

//
// A pedantic protocol that checks to make sure the response sequence ID
// is the same as the sent sequence ID.  lib/cpp always sends zero for
// synchronous clients, so this bumps the number to make sure it gets
// returned properly from the remote server.  Any server that does not
// respond with the same sequence number is violating the sequence ID
// agreement between client and server.
//

template<typename Proto>
class TPedanticProtocol : public Proto
{
    public:
        TPedanticProtocol(std::shared_ptr<TTransport>& transport)
          : Proto(transport), m_last_seqid((std::numeric_limits<int32_t>::max)() - 10) { }

        virtual uint32_t writeMessageBegin_virt(const std::string& name,
                                           const TMessageType messageType,
                                           const int32_t in_seqid) override
        {
            int32_t seqid = in_seqid;
            if (!seqid) { // this is typical for normal cpp generated code
                seqid = ++m_last_seqid;
            }

            return Proto::writeMessageBegin_virt(name, messageType, seqid);
        }

        virtual uint32_t readMessageBegin_virt(std::string& name,
                                          TMessageType& messageType,
                                          int32_t& seqid) override
        {
            uint32_t result = Proto::readMessageBegin_virt(name, messageType, seqid);
            if (seqid != m_last_seqid) {
                std::stringstream ss;
                ss << "ERROR: send request with seqid " << m_last_seqid << " and got reply with seqid " << seqid;
                throw std::logic_error(ss.str());
            } /* else {
                std::cout << "verified seqid " << m_last_seqid << " round trip OK" << '\n';
            } */
            return result;
        }

    private:
        int32_t m_last_seqid;
};

// Current time, microseconds since the epoch
uint64_t now() {
  int64_t ret;
  struct timeval tv;

  THRIFT_GETTIMEOFDAY(&tv, nullptr);
  ret = tv.tv_sec;
  ret = ret * 1000 * 1000 + tv.tv_usec;
  return ret;
}

static void testString_clientReturn(event_base* base,
                                    int testNr,
                                    ThriftTestCobClient* client) {
  try {
    string s;
    client->recv_testString(s);
    std::ostringstream os;
    os << "test" << testNr;
    const bool ok = (s == os.str());
    cout << "testString: " << s << " " << ((ok) ? "ok" : "failed") << '\n';
  } catch (TException& exn) {
    cout << "Error: " << exn.what() << '\n';
  }

  if (testNr == 9)
    event_base_loopbreak(base); // end test
}

static void testVoid_clientReturn(event_base* base, ThriftTestCobClient* client) {
  try {
    client->recv_testVoid();
    cout << "testVoid" << '\n';

    for (int testNr = 0; testNr < 10; ++testNr) {
      std::ostringstream os;
      os << "test" << testNr;
      client->testString(std::bind(testString_clientReturn,
                                    base,
                                    testNr,
                                    std::placeholders::_1),
                       os.str());
    }
  } catch (TException& exn) {
    cout << "Error: " << exn.what() << '\n';
  }
}

// Workaround for absense of C++11 "auto" keyword.
template <typename T>
bool print_eq(T expected, T actual) {
  cout << "(" << actual << ")" << '\n';
  if (expected != actual) {
    cout << "*** FAILED ***" << '\n' << "Expected: " << expected << " but got: " << actual << '\n';
    return false;
  }
  return true;
}

#define BASETYPE_IDENTITY_TEST(func, value)                                                        \
  cout << #func "(" << value << ") = ";                                                            \
  try {                                                                                            \
    if (!print_eq(value, testClient.func(value)))                                                  \
      return_code |= ERR_BASETYPES;                                                                \
  } catch (TTransportException&) {                                                                 \
    throw;                                                                                         \
  } catch (exception & ex) {                                                                       \
    cout << "*** FAILED ***" << '\n' << ex.what() << '\n';                                         \
    return_code |= ERR_BASETYPES;                                                                  \
  }

#define UUID_TEST(func, value, expected)                                                           \
  cout << #func "(" << value << ") = ";                                                            \
  try {                                                                                            \
    TUuid ret;                                                                                     \
    testClient.func(ret, value);                                                                   \
    if (!print_eq(expected, ret))                                                                  \
      return_code |= ERR_BASETYPES;                                                                \
  } catch (TTransportException&) {                                                                 \
    throw;                                                                                         \
  } catch (exception & ex) {                                                                       \
    cout << "*** FAILED ***" << '\n' << ex.what() << '\n';                                         \
    return_code |= ERR_BASETYPES;                                                                  \
  }

int binary_test(ThriftTestClient& testClient, string::size_type siz);

BOOST_CONSTEXPR_OR_CONST int ERR_BASETYPES = 1;
BOOST_CONSTEXPR_OR_CONST int ERR_STRUCTS = 2;
BOOST_CONSTEXPR_OR_CONST int ERR_CONTAINERS = 4;
BOOST_CONSTEXPR_OR_CONST int ERR_EXCEPTIONS = 8;
BOOST_CONSTEXPR_OR_CONST int ERR_UNKNOWN = 64;

int main(int argc, char** argv) {
  cout.precision(19);

  string testDir  = boost::filesystem::system_complete(argv[0]).parent_path().parent_path().parent_path().string();
  string caPath   = testDir + "/keys/CA.pem";
  string certPath = testDir + "/keys/client.crt";
  string keyPath  = testDir + "/keys/client.key";

#if _WIN32
  transport::TWinsockSingleton::create();
#endif
  string host = "localhost";
  int port = 9090;
  int numTests = 1;
  bool ssl = false;
  bool zlib = false;
  string transport_type = "buffered";
  string protocol_type = "binary";
  string domain_socket = "";
  bool abstract_namespace = false;
  bool noinsane = false;

  int return_code = 0;

  boost::program_options::options_description desc("Allowed options");
  desc.add_options()
      ("help,h", "produce help message")
      ("host",
          boost::program_options::value<string>(&host)->default_value(host),
          "Host to connect")
      ("port",
          boost::program_options::value<int>(&port)->default_value(port),
          "Port number to connect")
      ("domain-socket",
          boost::program_options::value<string>(&domain_socket)->default_value(domain_socket),
          "Domain Socket (e.g. /tmp/ThriftTest.thrift), instead of host and port")
      ("abstract-namespace",
          "Look for the domain socket in the Abstract Namespace"
          " (no connection with filesystem pathnames)")
      ("transport",
          boost::program_options::value<string>(&transport_type)->default_value(transport_type),
          "Transport: buffered, framed, http, evhttp, zlib")
      ("protocol",
          boost::program_options::value<string>(&protocol_type)->default_value(protocol_type),
          "Protocol: binary, compact, header, json, multi, multic, multih, multij")
      ("ssl",
          "Encrypted Transport using SSL")
      ("zlib",
          "Wrap Transport with Zlib")
      ("testloops,n",
          boost::program_options::value<int>(&numTests)->default_value(numTests),
          "Number of Tests")
      ("noinsane",
          "Do not run insanity test");

  boost::program_options::variables_map vm;
  boost::program_options::store(boost::program_options::parse_command_line(argc, argv, desc), vm);
  boost::program_options::notify(vm);

  if (vm.count("help")) {
    cout << desc << '\n';
    return ERR_UNKNOWN;
  }

  try {
    if (!protocol_type.empty()) {
      if (protocol_type == "binary") {
      } else if (protocol_type == "compact") {
      } else if (protocol_type == "header") {
      } else if (protocol_type == "json") {
      } else if (protocol_type == "multi") {
      } else if (protocol_type == "multic") {
      } else if (protocol_type == "multih") {
      } else if (protocol_type == "multij") {
      } else {
        throw invalid_argument("Unknown protocol type " + protocol_type);
      }
    }

    if (!transport_type.empty()) {
      if (transport_type == "buffered") {
      } else if (transport_type == "framed") {
      } else if (transport_type == "http") {
      } else if (transport_type == "evhttp") {
      } else if (transport_type == "zlib") {
        // crosstest will pass zlib as a transport and as a flag right now..
      } else {
        throw invalid_argument("Unknown transport type " + transport_type);
      }
    }

  } catch (exception& e) {
    cerr << e.what() << '\n';
    cout << desc << '\n';
    return ERR_UNKNOWN;
  }

  if (vm.count("ssl")) {
    ssl = true;
  }

  if (vm.count("zlib")) {
    zlib = true;
  }

  if (vm.count("abstract-namespace")) {
    abstract_namespace = true;
  }

  if (vm.count("noinsane")) {
    noinsane = true;
  }

  // THRIFT-4164: The factory MUST outlive any sockets it creates for correct behavior!
  std::shared_ptr<TSSLSocketFactory> factory;
  std::shared_ptr<TSocket> socket;
  std::shared_ptr<TTransport> transport;
  std::shared_ptr<TProtocol> protocol;
  std::shared_ptr<TProtocol> protocol2;  // SecondService for multiplexed

  if (ssl) {
    cout << "Client Certificate File: " << certPath << '\n';
    cout << "Client Key         File: " << keyPath << '\n';
    cout << "CA                 File: " << caPath << '\n';

    factory = std::shared_ptr<TSSLSocketFactory>(new TSSLSocketFactory());
    factory->ciphers("ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
    factory->loadTrustedCertificates(caPath.c_str());
    factory->loadCertificate(certPath.c_str());
    factory->loadPrivateKey(keyPath.c_str());
    factory->authenticate(true);
    socket = factory->createSocket(host, port);
  } else {
    if (domain_socket != "") {
      if (abstract_namespace) {
        std::string abstract_socket("\0", 1);
        abstract_socket += domain_socket;
        socket = std::shared_ptr<TSocket>(new TSocket(abstract_socket));
      } else {
        socket = std::shared_ptr<TSocket>(new TSocket(domain_socket));
      }
      port = 0;
    } else {
      socket = std::shared_ptr<TSocket>(new TSocket(host, port));
    }
  }

  if (transport_type.compare("http") == 0) {
    transport = std::make_shared<THttpClient>(socket, host, "/service");
  } else if (transport_type.compare("framed") == 0) {
    transport = std::make_shared<TFramedTransport>(socket);
  } else {
    transport = std::make_shared<TBufferedTransport>(socket);
  }

  if (zlib) {
    transport = std::make_shared<TZlibTransport>(transport);
  }

  if (protocol_type == "json" || protocol_type == "multij") {
    typedef TPedanticProtocol<TJSONProtocol> TPedanticJSONProtocol;
    protocol = std::make_shared<TPedanticJSONProtocol>(transport);
  } else if (protocol_type == "compact" || protocol_type == "multic") {
    typedef TPedanticProtocol<TCompactProtocol> TPedanticCompactProtocol;
    protocol = std::make_shared<TPedanticCompactProtocol>(transport);
  } else if (protocol_type == "header" || protocol_type == "multih") {
    typedef TPedanticProtocol<THeaderProtocol> TPedanticHeaderProtocol;
    protocol = std::make_shared<TPedanticHeaderProtocol>(transport);
  } else {
    typedef TPedanticProtocol<TBinaryProtocol> TPedanticBinaryProtocol;
    protocol = std::make_shared<TPedanticBinaryProtocol>(transport);
  }

  if (boost::starts_with(protocol_type, "multi")) {
    protocol2 = std::make_shared<TMultiplexedProtocol>(protocol, "SecondService");
    // we don't need access to the original protocol any more, so...
    protocol = std::make_shared<TMultiplexedProtocol>(protocol, "ThriftTest");
  }

  // Connection info
  cout << "Connecting (" << transport_type << "/" << protocol_type << ") to: ";
  if (abstract_namespace) {
    cout << '@';
  }
  cout << domain_socket;
  if (port != 0) {
    cout << host << ":" << port;
  }
  cout << '\n';

  if (transport_type.compare("evhttp") == 0) {
    event_base* base = event_base_new();
    cout << "Libevent Version: " << event_get_version() << '\n';
    cout << "Libevent Method: " << event_base_get_method(base) << '\n';
#if LIBEVENT_VERSION_NUMBER >= 0x02000000
    cout << "Libevent Features: 0x" << hex << event_base_get_features(base) << '\n';
#endif

    std::shared_ptr<TProtocolFactory> protocolFactory(new TBinaryProtocolFactory());

    std::shared_ptr<TAsyncChannel> channel(
        new TEvhttpClientChannel(host.c_str(), "/", host.c_str(), port, base));
    ThriftTestCobClient* client = new ThriftTestCobClient(channel, protocolFactory.get());
    client->testVoid(std::bind(testVoid_clientReturn,
                                base,
                                std::placeholders::_1));

    event_base_loop(base, 0);
    return 0;
  }

  ThriftTestClient testClient(protocol);

  uint64_t time_min = 0;
  uint64_t time_max = 0;
  uint64_t time_tot = 0;

  int test = 0;
  for (test = 0; test < numTests; ++test) {

    try {
      transport->open();
    } catch (TTransportException& ex) {
      cout << "Connect failed: " << ex.what() << '\n';
      return ERR_UNKNOWN;
    }

    /**
     * CONNECT TEST
     */
    printf("Test #%d, connect %s:%d\n", test + 1, host.c_str(), port);

    uint64_t start = now();

    /**
     * VOID TEST
     */
    try {
      cout << "testVoid()" << flush;
      testClient.testVoid();
      cout << " = void" << '\n';
    } catch (TTransportException&) {
      // Stop here if transport got broken
      throw;
    } catch (exception& ex) {
      cout << "*** FAILED ***" << '\n' << ex.what() << '\n';
      return_code |= ERR_BASETYPES;
    }

    /**
     * STRING TEST
     */
    cout << "testString(\"Test\")" << flush;
    string s;
    testClient.testString(s, "Test");
    cout << " = " << s << '\n';
    if (s != "Test") {
      cout << "*** FAILED ***" << '\n';
      return_code |= ERR_BASETYPES;
    }

    //
    // Multiplexed protocol - call another service method
    // in the middle of the ThriftTest
    //
    if (boost::starts_with(protocol_type, "multi")) {
    SecondServiceClient ssc(protocol2);
    // transport is already open...

        try {
          cout << "secondService.secondTestString(\"foo\") => " << flush;
        std::string result;
      ssc.secondtestString(result, "foo");
      cout << "{" << result << "}" << '\n';
      } catch (std::exception& e) {
      cout << "  *** FAILED *** " << e.what() << '\n';
      return_code |= ERR_EXCEPTIONS;
    }
    }

    try {
#ifdef _MSC_VER
#pragma warning( push )
#pragma warning( disable : 4566 )
#endif
      string str(
          "}{Afrikaans, Alemannisch, Aragonés, العربية, مصرى, "
          "Asturianu, Aymar aru, Azərbaycan, Башҡорт, Boarisch, Žemaitėška, "
          "Беларуская, Беларуская (тарашкевіца), Български, Bamanankan, "
          "বাংলা, Brezhoneg, Bosanski, Català, Mìng-dĕ̤ng-ngṳ̄, Нохчийн, "
          "Cebuano, ᏣᎳᎩ, Česky, Словѣ́ньскъ / ⰔⰎⰑⰂⰡⰐⰠⰔⰍⰟ, Чӑвашла, Cymraeg, "
          "Dansk, Zazaki, ދިވެހިބަސް, Ελληνικά, Emiliàn e rumagnòl, English, "
          "Esperanto, Español, Eesti, Euskara, فارسی, Suomi, Võro, Føroyskt, "
          "Français, Arpetan, Furlan, Frysk, Gaeilge, 贛語, Gàidhlig, Galego, "
          "Avañe'ẽ, ગુજરાતી, Gaelg, עברית, हिन्दी, Fiji Hindi, Hrvatski, "
          "Kreyòl ayisyen, Magyar, Հայերեն, Interlingua, Bahasa Indonesia, "
          "Ilokano, Ido, Íslenska, Italiano, 日本語, Lojban, Basa Jawa, "
          "ქართული, Kongo, Kalaallisut, ಕನ್ನಡ, 한국어, Къарачай-Малкъар, "
          "Ripoarisch, Kurdî, Коми, Kernewek, Кыргызча, Latina, Ladino, "
          "Lëtzebuergesch, Limburgs, Lingála, ລາວ, Lietuvių, Latviešu, Basa "
          "Banyumasan, Malagasy, Македонски, മലയാളം, मराठी, مازِرونی, Bahasa "
          "Melayu, Nnapulitano, Nedersaksisch, नेपाल भाषा, Nederlands, ‪"
          "Norsk (nynorsk)‬, ‪Norsk (bokmål)‬, Nouormand, Diné bizaad, "
          "Occitan, Иронау, Papiamentu, Deitsch, Polski, پنجابی, پښتو, "
          "Norfuk / Pitkern, Português, Runa Simi, Rumantsch, Romani, Română, "
          "Русский, Саха тыла, Sardu, Sicilianu, Scots, Sámegiella, Simple "
          "English, Slovenčina, Slovenščina, Српски / Srpski, Seeltersk, "
          "Svenska, Kiswahili, தமிழ், తెలుగు, Тоҷикӣ, ไทย, Türkmençe, Tagalog, "
          "Türkçe, Татарча/Tatarça, Українська, اردو, Tiếng Việt, Volapük, "
          "Walon, Winaray, 吴语, isiXhosa, ייִדיש, Yorùbá, Zeêuws, 中文, "
          "Bân-lâm-gú, 粵語");
#ifdef _MSC_VER
#pragma warning( pop )
#endif
      cout << "testString(" << str << ") = " << flush;
      testClient.testString(s, str);
      cout << s << '\n';
      if (s != str) {
        cout.imbue(locale("en_US.UTF8"));
        cout << "*** FAILED ***" << '\n' << "Expected string: " << str << " but got: " << s << '\n' << "CLEAR";
        return_code |= ERR_BASETYPES;
      }
    } catch (TTransportException&) {
      throw;
    } catch (exception& ex) {
      cout << "*** FAILED ***" << '\n' << ex.what() << '\n';
      return_code |= ERR_BASETYPES;
      return return_code;
    }
    try {
      string str(
          "quote: \" backslash:"
          " forwardslash-escaped: \\/ "
          " backspace: \b formfeed: \f newline: \n return: \r tab: "
          " now-all-of-them-together: \"\\\\/\b\n\r\t"
          " now-a-bunch-of-junk: !@#$%&()(&%$#{}{}<><><"
          " char-to-test-json-parsing: ]] \"]] \\\" }}}{ [[[ ");
      cout << "testString(" << str << ") = " << flush;
      testClient.testString(s, str);
      cout << s << '\n';
      if (s != str) {
        cout.imbue(locale("en_US.UTF8"));
        cout << "*** FAILED ***" << '\n'
             << "Expected string: " << str << " but got: " << s << '\n'
             << "CLEAR";
        ;
        return_code |= ERR_BASETYPES;
      }
    } catch (TTransportException&) {
      throw;
    } catch (exception& ex) {
      cout << "*** FAILED ***" << '\n' << ex.what() << '\n';
      return_code |= ERR_BASETYPES;
      return return_code;
    }

    /**
     * BOOL TEST
     */
    cout << boolalpha;
    BASETYPE_IDENTITY_TEST(testBool, true);
    BASETYPE_IDENTITY_TEST(testBool, false);

    /**
     * BYTE TEST
     */
    BASETYPE_IDENTITY_TEST(testByte, (int8_t)0);
    BASETYPE_IDENTITY_TEST(testByte, (int8_t)-1);
    BASETYPE_IDENTITY_TEST(testByte, (int8_t)42);
    BASETYPE_IDENTITY_TEST(testByte, (int8_t)-42);
    BASETYPE_IDENTITY_TEST(testByte, (int8_t)127);
    BASETYPE_IDENTITY_TEST(testByte, (int8_t)-128);

    /**
     * I32 TEST
     */
    BASETYPE_IDENTITY_TEST(testI32, 0);
    BASETYPE_IDENTITY_TEST(testI32, -1);
    BASETYPE_IDENTITY_TEST(testI32, 190000013);
    BASETYPE_IDENTITY_TEST(testI32, -190000013);
    BASETYPE_IDENTITY_TEST(testI32, (numeric_limits<int32_t>::max)());
    BASETYPE_IDENTITY_TEST(testI32, (numeric_limits<int32_t>::min)());

    /**
     * I64 TEST
     */
    BASETYPE_IDENTITY_TEST(testI64, (int64_t)0);
    BASETYPE_IDENTITY_TEST(testI64, (int64_t)-1);
    BASETYPE_IDENTITY_TEST(testI64, (int64_t)7000000000000000123LL);
    BASETYPE_IDENTITY_TEST(testI64, (int64_t)-7000000000000000123LL);
    BASETYPE_IDENTITY_TEST(testI64, (int64_t)pow(static_cast<double>(2LL), 32));
    BASETYPE_IDENTITY_TEST(testI64, (int64_t)-pow(static_cast<double>(2LL), 32));
    BASETYPE_IDENTITY_TEST(testI64, (int64_t)pow(static_cast<double>(2LL), 32) + 1);
    BASETYPE_IDENTITY_TEST(testI64, (int64_t)-pow(static_cast<double>(2LL), 32) - 1);
    BASETYPE_IDENTITY_TEST(testI64, (numeric_limits<int64_t>::max)());
    BASETYPE_IDENTITY_TEST(testI64, (numeric_limits<int64_t>::min)());

    /**
     * DOUBLE TEST
     */
    // Comparing double values with plain equality because Thrift handles full precision of double
    BASETYPE_IDENTITY_TEST(testDouble, 0.0);
    BASETYPE_IDENTITY_TEST(testDouble, -1.0);
    BASETYPE_IDENTITY_TEST(testDouble, -5.2098523);
    BASETYPE_IDENTITY_TEST(testDouble, -0.000341012439638598279);
    BASETYPE_IDENTITY_TEST(testDouble, pow(static_cast<double>(2), 32));
    BASETYPE_IDENTITY_TEST(testDouble, pow(static_cast<double>(2), 32) + 1);
    BASETYPE_IDENTITY_TEST(testDouble, pow(static_cast<double>(2), 53) - 1);
    BASETYPE_IDENTITY_TEST(testDouble, -pow(static_cast<double>(2), 32));
    BASETYPE_IDENTITY_TEST(testDouble, -pow(static_cast<double>(2), 32) - 1);
    BASETYPE_IDENTITY_TEST(testDouble, -pow(static_cast<double>(2), 53) + 1);

    try {
      double expected = pow(static_cast<double>(10), 307);
      cout << "testDouble(" << expected << ") = " << flush;
      double actual = testClient.testDouble(expected);
      cout << "(" << actual << ")" << '\n';
      if (expected - actual > pow(static_cast<double>(10), 292)) {
        cout << "*** FAILED ***" << '\n'
             << "Expected: " << expected << " but got: " << actual << '\n';
      }
    } catch (TTransportException&) {
      throw;
    } catch (exception& ex) {
      cout << "*** FAILED ***" << '\n' << ex.what() << '\n';
      return_code |= ERR_BASETYPES;
    }

    try {
      double expected = pow(static_cast<double>(10), -292);
      cout << "testDouble(" << expected << ") = " << flush;
      double actual = testClient.testDouble(expected);
      cout << "(" << actual << ")" << '\n';
      if (expected - actual > pow(static_cast<double>(10), -307)) {
        cout << "*** FAILED ***" << '\n'
             << "Expected: " << expected << " but got: " << actual << '\n';
      }
    } catch (TTransportException&) {
      throw;
    } catch (exception& ex) {
      cout << "*** FAILED ***" << '\n' << ex.what() << '\n';
      return_code |= ERR_BASETYPES;
    }

    /**
     * BINARY TEST
     */
    for (string::size_type i = 0; i < 131073 && !return_code; ) {
      return_code |= binary_test(testClient, i);
      if (i > 0) { i *= 2; } else { ++i; }
    }

    /**
     * UUID TEST
     */
    const TUuid expected_uuid{"5e2ab188-1726-4e75-a04f-1ed9a6a89c4c"};
    UUID_TEST(testUuid, TUuid{"{5e2ab188-1726-4e75-a04f-1ed9a6a89c4c}"}, expected_uuid);
    UUID_TEST(testUuid, TUuid{"5e2ab188-1726-4e75-a04f-1ed9a6a89c4c"}, expected_uuid);
    UUID_TEST(testUuid, TUuid{"5e2ab18817264e75a04f1ed9a6a89c4c"}, expected_uuid);
    UUID_TEST(testUuid, TUuid{"{5e2ab18817264e75a04f1ed9a6a89c4c}"}, expected_uuid);
    UUID_TEST(testUuid, TUuid{}, TUuid{"00000000-0000-0000-0000-000000000000"});

    /**
     * STRUCT TEST
     */
    cout << "testStruct({\"Zero\", 1, -3, -5})" << flush;
    Xtruct out;
    out.string_thing = "Zero";
    out.byte_thing = 1;
    out.i32_thing = -3;
    out.i64_thing = -5;
    Xtruct in;
    testClient.testStruct(in, out);
    printf(" = {\"%s\", %d, %d, %" PRId64 "}\n",
           in.string_thing.c_str(),
           (int)in.byte_thing,
           in.i32_thing,
           in.i64_thing);
    if (in != out) {
      cout << "*** FAILED ***" << '\n';
      return_code |= ERR_STRUCTS;
    }

    /**
     * NESTED STRUCT TEST
     */
    cout << "testNest({1, {\"Zero\", 1, -3, -5}), 5}" << flush;
    Xtruct2 out2;
    out2.byte_thing = 1;
    out2.struct_thing = out;
    out2.i32_thing = 5;
    Xtruct2 in2;
    testClient.testNest(in2, out2);
    in = in2.struct_thing;
    printf(" = {%d, {\"%s\", %d, %d, %" PRId64 "}, %d}\n",
           in2.byte_thing,
           in.string_thing.c_str(),
           (int)in.byte_thing,
           in.i32_thing,
           in.i64_thing,
           in2.i32_thing);
    if (in2 != out2) {
      cout << "*** FAILED ***" << '\n';
      return_code |= ERR_STRUCTS;
    }

    /**
     * MAP TEST
     */
    map<int32_t, int32_t> mapout;
    for (int32_t i = 0; i < 5; ++i) {
      mapout.insert(make_pair(i, i - 10));
    }
    cout << "testMap({" << flush;
    map<int32_t, int32_t>::const_iterator m_iter;
    bool first = true;
    for (m_iter = mapout.begin(); m_iter != mapout.end(); ++m_iter) {
      if (first) {
        first = false;
      } else {
        cout << ",";
      }
      cout << m_iter->first << " => " << m_iter->second;
    }
    cout << "})";
    map<int32_t, int32_t> mapin;
    testClient.testMap(mapin, mapout);
    cout << " = {";
    first = true;
    for (m_iter = mapin.begin(); m_iter != mapin.end(); ++m_iter) {
      if (first) {
        first = false;
      } else {
        cout << ",";
      }
      cout << m_iter->first << " => " << m_iter->second;
    }
    cout << "}" << '\n';
    if (mapin != mapout) {
      cout << "*** FAILED ***" << '\n';
      return_code |= ERR_CONTAINERS;
    }

    /**
     * STRING MAP TEST
     */
    cout << "testStringMap({a => 2, b => blah, some => thing}) = {" << flush;
    map<string, string> smapin;
    map<string, string> smapout;
    smapin["a"] = "2";
    smapin["b"] = "blah";
    smapin["some"] = "thing";
    try {
      testClient.testStringMap(smapout, smapin);
      first = true;
      for (map<string, string>::const_iterator it = smapout.begin(); it != smapout.end(); ++it) {
        if (first)
          cout << ",";
        else
          first = false;
        cout << it->first << " => " << it->second;
      }
      cout << "}" << '\n';
      if (smapin != smapout) {
        cout << "*** FAILED ***" << '\n';
        return_code |= ERR_CONTAINERS;
      }
    } catch (TTransportException&) {
      throw;
    } catch (exception& ex) {
      cout << "*** FAILED ***" << '\n' << ex.what() << '\n';
      return_code |= ERR_CONTAINERS;
    }

    /**
     * SET TEST
     */
    set<int32_t> setout;
    for (int32_t i = -2; i < 3; ++i) {
      setout.insert(i);
    }
    cout << "testSet({" << flush;
    set<int32_t>::const_iterator s_iter;
    first = true;
    for (s_iter = setout.begin(); s_iter != setout.end(); ++s_iter) {
      if (first) {
        first = false;
      } else {
        cout << ",";
      }
      cout << *s_iter;
    }
    cout << "})";
    set<int32_t> setin;
    testClient.testSet(setin, setout);
    cout << " = {";
    first = true;
    for (s_iter = setin.begin(); s_iter != setin.end(); ++s_iter) {
      if (first) {
        first = false;
      } else {
        cout << ",";
      }
      cout << *s_iter;
    }
    cout << "}" << '\n';
    if (setin != setout) {
      cout << "*** FAILED ***" << '\n';
      return_code |= ERR_CONTAINERS;
    }

    /**
     * LIST TEST
     */
    cout << "testList(empty)" << flush;
    try {
      vector<int32_t> listout;
      testClient.testList(listout, vector<int32_t>());
      if (!listout.empty()) {
        cout << "*** FAILED ***" << '\n';
        cout << "invalid length: " << listout.size() << '\n';
        return_code |= ERR_CONTAINERS;
      }
    } catch (TTransportException&) {
      throw;
    } catch (exception& ex) {
      cout << "*** FAILED ***" << '\n' << ex.what() << '\n';
      return_code |= ERR_CONTAINERS;
    }
    try {
      vector<int32_t> listout;
      for (int32_t i = -2; i < 3; ++i) {
        listout.push_back(i);
      }
      cout << "testList({" << flush;
      vector<int32_t>::const_iterator l_iter;
      first = true;
      for (l_iter = listout.begin(); l_iter != listout.end(); ++l_iter) {
        if (first) {
          first = false;
        } else {
          cout << ",";
        }
        cout << *l_iter;
      }
      cout << "})";
      vector<int32_t> listin;
      testClient.testList(listin, listout);
      cout << " = {";
      first = true;
      for (l_iter = listin.begin(); l_iter != listin.end(); ++l_iter) {
        if (first) {
          first = false;
        } else {
          cout << ",";
        }
        cout << *l_iter;
      }
      cout << "}" << '\n';
      if (listin != listout) {
        cout << "*** FAILED ***" << '\n';
        return_code |= ERR_CONTAINERS;
      }
    } catch (TTransportException&) {
      throw;
    } catch (exception& ex) {
      cout << "*** FAILED ***" << '\n' << ex.what() << '\n';
      return_code |= ERR_CONTAINERS;
    }

    /**
     * ENUM TEST
     */
    cout << "testEnum(ONE)" << flush;
    Numberz::type ret = testClient.testEnum(Numberz::ONE);
    cout << " = " << ret << '\n';
    if (ret != Numberz::ONE) {
      cout << "*** FAILED ***" << '\n';
      return_code |= ERR_STRUCTS;
    }

    cout << "testEnum(TWO)" << flush;
    ret = testClient.testEnum(Numberz::TWO);
    cout << " = " << ret << '\n';
    if (ret != Numberz::TWO) {
      cout << "*** FAILED ***" << '\n';
      return_code |= ERR_STRUCTS;
    }

    cout << "testEnum(THREE)" << flush;
    ret = testClient.testEnum(Numberz::THREE);
    cout << " = " << ret << '\n';
    if (ret != Numberz::THREE) {
      cout << "*** FAILED ***" << '\n';
      return_code |= ERR_STRUCTS;
    }

    cout << "testEnum(FIVE)" << flush;
    ret = testClient.testEnum(Numberz::FIVE);
    cout << " = " << ret << '\n';
    if (ret != Numberz::FIVE) {
      cout << "*** FAILED ***" << '\n';
      return_code |= ERR_STRUCTS;
    }

    cout << "testEnum(EIGHT)" << flush;
    ret = testClient.testEnum(Numberz::EIGHT);
    cout << " = " << ret << '\n';
    if (ret != Numberz::EIGHT) {
      cout << "*** FAILED ***" << '\n';
      return_code |= ERR_STRUCTS;
    }

    /**
     * TYPEDEF TEST
     */
    cout << "testTypedef(309858235082523)" << flush;
    UserId uid = testClient.testTypedef(309858235082523LL);
    cout << " = " << uid << '\n';
    if (uid != 309858235082523LL) {
      cout << "*** FAILED ***" << '\n';
      return_code |= ERR_STRUCTS;
    }

    /**
     * NESTED MAP TEST
     */
    cout << "testMapMap(1)" << flush;
    map<int32_t, map<int32_t, int32_t> > mm;
    testClient.testMapMap(mm, 1);
    cout << " = {";
    map<int32_t, map<int32_t, int32_t> >::const_iterator mi;
    for (mi = mm.begin(); mi != mm.end(); ++mi) {
      printf("%d => {", mi->first);
      map<int32_t, int32_t>::const_iterator mi2;
      for (mi2 = mi->second.begin(); mi2 != mi->second.end(); ++mi2) {
        cout << mi2->first << " => " << mi2->second;
      }
      cout << "}, ";
    }
    cout << "}" << '\n';
    if (mm.size() != 2 ||
        mm[-4][-4] != -4 ||
        mm[-4][-3] != -3 ||
        mm[-4][-2] != -2 ||
        mm[-4][-1] != -1 ||
        mm[4][4] != 4 ||
        mm[4][3] != 3 ||
        mm[4][2] != 2 ||
        mm[4][1] != 1) {
      cout << "*** FAILED ***" << '\n';
      return_code |= ERR_CONTAINERS;
    }

    /**
     * INSANITY TEST
     */
    if (!noinsane) {
      Insanity insane;
      insane.userMap.insert(make_pair(Numberz::FIVE, 5));
      insane.userMap.insert(make_pair(Numberz::EIGHT, 8));
      Xtruct truck;
      truck.string_thing = "Goodbye4";
      truck.byte_thing = 4;
      truck.i32_thing = 4;
      truck.i64_thing = 4;
      Xtruct truck2;
      truck2.string_thing = "Hello2";
      truck2.byte_thing = 2;
      truck2.i32_thing = 2;
      truck2.i64_thing = 2;
      insane.xtructs.push_back(truck);
      insane.xtructs.push_back(truck2);
      cout << "testInsanity()" << flush;
      map<UserId, map<Numberz::type, Insanity> > whoa;
      testClient.testInsanity(whoa, insane);
      cout << " = {";
      map<UserId, map<Numberz::type, Insanity> >::const_iterator i_iter;
      for (i_iter = whoa.begin(); i_iter != whoa.end(); ++i_iter) {
        printf("%" PRId64 " => {", i_iter->first);
        map<Numberz::type, Insanity>::const_iterator i2_iter;
        for (i2_iter = i_iter->second.begin(); i2_iter != i_iter->second.end(); ++i2_iter) {
          printf("%d => {", i2_iter->first);
          map<Numberz::type, UserId> userMap = i2_iter->second.userMap;
          map<Numberz::type, UserId>::const_iterator um;
          cout << "{";
          for (um = userMap.begin(); um != userMap.end(); ++um) {
            cout << um->first << " => " << um->second;
          }
          cout << "}, ";

          vector<Xtruct> xtructs = i2_iter->second.xtructs;
          vector<Xtruct>::const_iterator x;
          cout << "{";
          for (x = xtructs.begin(); x != xtructs.end(); ++x) {
            printf("{\"%s\", %d, %d, %" PRId64 "}, ",
                   x->string_thing.c_str(),
                   (int)x->byte_thing,
                   x->i32_thing,
                   x->i64_thing);
          }
          cout << "}";

          cout << "}, ";
        }
        cout << "}, ";
      }
      cout << "}" << '\n';
      bool failed = false;
      map<UserId, map<Numberz::type, Insanity> >::const_iterator it1 = whoa.find(UserId(1));
      if (whoa.size() != 2) {
        failed = true;
      }
      if (it1 == whoa.end()) {
        failed = true;
      } else {
        auto it12 = it1->second.find(Numberz::TWO);
        if (it12 == it1->second.end() || it12->second != insane) {
          failed = true;
        }
        auto it13 = it1->second.find(Numberz::THREE);
        if (it13 == it1->second.end() || it13->second != insane) {
          failed = true;
        }
      }
      map<UserId, map<Numberz::type, Insanity> >::const_iterator it2 = whoa.find(UserId(2));
      if (it2 == whoa.end()) {
        failed = true;
      } else {
        auto it26 = it2->second.find(Numberz::SIX);
        if (it26 == it2->second.end() || it26->second != Insanity()) {
          failed = true;
        }
      }
      if (failed) {
        cout << "*** FAILED ***" << '\n';
        return_code |= ERR_STRUCTS;
      }
    }

    /**
     * MULTI TEST
     */
    cout << "testMulti()" << '\n';
    try {
      map<int16_t, string> mul_map;
      Xtruct mul_result;
      mul_map[1] = "blah";
      mul_map[2] = "thing";
      testClient.testMulti(mul_result, 42, 4242, 424242, mul_map, Numberz::EIGHT, UserId(24));
      Xtruct xxs;
      xxs.string_thing = "Hello2";
      xxs.byte_thing = 42;
      xxs.i32_thing = 4242;
      xxs.i64_thing = 424242;
      if (mul_result != xxs) {
        cout << "*** FAILED ***" << '\n';
        return_code |= ERR_STRUCTS;
      }
    } catch (TTransportException&) {
      throw;
    } catch (exception& ex) {
      cout << "*** FAILED ***" << '\n' << ex.what() << '\n';
      return_code |= ERR_STRUCTS;
    }

    /* test exception */

    try {
      cout << "testClient.testException(\"Xception\") =>" << flush;
      testClient.testException("Xception");
      cout << "  void\n*** FAILED ***" << '\n';
      return_code |= ERR_EXCEPTIONS;

    } catch (Xception& e) {
      printf("  {%u, \"%s\"}\n", e.errorCode, e.message.c_str());
    }

    try {
      cout << "testClient.testException(\"TException\") =>" << flush;
      testClient.testException("TException");
      cout << "  void\n*** FAILED ***" << '\n';
      return_code |= ERR_EXCEPTIONS;

    } catch (const TException&) {
      cout << "  Caught TException" << '\n';
    }

    try {
      cout << "testClient.testException(\"success\") =>" << flush;
      testClient.testException("success");
      cout << "  void" << '\n';
    } catch (exception & ex) {                                                                       \
      cout << "*** FAILED ***" << '\n' << ex.what() << '\n';
      return_code |= ERR_EXCEPTIONS;
    }

    /* test multi exception */

    try {
      cout << "testClient.testMultiException(\"Xception\", \"test 1\") =>" << flush;
      Xtruct result;
      testClient.testMultiException(result, "Xception", "test 1");
      cout << "  result\n*** FAILED ***" << '\n';
      return_code |= ERR_EXCEPTIONS;
    } catch (Xception& e) {
      printf("  {%u, \"%s\"}\n", e.errorCode, e.message.c_str());
    }

    try {
      cout << "testClient.testMultiException(\"Xception2\", \"test 2\") =>" << flush;
      Xtruct result;
      testClient.testMultiException(result, "Xception2", "test 2");
      cout << "  result\n*** FAILED ***" << '\n';
      return_code |= ERR_EXCEPTIONS;

    } catch (Xception2& e) {
      printf("  {%u, {\"%s\"}}\n", e.errorCode, e.struct_thing.string_thing.c_str());
    }

    try {
      cout << "testClient.testMultiException(\"success\", \"test 3\") =>" << flush;
      Xtruct result;
      testClient.testMultiException(result, "success", "test 3");
      printf("  {{\"%s\"}}\n", result.string_thing.c_str());
    } catch (exception & ex) {                                                                       \
      cout << "*** FAILED ***" << '\n' << ex.what() << '\n';
      return_code |= ERR_EXCEPTIONS;
    }

    /* test oneway void */
    {
      cout << "testClient.testOneway(1) =>" << flush;
      uint64_t startOneway = now();
      testClient.testOneway(1);
      uint64_t elapsed = now() - startOneway;
      if (elapsed > 200 * 1000) { // 0.2 seconds
        printf("*** FAILED *** - took %.2f ms\n", (double)elapsed / 1000.0);
      return_code |= ERR_BASETYPES;
      } else {
        printf("  success - took %.2f ms\n", (double)elapsed / 1000.0);
      }
    }

    /**
     * redo a simple test after the oneway to make sure we aren't "off by one" --
     * if the server treated oneway void like normal void, this next test will
     * fail since it will get the void confirmation rather than the correct
     * result. In this circumstance, the client will throw the exception:
     *
     *   TApplicationException: Wrong method namea
     */
    /**
     * I32 TEST
     */
    cout << "re-test testI32(-1)" << flush;
    int i32 = testClient.testI32(-1);
    cout << " = " << i32 << '\n';
    if (i32 != -1)
      return_code |= ERR_BASETYPES;

    cout << '\n' << "All tests done." << '\n' << flush;

    uint64_t stop = now();
    uint64_t tot = stop - start;

    cout << "Total time: " << stop - start << " us" << '\n';

    time_tot += tot;
    if (time_min == 0 || tot < time_min) {
      time_min = tot;
    }
    if (tot > time_max) {
      time_max = tot;
    }

    cout << flush;
    transport->close();
  }


  uint64_t time_avg = time_tot / numTests;

  cout << "Min time: " << time_min << " us" << '\n';
  cout << "Max time: " << time_max << " us" << '\n';
  cout << "Avg time: " << time_avg << " us" << '\n';

  return return_code;
}

void binary_fill(std::string& str, string::size_type siz)
{
    static const signed char bin_data[256]
        = {-128, -127, -126, -125, -124, -123, -122, -121, -120, -119, -118, -117, -116, -115, -114,
           -113, -112, -111, -110, -109, -108, -107, -106, -105, -104, -103, -102, -101, -100, -99,
           -98,  -97,  -96,  -95,  -94,  -93,  -92,  -91,  -90,  -89,  -88,  -87,  -86,  -85,  -84,
           -83,  -82,  -81,  -80,  -79,  -78,  -77,  -76,  -75,  -74,  -73,  -72,  -71,  -70,  -69,
           -68,  -67,  -66,  -65,  -64,  -63,  -62,  -61,  -60,  -59,  -58,  -57,  -56,  -55,  -54,
           -53,  -52,  -51,  -50,  -49,  -48,  -47,  -46,  -45,  -44,  -43,  -42,  -41,  -40,  -39,
           -38,  -37,  -36,  -35,  -34,  -33,  -32,  -31,  -30,  -29,  -28,  -27,  -26,  -25,  -24,
           -23,  -22,  -21,  -20,  -19,  -18,  -17,  -16,  -15,  -14,  -13,  -12,  -11,  -10,  -9,
           -8,   -7,   -6,   -5,   -4,   -3,   -2,   -1,   0,    1,    2,    3,    4,    5,    6,
           7,    8,    9,    10,   11,   12,   13,   14,   15,   16,   17,   18,   19,   20,   21,
           22,   23,   24,   25,   26,   27,   28,   29,   30,   31,   32,   33,   34,   35,   36,
           37,   38,   39,   40,   41,   42,   43,   44,   45,   46,   47,   48,   49,   50,   51,
           52,   53,   54,   55,   56,   57,   58,   59,   60,   61,   62,   63,   64,   65,   66,
           67,   68,   69,   70,   71,   72,   73,   74,   75,   76,   77,   78,   79,   80,   81,
           82,   83,   84,   85,   86,   87,   88,   89,   90,   91,   92,   93,   94,   95,   96,
           97,   98,   99,   100,  101,  102,  103,  104,  105,  106,  107,  108,  109,  110,  111,
           112,  113,  114,  115,  116,  117,  118,  119,  120,  121,  122,  123,  124,  125,  126,
           127};

    str.resize(siz);
    char *ptr = &str[0];
    string::size_type pos = 0;
    for (string::size_type i = 0; i < siz; ++i)
    {
        if (pos == 255) { pos = 0; } else { ++pos; }
        *ptr++ = bin_data[pos];
    }
}

int binary_test(ThriftTestClient& testClient, string::size_type siz)
{
    string bin_request;
    string bin_result;

    cout << "testBinary(siz = " << siz << ")" << '\n';
    binary_fill(bin_request, siz);
    try {
        testClient.testBinary(bin_result, bin_request);

        if (bin_request.size() != bin_result.size()) {
            cout << "*** FAILED: request size " << bin_request.size() << "; result size " << bin_result.size() << '\n';
            return ERR_BASETYPES;
        }

        for (string::size_type i = 0; i < siz; ++i) {
            if (bin_request.at(i) != bin_result.at(i)) {
                cout << "*** FAILED: at position " << i << " request[i] is h" << hex << bin_request.at(i) << " result[i] is h" << hex << bin_result.at(i) << '\n';
                return ERR_BASETYPES;
            }
        }
    } catch (TTransportException&) {
        throw;
    } catch (exception& ex) {
        cout << "*** FAILED ***" << '\n' << ex.what() << '\n';
        return ERR_BASETYPES;
    }

    return 0;
}
