#pragma once

#include <atomic>
#include <mutex>
#include <chrono>
#include <thread>
#include <future>

#if defined(__linux__)
#include <memory>
#include <vector>
#include <fstream>
#include <sstream>
#include <iterator>
#include <algorithm>

#include <sys/types.h>
#include <sys/sysinfo.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#elif defined(_WIN32)
#include <Windows.h>
#endif

class InputDeviceListener final
{
public:
	InputDeviceListener() {}
    ~InputDeviceListener() { stop(); }

	InputDeviceListener(const InputDeviceListener &) = delete;
	InputDeviceListener(InputDeviceListener &&) = delete;

	InputDeviceListener &operator=(const InputDeviceListener &) = delete;
	InputDeviceListener &operator=(InputDeviceListener &&) = delete;

    bool isRunning() const { return m_isRunning; }

    time_t lastOperateTime() const { return m_lastOperateTime; }

    bool start() 
	{
		std::unique_lock<std::mutex> lock{ m_mtx };
		return listen(); 
	}
    void stop() 
	{
		std::unique_lock<std::mutex> lock{ m_mtx };
		shutdown();
	}

private:
#if defined(__linux__)
    class InputDevice
    {
    public:
        InputDevice(const std::string &id, const std::string &name, const std::string &handler)
            : m_fd{ -1 }, m_id{ id }, m_name{ name }, m_handler{ handler } {}
        ~InputDevice() { close(); }

        InputDevice(const InputDevice &) = delete;
        InputDevice &operator=(const InputDevice &) = delete;

        InputDevice(InputDevice &&other)
        {
            *this = std::move(other);
        }

        InputDevice &operator=(InputDevice &&other)
        {
            m_fd = other.m_fd;
            m_id = std::move(other.m_id);
            m_name = std::move(other.m_name);
            m_handler = std::move(other.m_handler);

            other.m_fd = -1;

            return *this;
        }

        bool isOpened() const
        {
            if (::access(m_handler.c_str(), F_OK) == 0)
            {
                return (m_fd == -1);
            }

            return false;
        }

        bool open()
        {
            if (m_fd != -1)
            {
                return true;
            }

            if (m_handler.empty())
            {
                return false;
            }

            int fd = ::open(m_handler.c_str(), O_RDONLY);
            if (fd < 0)
            {
                ::perror(m_handler.c_str());
                return false;
            }

            m_fd = fd;
            return true;
        }

        void close()
        {
            if (m_fd != -1)
            {
                ::close(m_fd);
                m_fd = -1;
            }
        }

        int fd() const { return m_fd; }
        std::string id() const { return m_name; }
        std::string name() const { return m_id; }

        bool operator==(const InputDevice &other) const
        {
            if (!m_handler.empty() && m_handler == other.m_handler)
            {
                return true;
            }

            return (m_id == other.m_id);
        }

        bool operator<(const InputDevice &other) const
        {
            return m_fd < other.m_fd;
        }

        operator int() const
        {
            return m_fd;
        }

    private:
        int         m_fd{ -1 };
        std::string m_id;
        std::string m_name;
        std::string m_handler;
    };

    static void availableInputDevices(std::vector<InputDevice> &devices)
    {
        const static std::string devicesFile{ "/proc/bus/input/devices" };
        const static std::string devicePath{ "/dev/input/" };

        const static std::string idPrefix{ "I: " };
        const static std::string namePrefix{ "N: Name=" };
        const static std::string handlerPrefix{ "H: Handlers=" };
        const static std::string eventPrefix{ "B: EV=" };

        const static std::array<int, 4> bits{ 0x01, 0x02, 0x04, 0x88 };

        std::ifstream ifs{ devicesFile, std::ios::in };
        if (!ifs.is_open())
        {
            return;
        }

        ifs >> std::noskipws;

        std::string content{ std::istream_iterator<char>{ ifs }, std::istream_iterator<char>{} };
        ifs.close();

        std::vector<std::string> deviceInfo;
        {
            const std::string sep{ "\n\n" };

            size_t begin = 0;
            size_t end = 0;
            std::string token;

            while ((end = content.find(sep, begin)) != std::string::npos)
            {
                token = content.substr(begin, end - begin);
                if (!token.empty())
                {
                    begin += token.length();
                    deviceInfo.push_back(std::move(token));
                }
                begin += sep.length();
            }
        }

        for (auto &info : deviceInfo)
        {
            std::vector<std::string> props;
            {
                std::stringstream ss{ info };
                std::string token;

                while (std::getline(ss, token, '\n'))
                {
                    if (!token.empty())
                    {
                        props.push_back(std::move(token));
                    }
                }
            }

            if (props.empty())
            {
                continue;
            }

            std::string id;
            std::string name;
            std::string handler;
            bool isInputDevice{ false };

            for (auto &prop : props)
            {
                if (prop.find(idPrefix) == 0)
                {
                    id = prop.substr(idPrefix.length());
                }
                else if (prop.find(namePrefix) == 0)
                {
                    name = prop.substr(namePrefix.length());
                    if (!name.empty())
                    {
                        name.erase(0, name.find_first_not_of('\"'));
                        name.erase(name.find_last_not_of('\"') + 1);
                    }
                }
                else if (prop.find(handlerPrefix) == 0)
                {
                    std::stringstream ss{ prop.substr(handlerPrefix.length()) };
                    std::string token;

                    while (std::getline(ss, token, ' '))
                    {
                        if (!token.empty() && token.find("event") == 0)
                        {
                            handler = devicePath + token;
                            break;
                        }
                    }
                }
                else if (prop.find(eventPrefix) == 0)
                {
                    int i = 0, j = 0;
                    auto ev = prop.substr(eventPrefix.length());

                    for (auto it = ev.rbegin(); it != ev.rend(); ++it)
                    {
                        auto n = std::stoi(std::string{ *it }, nullptr, 16);
                        for (size_t k = 0; k < bits.size(); ++k)
                        {
                            if (n & bits[k])
                            {
                                if ((i + j) == EV_KEY || (i + j) == EV_REL || (i + j) == EV_ABS)
                                {
                                    isInputDevice = true;
                                    break;
                                }
                            }

                            if (++i > 9)
                            {
                                i = 0;
                                j += 10;
                            }
                        }
                    }
                }
            }

            if (isInputDevice && !handler.empty())
            {
                devices.emplace_back(id, name, handler);
            }
        }
    }

    static void openInputDevices(fd_set &allfds, std::vector<InputDevice> &devices)
    {
        FD_ZERO(&allfds);

        std::vector<InputDevice> allDevices;
        std::vector<InputDevice> openedDevices;

        availableInputDevices(allDevices);
        for (auto it = allDevices.begin(); it != allDevices.end(); ++it)
        {
            bool isOpened{ false };

            auto dIt = std::find_if(devices.begin(), devices.end(), [&it](const InputDevice &dev){ return *it == dev; });
            if (dIt != devices.end())
            {
                isOpened = dIt->isOpened();
                if (isOpened)
                {
                    FD_SET(*dIt, &allfds);
                    openedDevices.push_back(std::move(*dIt));
                }
            }

            if (!isOpened)
            {
                if (it->open())
                {
                    FD_SET(*it, &allfds);
                    openedDevices.push_back(std::move(*it));
                }
            }
        }

        std::sort(openedDevices.begin(), openedDevices.end(), std::less<InputDevice>{});
        devices.swap(openedDevices);
    }

    static void closeInputDevices(std::vector<InputDevice> &devices)
    {
        for (auto &device : devices)
        {
            device.close();
        }
    }

    void run(std::promise<bool> quitPromise)
    {
        int ret{ -1 };
        bool isListening{ false };
        ssize_t n;
        fd_set rfds, allfds;

        struct timeval tv;
        struct input_event event;

        std::vector<InputDevice> devices;

        auto now = getCurrentTime();
        auto last = now;
        while (m_isRunning)
        {
            openInputDevices(allfds, devices);
            if (devices.empty())
            {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                continue;
            }

            isListening = true;
            last = getCurrentTime();
            while (m_isRunning && isListening)
            {
                now = getCurrentTime();
                if (last - now > 5)
                {
                    break;
                }
                last = now;

                rfds = allfds;
                tv.tv_sec = 5;
                tv.tv_usec = 0;

                ret = ::select(devices.back() + 1, &rfds, nullptr, nullptr, &tv);
                if (ret < 0)
                {
                    break;
                }
                else if (ret == 0)
                {
                    continue;
                }
                else
                {
                    for (auto it = devices.begin(); it != devices.end(); ++it)
                    {
                        if (FD_ISSET(*it, &rfds))
                        {
                            n = ::read(*it, &event, sizeof(event));
                            if (n == sizeof(event))
                            {
                                if (event.type == EV_KEY || event.type == EV_REL || event.type == EV_ABS)
                                {
                                    m_lastOperateTime = getCurrentTime();
                                }
                            }
                            else if (n <= 0)
                            {
                                FD_CLR(*it, &allfds);
                                it->close();
                                isListening = false;
                                break;
                            }
                        }
                    }
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        closeInputDevices(devices);

        m_isRunning = false;
        quitPromise.set_value(m_isRunning);
    }

    time_t getCurrentTime() const
    {
        struct timespec res;
        clock_gettime(CLOCK_MONOTONIC, &res);
        return res.tv_sec;

        // return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

#elif defined(_WIN32)
	void run(std::promise<bool> quitPromise)
	{
		LASTINPUTINFO plii;

		while (m_isRunning)
		{
			plii.cbSize = sizeof(LASTINPUTINFO);
            if (::GetLastInputInfo(&plii))
			{
                m_lastOperateTime = plii.dwTime / 1000;
			}

			std::this_thread::sleep_for(std::chrono::seconds(1));
		}

		m_isRunning = false;
		quitPromise.set_value(m_isRunning);
	}

#endif

	bool listen()
	{
		if (m_isRunning)
		{
			return true;
		}

		m_isRunning = true;

		std::promise<bool> quitPromise;
		m_future = quitPromise.get_future();

		std::thread thd{ &InputDeviceListener::run, this, std::move(quitPromise) };
		thd.detach();

		return true;
	}

	void shutdown()
	{
		if (m_isRunning)
		{
			m_isRunning = false;
			m_future.get();
		}
	}

private:
	std::mutex m_mtx;
	std::future<bool> m_future;
    std::atomic_bool m_isRunning{ false };
    std::atomic<time_t> m_lastOperateTime{ 0 };
};
