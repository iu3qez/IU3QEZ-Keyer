/**
 * @file dns_server_test.cpp
 * @brief Unit tests for captive portal DNS server.
 */

#include "captive_portal/dns_server.hpp"

#include "gtest/gtest.h"

#include <cstring>

namespace {

/**
 * @brief Test fixture for DNS server tests.
 */
class DnsServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // DNS server tests are currently disabled due to complex lwip stubbing requirements
    GTEST_SKIP() << "DNS server tests require lwip socket stubbing (to be implemented)";
  }
};

}  // namespace

// DNS Packet Parsing Tests
TEST_F(DnsServerTest, ParsesSimpleDnsQuery) {
  // Test DNS query parsing for "example.com" A record
  // TODO: Implement when lwip stubs are ready
}

TEST_F(DnsServerTest, ParsesAAAAQuery) {
  // Test DNS query parsing for IPv6 AAAA record
  // TODO: Implement when lwip stubs are ready
}

TEST_F(DnsServerTest, RejectsmalformedPacket) {
  // Test that malformed DNS packets are rejected
  // TODO: Implement when lwip stubs are ready
}

// DNS Response Building Tests
TEST_F(DnsServerTest, BuildsARecordResponse) {
  // Test building A record response with target IP 192.168.4.1
  // TODO: Implement when lwip stubs are ready
}

TEST_F(DnsServerTest, BuildsNXDomainForAAAA) {
  // Test that AAAA queries get NXDOMAIN response
  // TODO: Implement when lwip stubs are ready
}

// Rate Limiting Tests
TEST_F(DnsServerTest, AllowsQueriesUnderRateLimit) {
  // Test that queries under 100/sec are allowed
  // TODO: Implement when lwip stubs are ready
}

TEST_F(DnsServerTest, BlocksQueriesOverRateLimit) {
  // Test that queries over 100/sec are blocked
  // TODO: Implement when lwip stubs are ready
}

// Server Lifecycle Tests
TEST_F(DnsServerTest, StartsSuccessfully) {
  // Test that DNS server starts and binds to port 53
  // TODO: Implement when lwip stubs are ready
}

TEST_F(DnsServerTest, StopsSuccessfully) {
  // Test that DNS server stops and frees resources
  // TODO: Implement when lwip stubs are ready
}

TEST_F(DnsServerTest, RejectsDoubleStart) {
  // Test that starting an already-running server fails gracefully
  // TODO: Implement when lwip stubs are ready
}
