#include "mainwindow.h"

namespace
{
/** sysfs GPIO 节点在 export 后可能延迟出现，对 ENOENT/EINTR 做短重试。 */
int openSysfsGpioRetry(const char *path, int flags)
{
    int fd = -1;
    for (int i = 0; i < 80; ++i)
    {
        fd = open(path, flags);
        if (fd >= 0)
            return fd;
        if (errno != ENOENT && errno != EINTR)
            break;
        usleep(2500);
    }
    return -1;
}

bool readSysfsGpioText(const char *path, char *buffer, size_t bufferSize)
{
    if (!buffer || bufferSize == 0)
        return false;

    const int fd = openSysfsGpioRetry(path, O_RDONLY);
    if (fd < 0)
        return false;

    errno = 0;
    const ssize_t bytesRead = read(fd, buffer, bufferSize - 1);
    close(fd);
    if (bytesRead <= 0)
        return false;

    buffer[bytesRead] = '\0';
    return true;
}

bool gpioDirectionIsOut(const char *pin)
{
    char path[128];
    char direction[16];

    snprintf(path, sizeof(path), GPIO_PATH "/gpio%s/direction", pin);
    if (!readSysfsGpioText(path, direction, sizeof(direction)))
        return false;

    return strncmp(direction, "out", 3) == 0;
}
} // namespace

void MainWindow::initGPIO()
{
    Logger::Log("Initializing GPIO...", LogLevel::INFO, DeviceType::MAIN);
    exportGPIO(GPIO_PIN_1);
    setGPIODirection(GPIO_PIN_1, "out");
    Logger::Log(gpioDirectionIsOut(GPIO_PIN_1)
                    ? "Set direction of GPIO_PIN_1 to output completed!"
                    : "GPIO_PIN_1 direction is not out after initialization",
                gpioDirectionIsOut(GPIO_PIN_1) ? LogLevel::INFO : LogLevel::WARNING,
                DeviceType::MAIN);

    exportGPIO(GPIO_PIN_2);
    setGPIODirection(GPIO_PIN_2, "out");
    Logger::Log(gpioDirectionIsOut(GPIO_PIN_2)
                    ? "Set direction of GPIO_PIN_2 to output completed!"
                    : "GPIO_PIN_2 direction is not out after initialization",
                gpioDirectionIsOut(GPIO_PIN_2) ? LogLevel::INFO : LogLevel::WARNING,
                DeviceType::MAIN);

    setGPIOValue(GPIO_PIN_1, "1");
    Logger::Log("Set GPIO_PIN_1 level to high completed!", LogLevel::INFO, DeviceType::MAIN);
    setGPIOValue(GPIO_PIN_2, "1");
    Logger::Log("Set GPIO_PIN_2 level to high completed!", LogLevel::INFO, DeviceType::MAIN);

    Logger::Log("GPIO initialization completed!", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::exportGPIO(const char *pin)
{
    int fd;
    char buf[64];

    fd = open(GPIO_EXPORT, O_WRONLY);
    if (fd < 0)
    {
        Logger::Log("Failed to open export file for writing", LogLevel::WARNING, DeviceType::MAIN);
        return;
    }
    snprintf(buf, sizeof(buf), "%s", pin);
    errno = 0;
    if (write(fd, buf, strlen(buf)) != (ssize_t) strlen(buf))
    {
        if (errno == EBUSY)
        {
            Logger::Log(std::string("GPIO already exported: pin=") + pin, LogLevel::DEBUG, DeviceType::MAIN);
        }
        else
        {
            Logger::Log(std::string("Failed to write to export file: ") + strerror(errno), LogLevel::WARNING, DeviceType::MAIN);
        }
        close(fd);
        return;
    }
    close(fd);
    Logger::Log("GPIO pin export successful", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::setGPIODirection(const char *pin, const char *direction)
{
    int fd;
    char path[128];

    snprintf(path, sizeof(path), GPIO_PATH "/gpio%s/direction", pin);
    fd = openSysfsGpioRetry(path, O_WRONLY);
    if (fd < 0)
    {
        Logger::Log(std::string("Failed to open GPIO direction file for writing: ") + path + " " + strerror(errno),
                    LogLevel::WARNING, DeviceType::MAIN);
        return;
    }
    errno = 0;
    if (write(fd, direction, strlen(direction)) != (ssize_t) strlen(direction))
    {
        Logger::Log(std::string("Failed to set GPIO direction: ") + strerror(errno), LogLevel::WARNING, DeviceType::MAIN);
        close(fd);
        return;
    }
    close(fd);
    Logger::Log("GPIO direction set successfully", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::setGPIOValue(const char *pin, const char *value)
{
    int fd;
    char path[128];

    if (!gpioDirectionIsOut(pin))
    {
        Logger::Log(std::string("GPIO direction is not out before writing value, trying to repair: pin=") + pin,
                    LogLevel::WARNING, DeviceType::MAIN);
        setGPIODirection(pin, "out");
        if (!gpioDirectionIsOut(pin))
        {
            Logger::Log(std::string("GPIO direction is still not out, abort writing value: pin=") + pin,
                        LogLevel::WARNING, DeviceType::MAIN);
            return;
        }
    }

    snprintf(path, sizeof(path), GPIO_PATH "/gpio%s/value", pin);
    fd = openSysfsGpioRetry(path, O_WRONLY);
    if (fd < 0)
    {
        Logger::Log(std::string("Failed to open GPIO value file for writing: ") + path + " " + strerror(errno),
                    LogLevel::WARNING, DeviceType::MAIN);
        return;
    }
    errno = 0;
    if (write(fd, value, strlen(value)) != (ssize_t) strlen(value))
    {
        Logger::Log(std::string("Failed to write to GPIO value: ") + strerror(errno), LogLevel::WARNING, DeviceType::MAIN);
        close(fd);
        return;
    }
    close(fd);
    Logger::Log("GPIO value set successfully", LogLevel::INFO, DeviceType::MAIN);
}

int MainWindow::readGPIOValue(const char *pin)
{
    char path[128];
    char value[8];

    snprintf(path, sizeof(path), GPIO_PATH "/gpio%s/value", pin);

    if (!readSysfsGpioText(path, value, sizeof(value)))
    {
        Logger::Log(std::string("Failed to read GPIO value file: ") + path + " " + strerror(errno),
                    LogLevel::WARNING, DeviceType::MAIN);
        return -1;
    }

    if (value[0] == '1')
    {
        return 1;
    }
    else if (value[0] == '0')
    {
        return 0;
    }
    else
    {
        return -1;
    }
}

void MainWindow::getGPIOsStatus()
{
    int value1 = readGPIOValue(GPIO_PIN_1);
    emit wsThread->sendMessageToClient("OutputPowerStatus:1:" + QString::number(value1));
    int value2 = readGPIOValue(GPIO_PIN_2);
    emit wsThread->sendMessageToClient("OutputPowerStatus:2:" + QString::number(value2));
}
