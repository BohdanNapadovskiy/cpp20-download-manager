#pragma once

namespace dm::core {
    enum class TransferError {
        None,
        Network,
        Tls,
        DiskWrite,
        HttpStatus
    };
}

