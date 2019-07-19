#pragma once

#include <future>
#include <chrono>
#include <memory>

#include <boost/asio.hpp>

namespace detail
{
    template <typename T> struct is_chrono_duration : std::false_type {};

    template <typename R, typename P>
    struct is_chrono_duration<std::chrono::duration<R, P>> : std::true_type {};
}

struct asio_sync_call : std::enable_shared_from_this<asio_sync_call>
{
    asio_sync_call(boost::asio::io_service& io)
        : _io(io)
    {
    }

    template <typename F, typename Param,
        typename std::enable_if<std::is_integral<Param>::value>::type* = nullptr>
        auto wait(F handler, Param count)
    {
        std::promise<boost::system::error_code> is_done;
        auto self = this->shared_from_this();

        make_async(handler, [&count]() { return count-- > 0; }, is_done);

        return is_done.get_future().get();
    }

    template <typename F,
        typename Timer = boost::asio::steady_timer,
        typename Param,
        typename std::enable_if<detail::is_chrono_duration<Param>::value>::type* = nullptr>
        auto wait(F handler, Param duration)
    {
        std::promise<boost::system::error_code> is_done;
        const auto self = this->shared_from_this();

        auto timer = std::make_shared<Timer>(_io);
        timer->expires_after(duration);
        timer->async_wait([self, timer, &is_done](const boost::system::error_code& ec)
            {
                if (!ec)
                {
                    if (timer->expires_at() == Timer::time_point::min())
                    {
                        self->cancel(boost::asio::error::operation_aborted);
                    }
                    else
                    {
                        self->cancel(boost::asio::error::timed_out);
                        timer->expires_at(Timer::time_point::min());
                    }
                }
                else
                {
                    self->cancel(ec);
                }
            });

        make_async(handler, [timer]() { return true; }, is_done);

        return is_done.get_future().get();
    }

    void cancel(boost::system::error_code ec = make_error_code(boost::asio::error::operation_aborted))
    {
        const auto self = this->shared_from_this();
        _io.dispatch([self, ec]()
            {
                self->_cancel = true;
                self->_cancel_error_code = ec;
            });
    }

private:
    template <typename F, typename Condition>
    void make_async(F handler, Condition predicate, std::promise<boost::system::error_code>& done)
    {
        const auto self = this->shared_from_this();
        _io.post([self, handler, predicate, &done]()
            {
                if (self->_cancel)
                {
                    done.set_value(self->_cancel_error_code);
                    return;
                }

                if (predicate())
                {
                    boost::system::error_code ec;
                    handler(ec);
                    if (ec)
                    {
                        self->make_async<decltype(handler)>(handler, predicate, done);
                        return;
                    }
                }

                self->cancel(make_error_code(boost::system::errc::success));
                self->make_async<decltype(handler)>(handler, predicate, done);
            });
    }

    boost::asio::io_service& _io;
    bool _cancel{ false };
    boost::system::error_code _cancel_error_code;
};

