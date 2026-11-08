#pragma once

#include "core/types.hpp"

#include <ostream>

// GoogleTest prints unknown types as raw bytes ("4-byte object <01-00 00-00>").
// PrintTo in the type's own namespace makes failures readable: "Network" instead of bytes.
namespace dm::core {
    inline void PrintTo(TransferError error, std::ostream* os) {
        switch (error) {
            case TransferError::None:       *os << "None"; return;
            case TransferError::Network:    *os << "Network"; return;
            case TransferError::Tls:        *os << "Tls"; return;
            case TransferError::DiskWrite:  *os << "DiskWrite"; return;
            case TransferError::HttpStatus: *os << "HttpStatus"; return;
        }
        *os << "TransferError(" << static_cast<int>(error) << ")";
    }
}
