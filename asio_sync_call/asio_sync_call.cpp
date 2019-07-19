#include <future>
#include <chrono>
#include <iostream>
#include <memory>

#include <boost/asio.hpp>

#include <gtest/gtest.h>

template <typename TTimer = boost::asio::steady_timer>
struct TimedSyncCall : std::enable_shared_from_this<TimedSyncCall<TTimer>>
{
    TimedSyncCall(boost::asio::io_service& io)
        : mTimer(io)
    {
    }

    template <typename THandler, typename TDuration = TTimer::clock_type::duration>
    boost::system::error_code wait(TDuration duration, THandler handler)
    {
        auto self = this->shared_from_this();

        mTimer.expires_after(duration);
        mTimer.async_wait([self](const boost::system::error_code& ec)
            {
                if (!ec)
                {
                    self->mPromise.set_value(boost::asio::error::timed_out);
                }
                else
                {
                    self->mPromise.set_value(self->mHandlerErrorCode);
                }
            });

        boost::asio::post(mTimer.get_executor(), [handler, self]()
            {
                handler([self](const boost::system::error_code& ec)
                    {
                        self->mHandlerErrorCode = ec;
                        self->mTimer.cancel();
                    });
            });

        return mPromise.get_future().get();
    }

    TTimer mTimer;
    std::promise<boost::system::error_code> mPromise;
    boost::system::error_code mHandlerErrorCode;
};


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


struct asio_sync_call_fixture : ::testing::Test
{
    asio_sync_call_fixture() = default;

protected:
    void SetUp()
    {
        _worker.reset(new boost::asio::io_service::work(_io));
        _runner = std::async(std::launch::async, [this]()
            {
                _io.run();
                std::cout << "io_service::run() exited\n";
            });
    }

    void TearDown()
    {
        _worker.reset();
    }

    boost::asio::io_service _io;
    std::shared_ptr<boost::asio::io_service::work> _worker;
    std::future<void> _runner;
};

TEST_F(asio_sync_call_fixture, repeat_0_times)
{
    auto t = std::make_shared<asio_sync_call>(_io);

    std::size_t counter = 0;
    auto rc = t->wait([&counter](boost::system::error_code& ec)
        {
            ec = make_error_code(boost::asio::error::access_denied);
            counter++;
        },
        0);

    ASSERT_EQ(counter, 0);
    ASSERT_TRUE(rc == boost::system::errc::success);
}

TEST_F(asio_sync_call_fixture, repeat_5_times)
{
    auto t = std::make_shared<asio_sync_call>(_io);

    std::size_t counter = 0;
    auto rc = t->wait([&counter](boost::system::error_code& ec)
        {
            ec = make_error_code(boost::asio::error::access_denied);
            counter++;
        },
        5);

    ASSERT_EQ(counter, 5);
    ASSERT_TRUE(rc == boost::system::errc::success);
}

TEST_F(asio_sync_call_fixture, repeat_5_times_exit_on_3)
{
    auto t = std::make_shared<asio_sync_call>(_io);

    std::size_t counter = 0;
    auto rc = t->wait([&counter](boost::system::error_code& ec)
        {
            if (counter == 3) ec = make_error_code(boost::system::errc::success);
            else ec = make_error_code(boost::asio::error::access_denied);
            counter++;
        },
        5);

    ASSERT_EQ(counter, 4);
    ASSERT_TRUE(rc == boost::system::errc::success);
}


TEST_F(asio_sync_call_fixture, repeat_10_times_cancel)
{
    auto t = std::make_shared<asio_sync_call>(_io);

    auto async_cancel = std::async(std::launch::async, [t]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            t->cancel();
        });

    auto rc = t->wait([](boost::system::error_code& ec)
        {
            ec = make_error_code(boost::asio::error::access_denied);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        },
        10);

    ASSERT_EQ(rc, boost::asio::error::operation_aborted);
}

TEST_F(asio_sync_call_fixture, wait_for_1sec_timeout)
{
    auto t = std::make_shared<asio_sync_call>(_io);

    auto ec = t->wait([&](boost::system::error_code & ec)
        {
            ec = make_error_code(boost::asio::error::access_denied);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        },
        std::chrono::milliseconds(1000));
    ASSERT_EQ(ec, boost::asio::error::timed_out);
}

TEST_F(asio_sync_call_fixture, wait_for_1sec_success)
{
    auto t = std::make_shared<asio_sync_call>(_io);
  
    auto ec = t->wait([&](boost::system::error_code& ec)
        {
            ec = make_error_code(boost::system::errc::success);
        },
        std::chrono::milliseconds(1000));
    ASSERT_EQ(ec, boost::system::errc::success);
}

TEST_F(asio_sync_call_fixture, wait_for_1sec_cancel)
{
    auto t = std::make_shared<asio_sync_call>(_io);

    auto async_cancel = std::async(std::launch::async, [t]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            t->cancel();
        });

    auto ec = t->wait([&](boost::system::error_code& ec)
        {
            ec = make_error_code(boost::asio::error::access_denied);
        },
        std::chrono::milliseconds(1000));
    ASSERT_EQ(ec, boost::asio::error::operation_aborted);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

