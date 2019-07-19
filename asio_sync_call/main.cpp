
#include <gtest/gtest.h>
#include "asio_sync_call.hpp"


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

