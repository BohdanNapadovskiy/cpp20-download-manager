#include "core/http_transfer.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <fstream>
#include <filesystem>
#include <cstddef>
#include <charconv>
#include <string_view>
#include <curl/curl.h>

namespace dm::core {
    namespace {
        struct FileWriteState;

        struct ProgressContext {
            const std::stop_token *stop_token = nullptr;
            const FileWriteState *state = nullptr;
            const std::function<void(std::uint64_t, std::uint64_t)> *on_progress = nullptr;
        };

        using EasyHandle = std::unique_ptr<CURL, decltype([](CURL *h) {
            curl_easy_cleanup(h);
        })>;

        enum class WriteFailure { None, Open, Write, Close, Exception };

        struct FileWriteState {
            std::filesystem::path part_path;
            std::uint64_t resume_from = 0;
            std::ofstream stream{};
            std::uintmax_t bytes_transferred = 0;
            int http_status = 0;
            WriteFailure failure = WriteFailure::None;
        };

        // path::string() converts to the ANSI code page on Windows and mangles non-ASCII names.
        std::string to_utf8(const std::filesystem::path &path) {
            const std::u8string utf8 = path.u8string();
            return {reinterpret_cast<const char *>(utf8.data()), utf8.size()};
        }

        // Message in the "<function>() failed: <reason>" format from README.
        std::string describe(WriteFailure failure, const std::filesystem::path &path) {
            const std::string file = to_utf8(path);
            switch (failure) {
                case WriteFailure::Open: return "open() failed: cannot open " + file + " for writing";
                case WriteFailure::Write: return "write() failed: cannot write to " + file;
                case WriteFailure::Close: return "close() failed: cannot flush " + file;
                case WriteFailure::Exception: return "write_callback() failed: unexpected exception writing " + file;
                case WriteFailure::None: break;
            }
            return {};
        }

        std::size_t header_callback(char *ptr, std::size_t size, std::size_t count, void *data) {
            const std::size_t total_size = size * count;
            auto *s = static_cast<FileWriteState *>(data);

            if (!s) {
                return 0;
            }
            std::string_view header_line(ptr, total_size);
            if (header_line.starts_with("HTTP/")) {
                std::size_t first_space = header_line.find(' ');
                if (first_space != std::string_view::npos) {
                    std::size_t status_pos = first_space + 1;
                    std::size_t length = (header_line.size() - status_pos);

                    if (length > 0) {
                        std::string_view status_str = header_line.substr(status_pos, length);
                        int parsed_code = 0;
                        auto res = std::from_chars(status_str.data(), status_str.data() + status_str.size(),
                                                   parsed_code);
                        if (res.ec == std::errc{}) {
                            s->http_status = parsed_code;
                        }
                    }
                }
            }
            return total_size;
        }

        std::size_t write_callback(char *ptr, std::size_t size, std::size_t nmemb, void *data) {
            auto *s = static_cast<FileWriteState *>(data);
            try {
                if (!s) {
                    return 0;
                }
                const std::size_t bytes_to_write = size * nmemb;
                if (!is_success_status(s->http_status)) {
                    return bytes_to_write;
                }
                if (!s->stream.is_open()) {
                    std::ios::openmode mode = std::ios::out | std::ios::binary;
                    if (s->http_status == 206) {
                        mode |= std::ios::app;
                    }
                    s->stream.open(s->part_path, mode);
                    if (!s->stream) {
                        s->failure = WriteFailure::Open;
                        return 0;
                    }
                }
                s->stream.write(ptr, static_cast<std::streamsize>(bytes_to_write));
                if (!s->stream) {
                    s->failure = WriteFailure::Write;
                    return 0;
                }
                s->bytes_transferred += bytes_to_write;
                return bytes_to_write;
            } catch (...) {
                if (s != nullptr) {
                    s->failure = WriteFailure::Exception;
                }
                return 0;
            }
        }

        int xferinfo_callback(void *clientp,
                              curl_off_t dltotal,
                              curl_off_t dlnow,
                              curl_off_t /*ultotal*/,
                              curl_off_t /*ulnow*/) noexcept {
            const auto *ctx = static_cast<const ProgressContext *>(clientp);
            if (ctx == nullptr) {
                return 0;
            }
            if (ctx->stop_token != nullptr && ctx->stop_token->stop_requested()) {
                return 1;
            }
            try {
                if (ctx->on_progress != nullptr && *ctx->on_progress) {
                    const std::uint64_t base = (ctx->state != nullptr && ctx->state->http_status == 206)
                                                   ? ctx->state->resume_from
                                                   : 0;
                    const auto current_dl = dlnow > 0 ? static_cast<std::uint64_t>(dlnow) : std::uint64_t{0};
                    const std::uint64_t downloaded = base + current_dl;
                    const std::uint64_t total = dltotal > 0
                                                    ? base + static_cast<std::uint64_t>(dltotal)
                                                    : std::uint64_t{0};
                    (*ctx->on_progress)(downloaded, total);
                }
            } catch (...) {
                // Ігноруємо виняток спостерігача, щоб не переривати передачу
                // і не маскувати збій під TransferError::Aborted
            }
            return 0;
        }

        EasyHandle create_easy_handle() {
            CURL *handle = curl_easy_init();
            if (handle == nullptr) {
                throw std::runtime_error("curl_easy_init() failed: returned nullptr");
            }
            return EasyHandle{handle};
        }

        [[nodiscard]] bool is_tls_error(CURLcode code) noexcept {
            switch (code) {
                case CURLE_PEER_FAILED_VERIFICATION:
                case CURLE_SSL_CONNECT_ERROR:
                    return true;
                default:
                    return false;
            }
        }

        std::string curl_error_message(const char *error_buffer, CURLcode code) {
            if (error_buffer[0] != '\0') {
                return error_buffer;
            } else {
                return curl_easy_strerror(code);
            }
        }
    }

    TransferResult download_once(const TransferRequest &request) {
        EasyHandle handle = create_easy_handle();
        char error_buffer[CURL_ERROR_SIZE];
        error_buffer[0] = '\0';
        long response_code = 0;
        const long connect_timeout_s = static_cast<long>(request.connect_timeout.count());
        const long low_speed_time = static_cast<long>(request.low_speed_time.count());
        std::error_code size_ec;
        std::uint64_t resume_from = std::filesystem::file_size(request.part_path, size_ec);
        if (size_ec) {
            resume_from = 0;
        }
        FileWriteState state{.part_path = request.part_path, .resume_from = resume_from};
        ProgressContext progress_context{
            .stop_token = &request.stop_token, .state = &state, .on_progress = &request.on_progress
        };
        curl_easy_setopt(handle.get(), CURLOPT_URL, request.url.c_str());
        curl_easy_setopt(handle.get(), CURLOPT_ERRORBUFFER, error_buffer);
        curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &state);
        curl_easy_setopt(handle.get(), CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(handle.get(), CURLOPT_HEADERDATA, &state);
        curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(handle.get(), CURLOPT_MAXREDIRS, 10L);
        curl_easy_setopt(handle.get(), CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT, connect_timeout_s);
        curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(handle.get(), CURLOPT_XFERINFOFUNCTION, xferinfo_callback);
        curl_easy_setopt(handle.get(), CURLOPT_XFERINFODATA, &progress_context);
        curl_easy_setopt(handle.get(), CURLOPT_LOW_SPEED_LIMIT, request.low_speed_limit_bps);
        curl_easy_setopt(handle.get(), CURLOPT_LOW_SPEED_TIME, low_speed_time);
        auto resume_offset = static_cast<curl_off_t>(state.resume_from);
        curl_easy_setopt(handle.get(), CURLOPT_RESUME_FROM_LARGE, resume_offset);
        const CURLcode code = curl_easy_perform(handle.get());
        auto info_code = curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &response_code);
        if (info_code != CURLE_OK) {
            response_code = 0;
        }
        const int http_status = static_cast<int>(response_code);
        if (state.stream.is_open()) {
            state.stream.close();
            if (state.stream.fail() && state.failure == WriteFailure::None) {
                state.failure = WriteFailure::Close;
            }
        }
        TransferError error = TransferError::None;
        std::string error_msg;
        if (state.failure != WriteFailure::None) {
            error = TransferError::DiskWrite;
            error_msg = describe(state.failure, state.part_path);
        } else if (is_tls_error(code)) {
            error = TransferError::Tls;
            error_msg = "download_once() failed: TLS certificate issues: " + curl_error_message(error_buffer, code);
        } else if (code == CURLE_ABORTED_BY_CALLBACK) {
            error = TransferError::Aborted;
            error_msg = "download_once() aborted: stop requested";
        } else if (code == CURLE_RANGE_ERROR || (code == CURLE_OK && http_status == 416 && state.resume_from > 0) ) {
            error = TransferError::ResumeRejected;
            error_msg = "download_once() failed: server refused to resume from byte: " + std::to_string(state.resume_from);
            std::error_code remove_ec;
            std::filesystem::remove(state.part_path, remove_ec);
        } else if (code != CURLE_OK) {
            error_msg = curl_error_message(error_buffer, code);
            error = TransferError::Network;
        } else if (!is_success_status(http_status)) {
            error = TransferError::HttpStatus;
            error_msg = "download_once() failed: server responded with HTTP " + std::to_string(http_status);
        }
        return TransferResult{
            .error = error,
            .http_status_code = http_status,
            .error_message = error_msg,
            .bytes_transferred = state.bytes_transferred
        };
    }
}
