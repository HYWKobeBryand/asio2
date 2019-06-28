/*
 * COPYRIGHT (C) 2017-2019, zhllxt
 *
 * author   : zhllxt
 * email    : 37792738@qq.com
 * 
 */

#ifndef ASIO_STANDALONE

#ifndef __ASIO2_WS_SERVER_HPP__
#define __ASIO2_WS_SERVER_HPP__

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
#pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <asio2/tcp/tcp_server.hpp>
#include <asio2/http/ws_session.hpp>

namespace asio2::detail
{
	template<class derived_t, class session_t>
	class ws_server_impl_t : public tcp_server_impl_t<derived_t, session_t>
	{
		template <class, bool>  friend class user_timer_cp;
		template <class, class> friend class server_impl_t;
		template <class, class> friend class tcp_server_impl_t;

	public:
		using self = ws_server_impl_t<derived_t, session_t>;
		using super = tcp_server_impl_t<derived_t, session_t>;
		using session_type = session_t;

		/**
		 * @constructor
		 */
		explicit ws_server_impl_t(
			std::size_t init_buffer_size = tcp_frame_size,
			std::size_t max_buffer_size = (std::numeric_limits<std::size_t>::max)(),
			std::size_t concurrency = std::thread::hardware_concurrency() * 2
		)
			: super(init_buffer_size, max_buffer_size, concurrency)
		{
		}

		/**
		 * @destructor
		 */
		~ws_server_impl_t()
		{
			this->stop();
			this->iopool_.stop();
		}

		/**
		 * @function : start the server
		 * @param service A string identifying the requested service. This may be a
		 * descriptive name or a numeric string corresponding to a port number.
		 */
		bool start(std::string_view service)
		{
			return this->start(std::string_view{}, service);
		}

		/**
		 * @function : start the server
		 * @param host A string identifying a location. May be a descriptive name or
		 * a numeric address string.
		 * @param service A string identifying the requested service. This may be a
		 * descriptive name or a numeric string corresponding to a port number.
		 */
		bool start(std::string_view host, std::string_view service)
		{
			return this->derived()._do_start(host, service, condition_wrap<void>{});
		}

	public:
		/**
		 * @function : bind websocket upgrade listener
		 * @param    : fun - a user defined callback function
		 * Function signature : void(std::shared_ptr<asio2::ws_session>& session_ptr, asio::error_code ec)
		 */
		template<class F, class ...C>
		inline derived_t & bind_upgrade(F&& fun, C&&... obj)
		{
			this->listener_.bind(event::upgrade, observer_t<std::shared_ptr<session_t>&, error_code>(std::forward<F>(fun), std::forward<C>(obj)...));
			return (this->derived());
		}

	protected:

	};
}

namespace asio2
{
	template<class session_t>
	class ws_server_t : public detail::ws_server_impl_t<ws_server_t<session_t>, session_t>
	{
	public:
		using detail::ws_server_impl_t<ws_server_t<session_t>, session_t>::ws_server_impl_t;
	};

	using ws_server = ws_server_t<ws_session>;
}

#endif // !__ASIO2_WS_SERVER_HPP__

#endif
