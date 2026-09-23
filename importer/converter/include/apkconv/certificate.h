// X.509 certificates, as far as a code signature needs them (ADR-0022): who issued a certificate
// and to whom, for building the chain and for the signer's IssuerAndSerialNumber in the CMS
// signature; the subject's common name, which the designated requirement names; its
// organizational unit, which is the team ID in an Apple certificate; and its validity.
#pragma once

#include "apkconv/der.h"

#include <chrono>
#include <span>
#include <string>

namespace apk::x509 {

struct Certificate {
    der::Bytes encoding;       // the whole certificate
    der::Bytes issuer;         // the issuer's Name, as encoded
    der::Bytes subject;        // the subject's Name, as encoded
    der::Bytes serial_number;  // the INTEGER element, as encoded
    std::string common_name;          // subject CN, empty when there is none
    std::string organizational_unit;  // subject OU, the team ID of an Apple certificate
    std::string issuer_common_name;   // issuer CN, for messages about the chain
    std::chrono::sys_seconds not_before{};
    std::chrono::sys_seconds not_after{};

    bool self_issued() const { return issuer == subject; }
};

/// Takes the certificate apart. Throws der::FormatError when it is not an X.509 certificate.
Certificate parse(std::span<const std::byte> encoding);

}  // namespace apk::x509
