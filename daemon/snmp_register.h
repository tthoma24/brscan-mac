#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "brscan/types.h"

// SNMPv1 registration for the Brother Scan-button destination table: this
// Mac tells the printer, over SNMP Set, where to send a button-press
// notification (this Mac's IP/port), what to call itself in the printer's
// Scan menu, and which destination (FUNC) the registration is for. See
// reference/protocol-notes-button.md for the captured protocol and
// PROVENANCE.md for the source capture.
namespace brscan::scand {

// Brother application numbers for FUNC, as observed in the capture.
constexpr int kAppNumImage = 1;
constexpr int kAppNumEmail = 2;
constexpr int kAppNumOcr = 3;
constexpr int kAppNumFile = 5;

// Default registration lifetime advertised to the printer, in seconds; the
// caller should re-register before this elapses.
constexpr int kDefaultRegistrationDurationSec = 360;

// Composes the OctetString value carried by the SNMP Set: e.g.
// "TYPE=BR;BUTTON=SCAN;DURATION=360;CC=1;HOST=192.0.2.10:54925;
// USER=\"Test Mac\";FUNC=FILE;APPNUM=5;". `ip`/`port` are where the printer
// should send button notifications (this Mac's UDP listener, see
// reference/protocol-notes-button.md section 2); `name` is the computer
// name shown in the printer's Scan menu; `func` is one of
// FILE|IMAGE|EMAIL|OCR; `appnum` is the matching Brother application number
// (see the kAppNum* constants above); `duration_sec` is the registration
// lifetime in seconds.
std::string BuildRegisterValue(
    const std::string& ip, uint16_t port, const std::string& name,
    const std::string& func, int appnum,
    int duration_sec = kDefaultRegistrationDurationSec);

// Encodes a full SNMPv1 set-request PDU registering `value` at the Brother
// button-destination OID 1.3.6.1.4.1.2435.2.3.9.2.11.1.1.0, under
// `community`, tagged with `request_id`. Pure BER construction; does no
// I/O. See reference/protocol-notes-button.md for the byte layout this
// mirrors.
std::vector<uint8_t> BuildSnmpSetRegister(const std::string& community,
                                           uint32_t request_id,
                                           const std::string& value);

// Sends the PDU built by BuildSnmpSetRegister(community, request_id, value)
// to `printer_host` on the standard SNMP port (UDP 161). Fire-and-forget:
// registration is a plain SNMP Set with no application-level reply, so this
// does not wait for or parse a response. Returns Status::kIoError if the
// host cannot be resolved or the packet cannot be handed to the kernel;
// Status::kOk otherwise (a UDP send succeeding is not itself delivery
// confirmation).
Status SendSnmpRegister(const std::string& printer_host,
                         const std::string& community,
                         const std::string& value, uint32_t request_id);

}  // namespace brscan::scand
