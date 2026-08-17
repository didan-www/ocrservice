if(NOT DEFINED CROW_SOURCE_DIR)
    message(FATAL_ERROR "CROW_SOURCE_DIR is required")
endif()

function(ocr_patch_crow relative_path original replacement)
    set(path "${CROW_SOURCE_DIR}/${relative_path}")
    file(READ "${path}" content)
    if(ARGC GREATER 3)
        set(marker "${ARGV3}")
    else()
        set(marker "${replacement}")
    endif()
    string(FIND "${content}" "${marker}" replacement_position)
    if(NOT replacement_position EQUAL -1)
        return()
    endif()
    string(FIND "${content}" "${original}" original_position)
    if(original_position EQUAL -1)
        if(ARGC GREATER 4)
            set(original "${ARGV4}")
            string(FIND "${content}" "${original}" original_position)
        endif()
        if(original_position EQUAL -1)
            message(FATAL_ERROR "Crow 1.2.1 patch anchor not found: ${relative_path}")
        endif()
    endif()
    string(REPLACE "${original}" "${replacement}" content "${content}")
    file(WRITE "${path}" "${content}")
endfunction()

ocr_patch_crow(
    "include/crow/parser.h"
    [=[            self->set_connection_parameters();

            self->process_header();]=]
    [=[            self->set_connection_parameters();
            const std::size_t maximum = self->handler_->maximum_body_size(self->req);
            if (self->content_length != CROW_ULLONG_MAX && self->content_length > maximum)
            {
                self->error_kind_ = http_parse_error_kind::body_too_large;
                return -1;
            }

            self->process_header();]=]
)

ocr_patch_crow(
    "include/crow/parser.h"
    [=[#include <algorithm>

#include "crow/http_request.h"]=]
    [=[#include <algorithm>
#include <cstddef>

#include "crow/http_request.h"]=]
)

ocr_patch_crow(
    "include/crow/parser.h"
    [=[namespace crow
{
    /// A wrapper for `nodejs/http-parser`.]=]
    [=[namespace crow
{
    enum class http_parse_error_kind
    {
        malformed,
        header_too_large,
        url_too_large,
        body_too_large
    };

    /// A wrapper for `nodejs/http-parser`.]=]
)

ocr_patch_crow(
    "include/crow/parser.h"
    [=[            HTTPParser* self = static_cast<HTTPParser*>(self_);
            self->req.raw_url.insert(self->req.raw_url.end(), at, at + length);
            self->req.url_params = query_string(self->req.raw_url);]=]
    [=[            HTTPParser* self = static_cast<HTTPParser*>(self_);
            const std::size_t maximum = self->handler_->maximum_url_size();
            if (self->req.raw_url.size() > maximum || length > maximum - self->req.raw_url.size())
            {
                self->error_kind_ = http_parse_error_kind::url_too_large;
                return -1;
            }
            self->req.raw_url.insert(self->req.raw_url.end(), at, at + length);
            self->req.url_params = query_string(self->req.raw_url);]=]
)

ocr_patch_crow(
    "include/crow/parser.h"
    [=[            HTTPParser* self = static_cast<HTTPParser*>(self_);
            self->req.body.insert(self->req.body.end(), at, at + length);
            return 0;]=]
    [=[            HTTPParser* self = static_cast<HTTPParser*>(self_);
            const std::size_t maximum = self->handler_->maximum_body_size(self->req);
            if (self->req.body.size() > maximum || length > maximum - self->req.body.size())
            {
                self->error_kind_ = http_parse_error_kind::body_too_large;
                return -1;
            }
            self->req.body.insert(self->req.body.end(), at, at + length);
            return 0;]=]
)

ocr_patch_crow(
    "include/crow/parser.h"
    [=[            if (http_errno != CHPE_OK)
            {
                return false;
            }]=]
    [=[            if (http_errno != CHPE_OK)
            {
                if (http_errno == CHPE_HEADER_OVERFLOW)
                    error_kind_ = http_parse_error_kind::header_too_large;
                return false;
            }]=]
)

ocr_patch_crow(
    "include/crow/parser.h"
    [=[            message_complete = false;
            state = CROW_NEW_MESSAGE();]=]
    [=[            message_complete = false;
            error_kind_ = http_parse_error_kind::malformed;
            state = CROW_NEW_MESSAGE();]=]
)

ocr_patch_crow(
    "include/crow/parser.h"
    [=[        /// The final request that this parser outputs.
        ///
        /// Data parsed is put directly into this object as soon as the related callback returns. (e.g. the request will have the cooorect method as soon as on_method() returns)
        request req;]=]
    [=[        http_parse_error_kind error_kind() const noexcept
        {
            return error_kind_;
        }

        /// The final request that this parser outputs.
        ///
        /// Data parsed is put directly into this object as soon as the related callback returns. (e.g. the request will have the cooorect method as soon as on_method() returns)
        request req;]=]
)

ocr_patch_crow(
    "include/crow/parser.h"
    [=[        std::string header_value;

        Handler* handler_;]=]
    [=[        std::string header_value;
        http_parse_error_kind error_kind_{http_parse_error_kind::malformed};

        Handler* handler_;]=]
)

ocr_patch_crow(
    "include/crow/app.h"
    [=[        /// \brief Create a dynamic route using a rule (**Use CROW_ROUTE instead**)
        DynamicRule& route_dynamic(const std::string& rule)]=]
    [=[        self_t& max_request_url_size(std::size_t bytes)
        {
            max_request_url_size_ = bytes;
            return *this;
        }

        std::size_t max_request_url_size() const noexcept
        {
            return max_request_url_size_;
        }

        template<typename Func>
        self_t& request_body_limit(Func&& f)
        {
            request_body_limit_handler_ = std::forward<Func>(f);
            return *this;
        }

        std::size_t request_body_limit_for(const request& req) const
        {
            return request_body_limit_handler_(req);
        }

        template<typename Func>
        self_t& parser_error_handler(Func&& f)
        {
            parser_error_handler_ = std::forward<Func>(f);
            return *this;
        }

        void handle_parser_error(const request& req, response& res, http_parse_error_kind kind)
        {
            try
            {
                parser_error_handler_(req, res, kind);
            }
            catch (...)
            {
                res = response(500);
            }
        }

        /// \brief Create a dynamic route using a rule (**Use CROW_ROUTE instead**)
        DynamicRule& route_dynamic(const std::string& rule)]=]
    "self_t& max_request_url_size(std::size_t bytes)"
)

ocr_patch_crow(
    "include/crow/app.h"
    "std::size_t request_body_limit(const request& req) const"
    "std::size_t request_body_limit_for(const request& req) const"
)

ocr_patch_crow(
    "include/crow/app.h"
    [=[        uint64_t max_payload_{UINT64_MAX};
        std::string server_name_]=]
    [=[        uint64_t max_payload_{UINT64_MAX};
        std::size_t max_request_url_size_{static_cast<std::size_t>(-1)};
        std::function<std::size_t(const request&)> request_body_limit_handler_ =
          [](const request&) { return static_cast<std::size_t>(-1); };
        std::function<void(const request&, response&, http_parse_error_kind)> parser_error_handler_ =
          [](const request&, response& res, http_parse_error_kind) { res = response(400); };
        std::string server_name_]=]
)

ocr_patch_crow(
    "include/crow/http_connection.h"
    [=[        void start()
        {]=]
    [=[        std::size_t maximum_url_size() const noexcept
        {
            return handler_->max_request_url_size();
        }

        std::size_t maximum_body_size(const request& req) const
        {
            return handler_->request_body_limit_for(req);
        }

        void start()
        {]=]
    "std::size_t maximum_url_size() const noexcept"
)

ocr_patch_crow(
    "include/crow/http_connection.h"
    "return handler_->request_body_limit(req);"
    "return handler_->request_body_limit_for(req);"
)

ocr_patch_crow(
    "include/crow/http_connection.h"
    [=[if (error_while_reading)
                  {
                      self->cancel_deadline_timer();
                      self->parser_.done();
                      self->adaptor_.shutdown_read();
                      self->adaptor_.close();
                      CROW_LOG_DEBUG << self << " from read(1) with description: \"" << http_errno_description(static_cast<http_errno>(self->parser_.http_errno)) << '\"';
                  }]=]
    [=[if (error_while_reading)
                  {
                      self->cancel_deadline_timer();
                      const bool eof_while_reading = ec == asio::error::eof;
                      const bool parser_failed = !ec;
                      const bool incomplete_at_eof = eof_while_reading && !self->parser_.done();
                      if ((parser_failed || incomplete_at_eof) && self->adaptor_.is_open())
                      {
                          self->close_connection_ = true;
                          self->handler_->handle_parser_error(self->req_, self->res, self->parser_.error_kind());
                          self->complete_request();
                      }
                      else
                      {
                          self->adaptor_.shutdown_read();
                          self->adaptor_.close();
                      }
                  }]=]
    "const bool eof_while_reading = ec == asio::error::eof;"
    [=[if (error_while_reading)
                  {
                      self->cancel_deadline_timer();
                      if (!ec && self->adaptor_.is_open())
                      {
                          self->close_connection_ = true;
                          self->handler_->handle_parser_error(self->req_, self->res, self->parser_.error_kind());
                          self->complete_request();
                      }
                      else
                      {
                          self->adaptor_.shutdown_read();
                          self->adaptor_.close();
                      }
                  }]=]
)

ocr_patch_crow(
    "include/crow/routing.h"
    "else if (req.method == HTTPMethod::Head)"
    "else if (req.method == HTTPMethod::Head && CROW_OCR_AUTOMATIC_HEAD_OPTIONS)"
)

ocr_patch_crow(
    "include/crow/routing.h"
    "else if (req.method == HTTPMethod::Options)"
    "else if (req.method == HTTPMethod::Options && CROW_OCR_AUTOMATIC_HEAD_OPTIONS)"
)

ocr_patch_crow(
    "include/crow/routing.h"
    [=[                CROW_LOG_INFO << "Redirecting to a url with trailing slash: " << req.url;
                res = response(301);

                // TODO(ipkn) absolute url building
                if (req.get_header_value("Host").empty())
                {
                    res.add_header("Location", req.url + "/");
                }
                else
                {
                    res.add_header("Location", "http://" + req.get_header_value("Host") + req.url + "/");
                }
                res.end();]=]
    [=[                res = response(404);
                res.end();]=]
)

ocr_patch_crow(
    "include/crow/routing.h"
    [=[                get_error(404, found, req, res);
                res.end();]=]
    [=[                res = response(404);
                res.end();]=]
)

ocr_patch_crow(
    "include/crow/http_response.h"
    [=[            completed_ = false;
            file_info = static_file_info{};]=]
    [=[            completed_ = false;
            skip_body = false;
            manual_length_header = false;
            is_alive_helper_ = nullptr;
            stream_start_handler_ = nullptr;
            stream_write_handler_ = nullptr;
            stream_finish_handler_ = nullptr;
            stream_abort_handler_ = nullptr;
            file_info = static_file_info{};]=]
    "stream_start_handler_ = nullptr;"
)

ocr_patch_crow(
    "include/crow/http_response.h"
    [=[            completed_ = false;
            is_alive_helper_ = nullptr;]=]
    [=[            completed_ = false;
            skip_body = false;
            manual_length_header = false;
            is_alive_helper_ = nullptr;]=]
)

ocr_patch_crow(
    "include/crow/http_response.h"
    [=[#include <string>
#include <unordered_map>]=]
    [=[#include <string>
#include <string_view>
#include <functional>
#include <unordered_map>]=]
)

ocr_patch_crow(
    "include/crow/http_response.h"
    [=[        /// Check whether the response has a static file defined.
        bool is_static_type()]=]
    [=[        bool start_stream()
        {
            return stream_start_handler_ && stream_start_handler_();
        }

        bool write_stream(std::string_view body_part)
        {
            return stream_write_handler_ && stream_write_handler_(body_part);
        }

        bool finish_stream()
        {
            return stream_finish_handler_ && stream_finish_handler_();
        }

        void abort_stream() noexcept
        {
            if (stream_abort_handler_)
                stream_abort_handler_();
        }

        /// Check whether the response has a static file defined.
        bool is_static_type()]=]
)

ocr_patch_crow(
    "include/crow/http_response.h"
    [=[        std::function<bool()> is_alive_helper_;
        static_file_info file_info;]=]
    [=[        std::function<bool()> is_alive_helper_;
        std::function<bool()> stream_start_handler_;
        std::function<bool(std::string_view)> stream_write_handler_;
        std::function<bool()> stream_finish_handler_;
        std::function<void()> stream_abort_handler_;
        static_file_info file_info;]=]
)

ocr_patch_crow(
    "include/crow/http_connection.h"
    [=[                res.is_alive_helper_ = [self]() -> bool {
                    return self->adaptor_.is_open();
                };]=]
    [=[                std::weak_ptr<Connection> weak_self = self;
                res.is_alive_helper_ = [weak_self]() -> bool {
                    const auto locked = weak_self.lock();
                    return locked && locked->adaptor_.is_open();
                };
                res.stream_start_handler_ = [weak_self]() {
                    const auto locked = weak_self.lock();
                    return locked && locked->start_stream();
                };
                res.stream_write_handler_ = [weak_self](std::string_view part) {
                    const auto locked = weak_self.lock();
                    return locked && locked->write_stream(part);
                };
                res.stream_finish_handler_ = [weak_self]() {
                    const auto locked = weak_self.lock();
                    return locked && locked->finish_stream();
                };
                res.stream_abort_handler_ = [weak_self]() {
                    const auto locked = weak_self.lock();
                    if (locked)
                        locked->abort_stream();
                };]=]
    "std::weak_ptr<Connection> weak_self = self;"
)

ocr_patch_crow(
    "include/crow/http_connection.h"
    [=[        void do_write_general()
        {]=]
    [=[        bool write_sync(const std::vector<asio::const_buffer>& buffers)
        {
            error_code ec;
            asio::write(adaptor_.socket(), buffers, ec);
            if (ec)
            {
                adaptor_.shutdown_readwrite();
                adaptor_.close();
                return false;
            }
            return true;
        }

        bool start_stream()
        {
            if (streaming_ || !adaptor_.is_open())
                return false;
            streaming_ = true;
            res.completed_ = true;
            cancel_deadline_timer();
            if (need_to_call_after_handlers_)
            {
                need_to_call_after_handlers_ = false;
                detail::after_handlers_call_helper<
                  detail::middleware_call_criteria_only_global,
                  (static_cast<int>(sizeof...(Middlewares)) - 1),
                  decltype(ctx_),
                  decltype(*middlewares_)>({}, *middlewares_, ctx_, req_, res);
            }
            stream_chunked_ = !res.headers.count("content-length");
            if (stream_chunked_)
            {
                res.set_header("Transfer-Encoding", "chunked");
                res.manual_length_header = true;
            }
            prepare_buffers();
            const bool written = write_sync(buffers_);
            buffers_.clear();
            if (!written)
                streaming_ = false;
            return written;
        }

        bool write_stream(std::string_view part)
        {
            if (!streaming_ || !adaptor_.is_open())
                return false;
            if (part.empty())
                return true;
            std::vector<asio::const_buffer> output;
            std::string size;
            static const std::string crlf = "\r\n";
            if (stream_chunked_)
            {
                std::ostringstream encoded;
                encoded << std::hex << part.size();
                size = encoded.str();
                output = {asio::buffer(size), asio::buffer(crlf), asio::buffer(part.data(), part.size()), asio::buffer(crlf)};
            }
            else
            {
                output = {asio::buffer(part.data(), part.size())};
            }
            return write_sync(output);
        }

        bool finish_stream()
        {
            if (!streaming_ || !adaptor_.is_open())
                return false;
            bool written = true;
            if (stream_chunked_)
            {
                static const std::string ending = "0\r\n\r\n";
                const std::vector<asio::const_buffer> output{asio::buffer(ending)};
                written = write_sync(output);
            }
            streaming_ = false;
            stream_completed_ = written;
            if (close_connection_ && adaptor_.is_open())
            {
                adaptor_.shutdown_readwrite();
                adaptor_.close();
            }
            return written;
        }

        void abort_stream() noexcept
        {
            streaming_ = false;
            stream_completed_ = false;
            if (adaptor_.is_open())
            {
                adaptor_.shutdown_readwrite();
                adaptor_.close();
            }
        }

        void do_write_general()
        {]=]
    "bool write_sync(const std::vector<asio::const_buffer>& buffers)"
)

ocr_patch_crow(
    "include/crow/http_connection.h"
    [=[            if (!streaming_ || !adaptor_.is_open())
                return false;
            std::vector<asio::const_buffer> output;]=]
    [=[            if (!streaming_ || !adaptor_.is_open())
                return false;
            if (part.empty())
                return true;
            std::vector<asio::const_buffer> output;]=]
)

ocr_patch_crow(
    "include/crow/http_connection.h"
    [=[else if (!self->need_to_call_after_handlers_)
                  {
                      self->start_deadline();
                      self->do_read();
                  }]=]
    [=[else if (!self->need_to_call_after_handlers_)
                  {
                      if (self->stream_completed_)
                      {
                          self->stream_completed_ = false;
                          self->res.clear();
                          self->parser_.clear();
                      }
                      self->start_deadline();
                      self->do_read();
                  }]=]
)

ocr_patch_crow(
    "include/crow/http_connection.h"
    [=[        bool close_connection_ = false;

        const std::string& server_name_;]=]
    [=[        bool close_connection_ = false;
        bool streaming_ = false;
        bool stream_chunked_ = false;
        bool stream_completed_ = false;

        const std::string& server_name_;]=]
)

ocr_patch_crow(
    "include/crow/http_server.h"
    "            shutting_down_ = true; // Prevent the acceptor from taking new connections"
    "            shutting_down_.store(true, std::memory_order_release); // Prevent the acceptor from taking new connections"
)

ocr_patch_crow(
    "include/crow/http_server.h"
    "            if (!shutting_down_)"
    "            if (!shutting_down_.load(std::memory_order_acquire))"
)

ocr_patch_crow(
    "include/crow/http_server.h"
    "        bool shutting_down_ = false;"
    "        std::atomic<bool> shutting_down_{false};"
)
