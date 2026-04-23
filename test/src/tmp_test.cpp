#include "memix/tmp.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("memix::add sums two integers", "[memix]")
{
  REQUIRE(memix::add(1, 2) == 3);
  REQUIRE(memix::add(-2, 2) == 0);
}
