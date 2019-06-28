/*
 * COPYRIGHT (C) 2017-2019, zhllxt
 *
 * author   : zhllxt
 * email    : 37792738@qq.com
 *
 */

#ifndef __ASIO2_RPC_INVOKER_HPP__
#define __ASIO2_RPC_INVOKER_HPP__

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
#pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <cstdint>
#include <memory>
#include <chrono>
#include <functional>
#include <atomic>
#include <string>
#include <string_view>
#include <queue>
#include <any>
#include <future>
#include <tuple>
#include <unordered_map>
#include <type_traits>

#include <asio2/base/selector.hpp>
#include <asio2/base/iopool.hpp>
#include <asio2/base/error.hpp>

#include <asio2/base/detail/function_traits.hpp>
#include <asio2/rpc/detail/serialization.hpp>
#include <asio2/rpc/detail/protocol.hpp>

namespace asio2::detail
{
	template<class T>
	struct result_t
	{
		using type = T;
	};

	template<>
	struct result_t<void>
	{
		using type = std::int8_t;
	};

	class invoker
	{
	public:
		using self = invoker;

		/**
		 * @constructor
		 */
		invoker()
		{
		}

		/**
		 * @destructor
		 */
		~invoker() = default;

		/**
		 * @function : bind a rpc function
		 * @param    : name - Function name in string format
		 * @param    : fun - Function object
		 * @param    : obj - A pointer or reference to a class object, this parameter can be none
		 * if fun is nonmember function, the obj param must be none, otherwise the obj must be the
		 * the class object's pointer or refrence.
		 */
		template<class F, class ...C>
		inline void bind(std::string const& name, F&& fun, C&&... obj)
		{
#if defined(_DEBUG) || defined(DEBUG)
			{
				//std::shared_lock<std::shared_mutex> guard(this->mutex_);
				ASIO2_ASSERT(this->invokers_.find(name) == this->invokers_.end());
			}
#endif
			this->_bind(name, std::forward<F>(fun), std::forward<C>(obj)...);
		}

		/**
		 * @function : unbind a rpc function
		 */
		inline void unbind(std::string const& name)
		{
			//std::unique_lock<std::shared_mutex> guard(this->mutex_);
			this->invokers_.erase(name);
		}

		/**
		 * @function : find binded rpc function by name
		 */
		inline std::function<void(serializer&, deserializer&)>* find(std::string const& name)
		{
			//std::shared_lock<std::shared_mutex> guard(this->mutex_);
			auto iter = this->invokers_.find(name);
			if (iter == this->invokers_.end())
				return nullptr;
			return (&(iter->second));
		}

	protected:
		inline invoker& _invoker()
		{
			return (*this);
		}

		template<class F>
		inline void _bind(std::string const& name, F f)
		{
			//std::unique_lock<std::shared_mutex> guard(this->mutex_);
			this->invokers_[name] = std::bind(&self::template _proxy<F>, this, std::move(f),
				std::placeholders::_1, std::placeholders::_2);
		}

		template<class F, class C>
		inline void _bind(std::string const& name, F f, C& c)
		{
			//std::unique_lock<std::shared_mutex> guard(this->mutex_);
			this->invokers_[name] = std::bind(&self::template _proxy<F, C>, this, std::move(f), &c,
				std::placeholders::_1, std::placeholders::_2);
		}

		template<class F, class C>
		inline void _bind(std::string const& name, F f, C* c)
		{
			//std::unique_lock<std::shared_mutex> guard(this->mutex_);
			this->invokers_[name] = std::bind(&self::template _proxy<F, C>, this, std::move(f), c,
				std::placeholders::_1, std::placeholders::_2);
		}

		template<class F>
		inline void _proxy(F f, serializer& sr, deserializer& dr)
		{
			using fun_traits_type = function_traits<F>;
			using fun_args_tuple = typename fun_traits_type::pod_tuple_type;
			using fun_ret_type = typename fun_traits_type::return_type;

			fun_args_tuple tp;
			dr >> tp;
			_invoke<fun_ret_type>(f, sr, dr, tp);
		}

		template<class F, class C>
		inline void _proxy(F f, C* c, serializer& sr, deserializer& dr)
		{
			using fun_traits_type = function_traits<F>;
			using fun_args_tuple = typename fun_traits_type::pod_tuple_type;
			using fun_ret_type = typename fun_traits_type::return_type;

			fun_args_tuple tp;
			dr >> tp;
			_invoke<fun_ret_type>(f, c, sr, dr, tp);
		}

		template<typename R, typename F, typename... Args>
		inline void _invoke(const F& f, serializer& sr, deserializer& dr, const std::tuple<Args...>& tp)
		{
			ignore::unused(dr);
			typename result_t<R>::type r = _invoke_impl<R>(f, std::make_index_sequence<sizeof...(Args)>{}, tp);
			sr << error_code{};
			sr << r;
		}

		template<typename R, typename F, typename C, typename... Args>
		inline void _invoke(const F& f, C* c, serializer& sr, deserializer& dr, const std::tuple<Args...>& tp)
		{
			ignore::unused(dr);
			typename result_t<R>::type r = _invoke_impl<R>(f, c, std::make_index_sequence<sizeof...(Args)>{}, tp);
			sr << error_code{};
			sr << r;
		}

		template<typename R, typename F, size_t... I, typename... Args>
		typename std::enable_if_t<!std::is_same_v<R, void>, typename result_t<R>::type>
			inline _invoke_impl(const F& f, const std::index_sequence<I...>&, const std::tuple<Args...>& tp)
		{
			return f(std::get<I>(tp)...);
		}

		template<typename R, typename F, size_t... I, typename... Args>
		typename std::enable_if_t<std::is_same_v<R, void>, typename result_t<R>::type>
			inline _invoke_impl(const F& f, const std::index_sequence<I...>&, const std::tuple<Args...>& tp)
		{
			f(std::get<I>(tp)...);
			return 1;
		}

		template<typename R, typename F, typename C, size_t... I, typename... Args>
		typename std::enable_if_t<!std::is_same_v<R, void>, typename result_t<R>::type>
			inline _invoke_impl(const F& f, C* c, const std::index_sequence<I...>&, const std::tuple<Args...>& tp)
		{
			return (c->*f)(std::get<I>(tp)...);
		}

		template<typename R, typename F, typename C, size_t... I, typename... Args>
		typename std::enable_if_t<std::is_same_v<R, void>, typename result_t<R>::type>
			inline _invoke_impl(const F& f, C* c, const std::index_sequence<I...>&, const std::tuple<Args...>& tp)
		{
			(c->*f)(std::get<I>(tp)...);
			return 1;
		}

	protected:
		//std::shared_mutex                           mutex_;

		std::unordered_map<std::string, std::function<void(serializer&, deserializer&)>> invokers_;
	};
}

#endif // !__ASIO2_RPC_INVOKER_HPP__
