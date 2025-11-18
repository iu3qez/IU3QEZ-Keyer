/**
 * @file dns_server.hpp
 * @brief Minimal DNS server for captive portal wildcard resolution.
 * @details
 *  This file defines a lightweight DNS server that responds to all DNS queries
 *  with the AP's IP address (192.168.4.1). This enables captive portal detection
 *  on iOS, Android, Windows, and other platforms by intercepting DNS lookups for
 *  connectivity check domains.
 *
 *  The server listens on UDP port 53 and implements basic DNS packet parsing
 *  with rate limiting to prevent DOS attacks.
 *
 * @author Simone Fabris
 * @date 2025-11-15
 * @version 1.0
 */

#pragma once

#include <cstddef>  // size_t
#include <cstdint>

namespace captive_portal {

/**
 * @brief Minimal DNS server for captive portal wildcard resolution.
 *
 * This class implements a UDP-based DNS server that responds to all queries
 * with the device's AP IP address. It supports A record queries (IPv4) and
 * returns NXDOMAIN for AAAA queries (IPv6).
 */
class DnsServer {
 public:
  /**
   * @brief  Constructs a DNS server instance.
   */
  DnsServer();

  /**
   * @brief  Destructor, ensures server is stopped and resources are freed.
   */
  ~DnsServer();

  /**
   * @brief  Starts the DNS server on port 53.
   * @return True if the server started successfully, false otherwise.
   * @note   This method creates a FreeRTOS task that runs the server loop.
   */
  bool Start();

  /**
   * @brief  Stops the DNS server and frees resources.
   * @return True if the server stopped successfully, false otherwise.
   */
  bool Stop();

  /**
   * @brief  Checks if the DNS server is currently running.
   * @return True if running, false otherwise.
   */
  bool IsRunning() const;

  /**
   * @brief  Sets the target IP address to return in DNS responses.
   * @param  ip_addr Target IPv4 address in host byte order (e.g., 0xC0A80401 for 192.168.4.1).
   * @note   Must be called before Start() or will take effect on next query.
   */
  void SetTargetIP(uint32_t ip_addr);

 private:
  /**
   * @brief  FreeRTOS task entry point for DNS server loop.
   * @param  arg Pointer to DnsServer instance (passed as void*).
   */
  static void ServerTask(void* arg);

  /**
   * @brief  Main server loop: receive DNS query → parse → respond.
   * @note   Runs in the FreeRTOS task until Stop() is called.
   */
  void ServerLoop();

  /**
   * @brief  Parses a DNS query packet and extracts the queried domain name.
   * @param  packet DNS query packet buffer.
   * @param  length Length of the packet in bytes.
   * @param  out_domain Output buffer for extracted domain name (max 256 bytes).
   * @param  out_query_type Output query type (1 = A, 28 = AAAA).
   * @return True if parsing succeeded, false if packet is malformed.
   */
  bool ParseDnsQuery(const uint8_t* packet, size_t length, char* out_domain, uint16_t* out_query_type);

  /**
   * @brief  Builds a DNS response packet for an A record query.
   * @param  query_packet Original DNS query packet (for copying transaction ID and flags).
   * @param  query_length Length of the original query packet.
   * @param  response_packet Output buffer for the response packet.
   * @param  max_response_len Maximum size of response buffer.
   * @return Length of the response packet, or 0 if buffer too small.
   */
  size_t BuildDnsResponse(const uint8_t* query_packet, size_t query_length,
                          uint8_t* response_packet, size_t max_response_len);

  /**
   * @brief  Checks rate limiting for a client IP address.
   * @param  client_ip Client IPv4 address in network byte order.
   * @return True if request is allowed, false if rate limit exceeded (>100 queries/sec).
   */
  bool CheckRateLimit(uint32_t client_ip);

  int socket_fd_;                    ///< UDP socket file descriptor (-1 if not open).
  uint32_t target_ip_;               ///< Target IP address to return in DNS responses (host byte order).
  bool running_;                     ///< True if server task is active.
  void* task_handle_;                ///< FreeRTOS task handle (TaskHandle_t, stored as void*).

  static constexpr uint16_t kDnsPort = 53;                 ///< DNS server port number.
  static constexpr size_t kMaxDnsPacketSize = 512;         ///< Maximum DNS packet size (RFC 1035).
  static constexpr uint32_t kDefaultTargetIP = 0xC0A80401; ///< Default target IP: 192.168.4.1 (host byte order).
  static constexpr uint8_t kMaxQueriesPerSecond = 100;     ///< Rate limit: max 100 queries/sec per client.
};

}  // namespace captive_portal
