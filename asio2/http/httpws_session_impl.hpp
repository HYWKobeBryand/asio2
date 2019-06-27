/*
 * COPYRIGHT (C) 2017, zhllxt
 *
 * author   : zhllxt
 * qq       : 37792738
 * email    : 37792738@qq.com
 * http://blog.csdn.net/zzhongcy/article/details/41981855
 */

#ifndef __ASIO2_HTTPWS_SESSION_IMPL_HPP__
#define __ASIO2_HTTPWS_SESSION_IMPL_HPP__

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
#pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#if defined(ASIO2_USE_HTTP)

#include <asio2/tcp/tcp_session_impl.hpp>

#include <asio2/http/http_request.hpp>
#include <asio2/http/http_response.hpp>
#include <asio2/http/http_file_response.hpp>
#include <asio2/http/ws_msg.hpp>
#include <asio2/http/http_util.hpp>

namespace asio2
{

	class httpws_session_impl : public tcp_session_impl
	{
		template<class _session_impl_t> friend class tcp_acceptor_impl;

		template<class _session_impl_t> friend class http_acceptor_impl;

	public:
		/**
		 * @construct
		 */
		explicit httpws_session_impl(
			std::shared_ptr<url_parser>             url_parser_ptr,
			std::shared_ptr<listener_mgr>           listener_mgr_ptr,
			std::shared_ptr<asio::io_context>       io_context_ptr,
			std::shared_ptr<session_mgr>            session_mgr_ptr
		)
			: tcp_session_impl(
				url_parser_ptr,
				listener_mgr_ptr,
				io_context_ptr,
				session_mgr_ptr
			)
		{
		}

		/**
		 * @destruct
		 */
		virtual ~httpws_session_impl()
		{
		}

		virtual bool start() override
		{
			this->m_state = state::starting;

			// reset the variable to default status
			this->m_fire_close_is_called.clear(std::memory_order_release);
			this->m_last_active_time = std::chrono::system_clock::now();
			this->m_connect_time = std::chrono::system_clock::now();

			this->m_state = state::started;

			auto this_ptr(shared_from_this());

			this->_fire_accept(this_ptr);

			// user has called stop in the listener function,so we can't start continue.
			if (this->m_state != state::started)
				return false;

			try
			{
				// set keeplive
				set_keepalive_vals(this->m_socket);

				// setsockopt SO_SNDBUF from url params
				if (this->m_url_parser_ptr->get_so_sndbuf_size() > 0)
				{
					asio::socket_base::send_buffer_size option(this->m_url_parser_ptr->get_so_sndbuf_size());
					this->m_socket.set_option(option);
				}

				// setsockopt SO_RCVBUF from url params
				if (this->m_url_parser_ptr->get_so_rcvbuf_size() > 0)
				{
					asio::socket_base::receive_buffer_size option(this->m_url_parser_ptr->get_so_rcvbuf_size());
					this->m_socket.set_option(option);
				}
			}
			catch (system_error & e)
			{
				set_last_error(e.code().value());
			}

			this->m_state = state::running;

			if (!this->m_session_mgr_ptr->start(this_ptr))
			{
				this->stop();
				return false;
			}

			// start the timer of check silence timeout
			if (this->m_url_parser_ptr->get_silence_timeout() > 0)
			{
				this->m_timer.expires_from_now(std::chrono::seconds(this->m_url_parser_ptr->get_silence_timeout()));
				this->m_timer.async_wait(
					this->m_strand_ptr->wrap(std::bind(&httpws_session_impl::_handle_timer, this,
						std::placeholders::_1, // error_code
						this_ptr
					)));
			}

			// to avlid the user call stop in another thread,then it may be m_socket.async_read_some and m_socket.close be called at the same time
			this->m_strand_ptr->post([this, this_ptr]()
			{
				this->_post_recv(std::move(this_ptr), std::make_shared<http_request>());
			});

			return true;
		}

		virtual void stop() override
		{
			if (this->m_state >= state::starting)
			{
				auto prev_state = this->m_state;
				this->m_state = state::stopping;

				// close the socket by post a event
				// asio don't allow operate the same socket in multi thread,if you close socket in one thread and another thread is 
				// calling socket's async_... function,it will crash.so we must care for operate the socket.when need close the
				// socket ,we use the strand to post a event,make sure the socket's close operation is in the same thread.
				try
				{
					auto this_ptr(this->shared_from_this());
					this->m_strand_ptr->post([this, this_ptr, prev_state]() mutable
					{
						if (prev_state == state::running)
							this->_fire_close(this_ptr, get_last_error());

						// call socket's close function to notify the _handle_recv function response with error > 0 ,then the socket 
						// can get notify to exit
						if (this->get_socket().is_open())
						{
							// close the socket
							error_code ec;
							this->get_socket().shutdown(asio::socket_base::shutdown_both, ec);
							// must ensure the close function has been called,otherwise the _handle_recv will never return
							this->get_socket().close(ec);
							set_last_error(ec.value());
						}

						// clost the timer
						this->m_timer.cancel();

						if (this->m_ping_timer_ptr)
						{
							this->m_ping_timer_ptr->cancel();
						}

						this->m_state = state::stopped;

						// remove this session from the session map
						this->m_session_mgr_ptr->stop(this_ptr);
					});
				}
				catch (std::exception &) {}
			}

			tcp_session_impl::stop();
		}

		/**
		 * @function : whether the session is started
		 */
		virtual bool is_started() override
		{
			if (this->m_websocket_ptr)
				return ((this->m_state >= state::started) && this->m_websocket_ptr->is_open());
			return tcp_session_impl::is_started();
		}

		/**
		 * @function : check whether the session is stopped
		 */
		virtual bool is_stopped() override
		{
			if (this->m_websocket_ptr)
				return ((this->m_state == state::stopped) && !this->m_websocket_ptr->is_open());
			return tcp_session_impl::is_stopped();
		}

		/**
		 * @function : send data
		 * note : cannot be executed at the same time in multithreading when "async == false"
		 */
		virtual bool send(const std::shared_ptr<buffer<uint8_t>> & buf_ptr) override
		{
			if (this->m_websocket_ptr)
			{
				return this->send(std::make_shared<ws_msg>(buf_ptr->data(), buf_ptr->size()));
			}
			else
			{
				error_code ec;
				boost::beast::http::response_parser<boost::beast::http::string_body> parser;
				parser.put(asio::buffer(buf_ptr->data(), buf_ptr->size()), ec);
				if (!ec)
				{
					auto http_msg_ptr = std::make_shared<http_response>(std::move(parser.get()));
					return this->send(http_msg_ptr);
				}
				set_last_error(ec.value());
			}

			return false;
		}

		/**
		 * @function : send data
		 * just used for http protocol
		 */
		virtual bool send(const std::shared_ptr<http_msg> & http_msg_ptr) override
		{
			// We must ensure that there is only one operation to send data at the same time,otherwise may be cause crash.
			if (is_started() && http_msg_ptr)
			{
				try
				{
					// must use strand.post to send data.why we should do it like this ? see udp_session._post_send.
					this->m_strand_ptr->post(std::bind(&httpws_session_impl::_post_send, this,
						shared_from_this(),
						http_msg_ptr
					));
					return true;
				}
				catch (std::exception &) {}
			}
			else if (!this->m_socket.is_open())
			{
				set_last_error((int)errcode::socket_not_ready);
			}
			else
			{
				set_last_error((int)errcode::invalid_parameter);
			}
			return false;
		}

	public:
		/**
		 * @function : get the socket refrence
		 */
		inline asio::ip::tcp::socket::lowest_layer_type & get_socket()
		{
			if (this->m_websocket_ptr)
			{
				return this->m_websocket_ptr->lowest_layer();
			}
			return this->m_socket;
		}

		/**
		 * @function : get the socket refrence
		 */
		inline boost::beast::websocket::stream<asio::ip::tcp::socket> & get_stream()
		{
			return (*(this->m_websocket_ptr));
		}

		/**
		 * @function : get the local address
		 */
		virtual std::string get_local_address() override
		{
			try
			{
				if (this->get_socket().is_open())
				{
					return this->get_socket().local_endpoint().address().to_string();
				}
			}
			catch (system_error & e)
			{
				set_last_error(e.code().value());
			}
			return std::string();
		}

		/**
		 * @function : get the local port
		 */
		virtual unsigned short get_local_port() override
		{
			try
			{
				if (this->get_socket().is_open())
				{
					return this->get_socket().local_endpoint().port();
				}
			}
			catch (system_error & e)
			{
				set_last_error(e.code().value());
			}
			return 0;
		}

		/**
		 * @function : get the remote address
		 */
		virtual std::string get_remote_address() override
		{
			try
			{
				if (this->get_socket().is_open())
				{
					return this->get_socket().remote_endpoint().address().to_string();
				}
			}
			catch (system_error & e)
			{
				set_last_error(e.code().value());
			}
			return std::string();
		}

		/**
		 * @function : get the remote port
		 */
		virtual unsigned short get_remote_port() override
		{
			try
			{
				if (this->get_socket().is_open())
				{
					return this->get_socket().remote_endpoint().port();
				}
			}
			catch (system_error & e)
			{
				set_last_error(e.code().value());
			}
			return 0;
		}

	protected:
		virtual void _post_recv(std::shared_ptr<session_impl> this_ptr, std::shared_ptr<http_msg> http_msg_ptr)
		{
			if (this->is_started())
			{
				// Make the request empty before reading,
				// otherwise the operation behavior is undefined.
				http_msg_ptr->_reset();

				// Clear the buffer
				this->m_buffer.consume(this->m_buffer.size());

				// Read a request
				http_request * request = static_cast<http_request *>(http_msg_ptr.get());
				boost::beast::http::async_read(this->m_socket, this->m_buffer, *request,
					asio::bind_executor(
						*(this->m_strand_ptr),
						std::bind(
							&httpws_session_impl::_handle_recv, this,
							std::placeholders::_1, // error_code
							std::placeholders::_2, // bytes_recvd
							std::move(this_ptr),
							std::move(http_msg_ptr)
						)));
			}
		}

		virtual void _handle_recv(const error_code & ec, std::size_t bytes_recvd, std::shared_ptr<session_impl> this_ptr, std::shared_ptr<http_msg> http_msg_ptr)
		{
			std::ignore = bytes_recvd;

			if (!ec)
			{
				// every times recv data,we update the last active time.
				this->reset_last_active_time();

				// See if it is a WebSocket Upgrade
				http_request * request = static_cast<http_request *>(http_msg_ptr.get());
				if (!this->m_websocket_ptr && boost::beast::websocket::is_upgrade(*request))
				{
					this->_ws_upgrade(std::move(this_ptr), std::move(http_msg_ptr));

					return;
				}

				auto use_count = http_msg_ptr.use_count();

				this->_fire_recv(this_ptr, http_msg_ptr);

				if (http_msg_ptr->need_eof())
				{
					this->stop();
				}
				else
				{
					if (use_count == http_msg_ptr.use_count())
					{
						this->_post_recv(std::move(this_ptr), std::move(http_msg_ptr));
					}
					else
					{
						this->_post_recv(std::move(this_ptr), std::make_shared<http_request>());
					}
				}
			}
			else
			{
				set_last_error(ec.value());

				this->stop();
			}

			// If an error occurs then no new asynchronous operations are started. This
			// means that all shared_ptr references to the connection object will
			// disappear and the object will be destroyed automatically after this
			// handler returns. The connection class's destructor closes the socket.
		}

		virtual void _post_send(std::shared_ptr<session_impl> this_ptr, std::shared_ptr<http_msg> http_msg_ptr)
		{
			if (is_started())
			{
				error_code ec;
				// Write the response
				if (this->m_websocket_ptr)
				{
					this->m_websocket_ptr->text(this->m_websocket_ptr->got_text());

					http_msg_ptr->_send(*this->m_websocket_ptr, ec);
				}
				else
				{
					http_msg_ptr->_send(this->m_socket, ec);
				}
				set_last_error(ec.value());
				this->_fire_send(this_ptr, http_msg_ptr, ec.value());
				if (ec)
				{
					ASIO2_DUMP_EXCEPTION_LOG_IMPL;
					this->stop();
				}
			}
			else
			{
				set_last_error((int)errcode::socket_not_ready);
				this->_fire_send(this_ptr, http_msg_ptr, get_last_error());
			}
		}

		void _ws_upgrade(std::shared_ptr<session_impl> this_ptr, std::shared_ptr<http_msg> http_msg_ptr)
		{
			// Create a WebSocket websocket_session by transferring the socket
			this->m_websocket_ptr = std::make_shared<boost::beast::websocket::stream<asio::ip::tcp::socket>>(std::move(this->m_socket));

			this->m_ping_timer_ptr = std::make_shared<asio::steady_timer>(this->m_websocket_ptr->get_executor().context());

			// Set the control callback. This will be called
			// on every incoming ping, pong, and close frame.
			this->m_websocket_ptr->control_callback(
				asio::bind_executor(*(this->m_strand_ptr),
					std::bind(
						&httpws_session_impl::_ws_handle_control_callback, this,
						std::placeholders::_1,
						std::placeholders::_2
					)));

			// Run the timer. The timer is operated
			// continuously, this simplifies the code.
			auto seconds = (std::min<long>)(this->m_url_parser_ptr->get_silence_timeout() / 2, ASIO2_DEFAULT_HTTP_SILENCE_TIMEOUT / 2);
			this->m_ping_timer_ptr->expires_after(std::chrono::seconds(seconds <= long(0) ? ASIO2_DEFAULT_HTTP_SILENCE_TIMEOUT / 2 : seconds));
			this->m_ping_timer_ptr->async_wait(
				asio::bind_executor(*(this->m_strand_ptr),
					std::bind(&httpws_session_impl::_ws_handle_ping_timer, this,
						std::placeholders::_1, // error_code
						this_ptr
					)));

			// Accept the websocket handshake
			http_request * request = static_cast<http_request *>(http_msg_ptr.get());
			this->m_websocket_ptr->async_accept(
				*request,
				asio::bind_executor(*(this->m_strand_ptr),
					std::bind(
						&httpws_session_impl::_ws_handle_accept, this,
						std::placeholders::_1,
						this_ptr,
						http_msg_ptr
					)));
		}

		void _ws_handle_control_callback(boost::beast::websocket::frame_type kind, boost::beast::string_view payload)
		{
			if (kind == boost::beast::websocket::frame_type::close)
			{
				this->stop();
			}
			else
			{
				// Note that the connection is alive
				this->reset_last_active_time();
			}
		}

		// Called when the timer expires.
		void _ws_handle_ping_timer(const error_code & ec, std::shared_ptr<session_impl> this_ptr)
		{
			if (!ec)
			{
				// See if the timer really expired since the deadline may have moved.
				if (this->m_ping_timer_ptr->expiry() <= std::chrono::steady_clock::now())
				{
					// If this is the first time the timer expired,
					// send a ping to see if the other end is there.
					if (this->m_websocket_ptr->is_open())
					{
						// Run the timer. The timer is operated
						// continuously, this simplifies the code.
						auto seconds = (std::min<long>)(this->m_url_parser_ptr->get_silence_timeout() / 2, ASIO2_DEFAULT_HTTP_SILENCE_TIMEOUT / 2);
						this->m_ping_timer_ptr->expires_after(std::chrono::seconds(seconds <= long(0) ? ASIO2_DEFAULT_HTTP_SILENCE_TIMEOUT / 2 : seconds));
						this->m_ping_timer_ptr->async_wait(
							asio::bind_executor(*(this->m_strand_ptr),
								std::bind(&httpws_session_impl::_ws_handle_ping_timer, this,
									std::placeholders::_1, // error_code
									this_ptr
								)));

						// Now send the ping
						this->m_websocket_ptr->async_ping({},
							asio::bind_executor(
								*(this->m_strand_ptr),
								std::bind(
									&httpws_session_impl::_ws_handle_ping, this,
									std::placeholders::_1,
									this_ptr
								)));
					}
					else
					{
						// The timer expired while trying to handshake,
						// or we sent a ping and it never completed or
						// we never got back a control frame, so close.

						// Closing the socket cancels all outstanding operations. They
						// will complete with asio::error::operation_aborted
						this->stop();
					}
				}
				else
				{
					// silence timeout has elasped,but has't data trans,don't post a timer event again,so this session shared_ptr will
					// disappear and the object will be destroyed automatically after this handler returns.
					this->stop();
				}
			}
			else
			{
				// occur error,may be cancel is called
				this->stop();
			}
		}

		void _ws_handle_accept(const error_code & ec, std::shared_ptr<session_impl> this_ptr, std::shared_ptr<http_msg> http_msg_ptr)
		{
			if (!ec)
			{
				auto ws_msg_ptr = std::make_shared<ws_msg>();
				ws_msg_ptr->version   (http_msg_ptr->version());
				ws_msg_ptr->keep_alive(http_msg_ptr->keep_alive());
				ws_msg_ptr->method    (http_msg_ptr->method());
				ws_msg_ptr->result    (http_msg_ptr->result());

				this->_ws_post_recv(std::move(this_ptr), std::move(ws_msg_ptr));
			}
			else
			{
				//// Happens when the timer closes the socket
				//if (ec == asio::error::operation_aborted)
				//	return;

				this->stop();
			}
		}

		void _ws_handle_ping(const error_code & ec, std::shared_ptr<session_impl> this_ptr)
		{
			if (!ec)
			{
			}
			else
			{
				//// Happens when the timer closes the socket
				//if (ec == asio::error::operation_aborted)
				//	return;

				this->stop();
			}
		}

		virtual void _ws_post_recv(std::shared_ptr<session_impl> this_ptr, std::shared_ptr<http_msg> http_msg_ptr)
		{
			if (this->is_started())
			{
				ws_msg * ws_msg_ptr = static_cast<ws_msg *>(http_msg_ptr.get());

				// Clear the buffer
				ws_msg_ptr->consume(ws_msg_ptr->size());

				// Read a message into our buffer
				this->m_websocket_ptr->async_read(*ws_msg_ptr,
					asio::bind_executor(
						*(this->m_strand_ptr),
						std::bind(
							&httpws_session_impl::_ws_handle_recv, this,
							std::placeholders::_1,
							std::placeholders::_2,
							std::move(this_ptr),
							std::move(http_msg_ptr)
						)));
			}
		}

		virtual void _ws_handle_recv(const error_code & ec, std::size_t bytes_recvd, std::shared_ptr<session_impl> this_ptr, std::shared_ptr<http_msg> http_msg_ptr)
		{
			std::ignore = bytes_recvd;

			if (!ec)
			{
				// every times recv data,we update the last active time.
				this->reset_last_active_time();

				auto use_count = http_msg_ptr.use_count();

				this->_fire_recv(this_ptr, http_msg_ptr);

				if (use_count == http_msg_ptr.use_count())
				{
					this->_ws_post_recv(std::move(this_ptr), std::move(http_msg_ptr));
				}
				else
				{
					auto ws_msg_ptr = std::make_shared<ws_msg>();
					ws_msg_ptr->version   (http_msg_ptr->version());
					ws_msg_ptr->keep_alive(http_msg_ptr->keep_alive());
					ws_msg_ptr->method    (http_msg_ptr->method());
					ws_msg_ptr->result    (http_msg_ptr->result());

					this->_ws_post_recv(std::move(this_ptr), std::move(ws_msg_ptr));
				}
			}
			else
			{
				set_last_error(ec.value());

				this->stop();
			}

			// If an error occurs then no new asynchronous operations are started. This
			// means that all shared_ptr references to the connection object will
			// disappear and the object will be destroyed automatically after this
			// handler returns. The connection class's destructor closes the socket.
		}

		/// must override all listener functions,and cast the m_listener_mgr_ptr to http_server_listener_mgr,
		/// otherwise it will crash when these listener was called.
	protected:
		virtual void _fire_accept(std::shared_ptr<session_impl> & this_ptr)
		{
			static_cast<http_server_listener_mgr *>(this->m_listener_mgr_ptr.get())->notify_accept(this_ptr);
		}

		virtual void _fire_recv(std::shared_ptr<session_impl> & this_ptr, std::shared_ptr<http_msg> & http_msg_ptr)
		{
			static_cast<http_server_listener_mgr *>(this->m_listener_mgr_ptr.get())->notify_recv(this_ptr, http_msg_ptr);
		}

		virtual void _fire_send(std::shared_ptr<session_impl> & this_ptr, std::shared_ptr<http_msg> & http_msg_ptr, int error)
		{
			static_cast<http_server_listener_mgr *>(this->m_listener_mgr_ptr.get())->notify_send(this_ptr, http_msg_ptr, error);
		}

		virtual void _fire_close(std::shared_ptr<session_impl> & this_ptr, int error) override
		{
			if (!this->m_fire_close_is_called.test_and_set(std::memory_order_acquire))
			{
				static_cast<http_server_listener_mgr *>(this->m_listener_mgr_ptr.get())->notify_close(this_ptr, error);
			}
		}

	protected:
		//boost::beast::flat_buffer m_buffer{ 8192 };
		boost::beast::multi_buffer m_buffer;

		std::shared_ptr<boost::beast::websocket::stream<asio::ip::tcp::socket>> m_websocket_ptr;

		std::shared_ptr<asio::steady_timer> m_ping_timer_ptr;
	};

}

#endif

#endif // !__ASIO2_HTTPWS_SESSION_IMPL_HPP__