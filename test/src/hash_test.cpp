#include "detail/hash.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("sha256 text matches standard test vectors", "[hash]") {
  const std::string empty = memix::detail::sha256_text("");
  REQUIRE(empty
          == "e3b0c44298fc1c149afbf4c8996fb924"
             "27ae41e4649b934ca495991b7852b855");

  const std::string abc = memix::detail::sha256_text("abc");
  REQUIRE(abc
          == "ba7816bf8f01cfea414140de5dae2223"
             "b00361a396177a9cb410ff61f20015ad");

  const std::string multi_block = memix::detail::sha256_text(
      "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
  REQUIRE(multi_block
          == "248d6a61d20638b8e5c026930c3e6039"
             "a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("sha256 supports incremental updates", "[hash]") {
  memix::detail::Sha256 sha256;
  sha256.update("abcdbcdecdefdefgefghfghi");
  sha256.update("ghijhijkijkljklmklmnlmnomnopnopq");

  REQUIRE(sha256.final_hex()
          == "248d6a61d20638b8e5c026930c3e6039"
             "a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("sha256 init resets state", "[hash]") {
  memix::detail::Sha256 sha256;
  sha256.update("discarded");
  static_cast<void>(sha256.final());

  sha256.init();
  sha256.update("abc");

  REQUIRE(sha256.final_hex()
          == "ba7816bf8f01cfea414140de5dae2223"
             "b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("sha256 result preserves state", "[hash]") {
  memix::detail::Sha256 sha256;
  sha256.update("abc");

  REQUIRE(memix::detail::hex_encode(sha256.result())
          == "ba7816bf8f01cfea414140de5dae2223"
             "b00361a396177a9cb410ff61f20015ad");

  sha256.update("dbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");

  REQUIRE(sha256.final_hex()
          == "248d6a61d20638b8e5c026930c3e6039"
             "a33ce45964ff2167f6ecedd419db06c1");
}
