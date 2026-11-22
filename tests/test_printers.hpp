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
            case TransferError::Aborted:    *os << "Aborted"; return;
            case TransferError::ResumeRejected: *os << "ResumeRejected"; return;
        }
        *os << "TransferError(" << static_cast<int>(error) << ")";
    }

    inline void PrintTo(TaskState state, std::ostream* os) {
        switch (state) {
            case TaskState::Queued:      *os << "Queued"; return;
            case TaskState::Downloading: *os << "Downloading"; return;
            case TaskState::Paused:      *os << "Paused"; return;
            case TaskState::Canceled:    *os << "Canceled"; return;
            case TaskState::Failed:      *os << "Failed"; return;
            case TaskState::Retrying:    *os << "Retrying"; return;
            case TaskState::Completed:   *os << "Completed"; return;
        }
        *os << "TaskState(" << static_cast<int>(state) << ")";
    }
}
