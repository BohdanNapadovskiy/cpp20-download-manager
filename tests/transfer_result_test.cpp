#include "core/http_transfer.hpp"
#include "test_printers.hpp"

#include <gtest/gtest.h>

using dm::core::TransferError;
using dm::core::TransferResult;

// Success rule (see TransferResult::ok): no error AND the status is 200 or 206.
namespace {
    TEST(TransferResultOk, FullBodyIsSuccess) {
        EXPECT_TRUE((TransferResult{.error = TransferError::None, .http_status_code = 200}).ok());
    }

    TEST(TransferResultOk, PartialContentIsSuccess) {
        EXPECT_TRUE((TransferResult{.error = TransferError::None, .http_status_code = 206}).ok());
    }

    TEST(TransferResultOk, NoContentIsNotSuccess) {
        EXPECT_FALSE((TransferResult{.error = TransferError::None, .http_status_code = 204}).ok());
    }

    TEST(TransferResultOk, DefaultConstructedIsNotSuccess) {
        EXPECT_FALSE(TransferResult{}.ok());
    }

    TEST(TransferResultOk, AnyErrorIsNotSuccessEvenWith200) {
        for (const auto error : {TransferError::Network, TransferError::Tls,
                                 TransferError::DiskWrite, TransferError::HttpStatus}) {
            EXPECT_FALSE((TransferResult{.error = error, .http_status_code = 200}).ok())
                    << "error " << ::testing::PrintToString(error);
        }
    }
}
